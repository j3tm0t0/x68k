# zmux の設計 (#23)

RS-232C 1 本で、X68000 (Compact XVI) と Raspberry Pi の間に次を同時に流す。

| チャネル | 中身 | X68000 側 | Pi 側 (zmuxd) |
|---|---|---|---|
| 0 | コンソール (COMMAND.X の CTTY AUX の入出力) | zmux.sys の文字デバイス `AUX` (RSDRV.SYS の AUX の代わり) | Pi の zmux のタブ 0 (`zmux attach 0`) |
| 1 | 入力の注入 (キー。マウスは予定、下の「チャネル 1 のメッセージ」) | zmux.sys が IOCS _B_KEYINP / _B_KEYSNS を横取り | `zmux key`、`zmux attach` の対応表 |
| 2 | リモートドライブ (x68kserremote の要求と応答の形) | zrmt.sys (Human68k のリモートドライブ、zmux.sys の入口で交換) | `--drive DIR` のディレクトリ (rdrive.rs) |
| 3 | ネットワーク: netpipe の交換 (X6NR レコード: TSP 0xA6、暗号 0xA5、端末のセッション 0xA7) | zmux.x (npd.x と同じ交換を、SCSI ID6 の代わりにこの線で) | TSP は普通のソケット、0xA5 は Rust の暗号ライブラリ、0xA7 は `zmux attach 1..4` |
| 15 | 線の管理 (あいさつ、速さの切り替え、統計、時刻) | zmux.x | zmuxd |

画面のレイアウトは X68000 では持たない。X68000 は独立したバイト列 (コンソール 1 本と zosh のセッション N 本) を出すだけ。
Pi の zmux が tmux のような端末の多重化ソフトになり、タブとペインを Pi で描く (§5、moto の決定 2026-10-25。herdr には頼らない)。

## 1. 線の上の形

### 素の文字とフレームの共存

起動した直後の線は今と同じ素の文字 (CTTY AUX の COMMAND.X、rx.x)。zmux が始まるとフレームだけになる。
Pi の zmuxd は最初から線を持ち、フレームの始まりの並びを見つけるまでは、届いた文字をそのままチャネル 0 (タブ 0) に渡す。

- フレームは `DLE STX` (10h 02h) で始まり `DLE ETX` (10h 03h) で終わる。中身の 10h は 10h 10h に重ねる (BISYNC の透過モードと同じ)。
- DLE (10h) は Human68k のコンソールの制御文字にも Shift_JIS の 1 バイト目にも使われないので、素の文字の中に `DLE STX` は現れない。
  (Shift_JIS の 2 バイト目は 40h 以上)
- zmuxd は `DLE STX` を見たらフレームの読み取りに入る。フレームが CRC で正しく、種類があいさつ (チャネル 15 の HELLO) なら zmux の状態に移る。
  zmux が止まった (BYE、または X68000 のリセットで 10 秒フレームが来ない) ら、素の文字に戻る。
- 素の文字の中で 10h が来ても (バイナリを TYPE したときなど)、続きが STX でなければそのまま渡す。

### フレーム

```
DLE STX | chan(1) | seq(1) | ack(1) | flags(1) | len(2) | data(len) | crc16(2) | DLE ETX
```

- `chan`: チャネル番号 (0〜15)。上の 4 ビットは予約。
- `seq` / `ack`: 線の番号 (0〜255、片方向ごと)。`ack` は相手から正しく受けた最後の `seq` (それまでの全部を受け取った)。
  データの無いフレーム (len 0) は確認だけのフレームで、`seq` を進めない。
- `flags`: bit0 = NAK (ack の次から送り直してほしい)、bit1 = 相手の受け取りに余裕がない (送るのを待ってほしい)。
- `len`: 0〜1024。CRC は CRC-16/CCITT (多項式 1021、初期値 FFFF) を `chan` から `data` の終わりまで、重ねる前の値で計算する
  (rx.x と同じ。68000 では 512 バイトの表で 1 バイト十数命令)。
- DLE を重ねる分の増え方は、ランダムなデータで 1/256。

### 送り直し (Go-Back-N、窓 4)

