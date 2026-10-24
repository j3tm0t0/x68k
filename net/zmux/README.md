# net/zmux

X68000 (Compact XVI) と Raspberry Pi の間の RS-232C 1 本で、コンソール・TCP (TSP)・暗号アクセラレータ・zosh のセッションを
多重化する (#23)。設計は [DESIGN.md](DESIGN.md)。

いまできているのは段階 1 (線とコンソール)。

| 置き場所 | 中身 |
|---|---|
| `src/` | Pi 側 (Rust): `zmux daemon` (zmuxd)、`zmux attach 0`、`zmux status` |
| `x68k/` | X68000 側 (elf2x68k): `zmux.sys` (線の処理と、CTTY AUX をチャネル 0 にする AUX デバイス)、`zmux.x` (線を回すスレッド) |
| `test/e2e.py` | XEiJ で、素の文字 → zmux → 素の文字の切り替えと、バイトを壊す線での再送を試す |

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
