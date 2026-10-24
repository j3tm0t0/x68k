# zmux の設計 (#23)

RS-232C 1 本で、X68000 (Compact XVI) と Raspberry Pi の間に次を同時に流す。

| チャネル | 中身 | X68000 側 | Pi 側 (zmuxd) |
|---|---|---|---|
| 0 | コンソール (COMMAND.X の CTTY AUX の入出力) | zmux.sys の文字デバイス `AUX` (RSDRV.SYS の AUX の代わり) | `zmux attach 0` (herdr のタブ 0) |
| 1 | netpipe の交換 (X6NR レコード: TSP 0xA6、暗号 0xA5、端末のセッション 0xA7) | zmux.x (npd.x と同じ交換を、SCSI ID6 の代わりにこの線で) | TSP は普通のソケット、0xA5 は Rust の暗号ライブラリ、0xA7 は `zmux attach 1..4` |
| 2 | リモートドライブ (後で) | zmux.sys のブロックデバイス | ホストのディレクトリ |
| 3 | キー入力の注入 | zmux.sys が IOCS _B_KEYINP / _B_KEYSNS を横取り | `zmux key`、`zmux attach` の対応表 |
| 15 | 線の管理 (あいさつ、速さの切り替え、統計、時刻) | zmux.x | zmuxd |

画面のレイアウトは X68000 では持たない。X68000 は独立したバイト列 (コンソール 1 本と zosh のセッション N 本) を出すだけで、
タブにするかペインにするかは herdr / tmux の使い方で決まる。

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

チャネル 0 と 3 はリング (各 2KB) の空きを、チャネル 1 は交換の単位 (下) で調整する。
相手のリングが一杯のときは送らずに持っておき (線の窓とは別)、空いたら送る。チャネル 0 の X68000 から Pi への向きは、
Pi に誰も attach していなくても zmuxd が読み捨てる (直近 64KB をスクロールバックとして持つ) ので、COMMAND.X が止まることはない。

## 2. チャネル 1: netpipe の交換を線に載せる

今の npd.x は SCSI ID6 の空き LBA に要求 (最大 4080 バイト) を書き、応答を読む「交換」を繰り返す。中身は長さ付きのレコードの列 (X6NR) で、
先頭の 1 バイトで IPv4 (4x)、暗号 0xA5、TSP 0xA6、端末のセッション 0xA7 を分ける。tis.x -p と zos.x は npd.sys に入口を預け (ETD 25〜28)、
npd.x が交換のたびに呼ぶ。

zmux.x はこの npd.x の交換の部分 (`zxchg`) だけを線に置き換えたものにする。

- 要求: チャネル 1 のフレームの列で、X6NR の要求ブロックをそのまま送る (1024 バイトずつに分け、最後のフレームに flags の印)。
- 応答: zmuxd が Pico と同じ規則で作った応答ブロックを、同じようにチャネル 1 で返す。
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
- rx.x / sx.py は zmux の前の段階 (bootstrap) で使う。zmux が動いている間にファイルを送るのは、チャネル 1 の TSP (rcmd.x、x68put) か、
  後のリモートドライブ。

コンソールの写し (contee.sys のように画面の出力を写す、段 1) は優先度を下げる (moto: リモートの画面は本体の画面と同じでなくてよい)。
作るなら zmux.sys が IOCS _B_PUTC / _B_PRINT を横取りしてチャネル 4 に流すだけで、Pi 側に Human68k のコンソールの再現を足す。

## 4. セッション (zosh のヘッドレスペイン、タブ / ペイン 1..n)

