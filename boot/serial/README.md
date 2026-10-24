# boot/serial: RS-232C だけで実機 X68000 にプログラムを転送する

SCSI も LAN もない X68000 (Compact XVI、主記憶 2MB) に、Human68k のシステムフロッピー、キーボード、RS-232C 接続だけで
ファイルを転送し、以後は Raspberry Pi (以下 Pi) か Mac からシリアル経由でコマンドを実行できるようにする。

X68000 のキーボードで打つのは `ctty aux` と Enter の 9 打鍵だけ (大文字でもよい)。以降の処理は、Pi で `sx.py bootstrap` を実行すると自動で進む。

## 流れ

| 段階 | X68000 で動くもの | 内容 |
|---|---|---|
| 0 (手で打つ) | COMMAND.X | `CTTY AUX` でコンソールを RS-232C に切り替える。以後の入力は Pi から届く |
| 1 | `COPY AUX` と X-BASIC | Pi から `COPY AUX RX.BAS` を入力して X-BASIC の受信プログラム `rx.bas` を送り、`BASIC RX` で動かす。rx.bas は 16 進数で表したデータを 1 行ずつ受信・検査し、`RX.X` に書く |
| 2 | rx.x | Pi から `RX` を入力して rx.x を起動し、19200bps に切り替える |
| 3 | rx.x | `files/` 内のプログラム (LHA.X など) を転送し、AUTOEXEC.BAT の末尾に `RX -B19200 -K2` と `CTTY AUX` を追記する |
| 以後 | rx.x | X68000 の起動時にコンソールが RS-232C に切り替わる。`sx.py run 'dir'` でコマンドを実行し、`put` / `get` でファイルを送受信する |

X-BASIC (`BASIC2\BASIC.X`) と `COPY` はシステムフロッピーに入っているものをそのまま使う。DB.X はシステムフロッピーに入っていない
(C コンパイラ PRO-68K に付属)。

## 用意するもの

- X68000 本体、キーボード、ディスプレイ、Human68k 3.0x のシステムフロッピー。
  シャープが無償公開した Human68k 3.02 のシステムディスクには `BASIC2\BASIC.X` (X-BASIC 2.02) と `BIN\SWITCH.X` が入っている。
- 複製先のフロッピー 1 枚 (用意できる場合)。システムフロッピーを `DISKCOPY` で複製し、複製したフロッピーで作業する。
  段階 1 から 3 では、フロッピーに `RX.BAS` `RX.X` `LHA.X` `AUTOEXEC.ORG` を書き、`AUTOEXEC.BAT` を書き換える。
  Human68k 3.02 のシステムディスクの空き容量は約 220KB で、このうち約 90KB を使う。
- RS-232C のケーブル。X68000 の RS-232C は D-sub 25 ピンのオス (DTE)。USB シリアル変換器 (D-sub 9 ピンのオス、DTE) とつなぐには
  クロス (ヌルモデム) の配線が要る。「25 ピン メス ⇔ 9 ピン メス のクロスケーブル」か、ストレートケーブルにヌルモデムアダプタを足す。
  フロー制御は使わないので TXD / RXD / GND の 3 本がつながっていれば足りる。
  ただし RTS/CTS や DTR/DSR を折り返していないケーブルで送信が止まる場合は、ハンドシェイク線も入ったフル結線のクロスケーブルを使う。