- 送る側は確認の来ていないフレームを 4 つまで持つ。CRC の合わないフレームや番号の飛んだフレームは捨て、NAK を返す。
- 確認は、反対向きのデータのフレームの `ack` に載せる。反対向きのフレームが 50ms 無ければ確認だけのフレームを送る。
- 確認が 1 秒来なければ、確認の来ていない最初のフレームから送り直す。
- 窓 4 x 1024 バイトは 19200bps で約 2.1 秒分。USB シリアルの遅れ (数 ms〜16ms) と、X68000 側がフロッピーを読み書きしている間
  (受信は割り込みで続くが、返事が遅れる) を隠す。

### 線の流量

X68000 の受信は RSDRV.SYS の割り込みに任せ、受信バッファを 32KB に付け替える (slipd.x の `grow_rsdrv` と同じ。RSDRV.SYS 2.02 の
元の大きさは 64 バイト)。zmux.x はスレッドから IOCS の _ISNS232C / _INP232C / _OSNS232C / _OUT232C でポーリングする。
自前の SCC 割り込みは xip.x を壊した前例があるので使わない (`net/slipd`、slip.sys を作ったときの記録)。
窓 4 x 1024 バイト = 4KB は 32KB に余裕で収まるので、XON/XOFF も RTS/CTS も使わない (3 線のケーブルで動く)。
flags bit1 は、X68000 側でチャネルの受け皿 (コンソールのリング、netpipe の交換の場所) が一杯のときに立てる。

### 各チャネルの流量

チャネル 0 と 1 はリング (各 2KB) の空きを、チャネル 3 は交換の単位 (下) で調整する。
相手のリングが一杯のときは送らずに持っておき (線の窓とは別)、空いたら送る。チャネル 0 の X68000 から Pi への向きは、
Pi に誰も attach していなくても zmuxd が読み捨てる (直近 64KB をスクロールバックとして持つ) ので、COMMAND.X が止まることはない。

### チャネル 1 のメッセージ (入力の注入)

Pi から X68000 への向きだけ。データは次のメッセージの列で、フレームの切れ目とメッセージの切れ目は一致しなくてよい。
メッセージの先頭 1 バイトが種類で、種類ごとに中身の長さが決まっている。

| 種類 | 中身 | 状態 |
|---|---|---|
| 1 (IN_KEY) | 2 バイト: スキャンコード、文字コード (IOCS _B_KEYINP の値の上位・下位) | 実装済み |
| 2 (IN_MOUSE) | 3 バイト: dx (int8)、dy (int8)、buttons (bit0 = 左、bit1 = 右) | 予定。X68000 側はまだ処理しない |

知らない種類のメッセージは長さが分からないので、X68000 はそのフレームの残りを捨てる。種類を足すときは Pi と X68000 の両方を同時に入れ替える。

## 2. チャネル 3: netpipe の交換を線に載せる

今の npd.x は SCSI ID6 の空き LBA に要求 (最大 4080 バイト) を書き、応答を読む「交換」を繰り返す。中身は長さ付きのレコードの列 (X6NR) で、
先頭の 1 バイトで IPv4 (4x)、暗号 0xA5、TSP 0xA6、端末のセッション 0xA7 を分ける。tis.x -p と zos.x は npd.sys に入口を預け (ETD 25〜28)、
npd.x が交換のたびに呼ぶ。

zmux.x はこの npd.x の交換の部分 (`zxchg`) だけを線に置き換えたものにする。

- 要求: チャネル 3 のフレームの列で、X6NR の要求ブロックをそのまま送る (1024 バイトずつに分け、最後のフレームの flags に F_END)。
- 応答: zmuxd が Pico と同じ規則で作った応答ブロックを、同じようにチャネル 3 で返す。
- tis.x / zos.x / accel 側の記録形式 (tsp.h、zss.h、accel_proto.h) と、それぞれの止まって待つ確認 (zseq / zack) は変えない。
  線の再送とは二重になるが、どちらも安い。
