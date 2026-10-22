# x68kzremotedrv-netpipe

[x68kzremotedrv](https://github.com/yunkya2/x68kzremotedrv) (yunkya2 氏、MIT) に netpipe を足すパッチ。
X68000 Z の SCSI ID 6 越しに SLIP のバイト列を運び、Z から Wi-Fi で TCP/IP を使えるようにする。
Z 側は [../../x68kcli](../../x68kcli) の `slipd.x -z` (`netup pico` / `netup bridge`)。

## 当て方

```sh
git clone https://github.com/yunkya2/x68kzremotedrv
cd x68kzremotedrv
git checkout 3afd408df99f3f9d1aafc7c090e54285cbeadba6   # "Show system statistics by default"
git am /path/to/x68k/x68kzremotedrv-netpipe/patches/*.patch
make
```

macOS (arm-gnu-toolchain 14.3) では次の設定でビルドした:

```sh
export PATH=~/opt/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin:$PATH
export CMAKE_POLICY_VERSION_MINIMUM=3.5
export CFLAGS="-include time.h -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion"
```

USB キーボード (0012) を入れるときは `cmake -DHID_KEYBOARD=ON ..` (既定はオフで、オフならバイナリは 0011 までと同じ)。

書き込みは BOOTSEL のまま `picotool load -x build/x68kzremotedrv.uf2`。設定 (フラッシュ 0x1F0000) は残る。

## 中身

| ファイル | 変更 |
|---|---|
| `src/netpipe.c` / `netpipe.h` | 新規。要求を LBA 0x100000 に書き、応答を 0x110000〜0x1ffff7 から読む。UDP モードとブリッジモード |
| `src/virtual_disk.c` | ID 6 の読み書きを netpipe に回す |
| `src/connect.c` | Wi-Fi の省電力を切る (応答が 50〜300ms 遅れて TCP が遅くなるため) |
| `src/accel.c` / `accel_ops.c` / `accel_proto.h` / `monocypher/` | 0011: 暗号アクセラレータ (X25519 / Ed25519 を Z の代わりに計算する。[../accel](../accel)) |
| `src/msclog.c` / `hidkbd.c` | 0013: 起動時の調査用の MSC ログ (`-DMSC_LOG=ON`、UDP 6870) と、6869 の `status` |
| `src/hidkbd.c` / `hidkbd_cmd.c` / `usb_descriptors.c` / `test/hidkbd/` | 0012: USB キーボード (HID) を MSC の後ろに足す (`-DHID_KEYBOARD=ON`)。UDP 6869 で [picokbd](../../harness/picokbd) と同じコマンドを受ける |

- **UDP モード** (`slipd.x -z`): SLIP を UDP 6868 で `config.txt` の SMB サーバ (Raspberry Pi) に送る。
  Pi 側は `x68kcli/templates/raspi/slipudp.py` + slattach (`MODE=pico`)。
- **ブリッジモード** (`slipd.x -z 192.168.1.68`): Pico がその IP を代理 ARP で名乗り、IPv4 を Wi-Fi に転送する。Pi は要らない。
  外からの ping (echo request) は Z に渡さない。

## USB キーボード (0012)

別の Pico W に入れていた [picokbd](../../harness/picokbd) を netpipe の Pico に取り込む。Pi の `x68key` の宛先をこの Pico にする
(`PICOKBD=192.168.1.117 x68key combo ctrl f11 del`)。コマンド (`key` / `combo` / `hold` / `type`) と押す間隔 (40ms) は picokbd と同じ。

- MSC は 0 番のインタフェースのまま、キーボード (boot protocol) を 1 番 (EP 0x82) に足す。VID:PID は 0xCafe:0x4002 のまま、bcdDevice を 0x0101 にする
- USB タスクは Z の SCSI も受け持つので待たない。lwIP の受信コールバックは 1 行を写すだけで、USB タスクが毎周、時刻が来ていればレポートを 1 つ送る
- 6869 の口は Wi-Fi がつながったら開く (Z が固まっていてもリセットできる)。実行中に来たコマンドには `err busy`、Z が HID を 1 秒読みに来なければ `err usb`
- 知らないキー名があれば何も押さずに `err key` (picokbd は手前まで押していた)
- 解釈部はホストで試験できる: `make -C test/hidkbd`
- `x68key status` (0013) で Pico の状態を 1 行で返す。キーは押さない。キーの実行中も、USB タスクが SMB の接続を待っている間も答える:
  `ok stat=5 wifi=up ip=192.168.1.118 rssi=-55 smb=mounted hds0=open remote0=ok uptime=812s usb=mounted reads=1234 writes=56 npd=789 accel=ready/3 hid=ok heap=60000`
  (stat は sysstatus、reads/writes は MSC のセクタ数、npd は netpipe の交換回数、accel は状態/計算した数、heap は空き)
- 古い picokbd を挿したまま `x68key` を既定 (ブロードキャスト) で使うと、両方が打って文字が二重になる

## MSC ログ (0013、調査用)

電源投入時にときどき「ディスクから起動できません」になる件の調査用。`cmake -DHID_KEYBOARD=ON -DMSC_LOG=ON ..`
(受け手は `-DMSC_LOG_HOST=a.b.c.d`、既定 192.168.1.108) で、Z からの USB MSC の命令を 1 件 1 行の UDP (6870) で送る。
Wi-Fi がつながる前の分もリング (128 件) に残して後で送る。USB の mount のあとと 2 秒以上途切れたあとの 300 件だけ。
ID 6 の netpipe / scsiremote の帯は数だけ (`skip=`)。受け手:

```sh
python3 -uc 'import socket,time;s=socket.socket(2,2);s.bind(("",6870));[print(time.strftime("%T"),s.recv(512).decode()) for _ in iter(int,1)]' | tee -a /tmp/msclog.txt
```

行の形 (時刻は Pico の起動からの ms):

- `seq 開始 所要 R|W <USB の LBA> n=<セクタ数> <場所> [err] [head=<先頭 8 バイト>] [tur=N] [skip=N]`。場所は仮想 FAT の中
  (bpb fat root x68z pscsi log config imgdir) か `id<SCSI ID>:<HDS の LBA>`。起動セクタは `id0:0x0` (`head=5836385343534931` = "X68SCSI1")
- `seq 時刻 C op=<SCSI 命令>` (TEST UNIT READY は数えるだけ)、`M <出来事>` (sysstatus、SMB、HDS を開いた、vd_sync の待ち)、`U mount|umount`

分かっていること (コードから): 読み込みの失敗は Z にはエラーではなく 0 埋めで返る。最初の HDS (と image ディレクトリ) の読み込みは、
起動時の接続 (繰り直し込み) が終わるまで USB タスクごと止まる (vd_sync)。

## 注意

- Z の Linux は SCSI の読み込みをキャッシュし、X68000 をリセットしても消えない。応答は毎回違う LBA から読む。
- 0x200000〜0x3fffff は scsiremote.sys (B: ドライブ) が使うので、netpipe の帯はその下に置く。
- xip.x は送るフレームを 60 バイトまで詰めるので、ブリッジは IP の全長で切って送る。