- Pi か Mac。Python 3 だけで動く (pyserial は要らない)。このディレクトリにある `sx.py`、`rx.bas`、`rx.x` を Pi か Mac に置く。
  段階 3 で転送するプログラムは `files/` に置く。中のファイルを全部 X68000 の `A:\` に転送する。`./getlha.sh` を実行すると、LHA for X680x0 の配布ファイルを
  retropc.net からダウンロードし、中の `LHA.x` を `files/LHA.X` に展開する (Pi / Mac に `lha` か `lhasa` が要る)。
  配布ファイルは自己解凍形式で、X68000 で実行すると上書きの確認でキー入力を待つため、Pi か Mac で展開してから転送する。

## 手順

1. システムフロッピー (複製) で起動し、`A>` が出るのを待つ。
2. X68000 のキーボードで次を打つ (9 打鍵)。

   ```
   ctty aux
   ```

   Enter を押すと画面への表示とキーボードからの入力が止まる。コンソールが RS-232C に切り替わったためで、正常な動作。
3. Pi で実行する (USB シリアルが `/dev/ttyUSB0` の場合。Mac なら `/dev/cu.usbserial-…`)。

   ```
   ./sx.py --port /dev/ttyUSB0 bootstrap
   ```

   最初は X68000 の SRAM にある RS-232C 設定の既定値 (9600bps、8 ビット、パリティなし、ストップビット 1、XON/XOFF) で通信する。
   進行状況は Pi の画面に表示される。X68000 の画面は、段階 1 の間は X-BASIC の画面になり、X-BASIC が終わると消える。
   段階 2 以降は rx.x が `rx: ready (ESC to stop)` や、受信したファイル名 (`rx: A:\LHA.X`) を表示する。
4. `sx: bootstrap done` が出たら、X68000 をリセットする。起動の最後に

   ```
   rx: CTTY AUX (RS-232C) -- press a key within 2 s to keep the keyboard
   ```

   と出る。何も押さなければ 2 秒後にコンソールが RS-232C (19200bps) に移る。キーを押すと、その起動中はキーボードから操作できる。
5. 以後は Pi から X68000 を操作する。

   ```
   export SX_PORT=/dev/ttyUSB0 SX_BAUD=19200
   ./sx.py run 'dir'                 # 実行して出力と終了コードを返す (A> に RX と打って rx.x を自動で起動する)
   ./sx.py put foo.x A:\FOO.X        # 送る
   ./sx.py get A:\CONFIG.SYS         # 受け取る
   ./sx.py quit                      # rx.x を止めて A> に戻す
   ./sx.py term                      # 端末として A> を直接操作する (Ctrl+] で終わる)
   ```

   rx.x が動いている間に X68000 のキーボードで ESC を押すと rx.x が止まる。
   `run` で実行するコマンドの標準入力は NUL デバイスにリダイレクトしている。ただしコンソールから直接読むプログラム (確認のキー入力を待つものなど) は
   RS-232C からの入力待ちになる。その場合は `./sx.py term` で応答を入力するか、Ctrl+C を送る。
   `harness/rcmd` の rcmd.x は xip.x を使った TCP 通信用なので、ネットワークのないこの構成では `sx.py run` を使う。
   rx.x が使う主記憶は約 20KB (プログラム約 4KB、スタックと送受信のバッファ)。残りの主記憶は、`run` で起動する COMMAND.X とその子プロセスが使う。

## 通信速度と所要時間

X68000 エミュレータの XEiJ (機種 Compact、MC68000 16MHz、主記憶 2MB、システムフロッピーだけで起動、RSDRV.SYS の受信バッファは実機と同じ 64 バイト) で測った値。XEiJ は RS-232C の通信速度を再現してデータを渡す。

| 段階 | 速さ | 時間 |
|---|---|---|
| 起動 (電源投入から `A>` の表示まで) | | 13 秒 |
| 0: `CTTY AUX` を打つ | 9 打鍵 | 数秒 |
| 1: RX.BAS (930 バイト) を `COPY AUX` で送る | 9600bps | 3 秒 |
| 1: rx.x (4612 バイト) を X-BASIC で受ける | 9600bps、1 行 64 バイト | 33 秒 |
| 2: rx.x を起動して 19200bps に切り替える | | 2 秒 |
| 3: LHA.X (81602 バイト) と AUTOEXEC.BAT | 19200bps、1.79 KB/s | 47 秒 |
| 0〜3 の合計 (最初のキー入力から `bootstrap done` の表示まで) | | 90 秒 |
| リセットしてから RS-232C の `A>` まで | | 13 秒 |

rx.x の転送は 19200bps で 1.7〜1.8 KB/s (回線の上限 1.92 KB/s の 9 割強)。X-BASIC による受信は 1 バイトあたり約 7ms かかる。
X-BASIC は 1 文に 0.5〜1ms かかるので、受信プログラムでは 1 バイトの処理で実行する文の数を減らしている。

5% の確率で送信データの 1 バイトを壊す試験 (`SX_CORRUPT=0.05`) でも、段階 1 では行単位、rx.x ではフレーム単位で再送され、転送が最後まで完了する。

通信速度は 19200bps を上限とする。X68000 の SCC はフロー制御なしの 38400bps で取りこぼすことがある。
また IOCS と RSDRV.SYS 2.02 で設定できる通信速度は 19200bps までで、XEiJ は RSDRV.SYS を書き換えて 38400bps に対応させている (実機の RSDRV.SYS にこの変更はない)。
rx.x は 1 フレーム (既定で 2048 バイト) を受信するたびに、フロッピーへの書き込みを終えてから応答を返すので、書き込み中にデータがあふれることはない。

## 段階 4: 同じ回線を SLIP にして rcmd.x で操作する

bootstrap のあと、同じ RS-232C を SLIP (シリアルの上で IP パケットを運ぶ方式) にして、TCP の上の rcmd.x で X68000 を操作できるようにする。
TCP/IP 本体は ESP/X の xip.x (K.Shirakata 氏の改良版 β5)、その下は `net/slip` の slip.sys と `net/slipd` の slipd.x。
X68000 Z で使っている組み合わせと同じで、SCC の割り込みは ROM と RSDRV.SYS に任せ、slipd.x が IOCS で 1 バイトずつ読み書きする。

1. Mac で `./getnet.sh` を実行し、`files-net/` に送るファイルを用意する。
   xip.x と ifconfig.x / inetdconf.x は retropc.net から取り、xip.x には `net/patchxip.py` で MSS 1460 などのパッチをあてる
   (改変したバイナリは配布しない)。slip.sys / slipd.x は elf2x68k でビルドする。
   rcmd.x は X68000 の上でビルドするものなので、x68kcli の HDS イメージ (`build/xipbase.hds` など) から取り出すか、`RCMD_X=パス` で渡す。
2. コンソールが RS-232C の状態 (段階 3 の後) で、Pi から実行する。

   ```
   ./sx.py --port /dev/ttyUSB0 --baud 19200 stage4 --prune
   ```

   約 230KB を送り、`CONFIG.SYS` に `PROCESS` と `DEVICE = \SYS\SLIP.SYS` を、`A:\NETUP.BAT` を作り、AUTOEXEC.BAT の最後を `NETUP` にする
   (元の CONFIG.SYS は `CONFIG.ORG`)。
   システムディスクの空きでは足りないので、`--prune` を付けるとこのキットが使わないファイル
   (`DEFSPTOOL.BAS` `DEFSPTOOL.HLP` `HISTORY.HLP` `BACKUP.X` `RESTORE.X` `RECOVER.X` `ASK68K.SYS`) を消し、CONFIG.SYS の ASK68K.SYS の行を REM にする。
   複製したディスクでだけ使う。ASK68K.SYS (かな漢字変換) を外すと主記憶も空く。
3. Pi を SLIP にして (`x68mode slip`、下の「Pi の準備」)、X68000 をリセットする。起動の最後に NETUP.BAT が
   xip.x → ifconfig → slipd.x と進み、rcmd.x が前面で待つ。X68000 の画面には rcmd.x が受けたコマンドが出る。
4. Pi から使う。

   ```
   x68 'dir'                        # COMMAND.X /c で実行して出力と終了コードを返す
   x68put foo.x 'A:\FOO.X'          # 送る (X68000 に tar.x が無いので圧縮しない)
   x68 'copy A:\AUTOEXEC.CON A:\AUTOEXEC.BAT'   # 次の起動からコンソール (CTTY AUX) に戻す
   ```

   rcmd.x は X68000 のキーボードの ESC か、Pi からの `exit` で止まる。コンソールに戻した後 SLIP にするには `sx mode slip`。
   SLIP の速さを変えるには、コンソールの状態で `sx mode slip --netspeed 38400` (Pi の `/etc/default/ppp-x68k` の SPEED も合わせる)。

段階 4 の後の主記憶の空きは約 1.1MB (XEiJ で xip.x と slipd.x が常駐し、rcmd.x を止めた状態。`rx -m` で表示)。
X68000 の IP は 192.168.68.2、Pi は 192.168.68.1 (`--ip` / `--peer` で変えられる)。Pi は IP 転送と NAT をするので、X68000 から外にも出られる。

### SLIP の速さ

| 速さ | x68put (30000 バイト) | 確かめたこと |
|---|---|---|
| 19200bps | 19.6 秒 (1.49 KB/s) | XEiJ で動作 |
| 38400bps | 10.5 秒 (2.80 KB/s) | XEiJ で動作。実機は未確認 |

XEiJ で確かめられるのは動くかどうかまで。XEiJ の SCC には受信の FIFO が無く、実機の SCC とは取りこぼし方が違う
(X68000 Z の実機では 38400bps で約 1.4%、XEiJ では 19200bps で約 5% の再送が出た)。
実機の Compact XVI の SCC がフロー制御なしの 38400bps で取りこぼすかどうかは、実機で試して確かめる。
RSDRV.SYS 2.02 は 38400bps (速度コード 9) を受け付ける。

## Pi の準備 (raspi/setup.sh)

X68000 を受け持つ Raspberry Pi (Pi 4、後で Pi Zero 2 W) を、Raspberry Pi OS / Debian の素の状態から用意する。何度実行してもよい。

```
git clone https://github.com/j3tm0t0/x68k && cd x68k/boot/serial
./getlha.sh                       # 任意: files/LHA.X (Pi に lhasa が要る。setup.sh が入れる)
#   files-net/ は Mac で ./getnet.sh を実行して作り、Pi の同じ場所にコピーする
sudo raspi/setup.sh --dev /dev/serial/by-id/usb-FTDI_…-if00-port0
```

| 入るもの | 中身 |
|---|---|
| `sx` | sx.py を `/etc/default/x68k-serial` のポートと速さで動かす (`sx bootstrap`、`sx run 'dir'`、`sx put` など) |
| `x68type` / `x68con` | コンソール (CTTY AUX) に 1 文字ずつ打つ / コンソールのログを追いかけて表示する |
| `x68` / `x68put` | rcmd.x (SLIP の上の TCP) でコマンドを実行する / ファイルを送る (harness/raspi のもの) |
| `x68mode console\|slip` | 起動時にどちらを動かすかを切り替える |
| `x68-serlog.service` | コンソールのログ係。`/var/log/x68k/x68con.log` に書く。`sx` が動く間はロックファイル (`/run/x68k-serial/lock`) を見てポートを空ける |
| `ppp-x68k.service` | SLIP (`net/raspi/ppp-x68k.sh`: slattach + IP 転送 + NAT + 時刻合わせ)。設定は `/etc/default/ppp-x68k` |

どちらのサービスもログインしなくても起動時に動く。`--mode console|slip` で最初の状態を決め、後から `x68mode` で変える。
USB シリアルは `/dev/serial/by-id/` の名前を使うと、挿し直しても名前が変わらない。Pi 自身の UART (`/dev/ttyS0` など) を使う場合は、
setup.sh が raspi-config でシリアルコンソールを止める。

### Pi を読み出し専用にする

Pi Zero 2 W を SCSI のターミネータ電源から給電すると、X68000 の電源を切ったときに Pi の電源も急に切れる。
SD カードのファイルシステムを壊さないように、ルートを読み出し専用にして書き込みを RAM に置く。

```
sudo raspi/setup.sh --data /data --overlay on     # 最後に raspi-config の overlayfs を有効にする。再起動で反映
sudo raspi/setup.sh --overlay off                 # 設定を変えるとき。再起動してから作業し、終わったら on に戻す
```

- `--overlay on` の後は、`/etc` などを変えても再起動で元に戻る。設定を変えるときは `--overlay off` で戻して再起動してから行う。
  setup.sh は読み出し専用の間は止まる (`--overlay off` のときだけ動く)。
- ログや PiSCSI のディスクイメージなど残したいものは、別のパーティションに置く。ext4 のパーティションに `X68DATA` というラベルを付けておくと、
  `--data /data` が `/etc/fstab` に書いて `/data` にマウントし、ログを `/data/log` に、PiSCSI 用に `/data/piscsi` を作る。
  パーティションを作るのは setup.sh ではしない (SD カードの後ろを空けて作るか、USB メモリを使う)。
  急な電源断でこのパーティションが壊れることはありうるので、PiSCSI のイメージは控えを取っておく。
- 読み出し専用の切り替えは raspi-config (Raspberry Pi OS) の機能を使う。raspi-config の無い Debian では setup.sh は切り替えず、メッセージを出すだけにする。
  そのため Pi には Raspberry Pi OS Lite を使う。

## うまくいかないとき

- `sx: no prompt`: `CTTY AUX` を打ったか、ケーブルがクロスか、両側の通信速度が一致しているかを確かめる。
  SRAM の RS-232C 設定を変更したことがある本体では 9600bps でないことがある。`./sx.py --baud 19200 probe` のように通信速度を変えて試すか、
  `SWITCH.X` の RS-232C の項目で 9600bps / 8 ビット / パリティなし / ストップビット 1 に戻す。
- 段階 1 で行が壊れた: rx.bas が各行のチェックサムと行番号を検査し、`N` を返すと sx.py が同じ行を再送する。操作は要らない。
  送信後 5 秒以内に応答がない行も再送する。20 回続けて失敗すると止まる。
- 段階 1 の X-BASIC がエラーで止まった (X68000 の画面に行番号付きのエラーと `Ok` が出る): X68000 のキーボードで `system` と Enter を打つと
  `A>` に戻る (X-BASIC はキーボードから読む)。そのあと `./sx.py --port … stage1` からやり直す。
- 段階 2 以降が止まった: もう一度 `./sx.py stage2` / `stage3` を実行する。rx.x の通信フレームには CRC とフレーム番号が付いていて、壊れたフレームは再送する。
- キーボードに戻したい: `./sx.py term` で `CTTY CON` と打つ。AUTOEXEC.BAT を元に戻すには `copy AUTOEXEC.ORG AUTOEXEC.BAT`。
- `CTTY AUX` を使わずに進める: `./sx.py --port … --manual stage1` にすると、`copy aux rx.bas` と `basic rx` を X68000 のキーボードで打つ手順になる
  (sx.py が各コマンドを打つタイミングを表示する)。その後 `A>` で `rx` と打ってから `./sx.py --port … stage2`、`stage3` と進める。
  キー操作は計 28 打鍵 (コマンド 3 行分) で、X-BASIC の段階は XEiJ で 49 秒 (CTTY AUX のときより遅い)。XEiJ で段階 2 まで確かめた。

## XEiJ で試す

`test/e2e.py RUNDIR` が、システムフロッピーのイメージ (XDF 形式) だけで Compact (主記憶 2MB) の XEiJ を起動し、`stage1.keys` に書いた 9 打鍵を
XEiJ にキーボード入力として送り、`sx.py bootstrap` を実行してから XEiJ をリセットし、19200bps の RS-232C で `run` / `put` / `get` を確かめる。
`E2E_PTY=1` を付けると、sx.py を疑似端末 (pty) 経由の `--port` で動かし、シリアルポートを開く処理 (termios) も通す (`test/ptybridge.py`)。
`SX_CORRUPT=0.05` を付けると、送信の 5% で 1 バイトを壊して再送を確かめる。
XEiJ は RSDRV.SYS 2.02 の受信バッファを 64 バイトから 1024 バイトに広げるパッチを自動であてるので、試験用のフロッピーでは
AUTOEXEC.BAT の最後に `test/rsbuf64.x` を足し、実機と同じ 64 バイトに戻してから試す (`test/testfloppy.py`。`XEIJ_RSBUF=1024` で無効)。
`E2E_SLIP=1` を付けると段階 4 も試す。SLIP の相手は `harness/xeij/xeijslip.py` が ssh 先の Pi (既定 raspi3、`E2E_SLIP_HOST`) に疑似端末と slattach で作り、
Pi から x68 / x68put で rcmd.x を使う。19200bps のあと 38400bps に切り替えてもう一度試す。
XEiJ の「TCP/IP ⇔ AUX」の設定で、RS-232C を TCP の 127.0.0.1:54368 につないでいる (`test/xeij-fd.sh`)。画面の文字は、文字表示用のメモリ (テキスト VRAM) を読んで確かめる (`test/xscreen.py`)。

## ファイル

| ファイル | 中身 |
|---|---|
| `stage1.keys` | X68000 のキーボードで打つキーの並び (`CTTY AUX` と Enter) |
| `rx.bas` | 段階 1 の X-BASIC の受信プログラム (送る側が `COPY AUX` で送り込む) |
| `rx.c` `start.S` `Makefile` | rx.x のソース。elf2x68k を使い、`make` でクロスビルドする |
| `rx.x` | ビルド済みの rx.x |
| `sx.py` | Pi / Mac で動かす転送とコマンド実行のプログラム |
| `getlha.sh` | `files/LHA.X` を用意する (LHA for X680x0 を取ってきて中の LHA.x を取り出す) |
| `files/` | 段階 3 で送るファイルを置く (Git の管理対象外) |
| `getnet.sh` | `files-net/` (段階 4 で送る xip.x、slip.sys、slipd.x、rcmd.x など) を用意する |
| `raspi/` | Pi の準備 (`setup.sh`、`x68serlog`、`x68type`、`x68mode`、`x68-serlog.service`) |
| `test/` | XEiJ での試験 (`e2e.py` `xeij-fd.sh` `xtype.py` `xscreen.py` `ptybridge.py` `testfloppy.py` `rsbuf64.c`) |