- 交換を始めるのは X68000 だけ (今と同じ)。ただし SCSI と違って線は空回りさせたくないので、送るものが無いときは交換しない。
  Pi に X68000 へ渡すものができたら、zmuxd がチャネル 15 で KICK (1 バイト) を送り、zmux.x はそれを見て交換する。
  (今の npd.x は応答の MORE で次の交換を急ぐ。それはそのまま使う)
- npd.sys はそのまま使う (xip.x の Ethernet ドライバと、tis.x / zos.x / accel の入口を預かる役)。xip.x の代わりに tis.x -p を使えば、
  線には IP も TCP の ACK も乗らない。

Pi 側 (zmuxd) は harness/picosim の netpipe.c と Pico ファームの tsp_svc.c / zsshd の Z 側の窓口と同じことを Rust で書く:
TSP の CONNECT / LISTEN / DATA / RESOLVE は `std::net` と `TcpStream` / `UdpSocket`、0xA5 は `x25519-dalek` / `ed25519-dalek`、
0xA7 は下の「セッション」。

## 3. チャネル 0: コンソール (タブ 0)

タブ 0 は今の CTTY AUX と同じ、Human68k の COMMAND.X をシリアルで使うもの。zmux が始まっても COMMAND.X はそのまま動く。

- zmux.sys は文字デバイス `AUX` を作る。後から組み込んだデバイスが先に見つかるので、`CTTY AUX` は RSDRV.SYS の AUX ではなく
  zmux.sys につながる (COMMAND.X 3.00 の CTTY は con と aux しか受け付けないので、`CTTY ZCON` のような別の名前は使えなかった)。
  bootstrap の AUTOEXEC.BAT の `CTTY AUX` はそのまま使える。AUX0〜AUX5 は RSDRV.SYS のまま。
- zmux が上がっていない間 (RAW)、この AUX は RSDRV.SYS の AUX と同じく素の文字を通す。COMMAND.X が AUX を読み書きしている間に
  5 秒ごとに HELLO を送り、Pi の zmuxd が答えれば UP になる。zmux.x のスレッドは UP の間だけ線を回す
  (RAW の間は rx.x などが IOCS で直接読み書きしているかもしれないので、横取りしない)。
- AUX の書き込みはチャネル 0 の送信リングへ、読み込みは受信リングから。zmux.x のスレッドと AUX の読み書きの両方が線を回す
  (どちらもスーパーバイザで動き、Human68k はその間スレッドを切り替えないので、同時には走らない)。
- zmux の前 (素の文字) と後 (チャネル 0) で、Pi の `zmux attach 0` に見えるものは同じ (上の「素の文字とフレームの共存」)。
- rx.x / sx.py は zmux の前の段階 (bootstrap) で使う。zmux が動いている間にファイルを送るのは、チャネル 3 の TSP (rcmd.x、x68put) か、
  後のリモートドライブ。

コンソールの写し (contee.sys のように画面の出力を写す、段 1) は優先度を下げる (moto: リモートの画面は本体の画面と同じでなくてよい)。
作るなら zmux.sys が IOCS _B_PUTC / _B_PRINT を横取りしてチャネル 4 に流すだけで、Pi 側に Human68k のコンソールの再現を足す。

## 4. セッション (zosh のヘッドレスペイン、タブ / ペイン 1..n)