Z-OS (x68k-zos) の sshd 用の見えないペイン (0xA7、net/npd/zss.h、x68k-zos の kernel/sshs.c、x68k-zos PR #34 でマージ済み) をそのまま使う。0xA7 は Pico の zsshd が SSH の
クライアントの代わりにセッションを開く形なので、zmuxd が zsshd の役をする。

- `zmux attach` (番号なし) は空いているセッション番号 (1〜4、ZSS_NSESS) を取り、zmuxd が `ZSS_E_OPEN` (端末の大きさ、TERM) を送る。
  zos.x は見えないペインを作って zosh を起動し、`ZSS_C_ACCEPT` を返す。
- 以後、クライアントの入力は `ZSS_E_INPUT`、出力は `ZSS_C_OUTPUT`、端末の大きさの変化 (SIGWINCH) は `ZSS_E_WINCH`。
  流量は zss.h の CREDIT / WINDOW のまま。
- クライアントが切れたら `ZSS_E_EOF`。zosh が終わったら `ZSS_C_EXIT` でクライアントも終わる。
- `zmux attach N` は既にあるセッション N につなぎ直す (zmuxd が出力の直近 64KB を持っていて、つないだときに流す)。
- herdr で新しいタブやペインを開いてそこで `zmux attach` を実行すれば、新しい zosh になる。herdr のソケット API で
  タブを作るのは herdr 側の設定に任せ、zmux は持たない (例: herdr の起動設定でタブ 0 に `zmux attach 0`、
  ペインを割ったら `zmux attach`)。必要なら後で `zmux sessions` (一覧の JSON) を出して、herdr の設定から使えるようにする。

## 5. キー

`zmux attach` にはホットキーも前置キーも自前のエスケープも無い。端末を raw にして、届いたバイトをそのまま送る
(CSI u などの拡張キーの並びも素通し)。タブ・ペイン・切り離しは herdr の役目 (moto の herdr と tmux の前置キーは Ctrl+X)。

- コンソール (チャネル 0) と zosh のセッションでは、キーはただのバイト列 (セッションは VT 風の並び)。
- X68000 にあって端末に無いキー (XF1〜XF5、HELP、UNDO、かな、ローマ字、コード入力、記号入力、登録、OPT.1/2、BREAK、COPY、CLR、
  テンキーの ENTER) は、`~/.config/zmux/keys.toml` の対応表で、端末から来る並びをキー入力の注入 (チャネル 3) に変える。
  既定は普段使われないキーにだけ割り当てる (F13〜F24。例: F13 = XF1 ... F17 = XF5、F18 = OPT.1、F19 = OPT.2、F20 = BREAK、
  F21 = COPY、F22 = HELP、F23 = UNDO、F24 = かな)。対応表に無い並びは全部そのままバイトとして送る。
- キー入力の注入 (チャネル 3): zmux.sys が IOCS _B_KEYINP / _B_KEYSNS を横取りし、チャネル 3 から来たキー (スキャンコード + 文字コード、
  _B_KEYINP の戻り値と同じ形) を先に返す。DOS のキー入力も IOCS を通るので届く。キーボードの行列を直接読むプログラム (ゲームなど)
  には届かない (README に書く)。`zmux key 'dir\r'` や `zmux key --scan 0x63` で、本体の画面で動いているもの (Z-OS の前の Human68k、
  画面のエディタ) を操作する。タブ 0 (COMMAND.X の CTTY) の入力とは別物。

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
2. **チャネル 1 (TSP と 0xA5)**: npd.x の交換を差し替えた zmux.x、zmuxd の TSP / 暗号。rcmd.x / ssh.x が tis.x -p でつながること。
3. **セッション (0xA7)**: `zmux attach` で zosh。herdr の中で複数。
4. **キーの注入 (チャネル 3)** と keys.toml。
5. **リモートドライブ (チャネル 2)**: x68kserremote の要求の形を参考に。
6. 実機 (Compact XVI) で 19200 / 38400 を測る。

## 8. 置き場所と道具

- `net/zmux/`: Rust のクレート (zmuxd と zmux の 2 つのバイナリ、1 つのクレートで `zmux` を `zmuxd` としても起動できる形)。
  term/zterm と同じく依存は少なく (libc、暗号のクレート、toml)。raspi5 (Debian arm64) で組む。
- `net/zmux/x68k/`: zmux.sys / zmux.x (elf2x68k、C とアセンブラ)。npd.x / slipd.x のコードを元にする。
- 開発の環境: raspi5 で zmuxd を動かし、XEiJ の RS-232C の TCP をそこへつなぐ。XEiJ を raspi5 の Docker で動かせるかは別に測る
  (遅ければ Mac の XEiJ から TCP で raspi5 へ)。
