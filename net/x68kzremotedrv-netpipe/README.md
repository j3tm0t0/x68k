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
| `src/virtual_disk.c` | 0034: 古い driver の数え方でも vdbuf_read の外を読まない (ID6 を別の機械が覗くと止まった。Pico 2 W で見つかった) |
| `src/virtual_disk.c` | 0032: 0030 の続き。範囲外のページを捨てさせるのは新しい driver (bit7) にだけ (古い driver は読み直し続けて固まるので、今までどおりに) |
| `src/virtual_disk.c` / `driver/scsiremote.c` / `driver/settinguisub.c` | 0030: リモートドライブ (B:) の応答のページを読まれた LBA から決める (下の「B: の読み込みが 8 ページずれる」) |
| `driver/mkbootwait.py` | 0029: 進み具合の最後の見出しを「BOOT」に |
| `src/tsp_svc.c` / `tsp.h` / `tsp_svc.h` / `tsp_plat.h` / `tsp_lwip/lwipopts.h` | 0031 (試し、**書き込んでいない**): tis.x -p のソケット窓口 ([../tistack](../tistack))。0xA6 のレコードを np_exchange の中で答える。`-DTSP=OFF` で外す。Pico W (264KB) では RAM が足りない見込み (ヒープ 112,340 → 100,020 B、動いている Pico のヒープの最大は約 104KB) なので、Pico 2 W のファームだけに入れる方針 |
| `src/msc_disk.c` | 0033: INQUIRY の文字列を欄 (8 / 16 / 4 バイト) に収める。上流から product id が 9 バイトはみ出していて、pico-sdk 2.1 の TinyUSB (Pico 2 W) では tud_task のスタックを壊し、USB ホストが INQUIRY を送った途端に落ちていた (Z にディスクが見えない) |
| `driver/mkbootwait.py` / `bootwait_gen_p2w.S` / `src/bootwait.c` | 0035: 待ち画面の副題のボード名を実物に (RP2350 では「Raspberry Pi Pico 2 W」。`BW_BOARD` で作り分け、Pico W の絵は今までと同じ) |
| `src/sdhds.c` / `sdhds.h` / `hw_config.c` / `hdscache.c` / `virtual_disk.c` / `connect.c` / `lib/FatFs_SD` | 0036: SPI の microSD の上の HDS。HDS の欄に `sd:/x68000z/hds1.hds` のように書くとカードのファイルを使う (SPI0: GP18 SCK / GP19 MOSI / GP20 MISO / GP21 CS、25MHz。`SD_SCK` などで変えられる。FatFs は [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) 6c644cf)。カードの HDS は Wi-Fi を待たない。**明示したときだけ**: SCSI ID を 1 つ足すと、起動時に CONFIG.SYS のドライバより先にドライブ名を取るので、たとえば HDS1 に入れると B: になり、**リモートドライブが C: にずれる** (実機の Z で確認)。最初の接続のあと、カードに `/x68000z/hds1.hds` が無く HDS0 と同じ共有に `sdcopy/hds1.hds` があれば、1 回だけカードへ写す (`.part` に書いてから名前を変える。写すだけで、HDS1 に書かない限りディスクにはならない) |
| `src/sdfs.c` / `sdfs.h` / `fileop.h` / `sdhds.c` / `virtual_disk.c` / `vd_command.c` / `hidkbd.c` / `connect.c` | 0037: SD カードを**リモートドライブのユニット**にする (SMB のユニットの後ろ。B: がリモートドライブなら **C:**)。SCSI ID は増やさないので B: はずれない。**明示したときだけ**: REMOTE の欄に `sd:/...` と書いたユニット (たとえばユニット数 2、REMOTE1 = `sd:/` で C:)。自動で足すと後ろのドライブ名が下がる (Z の内蔵 FD が C: D: から D: E: に。実機で確認) ので足さない。設定画面の共有の一覧にカードが `sd:` として出るので、HDS と REMOTE の欄でカードのファイルやディレクトリを選べる。Wi-Fi を待たない。`fileop.h` の `FUNC_*` が rootpath の `sd:` で FatFs に回す (ファイル・ディレクトリのハンドルは上位 32 ビットの印 0xfffe5344 で振り分ける。Z のドライバは READ / WRITE などにユニット番号を付けてこない)。ファイルの日付は変えられない (FatFs の `FF_USE_CHMOD` が 0。コピーしたファイルは書いた時刻になる)。FatFs は 1 本の錠で守る。FatFs の lib の `my_rtc.c` が `time()` を always-on タイマーに置き換えていて、止まったまま 0 を返していた (libsmb2 の時間切れが効かない) ので、起動時に 0 から動かし、SMB の時刻で合わせる。UDP 6869 に `sdcopy <共有名/パス> <sd のパス>`: 別のタスクと別の SMB の接続で共有からカードへ写す (`.part` に書いてから名前を変える)。HDS として開いているファイルと、写している最中の 2 本目は断る。`status` に `sdcopy=idle|run:N/M|done:N|err:<理由>`。実機の Z (自動で足す版で試験): `dir c:`・mkdir・ren・del・rmdir、B:→C:→B: のコピーが一致 (400KB)、sdcopy で 64MB を約 80 秒 (約 800KB/s) |
| `CMakeLists.txt` / `src/FreeRTOSConfig.h` / `src/tsp_lwip/lwipopts.h` | 0038: Pico 2 W 向けのビルド (`-DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350`、FreeRTOS は RP2350_ARM_NTZ の port、既定は 1 コア `P2W_CORES=1`)。`-DPICO_SDK_PATH=` (pico-sdk 2.1.1) と `-DFREERTOS_KERNEL_PATH=` (raspberrypi/FreeRTOS-Kernel 4f7299d) を指定する。0036 以降の SD カードは pico-sdk 2.x が要る (`pico/aon_timer`)。TSP は Pico 2 W では本来の大きさ。手順は [HANDOFF.md](HANDOFF.md) |
| `src/connect.c` / `config_file.c` / `config.tmpl.txt` / `include/vd_command.h` / `driver/settingui.c` / `hidkbd.c` | 0039: 基板ごとのホスト名 (設定 `HOSTNAME`、空欄なら `pico-` + ボード ID の下 4 桁。今までは全部 PicoW) と固定 IP (`IPADDR` / `NETMASK` / `GATEWAY` / `DNS`、`IPADDR` が空欄なら DHCP)。CONFIG.TXT と設定画面の右の「ネットワーク設定」で変える (再起動後に有効)。フラッシュでは v4 の項目の後ろに印 `NET1` を付けて置くので、古いファームに戻しても設定は読める (古いファームが書き直すと新しい項目は消える)。`status` に `host=<名前>` (0040 で位置を変えた)。ホスト名は実機で確認、固定 IP は未試験。フラッシュの並び: v4 の項目 (32〜1727) の後ろ 1728 に `NET1`、hostname 32・ipaddr 16・netmask 16・gateway 16・dns 16 (〜1827) |
| `src/hidkbd.c` | 0040: UDP 6869 の返事はどれも最初の語 (ok / err) の直後に `host=<ホスト名>` (`ok host=pico-f350 stat=5 ...`)。Pico Z Loader と同じ形 |
| `src/vd_command.c` | 0041: 設定画面の Wi-Fi の一覧 (WIFI_SCAN) を 10 秒で打ち切る。Wi-Fi のつなぎ直しと重なると終わらず、USB タスクが止まって Z からディスクが消えた (PZL で見つかった) |
| `src/connect.c` | 0042: 設定画面の SETCONFIG で Wi-Fi / SMB をつなぎ直す前に、SMB の HDS とリモートドライブを外し (その間は NOT READY)、つなぎ直したら開き直す。古い接続が残って次の keepalive まで -14 を返していた (PZL で見つかった)。SD カードの分はそのまま。実機の Z で SERVER を確定し直して確認 |
| `driver/settingui.c` | 0043: 設定画面のネットワークの項目を「その他の設定」の枠の中 (7〜11 行目) に入れる。別の見出しが上の枠の下の線と重なっていた (実機の Z で直ったことを確認) |
| `src/hdscache.c` | 0044: HDS の読み込みキャッシュに書いたとき、その sector を持つ組を全部直す (1 つ目で止めていた)。組は読み損ねた LBA から始まるので重なることがあり、古い中身を返し得た。ここでは 8 セクタの組がほぼ揃うので実害はまず無いが、PZL (32 セクタ) では Z が止まる原因だった (Charlie が見つけた) |
| `src/sdhds.c` / `src/FreeRTOSConfig.h` | 0045: Pico W (RP2040) でも SD を使えるように RAM を詰める: HDS の FIL は開くときに確保、sdcopy のタスク (8KB) は写すたびに作って消す、写す塊は RP2040 では 8KB。FreeRTOS 11 の RP2040 の port 用に `configUSE_PASSIVE_IDLE_HOOK` の既定。Pico W (`-DPICO_BOARD=pico_w -DPICO_PLATFORM=rp2040 -DTSP=OFF`、pico-sdk 2.1.1) の起動時の空きヒープ 107,468 B (SD の前の同じビルドは 112,264 B)。実機は未試験 |
| `CMakeLists.txt` / `src/sdhds.c` / `src/sdfs.c` / `src/connect.c` | 0046: SD カードをビルドの選択に (`-DSD_CARD=ON|OFF`、既定は pico2_w で ON、pico_w で OFF)。OFF なら FatFs を入れず、`sd:` は開けない。Pico W で ON にしても sdcopy と起動時の fetch は入れない (RAM)。起動時の空きヒープ: Pico W (SD なし) 112,136 B、Pico W (SD あり) 107,872 B、Pico 2 W 343,952 B |
| `driver/bootwait.S` / `driver/mkbootwait.py` | 0028: ◆ の行を本物の進み具合 (Wi-Fi → SMB → HDS → 起動) に |
| `driver/mkbootwait.py` | 0027: ロゴの行は見た目の中央から 20 ドット左 (最終の配置) |
| `driver/mkbootwait.py` | 0026: ロゴの行を「副題の左端にそろえる」と「見た目の中央」の中間に (既定 BW_LOGO=between) |
| `driver/mkbootwait.py` | 0025: 副題とタイトルの間を詰める (半角の中黒)。ロゴの置き方を選べる (BW_LOGO=center / subtitle) |
| `driver/mkbootwait.py` | 0024: 横並びのロゴに戻し、ロゴの行だけ副題の左端にそろえる |
| `driver/mkbootwait.py` / `driver/bootwait.S` | 0023: ロゴを縦積みに (上に紋章、下に 1 行の「PICO Z LOADER」) |
| `driver/mkbootwait.py` | 0022: 画面の左右の釣り合い (状態欄・◆ の行を中央に、下のバーを枠の線にそろえる、ロゴを見た目の中心に) |
| `driver/mkbootwait.py` / `driver/bootwait.S` | 0021: 「Pico Z Loader」のロゴ (電源の輪と Z、Orbitron の PICO Z / LOADER) |
| `driver/mkbootwait.py` / `driver/bootwait.S` | 0020: 青系の配色、三角形の 2 組が交互に光る |
| `driver/bootwait.S` / `driver/mkbootwait.py` / `driver/bootwait_gen.S` | 0019: 待ち画面の作り直し (三角形の X の紋章をテキスト VRAM に、枠・色・そろえた欄、◆ が走る行、反転の案内) |
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
- 0028: 飾りだった `[Pico W] ━◆━ [X68000 Z]` を、本当の進み具合「Wi-Fi ━━ SMB ━━ HDS ━━ BOOT」(0029 で「起動」から「BOOT」に) に替えた。済んだ段階は水色、
  今の段階は見出しが白で、その区間を ◆ が動く。SMB の再接続を待っている間は、Pico の「次の再接続まで」(5 秒の間隔に対する残り) に
  合わせて区間が埋まる。まだの段階はくすんだ青。HDS が開くと全部 (BOOT も) 点けて 0.5 秒見せてから本物の IPL に渡す
