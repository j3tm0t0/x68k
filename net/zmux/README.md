# net/zmux

X68000 (Compact XVI) と Raspberry Pi の間の RS-232C 1 本で、コンソール、TCP 通信 (TSP: TCP の処理を相手側に任せる tis.x -p の要求)、
暗号アクセラレータ、Z-OS のシェル zosh のセッションを多重化する (#23)。設計は [DESIGN.md](DESIGN.md)。

実装済みなのは、段階 1 (シリアル回線とコンソール)、段階 2 (チャネル 1 で `tis.x -p` のソケット要求と暗号の要求を Pi 側で処理)、
段階 3 (Z-OS の画面に出ないペインで動く zosh に `zmux attach` でつなぐ)、キー入力の注入 (チャネル 3)。

| 置き場所 | 中身 |
|---|---|
| `src/` | Pi 側 (Rust): `zmux daemon` (zmuxd)、`zmux attach 0`、`zmux status`。`link.rs` (シリアル回線のフレームと再送)、`netpipe.rs` (npd.x の要求・応答の転送)、`tsp.rs` (TSP の要求を Pi のソケットで処理)、`zss.rs` (zosh のセッション)、`accel.rs` (暗号)、`keys.rs` (キー) |
| `x68k/` | X68000 側 (elf2x68k): `zmux.sys` (線の処理と、CTTY AUX をチャネル 0 にする AUX デバイス)、`zmux.x` (線を回すスレッド) |
| `test/e2e.py` | XEiJ で、素の文字 → zmux → 素の文字の切り替えと、バイトを壊す線での再送を試す |
| `test/e2e_key.py` | XEiJ で、Pi から注入したキー (`zmux key`) が本体のキーボード入力として COMMAND.X に届くかを試す |
| `test/e2e_zss.py` | XEiJ (主記憶 12MB) で、zos.x を起動して `zmux attach` で zosh のセッションを開き、echo、2 つ同時、exit の終了コードを試す |
| `test/e2e_tsp.py` | XEiJ で、tis.x -p + npd.x -m のソケット (名前引き、往復、put / get の照合、rcmd.x の待ち受けと x68put) を試す |
| `../npd/npd.c` | `npd.x -m`: SCSI ID 6 の代わりに zmux のチャネル 1 で交換する |

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
- `zmux attach 0` にはホットキーも前置キーも無い。切り離しやタブは herdr / tmux で行う。
  zmuxd はチャネル 0 の直近 64KB を持っていて、つないだときに流す。

## 段階 2: TCP (TSP)

X68000 で tis.x -p (TCP / UDP / DNS を相手に任せる xip.x の代わり) と `npd.x -m` を動かすと、ソケット操作の要求がチャネル 1 で zmuxd に届き、
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

## キー

`zmux attach` にはホットキーも前置キーも自前のエスケープも無い。端末を raw にして、打ったバイトをそのまま送る。
タブやペインの切り替え・切り離しは herdr / tmux の役目 (moto の設定では前置キー Ctrl+X)。

| 何に打つか | 届き方 |
|---|---|
| `zmux attach 0` (タブ 0、COMMAND.X の CTTY AUX) | バイト列のまま COMMAND.X の AUX の入力へ。Ctrl+C なども 1 バイトとして届く |
| `zmux attach` / `zmux attach N` (zosh のセッション) | バイト列のまま。zos.x が矢印などの並び (ESC [ A …) を X68000 のキーに直す |
| `zmux kbd` / `zmux key` (本体のキーボード) | X68000 のキー入力 (IOCS _B_KEYINP / _B_KEYSNS) に注入する (チャネル 3)。本体の画面で動いているもの (CTTY CON の COMMAND.X、Z-OS の前面のペイン、X-BASIC など) に届く |

- 端末に無い X68000 のキー (XF1〜XF5、HELP、UNDO、かな、ローマ字、コード入力、記号入力、登録、OPT.1/2、BREAK、COPY、CLR、テンキーの ENTER) は、
  `~/.config/zmux/keys.toml` の対応表で、端末から来る並びに割り当てる。既定は普段使われない F13〜F20 と Shift+F1〜F9
  (`zmux keys` で一覧と既定の表)。対応表にある並びだけがキー入力の注入になり、ほかは全部そのまま送る。
  対応表を使うのは `zmux attach 0` と `zmux kbd`。zosh のセッションではバイト列のまま。
- `zmux key 'dir\r'` は文字を X68000 のキー (スキャンコードと文字コード) にして注入する。`zmux key -n XF1 OPT1` はキーの名前で。
- 注入はキーボードの行列を直接読むプログラム (ゲームなど) には届かない。zmux.sys が横取りするのは IOCS の _B_KEYINP / _B_KEYSNS だけ
  (DOS のキー入力もここを通る)。

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

### キーの注入 (`test/e2e_key.py`)

タブ 0 で `ctty con` にして COMMAND.X を本体のキーボードに戻し、`zmux key 'ver\r'` で本体の画面に ver の結果が出た。
`zmux key -n XF1 HELP OPT1` で落ちず、`zmux key 'ctty aux\r'` でタブ 0 に戻った。Z-OS も _B_KEYINP を横取りするが (zos.x が後から)、
段階 3 の試験は 3 回とも通った。
