# boot/serial/floppy: 実機 X68000 の起動フロッピー

Human68k 3.02 の起動に要るものと、RS-232C (rx.x / CTTY AUX)、SLIP、Z-OS (zos.x と zosh.x)、ssh.x を 1 枚の 2HD フロッピー
(XDF 形式、1232KB) にまとめる。システムフロッピーの代わりにこれで起動すると、コンソールが RS-232C になり、
キーボードを残せば zosh と ssh.x がすぐ使える。

```
cd boot/serial && ./getnet.sh                     # files-net/ (xip.x、slip.sys、slipd.x、rcmd.x など)
ZOS_X=…/x68k-zos/kernel/zos.x ZOSH_X=…/x68k-zos/shell/src/zosh.x \
ZRAM_SYS=…/x68k-zos/tools/gccspeed/zram.sys floppy/mkboot.sh
```

Z-OS (j3tm0t0/x68k-zos) は非公開なので、組んだ zos.x と zosh.x の場所を渡す。ssh.x は指定が無ければ j3tm0t0/x68k のリリース
v2026.10.23.1 から取る (SHA256SUMS で確かめる)。できたイメージは `floppy/out/` に置く。

## 2 種類のイメージ

| イメージ | 主記憶 | 違い | 空き |
|---|---|---|---|
| `x68k-boot-2m.xdf` | 2MB | RAM ディスクなし。`zos` は ZOS.BAT で `-m 640` (ペインあたり 640KB) | 216KB |
| `x68k-boot-12m.xdf` | 8MB 以上 | CONFIG.SYS で zram.sys の RAM ディスク (3MB) を作り、AUTOEXEC.BAT が zosh.x・ssh.x・slipd.x・rx.x を写して PATH の先頭にする | 215KB |

Human68k の CONFIG.SYS には主記憶の大きさで分ける書き方が無いので、イメージを 2 つ作る。

## 中身

| 場所 | ファイル |
|---|---|
| `A:\` | HUMAN.SYS、COMMAND.X、CONFIG.SYS、AUTOEXEC.BAT、STARTUP.ENV、ZOS.BAT、NETUP.BAT |
| `A:\SYS` | RSDRV.SYS、SLIP.SYS、(ZRAM.SYS) |
| `A:\BIN` | RX.X、ZOS.X、ZOSH.X、SSH.X、XIP.X、IFCONFIG.X、INETDCONF.X、SLIPD.X、RCMD.X |
| `A:\ETC` | ZOSHRC、hosts、networks、protocols、services |
| `A:\HOME` | ZOSH.RC (`HOME=A:/home`) |
| `A:\TMP` | 一時ファイル (`temp`) |

- CONFIG.SYS: `PROCESS = 16 10 20` (Z-OS が使う Human68k のスレッド)、RSDRV.SYS、SLIP.SYS、12m 版は `DEVICE = \SYS\ZRAM.SYS 3072`。
- AUTOEXEC.BAT の最後は boot/serial の README と同じ `RX -B19200 -K2` と `CTTY AUX`。2 秒以内にキーを押すとキーボードのまま使える。
- `zos` で Z-OS (zosh のペインが 1 つ)。終えるのは OPT.1 + Q。`zosh` で zosh だけを起動することもできる。
- `NETUP` で SLIP と rcmd.x を起動する (CTTY AUX のときは使えない。同じ RS-232C を使うため)。

## RAM ディスクの大きさ

zram.sys (x68k-zos の `tools/gccspeed`、Golf 作) は `DEVICE = \SYS\ZRAM.SYS 大きさ(KB)` で、主記憶の上の端から取る。
1 セクタ 1024 バイトで、起動のたびに空になる。ドライブ名は内蔵のフロッピーの次 (Compact XVI のように 2 台なら C:)。
1 台の機械では B: になるので、`ZRAM_DRIVE=B floppy/mkboot.sh` で作る。

主記憶 12MB の Compact XVI での目安:

| 使い道 | 大きさ |
|---|---|
| Human68k、RSDRV.SYS などの常駐 | 約 0.3MB |
| zos.x (プログラムと領域で約 1.5MB) | 1.5MB |
| Z-OS のペイン | 既定 2.5MB ずつ (`zos -m KB` で変える) |
| RAM ディスク (`ZRAM_KB`、既定 3072) | 3MB (写す道具は約 470KB) |

3MB の RAM ディスクなら、残りでペインを 2〜3 つ開ける。gcc を RAM ディスクで動かすなら 6MB 程度にして、ペインを `-m 1500` に減らす。

## XEiJ での確認

`test/floppy_test.py RUNDIR XDF 主記憶MB [コマンド]` が、フロッピーだけで XEiJ (Compact) を起動し、2 秒の間にキーを押してキーボードを残し、
コマンド (既定 `zos`) で zosh のプロンプトが出ることと、`ssh -V` で `Dropbear v2022.83` が出ることを確かめる。

| イメージ | 主記憶 | コマンド | 結果 |
|---|---|---|---|
| x68k-boot-12m.xdf | 12MB | `zos` | 起動から RX の 2 秒待ちまで 15 秒、`zos` から zosh のプロンプトまで 7 秒、`ssh -V` が出る |
| x68k-boot-2m.xdf | 2MB | `zosh` | 起動から 11 秒、zosh のプロンプトまで 3 秒、`ssh -V` が出る |
| x68k-boot-2m.xdf | 2MB | `zos` | **ペインが起動しない** (zos.x の領域が 1.24MB あり、ペインのスレッドを作れない。`zos -v` の記録で thread -1) |

主記憶 2MB では Z-OS (zos.x) のペインは動かない。zosh と ssh.x は単独で動く。

## vim と gcc (2 枚目、まだ作っていない)

vim.x と gcc (真里子版 1.42 + HAS + HLK + ライブラリ) は 1 枚目に入らないので、2 枚目 (B: に入れる) にする予定。
x68kcli のイメージの `/usr/bin/vim.x` と実行時ファイル、`/usr/bin/gcc*.x` `has.x` `hlk.x`、`/usr/lib` `/usr/include` から選んで入れ、
2 枚目の AUTOEXEC は無いので、1 枚目の AUTOEXEC.BAT に `PATH` と `include` / `lib` の設定を足す。
gcc を 2MB の機械で動かすのは難しいので、12MB の機械向けにする。

## 実機のフロッピーに書く

XDF は 1 トラック 8 セクタ x 1024 バイトの 2HD で、PC で書くには 3 モードのフロッピードライブが要る。
3 モードのドライブが無くても、X68000 自身に書かせられる (Compact XVI はドライブが 2 台ある)。

1. システムフロッピー (複製) で起動し、bootstrap (段階 1〜3) を済ませる。
2. B: に空のフロッピーを入れ、X68000 で `format b:` (システムディスクの FORMAT.X、2HD) を実行する。
3. Pi から書く。rx.x が 3 セクタずつ受けて DOS の _DISKWRT で B: に直接書き、最後に全部読み返して CRC で確かめる。

```
sx run 'rx -m'                                  # (任意) rx.x が動いているかの確認
sx writefd x68k-boot-12m.xdf B:
```

XEiJ では 19200bps で、書き込み 674 秒、読み返しの確認 23 秒 (合わせて約 12 分) だった。
