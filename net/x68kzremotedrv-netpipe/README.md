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
| `driver/bootwait.S` | 0018: 描く前に CRTC R21/R23 を 0 に戻し、書き直す行を同じ幅で埋める (設定画面から戻ると文字が重なった) |
| `driver/bootwait.S` / `src/connect.c` | 0017: 9 行のバナー、経過秒は Pico の時計で数える、「次の再接続まで N 秒」 |
| `driver/bootwait.S` / `src/virtual_disk.c` | 0016: 待ち画面に X68000 Z のバナーと動き、[S] で設定画面 (接続中でも使える) |
| `src/bootwait.c` / `driver/bootwait1.S` / `driver/bootwait.S` / `test/bootwait/waiter_test.py` | 0015: BOOTWAIT_SCREEN (既定オン)。HDS の先頭 16KB をフラッシュに控え、開けるまで Z の画面に接続待ちを出す |
| `src/msc_disk.c` / `virtual_disk.c` / `connect.c` / `test/bootwait/` | 0014: BOOTWAIT (既定オン)。HDS が開けるまで NOT READY で待たせ、接続は裏で繰り直し続ける |
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

## 起動待ち BOOTWAIT (0014)

電源投入時にときどき「ディスクから起動できません」になっていた件。0013 の MSC ログで分かったこと:

- Z の Linux が仮想 FAT を調べ終えて HDS を読み始めるのは電源投入から約 35 秒後。最初の読み込みは id0 の LBA 0〜31 (16KB) をまとめて読む
- ソフトリセット (Ctrl+F11+Del) のあとは Pico に 1 件も読みに来ない。起動は全部 Z の Linux のページキャッシュから
- 元のファームは最初の HDS (と image ディレクトリ) の読み込みで接続を待って USB タスクごと止まり (Linux は 30 秒で諦める)、
  接続に失敗すると HDS の読み込みに 0 埋めを「成功」として返していた。それがキャッシュに残り、Z の電源を切るまで直らない

0014 では:

- HDS が開けるまでの読み書きは、最大 5 秒待たせて (TinyUSB に 0 を返して呼び直させる。USB タスク・HID・netpipe は動いたまま)
  から失敗にし、sense を NOT READY / BECOMING READY (02/04/01) にする (TinyUSB の既定 02/3A/00 は Linux が媒体を外したと扱う)。
  Linux の sd は 04/01 を間を置いて出し直す (既定で約 3 分)。失敗した読み込みはキャッシュに残らない
- 開いている HDS の SMB の読み書きの失敗は MEDIUM ERROR (03/11/00)。仮想 FAT の他の場所は今までどおり
- image ディレクトリ・ID 6・netpipe は接続を待たない
- 最初の 5 回で接続できなくても 5 秒ごとに繰り直し、つながったら HDS を開く。`x68key status` は `smb=retrying`、`notready=` `ioerr=` を出す
- `-DBOOTWAIT=OFF` で元の動き

3 分を超えても開けないときは Z 側は読み込みエラーで止まるが、キャッシュは汚れないので、つながったあとに
`x68key hold 600; x68key combo ctrl f11 del` (ソフトリセット) で起動できる (電源の入れ直しは要らない、はず)。
画面に「接続待ち」を出すのは 0015 (下)。

## 接続待ちの画面 BOOTWAIT_SCREEN (0015)

最初の 16KB (署名・IPL・分割表・SCSI ドライバ) は Z にキャッシュされるので、偽物は返せない。そこで本物を Pico のフラッシュに控えておく:

- 起動ディスク (ID 0) の HDS を開けるたびに LBA 0-31 を控え (フラッシュ **0x1E0000-0x1E4FFF**: 見出し 4KB + 16KB。設定の 0x1F0000 とは別)
  と比べ、違うときだけ書き直す。見出し (config の HDS のパス、ファイルの大きさ、中身の FNV-1a) は最後に書く (途中で止まっても使われない)。
  書き込みは flash_safe_execute (このファームの FreeRTOS は 1 コアなので割り込みを止めるだけ。4KB ずつ消す)