- 0027: 最終の配置。ロゴの行は見た目の中央 (`BW_LOGO=center`、既定に戻した) から 20 ドット左 (`BW_LOGO_SHIFT=-20`、左端 x 211)。0026 の中間案はやめた
- 0026: ロゴの行の左端を、副題の左端 (x 176) と見た目の中央 (x 231) の中間 (x 204) にした (`BW_LOGO=between`、既定)
- 0025: 副題を「Pico Z Loader ･ Raspberry Pi Pico W / x68kzremotedrv」に (半角の中黒 0xA5、空白 1 つ。全角の ・ と空白 3 つでは
  8 桁の穴が空いていた)。タイトルも同じく詰めた。ロゴの行は `BW_LOGO=center` (既定、0022 の見た目の中央) か
  `BW_LOGO=subtitle` (左端を副題の左端に) を `mkbootwait.py` に渡して選ぶ
- 0024: 0023 の縦積みはやめ、0022 の横並びに戻した。ロゴの行だけ左に寄せ、左端を副題の左端にそろえる
  (輪が副題の「Pico Z Loader」の上に来る)。ロゴの持ち方 (帯 x プレーン、art_blits) は 0023 のまま
- 0023: ロゴを縦積みに。上に電源の輪と Z、その下に 1 行で「PICO Z LOADER」(PICO 白、Z 水色、LOADER 灰青)、どちらも中央。
  副題は「for X68000 Z ・ Raspberry Pi Pico W / x68kzremotedrv」、下の行を詰め直した。ロゴは横の帯 x プレーンごとに持ち
  (空のプレーンは持たない)、表 (art_blits) で描く
