# net/zmux

X68000 (Compact XVI) と Raspberry Pi の間の RS-232C 1 本で、コンソール、TCP 通信 (TSP: TCP の処理を相手側に任せる tis.x -p の要求)、
暗号アクセラレータ、Z-OS のシェル zosh のセッションを多重化する (#23)。設計は [DESIGN.md](DESIGN.md)。

実装済みなのは、段階 1 (シリアル回線とコンソール)、段階 2 (チャネル 3 で `tis.x -p` のソケット要求と暗号の要求を Pi 側で処理)、
段階 3 (Z-OS の画面に出ないペインで動く zosh に `zmux attach` でつなぐ)、キー入力の注入 (チャネル 1)、
tmux のような画面 (`zmux`、下の「zmux の画面」)、リモートドライブ (チャネル 2、`zrmt.sys`、下の「リモートドライブ」)。
画面とリモートドライブは、XEiJ の代わりに線の約束を話す「にせの X68000」(`test/fakex68k.py`) で端から端まで通した。XEiJ と実機ではまだ (下の「XEiJ での確認」)。

| 置き場所 | 中身 |
|---|---|
| `src/` | Pi 側 (Rust): `zmux daemon` (zmuxd)、`zmux` (画面)、`zmux attach 0`、`zmux status`。`link.rs` (シリアル回線のフレームと再送)、`netpipe.rs` (npd.x の要求・応答の転送)、`tsp.rs` (TSP の要求を Pi のソケットで処理)、`zss.rs` (zosh のセッション)、`accel.rs` (暗号)、`keys.rs` (キー)、`tui.rs` (tmux のような画面)、`rdrive.rs` (リモートドライブ) |
| `x68k/` | X68000 側 (elf2x68k): `zmux.sys` (線の処理と、CTTY AUX をチャネル 0 にする AUX デバイス)、`zmux.x` (線を回すスレッド)、`zrmt.sys` (リモートドライブ) |
| `test/fakex68k.py` | XEiJ の代わりの「にせの X68000」: 線の約束を TCP で話し、COMMAND.X・zos.x の 0xA7・zrmt.sys・キーの受け口のふりをする。19200bps の速さの真似もする |
| `test/e2e_fake.py` | にせの X68000 で、線、`zmux attach 0`、リモートドライブの全命令と速さ、画面 (pty で `zmux` を動かし pyte で読む) を試す。XEiJ も elf2x68k も要らない |
| `test/e2e_tui.py` | XEiJ で画面を試す (ZOS_X / ZOSH_X があれば zosh のタブも)。**まだ通していない** |
| `test/e2e_rdrive.py` | XEiJ で zrmt.sys を試す (dir / type / copy の往復と照合 / md / del / rd / ren、copy の速さ)。**まだ通していない** |
| `test/e2e.py` | XEiJ で、素の文字 → zmux → 素の文字の切り替えと、バイトを壊す線での再送を試す |
| `test/e2e_key.py` | XEiJ で、Pi から注入したキー (`zmux key`) が本体のキーボード入力として COMMAND.X に届くかを試す |
| `test/e2e_zss.py` | XEiJ (主記憶 12MB) で、zos.x を起動して `zmux attach` で zosh のセッションを開き、echo、2 つ同時、exit の終了コードを試す |
| `test/e2e_tsp.py` | XEiJ で、tis.x -p + npd.x -m のソケット (名前引き、往復、put / get の照合、rcmd.x の待ち受けと x68put) を試す |
| `../npd/npd.c` | `npd.x -m`: SCSI ID 6 の代わりに zmux のチャネル 3 で交換する |

## 使い方 (段階 1)

X68000:

```
CONFIG.SYS    DEVICE = \SYS\RSDRV.SYS
              DEVICE = \SYS\ZMUX.SYS
              PROCESS = 8 10 20
AUTOEXEC.BAT  RX -B19200
              ZMUX
              CTTY AUX
```

Pi:

```
cargo build --release
target/release/zmux daemon --port /dev/ttyUSB0 --baud 19200 &    # zmuxd
target/release/zmux attach 0                                    # herdr / tmux のタブ 0 で
```

- zmuxd が居ない間、X68000 の AUX は今までの CTTY AUX と同じ素の文字で、sx.py や普通の端末で使える。
  5 秒ごとに HELLO のフレーム (DLE STX で始まる 16 バイト) が混じる。
- zmuxd を起動すると、X68000 からの HELLO に応答し、フレーム形式の通信に切り替わる。zmuxd を止めると、X68000 は 10 秒後にフレームを使わない文字のやり取りに戻る。
- `zmux attach 0` にはホットキーも前置キーも無い (tmux の中で使うときの形)。タブとペインを zmux 自身で持つのが `zmux` (下の「zmux の画面」)。
  zmuxd はチャネル 0 の直近 64KB を持っていて、つないだときに流す。

## 段階 2: TCP (TSP)

X68000 で tis.x -p (TCP / UDP / DNS を相手に任せる xip.x の代わり) と `npd.x -m` を動かすと、ソケット操作の要求がチャネル 3 で zmuxd に届き、
zmuxd が Pi のソケットを使って処理する。シリアル回線には IP パケットも TCP の ACK も流れない。

```
CONFIG.SYS    DEVICE = \SYS\RSDRV.SYS
              DEVICE = \SYS\ZMUX.SYS
              DEVICE = \SYS\NPD.SYS
              PROCESS = 16 10 19
AUTOEXEC.BAT  RX -B19200
              ZMUX
              TIS -p
              NPD -m 192.168.68.2
              CTTY AUX
```

- X68000 から 192.168.68.1 (hosts の raspi) へつなぐと、zmuxd は Pi 自身 (127.0.0.1) につなぐ。X68000 の 127.0.0.0/8 は tis.x の中のループバック。
- X68000 で待ち受けたポート (rcmd.x の 2323 など) は、zmuxd が Pi で待ち受ける。Pi の `x68` / `x68put` は `X68_HOST=127.0.0.1` で使う。
- 暗号アクセラレータ (0xA5、ssh.x の `-oAccel=`) も同じ交換に載り、zmuxd が X25519 / Ed25519 を計算する (`src/accel.rs`、x25519-dalek / ed25519-dalek)。
- npd.x -m は送るものが無いときは交換しない。zmuxd に X68000 へ渡すものができると、チャネル 15 の KICK で交換を促す。

## 段階 3: zosh のセッション (0xA7)

X68000 で zos.x (Z-OS。X68000 Z の Pico W 上の SSH サーバー zsshd と組むための処理が入った版) が動いていると、`zmux attach` で新しい zosh のセッションが開く。
zmuxd が zsshd の代わりに 0xA7 の OPEN を送り、zos.x が画面に出ないペインで zosh を動かす。出力は UTF-8 で届く (zos.x が Shift_JIS から変える)。

```
zmux attach            # 新しいセッション (1〜4)。herdr / tmux の新しいタブやペインで実行する
zmux attach 2          # セッション 2 につなぎ直す (直近 64KB の出力を流してから)
zmux kill 2            # セッション 2 を閉じる
zmux status            # 線、交換の数、セッションの一覧
```

- クライアントが切れてもセッションは残る。zosh が `exit` するとクライアントも終わる (`[zmux: session N exited (終了コード)]`)。
- 端末の大きさの変化 (SIGWINCH) は zosh に WINCH として届く。
- タブ 0 (`zmux attach 0`) の COMMAND.X で zos.x を起動するので、zos.x が動いている間はタブ 0 は zos.x の終わりを待つ
  (本体の画面とキーボードは Z-OS のペインになる)。
- 試験には、x68k-zos (非公開) の `sshz` ブランチの zos.x を使った (x68k-zos PR #45)。主記憶 2MB では zos.x が動かない (起動フロッピーの README)。

## zmux の画面 (tmux のような使い方)

Pi に ssh で入って `zmux` を打つと、tmux のような画面になる。zmuxd (サーバー) がシリアル回線とセッションを持ち、`zmux` (クライアント) は
unix ソケットでつなぐだけなので、ssh が切れても zosh のセッションは残り、もう一度 `zmux` を打てばタブとペインの配置ごと戻る
(配置は zmuxd に置いてある。`zmux attach 0` や `zmux attach N` も今までどおり使える)。

```
target/release/zmux daemon --port /dev/ttyUSB0 --baud 19200 --drive ~/x68k-drive &
target/release/zmux
```

- タブ 0 はいつも COMMAND.X (CTTY AUX、チャネル 0)。ほかのタブとペインは zosh のセッション (0xA7)。
  COMMAND.X の出力は Shift_JIS なので UTF-8 に変えて表示し、打った字は Shift_JIS にして送る。zosh は UTF-8 のまま (zos.x が変える)。
- 前置キーは Ctrl+X。前置キーのあとに打つ 1 文字で動く。**ほかのホットキーは無い**。前置キーの組とマウスの並び以外のバイトは全部そのまま X68000 へ
  (カーソルキー、Ctrl+C、CSI u などの拡張キーの並びも素通し)。

| 前置キーのあと | 動き |
|---|---|
| `\|` | ペインを左右に割る (右が新しい zosh) |
| `-` | ペインを上下に割る (下が新しい zosh) |
| `c` | 新しいタブ (新しい zosh) |
| `n` / `p` / `0`〜`9` | 次のタブ / 前のタブ / その番号のタブ |
| `k` | 今のペインを閉じる (その zosh を終わらせる。タブ 0 は閉じられない) |
| `d` | 切り離す (zosh のセッションと配置は zmuxd に残る) |
| `?` | この一覧 (何かキーを押すと閉じる。そのキーは X68000 へ送らない) |
| `Ctrl+X` | Ctrl+X そのものを X68000 へ送る |

- マウス: クリックでペインとタブ (下の行) を選ぶ。ホイールでそのペインのスクロールバック (2000 行)。マウスの並びは X68000 へは送らない。
- 下の 1 行: タブの一覧 (`1:zosh 1+1` は zosh のセッション 1 のタブにペインが 2 つ)、線の状態 (up / raw)、速さ、受けた・送ったフレームの数、送り直しの数。
  知らせ (「zosh 2 が終わった: ...」など) もここに 4 秒出る。
- X68000 にしか無いキーは `~/.config/zmux/keys.toml` の対応表で (下の「キー」)。タブ 0 でだけキー入力の注入 (チャネル 1) になる。
- zosh が `exit` するとそのペインは消える。zos.x が居ないと (0xA7 の要求がまだ無いと) 新しいタブやペインは開けず、下の行に理由が出る。
- **注意**: タブ 0 の COMMAND.X で zos.x を起動するので、zos.x が動いている間はタブ 0 が zos.x の終わりを待ちになる (本体の画面とキーボードは Z-OS のペインになる)。
  zosh のタブはそのまま使える。Z-OS の側で、AUX の COMMAND.X を見えないペインの 1 つとして持つ (または zos.x を常駐させて COMMAND.X に戻る) 形にすれば、
  タブ 0 が使い続けられる (Bravo への提案、HANDOFF.md)。

端末のタイトル (OSC 0) は `XVI · zmux 19200 · [0] COMMAND.X` の形で、タブの切り替えと線の状態で更新する (線が上がっていなければ `XVI · 切断中`)。
終わるときは空にして、保存しておいた元のタイトルに戻す (CSI 22;0t / 23;0t)。tmux の中では `set-titles on` のときに外へ出る。

## リモートドライブ (チャネル 2)

X68000 の `zrmt.sys` が Human68k のリモートドライブ (ファイル単位の要求: CHDIR, FILES, OPEN, READ, WRITE …) を受け、要求 1 つをチャネル 2 の
1 ブロックとして Pi に送り、zmuxd (`src/rdrive.rs`) が `--drive DIR` のディレクトリで答える。要求と応答の形は yunkya2 氏の
[x68kserremote](https://github.com/yunkya2/x68kserremote) (MIT) の `x68kremote.h` のまま (この repo の x68kzremotedrv の B: も同じ形)。
`zrmt.sys` は同じ作者の `driver/remotedrv.c` を、Pi 側は `service/remoteserv.c` を元にしている。

```
CONFIG.SYS    DEVICE = \SYS\RSDRV.SYS
              DEVICE = \SYS\ZMUX.SYS
              DEVICE = \SYS\ZRMT.SYS          (ZMUX.SYS の後ろ。/u2 でドライブ 2 つ、--drive を 2 つ)
Pi            zmux daemon --port /dev/ttyUSB0 --drive ~/x68k-drive
```

- ドライブの文字は組み込んだ順 (フロッピー 2 台の次なら C:)。`zrmt.sys` が起動時に出す。
- できること: dir、type、copy (両方向)、md / rd、del、ren、属性、日付、空き容量。ファイル名は Shift_JIS で 18 + 3 文字まで (Human68k の制限)。
  Pi の名前が Shift_JIS にできないもの (絵文字など) や `/ \ , ; < = > [ ] |` を含むものは見えない。
- READ / WRITE は 1 回 4084 バイト (フレーム 4 つ = 窓 1 つ分)。小さな読み書きは `zrmt.sys` の中の 4084 バイトのキャッシュ (2 つ) でまとめる。
- zmuxd が居ない・線が UP でない・5 秒応答が無いときは「ドライブの準備ができていません」(0x1002) で失敗する (止まらない)。
  時間切れのあとで遅れて来た応答は捨てる。要求は同時に 1 つ。
- 速さ: にせの X68000 (19200bps の速さの真似、下) で READ 1.82 KB/s、WRITE 1.82 KB/s (40000 バイト)。線の見出し込みで 19200bps の上限は約 1.9 KB/s。
  XEiJ / 実機では 68000 の処理とフロッピーの読み書きが乗るので、`test/e2e_rdrive.py` で測り直す。

## キー

`zmux attach` にはホットキーも前置キーも自前のエスケープも無い。端末を raw にして、打ったバイトをそのまま送る。
タブやペインの切り替え・切り離しは `zmux` (上の画面) か herdr / tmux の役目 (前置キー Ctrl+X)。

| 何に打つか | 届き方 |
|---|---|
| `zmux` のタブ 0 / `zmux attach 0` (COMMAND.X の CTTY AUX) | バイト列のまま COMMAND.X の AUX の入力へ。Ctrl+C なども 1 バイトとして届く (`zmux` では UTF-8 の字を Shift_JIS にする) |
| `zmux attach` / `zmux attach N` (zosh のセッション) | バイト列のまま。zos.x がカーソルキーなどの並び (ESC [ A …) を X68000 のキーに直す |
| `zmux kbd` / `zmux key` (本体のキーボード) | X68000 のキー入力 (IOCS _B_KEYINP / _B_KEYSNS) に注入する (チャネル 1)。本体の画面で動いているもの (CTTY CON の COMMAND.X、Z-OS の前面のペイン、X-BASIC など) に届く |

- 端末に無い X68000 のキー (XF1〜XF5、HELP、UNDO、かな、ローマ字、コード入力、記号入力、登録、OPT.1/2、BREAK、COPY、CLR、テンキーの ENTER) は、
  `~/.config/zmux/keys.toml` の対応表で、端末から来る並びに割り当てる。既定は普段使われない F13〜F20 と Shift+F1〜F9
  (`zmux keys` で一覧と既定の表)。対応表にある並びだけがキー入力の注入になり、ほかは全部そのまま送る。
  対応表を使うのは `zmux` のタブ 0、`zmux attach 0`、`zmux kbd`。zosh のセッションではバイト列のまま。
- `zmux key 'dir\r'` は文字を X68000 のキー (スキャンコードと文字コード) にして注入する。`zmux key -n XF1 OPT1` はキーの名前で。
- 注入はキーボードの行列を直接読むプログラム (ゲームなど) には届かない。zmux.sys が横取りするのは IOCS の _B_KEYINP / _B_KEYSNS だけ
  (DOS のキー入力もここを通る)。

## 画面が CON のとき (CTTY CON)

zmux.x は常駐するときに CON デバイスを横取りする (zmux.sys 0.3 から)。COMMAND.X が CON にコンソールの入力を読みに来た後は
「コンソールは CON」とみなし、AUX を読みに来たら「AUX」に戻す (`ctty con` / `ctty aux` を打てばそのまま切り替わる)。CON の間は:

- CON に書かれた文字 (DOS の出力。COMMAND.X の表示、`dir`、`type` など) を、線が UP ならチャネル 0 にも写す。`zmux` のタブ 0 / `zmux attach 0` で見える。
  IOCS で直接描くもの (ssh.x、Z-OS のペインなど) は写らない。それは下の `zmux screen` で見る。
- タブ 0 で打った文字を、本体のキー入力 (_B_KEYSNS / _B_KEYINP) として渡す。スキャンコードは `zmux key` と同じ表 (JIS 配列)。
- 写す先の受け皿 (2KB) が一杯のときは、CON の出力が線の速さ (78125bps で約 7.8KB/s) まで待たされる (1 秒待って空かなければ捨てる)。
  線が UP でないとき (zmuxd がつながっていないとき) は何もしない。
- `zmux -m off` で写しとタブ 0 からのキー入力を止める (`-m on` で戻す。止めている間はタブ 0 から打てないので、本体のキーボードで)。
- AUX の間 (CTTY AUX) の動きはこれまでと同じ。`zmux -s` の `console` が 1 なら CON、`CON bytes mirrored` / `dropped` / `tab 0 keys` が数。

## 画面の写し (`zmux screen`)

`zmux screen -o FILE.png` は X68000 のテキスト VRAM をそのまま PNG にする (ファンクションキーの行、ssh.x や Z-OS が直接描いた画面も写る。グラフィック画面は写らない)。
`zmux screen -t` は CGROM の字と照らし合わせて文字にする (半角と、全角の 1〜8 区・第 1 水準)。

- 頼み方: チャネル 1 の新しいメッセージ IN_SCREEN (種類 3) で頼み、X68000 は新しいチャネル 4 で返す。行ごとに PackBits で詰め、ほかのチャネルの後回しで送る。
  形は `x68k/zmux.c` の `scr_start` のコメント。古い zmux.sys は IN_SCREEN を捨てるので、zmuxd は 120 秒で「返事が無い」と答える。
- 既定はプレーン 0 と 1 (Human68k の文字の色)。`-p 0123` で 4 枚。大きさは CRTMOD から (768x512 なら 96 バイト x 512 ラスタ)。`-y` / `-n` で行の範囲。
- `-t` は初回だけ X68000 から CGROM (約 140KB) を取り、`~/.cache/zmux/` に置く。
- `-t` は反転した字と強調 (太字) の字も引く。Human68k のエラーの箱 (白地に太字) も文字になる (「、」など一部の記号は `?` になる)。
- XEiJ (19200bps) で: Human68k の画面 4 枚 (192KB) が 21KB に詰まって 13 秒。78125bps ならおよそ 4 分の 1。

## XEiJ での確認

`test/e2e.py RUNDIR` (Mac、elf2x68k と cargo が要る)。zmuxd と XEiJ の間に、1 バイトあたり 0.03% (`CORRUPT`) でバイトを壊す中継を挟む。

| 項目 | 結果 |
|---|---|
| 起動から zmux.x のスレッドまで | 10 秒 |
| zmuxd なしで COMMAND.X が素の文字で答える | 答える |
| zmuxd を起動して UP | 1 秒以内 |
| `ver`、`type config.sys`、`dir` x5 (0.03% 破損) | 欠けずに届く。13 秒、送り直し 7、壊れたフレーム 8 |
| 同じことを 0.3% 破損で | 欠けずに届く。33 秒、送り直し 19、壊れたフレーム 59 |
| zmuxd を止めてから素の文字に戻るまで | 11〜14 秒 |

### 段階 2 (`test/e2e_tsp.py`、19200bps、RSDRV の受信バッファは zmux.sys が 32KB に広げる)

| 項目 | 結果 |
|---|---|
| 名前引き (localhost) | PASS、2ms |
| 1 バイトの往復 (20 回の平均) | 209ms |
| X68000 → Pi、30000 バイト (tisnet put、照合) | PASS、1.69 KB/s |
| Pi → X68000、30000 バイト (tisnet get、照合) | PASS、1.73 KB/s |
| rcmd.x (X68000 で待ち受け) に Pi から `x68 ver` | 1.1 秒 |
| x68put 30000 バイト (rcmd.x、A: へ) | 1.60 KB/s (SLIP の段階 4 は 1.49 KB/s) |
| ssh.x `-oAccel=all` で Pi (Mac) の sshd へ (0xA5 を Pi で計算) | 鍵交換とホスト鍵の検証まで 6.8 秒 (暗号の要求 4 回) |

直したこと: 未送信のバイトが残っている間は再送タイマーを止める (19200bps では 4KB の送信に約 2 秒かかり、まだ送っていないフレームまで再送していた)。
X68000 の送信は _OSNS232C を見て出すと 1 回に 1 バイトしか出ず、速さが半分になったので、_OUT232C で 128 バイトずつ出す。

### 段階 3 (`test/e2e_zss.py`、主記憶 12MB)

8 回続けて通った。`zmux attach` から zosh のプロンプトまで約 4 秒、echo、2 つのセッションを同時に、`exit 3` で `exited (3)`。

この段階で見つけて直したこと:

- **Z-OS のスレッド切り替えによる競合**: zos.x はスーパーバイザモードの処理中でもスレッドを切り替えるので、zmux.x と npd.x のスレッドと
  AUX の読み書きがシリアル回線の処理に同時に入り、受信したバイトが別々の処理に読まれてフレームが壊れた。zmux.sys に排他制御を入れた。
- **接続の確認**: Pi は KICK を頻繁に送るので確認応答だけのフレームを送らず、X68000 は番号の無いフレームを接続の確認に数えていなかったので、
  10 秒でフレームを使わないやり取りに戻っていた。どちらも、正しいフレームを受ければ接続が続いているとみなす。X68000 も 2 秒ごとに確認応答だけのフレームを送る。
- **再送タイマー**: Pi は再送タイマーの起点を「回線の送信が終わる見込みの時刻」にしていたので、KICK を送るたびに期限が延び、
  確認応答の来ないフレームを再送しなくなっていた。確認待ちのデータのフレームを送り終える時刻を起点にした。
- zmux.x のスレッドの回数を 2 から 10 に (Z-OS が動いている間、回ってくる間隔が 2.6 秒になっていた)。

### にせの X68000 での確認 (`test/e2e_fake.py`、XEiJ なし)

`test/fakex68k.py` は XEiJ の RS-232C と同じ TCP の口で zmux の線の約束 (HELLO、フレーム、CRC、Go-Back-N) を話し、COMMAND.X (ver / dir / echo)、
zos.x の 0xA7 (OPEN → ACCEPT、打った字をそのまま返す、exit)、zrmt.sys (要求を送って応答を待つ)、キーの受け口のふりをする。
1 バイト 10 ビット分の時間をかけて、19200bps の速さも真似る。`python3 test/e2e_fake.py` (pyte が要る: `python3 -m pip install pyte`)。
この機械 (クラウドの Linux) で 36 項目すべて通った:

| 項目 | 結果 |
|---|---|
| HELLO → UP、`zmux attach 0` で ver | 通る |
| リモートドライブ: init / mkdir / create + write / open + read の照合 / files + nfiles / rename / delete / rmdir / dskfre | 全部通る |
| リモートドライブの速さ (40000 バイト、19200bps の真似) | WRITE 1.82 KB/s、READ 1.82 KB/s |
| `zmux`: 下の行にタブ 0、ver が届いて描かれる、CSI u の並びがそのまま届く、F13 → XF1 の注入 | 通る |
| `zmux`: Ctrl+X ? (次のキーは送らない)、Ctrl+X Ctrl+X で Ctrl+X そのもの | 通る |
| `zmux`: Ctrl+X c / \| / - で zosh のタブとペイン、Ctrl+X k で閉じてセッションも消える、マウスでタブを選ぶ | 通る |
| `zmux`: Ctrl+X d で切り離し → 配置 `0;(v 1 2)` が zmuxd に残る → つなぎ直すとタブとスクロールバックが戻る | 通る |
| `zmux`: zosh の exit でペインが消え、下の行に知らせ。40x10 の端末でも動く | 通る |

見つけて直したこと: 描く回数を抑える判定でイベントの待ち行列を 1 つ読み捨てていて、つなぎ直したペインの直前の出力がときどき消えた。

### XEiJ での確認はまだ (`test/e2e_tui.py`、`test/e2e_rdrive.py`)

この回の作業環境 (クラウド) からは XEiJ (stdkmd.net)、Human68k (retropc.net)、elf2x68k のリリース、JDK 26 が取れず、XEiJ での試験ができなかった。
`zmux.sys` / `zrmt.sys` のコンパイルは、Ubuntu の `gcc-m68k-linux-gnu` に elf2x68k のヘッダとリンカスクリプトと `elf2x68k.py` を合わせたもので確かめただけ
(HANDOFF.md)。**`x68k/` の `zmux.sys` / `zmux.x` は前の版のまま**なので、XEiJ や実機で使う前に elf2x68k で `make -C x68k` する (`zrmt.sys` もそこでできる)。
2 つの試験は raspi5 か Mac で流す: `XEIJ_TOOLS=… HUMAN_XDF=… python3 test/e2e_rdrive.py RUN`、`ZOS_X=… ZOSH_X=… python3 test/e2e_tui.py RUN`。

### 画面が CON のときと画面の写し (`test/e2e_con.py`)

AUTOEXEC.BAT が `ZMUX -c` (CTTY AUX にしない) の起動フロッピーで、タブ 0 から `ver` / 120 文字の `echo` / 記号を打つと CON で動き、答えがタブ 0 に返る。
`zmux screen -p 0123` でほどいた 4 枚が、XEiJ のエージェントで取ったテキスト VRAM と 1 バイトも違わない。`-t` の文字に ver の答えがある。
タブ 0 から `ctty aux` → AUX で ver と echo → `ctty con` → また CON で ver。`zmux -m off` でタブ 0 に何も来ない、本体のキーボードで `-m on` で戻る。
Human68k のエラーの箱 (空のドライブ B: の「ディスクが入っていません」と、`test/buserr.c` の「バスエラーが発生しました」) が、箱がキーを待っている間に `zmux screen -t` で読め、
タブ 0 (CON のとき) か `zmux key a` (AUX のとき) で中止できる。

### キーの注入 (`test/e2e_key.py`)

タブ 0 で `ctty con` にして COMMAND.X を本体のキーボードに戻し、`zmux key 'ver\r'` で本体の画面に ver の結果が出た。
`zmux key -n XF1 HELP OPT1` で落ちず、`zmux key 'ctty aux\r'` でタブ 0 に戻った。Z-OS も _B_KEYINP を横取りするが (zos.x が後から)、
段階 3 の試験は 3 回とも通った。