Z-OS (x68k-zos) の sshd 用の見えないペイン (0xA7、net/npd/zss.h、x68k-zos の kernel/sshs.c、x68k-zos PR #34 でマージ済み) をそのまま使う。0xA7 は Pico の zsshd が SSH の
クライアントの代わりにセッションを開く形なので、zmuxd が zsshd の役をする。

- セッションの開き方は今の実装のとおり (`zmux attach` が OPEN を送る)。§5 の TUI では「新しいタブ / ペイン」が同じことをする。
- zmuxd はセッションの出力の直近 64KB を持ち、つなぎ直したときに流す。

## 4b. チャネル 2: リモートドライブ

Human68k のリモートドライブ (属性 0x2000 のデバイス。ファイル単位の要求 CHDIR / FILES / OPEN / READ / WRITE … が届く) を zrmt.sys が受け、
Pi のディレクトリで答える。要求と応答の形は yunkya2 氏の x68kserremote の `x68kremote.h` のまま (x68kzremotedrv の B: と同じ。README に表)。

- zrmt.sys は x68kserremote の `driver/remotedrv.c` を元に、線の読み書きを zmux.sys の入口 (命令 6: 要求のブロック、応答を書く先、待つ時間) に置き換えたもの。
  要求 1 つ = チャネル 2 の 1 ブロック (1024 バイトずつに分け、最後のフレームに F_END)。応答も同じ形で 1 ブロック。要求は同時に 1 つ。
- 応答を待つ間は zmux.sys の中で線を回す (AUX の読み込みと同じ。lock は回すたびに取って放すので、zmux.x や npd.x のスレッドも線を回せる)。
  5 秒応答が無ければ zrmt.sys は 1 回送り直し、それでも無ければ「ドライブの準備ができていません」(0x1002) を返す。時間切れのあとで遅れて来た応答は zmux.sys が捨てる。
  線が UP でないとき (zmuxd が居ない) はすぐ失敗する。CONFIG.SYS で組み込むときは Pi と話さない (zmux はまだ始まっていない)。
- READ / WRITE のデータは 1 回 4084 バイト: 応答 2 + 4084 = 4096 = 1024 バイトのフレーム 4 つ = 窓 1 つ分。要求 11 + 4084 も 4 つに収まる。
  小さな読み書きは zrmt.sys の 4084 バイトのキャッシュ (2 つ) でまとめる (remotedrv.c の dcache)。
- Pi (rdrive.rs): パスは dos_namebuf (区切り 0x09、主ファイル名 8 + 10、拡張子 3) をホストのパスにし、Shift_JIS ⇔ UTF-8 (encoding_rs)。
  FILES は一覧を FILBUF のアドレスごとに持ち、NFILES で順に返す。'?' の照合は HFS.java (XEiJ) の規則。errno は Human68k のエラーに直す。
- 速さの見込み (19200bps = 1920 バイト/秒): 4084 バイトの READ は 4 フレーム約 4160 バイトで 2.17 秒 + 要求 1 フレーム + 往復の遅れ。約 1.85 KB/s。
  にせの X68000 (fakex68k.py、速さの真似) で READ / WRITE とも 1.82 KB/s。68000 の処理とフロッピーが乗る実測は XEiJ / 実機で。

## 5. Pi の zmux: tmux のような TUI (moto の決定。実装済み、XEiJ と実機ではまだ)

Pi に ssh で入るか iTerm2 のペインで `zmux` を実行すると、tmux のような画面になる (tui.rs)。

- **サーバーとクライアント**: zmuxd (サーバー) がシリアル回線とセッションを持ち、`zmux` (クライアント) は unix ソケットでつなぐ。
  ssh が切れてもセッションは残り、`zmux` でつなぎ直す。
- **タブとペイン**: タブ 0 = CTTY AUX の COMMAND.X (チャネル 0)。ほかのタブ・ペインは zosh のセッション (0xA7) で、必要なときに開く。
  各ペインに VT のパーサ (`vt100` クレート、term/zterm と同じ) を持ち、ratatui + crossterm で描く。マウスあり。下に状態の行 (タブ、速さ、線の統計)。
- **キー** (moto の tmux / herdr の設定と同じ): 前置キー Ctrl+X のあとに `|` 縦に割る、`-` 横に割る、`c` 新しいタブ (新しい zosh のセッション)、
  `n` / `p` / 数字 タブの切り替え、`k` ペインを閉じる、`d` 切り離し、`?` 一覧、Ctrl+X で Ctrl+X そのもの。**ほかのホットキーは無い**。
  前置キーの組とマウスの並び (SGR 1006) 以外は全部そのまま XVI に送る (CSI u などの拡張キーも)。打ったバイトは crossterm のイベントにせず、
  自分で読んで前置キーとマウスの並びだけを拾う (tui.rs の `Input`)。
- **サーバーとの約束**: ペイン 1 つ = zmuxd へのソケット 1 本。タブ 0 は `attach 0`、新しい zosh は `open COLS ROWS TERM` (最初の 1 行が `ok N` か `err 理由`)、
  つなぎ直しは `attach N`。配置は `layout set TEXT` で zmuxd に置き、起動時に `layout get` と `sessions` で残っているセッションにつなぎ直す。
  TEXT はタブごとに `;` 区切り、ペインの木は `(v A B)` (左右) / `(h A B)` (上下)、葉はセッションの番号 (0 = コンソール)。例: `0;(v 1 2)`。
- **文字コード**: タブ 0 は Shift_JIS ⇔ UTF-8 (encoding_rs。途中で切れた字は次の読みまで持つ)。zosh のセッションは UTF-8 のまま。
- **X68000 にしか無いキー**: `~/.config/zmux/keys.toml` の対応表 (端末から来る並び → X68000 のキー) で、チャネル 1 のキー入力の注入に変える
  (実装済みの `keys.rs`)。既定は F13〜F20 と Shift+F1〜F9。
- 今ある `zmux attach 0 / N`、`zmux key`、`zmux kbd` は試験と道具として残す。

## 6. 速さの見込み

| 経路 | 線に乗るもの | 19200bps での見込み |
|---|---|---|
| 今の SLIP (段階 4) | IP + TCP の見出し 40 バイト、ACK のパケット、SLIP の 2 バイト | x68put で 1.49 KB/s (XEiJ) |
| zmux + TSP | フレームの見出し 10 バイト / 1024 バイト、TSP のメッセージ見出し 6 バイト | 1.8 KB/s 近く (rx.x と同じ程度) |

68000 側も IP と TCP の処理 (xip.x) が無くなるので軽くなる (tis.x -p は XEiJ + picosim で xip.x の 2.5〜10 倍だった)。38400bps は実機で試す
(XEiJ の SCC は受信の FIFO が無く、実機と取りこぼし方が違う)。

## 7. 作る順番

1. **線 (チャネル 0 と 15)**: zmux.x (IOCS ポーリング、フレーム、Go-Back-N) + zmux.sys (AUX) と、zmuxd の線の部分 + `zmux attach 0`。
   XEiJ (RS-232C を TCP に) で、素の文字 → zmux → 素の文字の切り替え、バイトの破損で再送、COMMAND.X が `CTTY AUX` で動くこと。**済み** (`test/e2e.py`)。
2. **チャネル 3 (TSP と 0xA5)**: npd.x の交換を差し替えた zmux.x、zmuxd の TSP / 暗号。rcmd.x / ssh.x が tis.x -p でつながること。**済み** (`npd.x -m`、TSP と 0xA5、`test/e2e_tsp.py`)。
3. **セッション (0xA7)**: `zmux attach` で zosh。**済み** (`test/e2e_zss.py`)。
3b. **tmux のような TUI** (§5)。**済み** (にせの X68000 `test/fakex68k.py` で `test/e2e_fake.py`。XEiJ の `test/e2e_tui.py` はまだ流していない)。
4. **キーの注入 (チャネル 1)** と keys.toml。**済み** (`zmux key` / `zmux kbd`、`test/e2e_key.py`)。
5. **リモートドライブ (チャネル 2)**: x68kserremote の要求の形のまま (§4b)。**済み** (にせの X68000 で全命令と速さ。XEiJ の `test/e2e_rdrive.py` と実機はまだ)。
6. 実機 (Compact XVI) で 19200 / 38400 を測る。

## 8. 置き場所と道具

- `net/zmux/`: Rust のクレート (zmuxd と zmux の 2 つのバイナリ、1 つのクレートで `zmux` を `zmuxd` としても起動できる形)。
  依存は libc、暗号のクレート、画面の ratatui + crossterm + vt100、文字コードの encoding_rs。raspi5 (Debian arm64) で組む。
- `net/zmux/x68k/`: zmux.sys / zmux.x (elf2x68k、C とアセンブラ)。npd.x / slipd.x のコードを元にする。
- 開発の環境: raspi5 で zmuxd を動かし、XEiJ の RS-232C の TCP をそこへつなぐ。XEiJ を raspi5 の Docker で動かせるかは別に測る
  (遅ければ Mac の XEiJ から TCP で raspi5 へ)。