- 0022: 状態欄 (見出し・値・付け足しの 46 桁の塊) と ◆ の行を画面の中央に、下のバーを枠の縦線から縦線まで (4-93 桁) に。
  ロゴは外形の中央と塗った点の重心の中間を画面の中央に合わせる (輪が細く軽いので外形の中央より右に 5 ドットほど)
- 0021: 名前を「Pico Z Loader」に。ロゴはオリジナル (電源の印の輪の中に Z、右に "PICO Z" と字間を空けた "LOADER")。
  輪は青 (パレット 12、◆ に合わせて明るくなる)、Z と軸と "Z" は水色 (4)、"PICO" は白 (プレーン 0+1。ロゴの行に IOCS の字は出さない)、
  "LOADER" は灰色がかった青 (8)。字は [Orbitron](https://github.com/theleagueof/orbitron) (SIL Open Font License 1.1、
  © The Orbitron Project Authors) を描いた絵で、フォント自体は入れていない (`mkbootwait.py` が作り直すときだけ `driver/fonts/` に取ってくる)。
  ロゴは 4 プレーンを使うので、待ち IPL は始めと渡す前に 4 プレーンとも自分で消す
- 0020: 青系に。枠と線は深い青、つながった値・副題・◆・案内は水色、見出しと待ちの値は白、知らせは白の反転。
  紋章は字と三角形の組 A が明るい水色、組 B が青、影は紺。組 B (パレット 12) を 0.5 秒ごとに明るくして交互に光らせる