- HDS がまだ開けていない間、控えがあれば LBA 0-31 を控えのとおりに返し、IPL (LBA 2) だけ待ち IPL の前半 (`driver/bootwait1.S`、180 バイト) にする。
  控えが無い (書き込んだ直後の最初の起動など) ・パスが違うときは 0014 と同じ NOT READY (画面は真っ黒のまま待つ)
- 前半は ROM から来た d4 を控え、後半 (`driver/bootwait.S`) を ID 6 の LBA 0x80 から $2800 に読んで飛ぶ
- 後半は「Pico: SMB 接続待ち… (Wi-Fi/SMB を確認してください)」と Wi-Fi / SMB の状態、Pico の IP、経過秒、くるくるを出し、
  0.5 秒ごとに ID 6 の LBA 0x40000-0xfffff を読む (時刻から決めた番地、32KB 以上離す。応答は読んだ LBA と Pico の起動からの時間を返すので、
  キャッシュの古い答えは捨てる。形は `src/bootwait_proto.h`)
  - 開けて控えと同じ: 応答に本物の IPL (と和) が載る。$2000 に写し、d4 を戻して飛ぶ (キャッシュの分割表とドライバは本物と同じ)
  - 控えと違う: 「HDS が変わりました。Z の電源を入れ直してください」で止まる (古い分割表で起動させない)。控えは書き直すので次の電源投入で直る
  - S か F1: 設定画面 (hdsboot の F1 と同じ settingui.bin を ID 6 の LBA 0x100 から読む)。0016 から、Pico が接続中でも使える
    (設定画面の命令は接続を待たず、接続タスクが Wi-Fi に繋ぎにいっている間は NOT READY で待たせる。変えた設定は次の繰り直しで使われる)。
    設定で HDS を変えたら Z の電源を入れ直す (画面にも出す。変えた HDS が開くと「HDS が変わりました」になる)
- 0018: 設定画面 (IOCS の TXBOX など) から戻ると、書き直す行に前の文字が混ざって見えた (テキストの同時アクセス R21 と
  ビットマスク R23 が残っていた)。待ち IPL は始めと毎回の表示の前に R21/R23 に 0 を書き (読まない)、行を 88 桁まで空白で埋める
- 0017: バナーを枠付きの 9 行に。経過秒は Z の IOCS _ONTIME ではなく応答の Pico の起動からの時間で数える
  (実機で秒が減って見えた)。_ONTIME は問い合わせの間隔と番地にだけ使い、差の絶対値で 0.5 秒を測る。
  応答に次の再接続までの時間を載せ、「(次の再接続まで N 秒)」/「(接続を試しています)」を出す
- 0016: 画面の上に 5 行の「X68000 Z」バナー (ふつうの ASCII。`\` は X68k では円記号なので使わない) と、
  `[Pico W] ....o.... [X68000 Z]` の動き
- HDS がすでに開いている普通の起動は hdsboot のまま (速さは変わらない)。そのときに設定画面に入るのは今までどおり起動時に F1。
  遠隔からは `x68key hold 600; x68key combo ctrl f11 del; x68key hold 5000; x68key combo f1` (未確認)
- `x68key status` の `snap=`: none (控えなし) / stored (控えあり、まだ開けていない) / ok (同じ) / updated (書き直した) / served (控えで起動を始め、同じだった) / changed
- Z 自身が HDS の先頭を書き換えた (FORMAT や分割の変更) あとの、次の「SMB が遅い」起動は一度だけ「HDS が変わりました」で止まる
- 試験: `make -C test/bootwait` (待ち IPL の前半と後半を Unicorn の 68000 で、ホスト向けに組んだ本物の bootwait.c と組み合わせて動かす。
  Linux のページキャッシュを真似る)


## 注意

- Z の Linux は SCSI の読み込みをキャッシュし、X68000 をリセットしても消えない。応答は毎回違う LBA から読む。
- 0x200000〜0x3fffff は scsiremote.sys (B: ドライブ) が使うので、netpipe の帯はその下に置く。
- xip.x は送るフレームを 60 バイトまで詰めるので、ブリッジは IP の全長で切って送る。
