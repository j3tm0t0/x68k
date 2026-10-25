# net/zmux 引き継ぎ (Hotel → 次の担当、2026-10-25、2 回目の更新)

zmux (#23) は X68000 (Compact XVI) と Raspberry Pi の RS-232C 1 本で、コンソール・TSP・暗号・zosh のセッション・キー入力を多重化する。
設計は [DESIGN.md](DESIGN.md)、使い方と試験の結果は [README.md](README.md)。PR は #27 (下書き、ブランチ `zmux`)。
`zmux` ブランチは起動フロッピーの PR #25 (`serialfloppy`) を取り込んでいる (#25 を先にマージ)。

チャネル番号 (2026-10-25 に付け替えた。X68000 側と Pi 側は同時に入れ替える): 0 コンソール、1 入力の注入 (先頭 1 バイトがメッセージの種類。1 = キー、2 = マウス (予定))、
2 リモートドライブ、3 ネットワーク (npd.x の交換: TSP / 暗号 / zosh)、15 線の管理。

## できていること (XEiJ で試験済み、実機は未確認)

| 段階 | 中身 | 試験 |
|---|---|---|
| 1 線とコンソール | zmux.sys (DLE STX のフレーム、CRC-16、Go-Back-N の窓 4、AUX デバイス)、zmux.x (スレッド)、zmuxd / `zmux attach 0` | `test/e2e.py` |
| 2 TSP と 0xA5 | `npd.x -m` (net/npd/npd.c)、zmuxd の netpipe.rs / tsp.rs / accel.rs | `test/e2e_tsp.py` |
| 3 zosh のセッション | zmuxd の zss.rs (Pico の zsshd の代わり)、`zmux attach [N]` / `zmux kill N` | `test/e2e_zss.py` (主記憶 12MB) |
| キーの注入 | zmux.sys が _B_KEYINP / _B_KEYSNS を横取り、keys.rs と keys.toml、`zmux key` / `zmux kbd` / `zmux keys` | `test/e2e_key.py` |
| tmux のような画面 (2 回目) | `zmux` (tui.rs): サーバー / クライアント、タブとペイン、前置キー Ctrl+X、マウス、下の行、配置を zmuxd に置く。zmuxd に `open` / `layout` / `sessions` | `test/e2e_fake.py` (にせの X68000)。XEiJ の `test/e2e_tui.py` は**まだ流していない** |
| リモートドライブ (2 回目) | `zrmt.sys` (x68k/zrmt.c、x68kserremote の remotedrv.c を元に) + zmux.sys の命令 6 (チャネル 2 の交換) + zmuxd の rdrive.rs (`--drive DIR`) | `test/e2e_fake.py` (全命令、19200bps の真似で 1.82 KB/s)。XEiJ の `test/e2e_rdrive.py` は**まだ流していない** |

数字は README の表。19200bps で put 1.61 / get 1.72 KB/s、往復 223ms、ssh.x `-oAccel=all` の鍵交換 6.2〜6.8 秒。

## 決まっていること (DESIGN.md)

- COMMAND.X 3.00 の CTTY は con と aux しか受け付けない。zmux.sys のデバイス名は AUX で、RSDRV の AUX を置き換える。タブ 0 = CTTY AUX の COMMAND.X。
- zmux の前は素の文字、HELLO / HELLO_ACK でフレームに切り替わる。DLE は Human68k のコンソールにも Shift_JIS にも出ない。
- X68000 側は IOCS のポーリング (自前の SCC 割り込みは xip.x を壊した前例)。RSDRV の受信バッファを 32KB に付け替える。
- **Pi の zmux は tmux のような TUI にする** (DESIGN.md §5、まだ作っていない): サーバー (zmuxd) とクライアント (`zmux`) を unix ソケットで分ける。
  タブとペインは Pi で描く (ratatui + crossterm、ペインごとに `vt100` クレート)。前置キー Ctrl+X のあと `|` `-` `c` `n` `p` 数字 `k` `d`。
  ほかのホットキーは無い。マウスあり、下に状態の行。X68000 にしか無いキーは keys.toml。herdr には頼らない。

## 直した不具合 (Z-OS が動くと出た、同じものを作り直すときに注意)

1. **排他**: zos.x はスーパーバイザの処理中でもスレッドを切り替えるので、zmux.x / npd.x のスレッドと AUX の読み書きがシリアル回線の処理に
   同時に入り、受信バイトが別々に読まれてフレームが壊れた。zmux.sys の lock() / unlock()。
2. **接続の確認**: Pi の KICK が確認応答だけのフレームの代わりになり、X68000 は番号の無いフレームを数えていなかったので 10 秒で切れた。
   正しいフレームなら何でも接続の確認に数える。X68000 も 2 秒ごとに確認応答だけのフレームを送る。
3. **再送タイマー**: Pi の再送タイマーを「回線の送信が終わる見込み」から数えていたので、KICK のたびに延びて再送しなくなった。
   確認待ちのデータのフレームを送り終える時刻 (`t_done`) から数える。
- ほかに: 未送信のバイトがある間は X68000 の再送タイマーを止める。X68000 の送信は _OSNS232C では 1 回 1 バイトしか出ないので _OUT232C で 128 バイトずつ。
  zmux.x のスレッドの回数は 10 (2 だと Z-OS の下で 2.6 秒回ってこなかった)。

## 2 回目 (2026-10-25、クラウドの Linux から) でやったこと

- `zmux` (引数なし) = tmux のような画面。前置キー Ctrl+X のあと `|` `-` `c` `n` `p` `0-9` `k` `d` `?`、Ctrl+X Ctrl+X で Ctrl+X そのもの。ほかのホットキーは無い。
  打ったバイトは自分で読み、前置キーの組とマウスの並び (SGR 1006) だけ拾ってほかは素通し。ペインごとに vt100 クレート、ratatui + crossterm で描く。
  タブ 0 は Shift_JIS ⇔ UTF-8。配置は zmuxd に `layout set` で置き、つなぎ直すと戻る。zmuxd 側は `open COLS ROWS TERM` (`ok N` / `err 理由`)、`layout get|set|clear`、`sessions` を足した。
  つなげなかった `attach` はソケットを閉じるようにした (前は開いたまま)。
- リモートドライブ: zmux.sys に命令 6 (チャネル 2 で要求 1 つを送り、応答 1 つを待つ。待つ間も線を回す)、`zrmt.sys` (CONFIG.SYS で ZMUX.SYS の後ろ、`/uN`)、
  zmuxd の `--drive DIR` (複数可 = ユニット)。READ / WRITE は 4084 バイト = フレーム 4 つ。詳しくは DESIGN.md §4b と README。
- `test/fakex68k.py`: XEiJ の代わりの「にせの X68000」(線の約束、COMMAND.X・zos.x の 0xA7・zrmt.sys・キーのふり、19200bps の真似)。
  `test/e2e_fake.py` はそれで zmuxd と `zmux` を端から端まで試す (36 項目、この機械で全部通った)。XEiJ も elf2x68k も要らないので、クラウドでも流せる。
- **この回の環境でできなかったこと**: クラウドの出口の制限で stdkmd.net (XEiJ)、retropc.net (Human68k)、GitHub のリリース (elf2x68k のバイナリ)、GNU のミラー、
  JDK 26 の配布が取れず、XEiJ の 4 つの e2e と新しい `e2e_tui.py` / `e2e_rdrive.py` は流していない。GitHub の `git clone` はできた (x68kserremote、elf2x68k のソース)。
  X68000 側のコンパイルは Ubuntu の `gcc-m68k-linux-gnu` (13.2) に elf2x68k の `iocs.h` / `dos.h` (gen スクリプトで inline 版を作る)、`x68k.ld`、`elf2x68k.py` を
  合わせた代用の `m68k-xelf-gcc` で通した (`-include stdint.h` が要る。出来たバイナリは elf2x68k の 13.5 のものとバイト単位では違う)。
  **`x68k/zmux.sys` と `zmux.x` は git では前の版のまま、`zrmt.sys` は入れていない**。XEiJ / 実機の前に elf2x68k で `make -C net/zmux/x68k`。

## 残り

1. **XEiJ で `test/e2e_tui.py` と `test/e2e_rdrive.py` を流す** (raspi5 か Mac。elf2x68k で x68k/ を作り直してから)。4 つの既存の e2e も同じ機械で。
   zrmt.sys は実機の Human68k で初めて動くので、まず `dir C:` と `type`、次に copy の往復。速さは README の表に足す。
2. **リモートドライブで気になるところ**: 要求に番号が無いので、5 秒の時間切れのあとに遅れて来た応答を捨てる (zmux.sys の rd_buf が無いとき) だけで、
   次の要求と入れ違う可能性はゼロではない (線が 5 秒止まるのは線が落ちたときくらいなので実害は小さい見込み)。気になれば x68kremote.h の形の外側に 1 バイトの番号を足す。
3. **タブ 0 が待ちになる**: タブ 0 の COMMAND.X で zos.x を起動するので、zos.x が動いている間はタブ 0 が待ちになる。
   Bravo (Z-OS) への提案: zos.x が AUX の COMMAND.X を見えないペインの 1 つとして持つ (または zos.x を常駐させて COMMAND.X に戻る) 形にすれば、タブ 0 が使い続けられる。
4. **実機の Compact XVI で 19200 / 38400bps** (XEiJ の SCC は受信 FIFO が無く、取りこぼし方が違う)。手順は boot/serial/RUNBOOK-xvi.md の 4 (rx.x で 38400 を試す)。
   画面 (`zmux`) は iTerm2 から ssh で実際に見る (pyte での試験は字の並びだけ。色と罫線の見た目は未確認)。
5. 主記憶 2MB では zos.x のペインが起動しない (Bravo の #48)。
6. 画面の細かいこと: ペインの大きさは半分ずつで変えられない。ホイールのスクロールバックは 2000 行。マウスの並びは X68000 へ送らない (Human68k に受け口が無い)。

## 試験を最初から流す (S3 を使わない)

Mac では `x68kcli/tools` (XEiJ、エージェントの jar、HUMAN302.XDF) がある前提でパスが書いてある (`HUMAN_XDF`、`XEIJ_TOOLS` で変えられる)。
クラウドや Docker では次をそろえる:

- **XEiJ**: `x68kcli/scripts/tools.sh` と同じく https://stdkmd.net/xeij/XEiJ_0261008.zip を取って展開する。中に `HUMAN302.XDF` (Human68k 3.02 の
  システムディスク) も入っている。Java 26 (XEiJ.jar は class 70) と Xvfb。
- **エージェント**: `harness/xeij/agent` を XEiJ.jar に対して javac で組み、`xeijagent.jar` (manifest.txt 付き) を `$XEIJ_TOOLS/xeijagent.jar` に。
- **m68k のもの**: elf2x68k (Linux 版は x86-64 だけ、https://github.com/yunkya2/elf2x68k/releases)。`make -C net/zmux/x68k`、`make -C net/npd`、
  `make -C net/tistack` と `net/tistack/tests` の `tisnet.x`、`make -C boot/serial rx.x`。
- **zmuxd**: `cargo build --release` (net/zmux)。クレートは crates.io から (ratatui、crossterm、vt100、encoding_rs が増えた)。
- **XEiJ なしの試験**: `python3 -m pip install pyte` のあと `python3 test/e2e_fake.py` (1 分ほど。19200bps の真似を切るなら `BAUD=0`)。
- **ほかの入力**: ssh.x は j3tm0t0/x68k のリリース v2026.10.23.1 (`SSH_X`)。zos.x / zosh.x は x68k-zos (非公開) の `sshz` ブランチで組む (`ZOS_X` / `ZOSH_X`)。
  rcmd.x は X68000 の上でしか組めないので x68kcli の HDS から取り出す (`RCMD_X`。無ければ rcmd の試験は飛ばす)。
- 流し方: `XEIJ_TOOLS=… HUMAN_XDF=… python3 test/e2e.py RUN` (ほかの 3 つも同じ)。XEiJ の RS-232C は 127.0.0.1:54368 (`XPORT`)。

試験のフロッピーは `boot/serial/floppy/mkxdf.py` が作る (rb-cli は要らない)。

## raspi5 でしかできないこと (クラウドでは「raspi5 で確かめる」として残す)

- raspi5 には XEiJ (JDK 26 を ~/opt、Xvfb、~/xeijtools) と rustup があり、4 つの e2e のうち 3 つ (線、TSP、セッション) を raspi5 だけで通した。
- `boot/serial/raspi/setup.sh` を raspi5 に適用済み (x68-serlog 有効、`/etc/default/x68k-serial`)。
- 実機の XVI と USB シリアル (`/dev/ttyUSB0`) を使う試験、TUI を実際の端末 (iTerm2 から ssh) で見ること、38400bps の取りこぼし。