- 0019: 画面を作り直した。紋章 (中心で向き合う 4 つの三角形の X と斜体の "X68000 Z"、自前の幾何学的な絵) はテキスト VRAM の
  プレーン 2・3 に直接描く (プレーン 0・1 は IOCS の文字)。罫線の枠、Wi-Fi / SMB / HDS / 経過の欄 (● つながった / ○ 待ち / × 開けない)、
  `[Pico W] ━━◆━━ [X68000 Z]`、下に反転の案内、カーソルは消す。テキストパレットは始めに控えて書き換え、本物の IPL・設定画面に渡す前に
  プレーン 2・3 の消去、パレット、色、カーソルを元に戻す。文字列・配置・紋章は `driver/mkbootwait.py` (PIL) が作る `bootwait_gen.S`
  (git に入れてあるのでビルドに PIL は要らない)。後半は 12KB になり、ID 6 の LBA 0x80-0x9f (32 セクタ) に置く。
  `make -C test/bootwait` が IOCS の画面・テキスト VRAM・パレットを真似て確かめ、`waiter_preview.png` (下見) を作る
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

## B: の読み込みが 8 ページずれる (0030、本家の不具合)

B: から読んだ一部のプログラムが起動直後にバスエラーになった (2026-10-23、ssh.x のパスワード版。A: に写すと動く)。
Human68k のローダは見出し → text+data → 再配置表の順に `_READ` する。再配置表の途中から、正しいデータの 8 ページ
(3968 バイト) 先の中身が入っていて、再配置されない番地に飛んでいた。ローダに限らず普通の `_READ` でも起きる。

- scsiremote.sys は応答 (1 回 最大 15KB) を 496 バイトのページに分け、ページ i を `sect + (i & 7)` から読む。
  8 ページごとと、命令の頭で sect を 8 下げる。Pico は読まれた回数でページを数えていた (`vdbuf_rcnt + lba % 8`、
  lba % 8 == 7 で +8)。
- 応答がちょうど 8 / 16 / 24 ページだと sect が 2 回続けて下がり、8 セクタの組が 1 つ読まれずに残る。次の命令で Z の
  Linux がそこを先読みすると数がずれ、その応答のページ 8 以降が 8 ページ先になる。ヘッダの seqno は今の命令のもの
  なので driver は気づかない。
- 起きる条件: 15360 バイト以上の `_READ` (キャッシュを通らず直に読む) の端数 (15360 で割った余り) の応答が
  8 / 16 / 24 ページ (余り 3471〜3966 / 7439〜7934 / 11407〜11902 バイト、約 1 割) で、次の命令の応答が 9 ページ以上
  (キャッシュの補充はいつも 31 ページ)。
  再現: 50000 バイトのファイルを `_READ` で A → 15360 バイト読む。A = 19160 / 23294 / 26972 (端数 8 / 16 / 24 ページ) なら
  2 回目の約 11350 バイトが違う。A = 18700 / 22798 / 26476 (7 / 15 / 23 ページ) なら一致。
- 直し方: driver が読み始めの sect の下位 16 ビット (8 単位) を命令のヘッダ `reserved[]` (bit7 = 新方式) で知らせ、
  Pico はページを `((起点 - (lba & ~7)) & 0xffff) / 8 * 8 + lba % 8` で決める。読み直し (sect - 0x10000) も同じ式で合う。
  範囲外 (上向きの先読みなど) は署名を壊して返し、driver はページ番号も照合して捨てる。bit7 の無い古い driver には今までどおり。
