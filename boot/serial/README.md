# boot/serial: RS-232C だけで実機 X68000 に道具を入れる

SCSI も LAN もない X68000 (Compact XVI、主記憶 2MB) に、Human68k のシステムフロッピーとキーボードと RS-232C だけで
ファイルを送り込み、以後は Raspberry Pi か Mac からシリアル経由でコマンドを実行できるようにする。

X68000 のキーボードで打つのは `ctty aux` と Enter の 9 打鍵だけ (大文字でもよい)。残りは Pi で `sx.py bootstrap` を実行すれば自動で進む。

## 流れ

| 段階 | X68000 で動くもの | 内容 |
|---|---|---|
| 0 (手で打つ) | COMMAND.X | `CTTY AUX` でコンソールを RS-232C に切り替える。以後の入力は Pi から届く |
| 1 | `COPY AUX` と X-BASIC | Pi が `COPY AUX RX.BAS` と打って X-BASIC の受信プログラム `rx.bas` を送り、`BASIC RX` で動かす。rx.bas は 16 進の行を 1 行ずつ受けて検査し、`RX.X` に書く |
| 2 | rx.x | Pi が `RX` と打って rx.x を起動し、19200bps に切り替える |
| 3 | rx.x | `files/` にある道具 (LHA.X など) を送り、AUTOEXEC.BAT の末尾に `RX -B19200 -K2` と `CTTY AUX` を足す |
| 以後 | rx.x | 起動するとコンソールが RS-232C になる。`sx.py run 'dir'` で実行、`put` / `get` でファイルの出し入れ |

X-BASIC (`BASIC2\BASIC.X`) と `COPY` はシステムフロッピーに入っているものをそのまま使う。DB.X はシステムフロッピーに入っていない
(C コンパイラ PRO-68K に付属)。

## 用意するもの

- X68000 本体、キーボード、ディスプレイ、Human68k 3.0x のシステムフロッピー。
  シャープが無償公開した Human68k 3.02 のシステムディスクには `BASIC2\BASIC.X` (X-BASIC 2.02) と `BIN\SWITCH.X` が入っている。
- 書き込み用のフロッピー 1 枚 (あれば)。システムフロッピーを `DISKCOPY` で複製し、複製の方で作業する。
  段階 1 から 3 では、フロッピーに `RX.BAS` `RX.X` `LHA.X` `AUTOEXEC.ORG` を書き、`AUTOEXEC.BAT` を書き換える。
  3.02 のディスクの空きは約 220KB で、このうち約 90KB を使う。
- RS-232C のケーブル。X68000 の RS-232C は D-sub 25 ピンのオス (DTE)。USB シリアル変換器 (D-sub 9 ピンのオス、DTE) とつなぐには
  クロス (ヌルモデム) の配線が要る。「25 ピン メス ⇔ 9 ピン メス のクロスケーブル」か、ストレートケーブルにヌルモデムアダプタを足す。
  フロー制御は使わないので TXD / RXD / GND の 3 本がつながっていれば足りる。
  ただし RTS/CTS や DTR/DSR を折り返していないケーブルで送信が止まる場合は、ハンドシェイク線も入ったフル結線のクロスケーブルを使う。
