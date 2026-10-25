# net/zmux

X68000 (Compact XVI) と Raspberry Pi の間の RS-232C 1 本で、コンソール・TCP (TSP)・暗号アクセラレータ・zosh のセッションを
多重化する (#23)。設計は [DESIGN.md](DESIGN.md)。

いまできているのは段階 1 (線とコンソール)、段階 2 (チャネル 1: tis.x -p のソケット要求を Pi のソケットで答える)、
段階 3 (Z-OS の見えないペインの zosh を `zmux attach` で使う)。

| 置き場所 | 中身 |
|---|---|
| `src/` | Pi 側 (Rust): `zmux daemon` (zmuxd)、`zmux attach 0`、`zmux status`。`link.rs` (線)、`netpipe.rs` (交換)、`tsp.rs` (TSP を普通のソケットで) |
| `x68k/` | X68000 側 (elf2x68k): `zmux.sys` (線の処理と、CTTY AUX をチャネル 0 にする AUX デバイス)、`zmux.x` (線を回すスレッド) |
| `test/e2e.py` | XEiJ で、素の文字 → zmux → 素の文字の切り替えと、バイトを壊す線での再送を試す |
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
- zmuxd が動くと、X68000 の HELLO に答えてフレームの状態になる。zmuxd を止めると、X68000 は 10 秒で素の文字に戻る。
- `zmux attach 0` にはホットキーも前置キーも無い。切り離しやタブは herdr / tmux で行う。
  zmuxd はチャネル 0 の直近 64KB を持っていて、つないだときに流す。

## 段階 2: TCP (TSP)

X68000 で tis.x -p (TCP / UDP / DNS を相手に任せる xip.x の代わり) と `npd.x -m` を動かすと、ソケットの要求がチャネル 1 で zmuxd に届き、
zmuxd が Pi の普通のソケットで答える。線には IP も TCP の ACK も乗らない。

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
- npd.x -m は送るものが無いときは交換しない。zmuxd に X68000 へ渡すものができると、チャネル 15 の KICK で交換を促す。

## 段階 3: zosh のセッション (0xA7)

X68000 で zos.x (Z-OS、sshd の Z 側が入った版) が動いていると、`zmux attach` で新しい zosh のセッションが開く。zmuxd が Pico の zsshd の
代わりに 0xA7 の OPEN を送り、zos.x が見えないペインで zosh を動かす。出力は UTF-8 で届く (zos.x が Shift_JIS から変える)。

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
- zos.x は x68k-zos の `sshz` (Foxtrot の PR #45) の版を使った。主記憶 2MB では zos.x が動かない (起動フロッピーの README)。

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

直したこと: 送り直しの時計を、線に出していないバイトがある間は止める (19200bps で 4KB は 2 秒かかり、出してもいないフレームを送り直していた)。
X68000 の送信は _OSNS232C を見て出すと 1 回に 1 バイトしか出ず、速さが半分になったので、_OUT232C で 128 バイトずつ出す。

### 段階 3 (`test/e2e_zss.py`、主記憶 12MB)

8 回続けて通った。`zmux attach` から zosh のプロンプトまで約 4 秒、echo、2 つのセッションを同時に、`exit 3` で `exited (3)`。

この段階で見つけて直したこと:

- **Z-OS の横取りの切り替え**: zos.x はスーパーバイザの途中でもスレッドを切り替えるので、zmux.x のスレッドと npd.x のスレッドと
  AUX の読み書きが線の処理の中で重なり、受信のバイトが二つに分かれてフレームが壊れた。zmux.sys に排他を入れた。
- **生存の確認**: Pi は KICK を頻繁に送るので確認だけのフレームを送らず、X68000 は番号の無いフレームを生存の印にしていなかったので、
  10 秒で素の文字に戻っていた。どちらも、正しいフレームなら何でも生存の印にする。X68000 も 2 秒ごとに確認だけのフレームを出す。
- **送り直しの時計**: Pi の送り直しの時計を「線を送り終える見込み」から数えていたので、KICK を送るたびに時計が延び、
  確認の来ないフレームを送り直さなくなっていた。確認待ちのデータのフレームを送り終える時刻から数えるようにした。
- zmux.x のスレッドの回数を 2 から 10 に (Z-OS が動いている間、回ってくる間隔が 2.6 秒になっていた)。