- Pi か Mac。Python 3 だけで動く (pyserial は要らない)。このディレクトリの `sx.py` `rx.bas` `rx.x` を置く。
  段階 3 で送る道具は `files/` に置く (中のファイルを全部 `A:\` に送る)。`./getlha.sh` を実行すると、LHA for X680x0 の配布ファイルを
  retropc.net から取り、中の `LHA.x` を `files/LHA.X` に取り出す (Pi / Mac に `lha` か `lhasa` が要る)。
  配布ファイルは自己解凍形式で、X68000 で実行すると上書きの確認でキー入力を待つため、取り出してから送る。

## 手順

1. システムフロッピー (複製) で起動し、`A>` が出るのを待つ。
2. X68000 のキーボードで次を打つ (9 打鍵)。

   ```
   ctty aux
   ```

   Enter を押すと画面には何も出なくなり、キーボードも受け付けなくなる。これで正常 (コンソールが RS-232C に移った)。
3. Pi で実行する (USB シリアルが `/dev/ttyUSB0` の場合。Mac なら `/dev/cu.usbserial-…`)。

   ```
   ./sx.py --port /dev/ttyUSB0 bootstrap
   ```

   最初の 9600bps は X68000 の SRAM の既定値 (9600bps、8 ビット、パリティなし、ストップ 1、XON/XOFF)。
   進み具合は Pi の画面に出る。X68000 の画面は、段階 1 の間は X-BASIC の画面になり、X-BASIC が終わると消える。
   段階 2 以降は rx.x が `rx: ready (ESC to stop)` や、受けたファイルの名前 (`rx: A:\LHA.X`) を出す。
4. `sx: bootstrap done` が出たら、X68000 をリセットする。起動の最後に

   ```
   rx: CTTY AUX (RS-232C) -- press a key within 2 s to keep the keyboard
   ```

   と出る。何も押さなければ 2 秒後にコンソールが RS-232C (19200bps) に移る。キーを押せば今回はキーボードのまま使える。
5. 以後は Pi から使う。

   ```
   export SX_PORT=/dev/ttyUSB0 SX_BAUD=19200
   ./sx.py run 'dir'                 # 実行して出力と終了コードを返す (A> に RX と打って rx.x を自動で起動する)
   ./sx.py put foo.x A:\FOO.X        # 送る
   ./sx.py get A:\CONFIG.SYS         # 受け取る
   ./sx.py quit                      # rx.x を止めて A> に戻す
   ./sx.py term                      # 端末として A> を直接操作する (Ctrl+] で終わる)
   ```

   rx.x が動いている間に X68000 のキーボードで ESC を押すと rx.x が止まる。
   `run` で実行するコマンドの標準入力は NUL にしてある。ただしコンソールから直接読むプログラム (確認のキー入力を待つものなど) は
   RS-232C から読もうとして止まる。そのときは `./sx.py term` で答えを打つか、Ctrl+C を送る。
   `harness/rcmd` の rcmd.x は TCP (xip.x) 用なので、ネットワークのないこの構成では `sx.py run` を使う。
   rx.x が使う主記憶は約 20KB (プログラム 4KB、スタックと送受信のバッファ)。残りは `run` で起動する COMMAND.X とその子が使う。

## 速さと時間

XEiJ (機種 Compact、MC68000 16MHz、主記憶 2MB、システムフロッピーだけで起動) で測った値。XEiJ は RS-232C の速さを再現して届ける。

| 段階 | 速さ | 時間 |
|---|---|---|
| 起動 (電源から `A>` まで) | | 13 秒 |
| 0: `CTTY AUX` を打つ | 9 打鍵 | 数秒 |
| 1: RX.BAS (930 バイト) を `COPY AUX` で送る | 9600bps | 3 秒 |
| 1: rx.x (4180 バイト) を X-BASIC で受ける | 9600bps、1 行 64 バイト | 30 秒 |
| 2: rx.x を起動して 19200bps に切り替える | | 2 秒 |
| 3: LHA.X (81602 バイト) と AUTOEXEC.BAT | 19200bps、1.79 KB/s | 47 秒 |
| 0〜3 の合計 (最初のキーから `bootstrap done` まで) | | 87 秒 |
| リセットしてから RS-232C の `A>` まで | | 13 秒 |

rx.x の転送は 19200bps で 1.7〜1.8 KB/s (回線の上限 1.92 KB/s の 9 割強)。X-BASIC の段階は 1 バイトに 7ms ほどかかる。
X-BASIC は 1 文に 0.5〜1ms かかるので、受信プログラムは 1 バイトあたりの文の数を減らしてある。

5% の送信でわざと 1 バイトを壊す試験 (`SX_CORRUPT=0.05`) でも、段階 1 の行と rx.x の枠がそれぞれ送り直されて最後まで通る。

19200bps より速くはしない。X68000 の SCC はフロー制御なしの 38400bps で取りこぼすことがある。
また IOCS と RSDRV.SYS 2.02 の速さの設定は 19200bps までで、XEiJ は RSDRV.SYS に手を入れて 38400bps を使えるようにしている (実機の RSDRV.SYS にはない)。
rx.x は 1 枠 (既定 2048 バイト) 受けるごとにフロッピーに書いてから返事をするので、書き込み中にデータがあふれることはない。

## うまくいかないとき

- `sx: no prompt`: `CTTY AUX` を打ったか、ケーブルがクロスか、速さが合っているかを確かめる。
  SRAM の RS-232C の設定を変えたことがある機械では 9600bps でないことがある。`./sx.py --baud 19200 probe` のように速さを変えて試すか、
  `SWITCH.X` の RS-232C の項目で 9600bps / 8 ビット / パリティなし / ストップ 1 に戻す。
- 段階 1 の行が壊れた: rx.bas が行ごとの合計と行番号で検査し、`N` を返すと sx.py が同じ行を送り直す。何もしなくてよい。
  5 秒返事がない行も送り直す。20 回続けて失敗すると止まる。
- 段階 1 の X-BASIC がエラーで止まった (X68000 の画面に行番号付きのエラーと `Ok` が出る): X68000 のキーボードで `system` と Enter を打つと
  `A>` に戻る (X-BASIC はキーボードから読む)。そのあと `./sx.py --port … stage1` からやり直す。
- 段階 2 以降が止まった: もう一度 `./sx.py stage2` / `stage3` を実行する。rx.x の枠には CRC と番号が付いていて、壊れた枠は送り直す。
- キーボードに戻したい: `./sx.py term` で `CTTY CON` と打つ。AUTOEXEC.BAT を元に戻すには `copy AUTOEXEC.ORG AUTOEXEC.BAT`。
- `CTTY AUX` を使わずに進める: `./sx.py --port … --manual stage1` にすると、`copy aux rx.bas` と `basic rx` を X68000 のキーボードで打つ手順になる
  (sx.py が打つ時機を表示する)。その後 `A>` で `rx` と打ってから `./sx.py --port … stage2`、`stage3` と進める。
  打鍵は 28 (3 行ぶん) で、X-BASIC の段階は XEiJ で 49 秒 (CTTY AUX のときより遅い)。XEiJ で段階 2 まで確かめた。

## XEiJ で試す

`test/e2e.py RUNDIR` が、システムフロッピーの XDF だけで Compact (主記憶 2MB) の XEiJ を起動し、`stage1.keys` の 9 打鍵を
XEiJ のキーボード入力から打ち、`sx.py bootstrap` を実行してから XEiJ をリセットし、19200bps の RS-232C で `run` / `put` / `get` を確かめる。
XEiJ の RS-232C は「TCP/IP ⇔ AUX」で 127.0.0.1:54368 に出している (`test/xeij-fd.sh`)。画面の文字はテキスト VRAM を読んで確かめる (`test/xscreen.py`)。

## ファイル

| ファイル | 中身 |
|---|---|
| `stage1.keys` | X68000 のキーボードで打つキーの並び (`CTTY AUX` と Enter) |
| `rx.bas` | 段階 1 の X-BASIC の受信プログラム (送る側が `COPY AUX` で送り込む) |
| `rx.c` `start.S` `Makefile` | rx.x (elf2x68k でクロスビルド。`make`) |
| `rx.x` | ビルド済みの rx.x |
| `sx.py` | Pi / Mac の送り手 |
| `getlha.sh` | `files/LHA.X` を用意する (LHA for X680x0 を取ってきて中の LHA.x を取り出す) |
| `files/` | 段階 3 で送るファイルを置く (git には入れない) |
| `test/` | XEiJ での試験 (`e2e.py` `xeij-fd.sh` `xtype.py` `xscreen.py`) |
