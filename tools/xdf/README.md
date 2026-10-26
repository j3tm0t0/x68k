# tools/xdf -- 2HD フロッピーと XDF イメージを X68000 で読み書きする

PC の 3 モードのフロッピードライブが無くても、X68000 自身にイメージ (XDF、1,261,568 バイト) をフロッピーへ書かせる道具。
イメージはリモートドライブ (XVI では zmux の C:) などに置いて、フロッピー 2 台の機械の B: に書く。

```
xdfwrite -s b:                    B: の状態を出す (ディスクの有無、書き込み禁止)
xdfwrite -f c:\boot26.xdf b:      B: を 2HD (1024 バイト x 8 セクタ) に初期化してから書き、全部読み返して比べる
xdfwrite c:\boot26.xdf b:         初期化済みならそのまま書く。2HD として読めなければ先に初期化する
xdfread b: c:\backup.xdf          B: を丸ごと読んでファイルにする (A: も読める)
```

- 2HD は 77 シリンダ x 2 ヘッド x 8 セクタ x 1024 バイト。IOCS の `_B_FORMAT` / `_B_WRITE` / `_B_READ` を 1 トラック (8KB) ずつ呼び、
  シリンダごとに進み具合を出す。最後に `OK` か、`NG` と最初に違ったセクタ (C/H/R とイメージの何セクタ目か) を出す。
- 読み返しは、書くときに取ったセクタごとの CRC32 と比べる。イメージのファイルは 1 度だけ読む (リモートドライブが遅いため)。
- DOS を通さないので、ディスクが無いときや書き込み禁止のときに Human68k のエラーの窓 (中止/再実行/無視) は出ない。
  始める前に IOCS `_B_DRVCHK` でディスクの有無と書き込み禁止を確かめ、駄目なら理由を出して終わる。
- A: は起動中のディスクなので、`xdfwrite` は `--force` が無いと断る。`xdfread` は A: も読める (読むだけ)。
- `xdfread` は書き出し先のファイルが既にあれば断る。読めないセクタは 0 で埋めて `bad sector C.. H.. R..` と出し、最後に `NG` にする。
- 書いた後のディスクを DOS から使う前に、いったん取り出して入れ直す (Human68k が前のディスクの FAT を覚えていることがある)。
- `-f` が無くても、最初と最後のトラックが 2HD (1024 バイト x 8 セクタ) として読めなければ、`drive B: is not X68000 2HD ...; formatting it first` と出して初期化する。
  市販の「フォーマット済み」フロッピーは DOS/V 用の 1.44MB (512 バイト x 18 セクタ) なので、この場合にあたる。
  途中のトラックだけ形が違って書けないときは `NG: write failed` と `-f` を付けて試すよう出す。
- ドライブ名はフロッピーの装置番号にそのまま対応させる (A: = 0、B: = 1)。フロッピーから起動した機械の並び。
  ハードディスクから起動した機械ではずれるので使わない。

## xdffs.py (ホストで XDF の中を扱う)

```
xdffs.py ls IMG                         全ファイルを一覧にする
xdffs.py extract IMG DIR                全ファイルを取り出す
xdffs.py build OUT BASE [--merge IMG] [--add HOSTDIR] [--rm PATH] ...
```

`build` は BASE の起動セクタ (IPL と BPB) をそのまま使い、中身に操作を順に重ねて、FAT とディレクトリを新しく書く。
HUMAN.SYS をクラスタ 2 から置き (IPL が読む)、どのファイルも連続したクラスタに置く。属性と日時は元のまま (`--add` はホストの日時)。
BPB から形を読むので、2HQ (1440KB) のイメージも扱える。

## 試験

`tests/xeij_xdf.py RUNDIR` が XEiJ (Compact、主記憶 2MB、フロッピー起動) を 6 回起動して試す (約 5 分。`ONLY=dosv` のように名前を指定すると、その試験だけ)。
C: (装置 2) に乱数の XDF を入れた 2HQ、D: (装置 3) に空の 2HQ を入れ、XVI のリモートドライブの代わりにする。

| 試験 | B: | 確かめること |
|---|---|---|
| refuse | 0 埋め | `-s` の表示、A: を断る、大きさ違いを断る、`xdfread` が既存のファイルを断る、B: は変わらない |
| format | 0 埋め | `-f` で初期化・書き込み・読み返しが OK、`xdfread` で読んだものとイメージが一致 |
| overwrite | HUMAN302 | 初期化なしで書いて OK、B: のイメージがバイト単位で一致 |
| dosv | DOS/V の 1.44MB (空) | `-f` 無しでも 2HD でないと気づいて初期化し、書いて OK。B: と読み返しが一致 |
| protected | HUMAN302 (書き込み禁止) | `write protected` で断り、B: は変わらない。`xdfread` は読める |
| empty | 無し | 書き込みも読み込みも `no disk` で終わる (止まらない)、ファイルを作らない |

`_B_DRVCHK` の値 (入っている 02、書き込み禁止 0a、無い 0c) と `_B_FORMAT` は XEiJ で確かめたもので、実機では未確認。
XEiJ は HUMAN302.XDF を読み込むとき、消されたファイル 2 つ (OPMDRV3.X、DUMP.X) を戻すので、書き込み禁止の試験には乱数のイメージを使う。
実機ではまず `xdfwrite -s b:` で値を見る。

## XVI の起動フロッピー (xvi/mkboot26.sh)

`xvi/mkboot26.sh HUMAN302.XDF XVIA.XDF BOOT26.XDF` が、Compact XVI の新しい起動フロッピーのイメージを作る。

- 起動セクタ (IPL)、ボリューム名、HUMAN.SYS、COMMAND.X は HUMAN302.XDF (シャープが無償公開した Human68k 3.02 のシステムディスク) から取る。
- `XVIA.XDF` は XVI の今の A: を `xdfread a: c:\xvia.xdf` で読んだもの。CONFIG.SYS、AUTOEXEC.BAT と、そこから使う SYS・BIN・ETC・HOME のファイルを取る。
- 入れないもの: A: にあった控え (`*.old`、`*.z6`、`AUTOEXEC.ORG` など)、試験の記録 (`snap*.txt`、`*.out`)、ベンチマーク (`cpuspd.x` など)、
  CONFIG.SYS と AUTOEXEC.BAT が使っていない古い版 (`zmux.sys`、`ZMUX7.SYS`、`contee.sys`、`BIN\zmux.x`、`BIN\RX.X`、`BIN\ZOSHR.X`)。
- HUMAN302 の BIN から小さな道具 (FORMAT.X、CHKDSK.X、COPYALL.X、SYS.X など) と、xdfwrite.x / xdfread.x を足す。空きは 62KB。

`xvi/test_boot26.py RUNDIR BOOT26.XDF XVIA.XDF` が XVI と同じ手順を XEiJ で試す。DOS/V の 1.44MB の空ディスクを B: に入れて
`xdfwrite c:\BOOT26.XDF b:` (自動で初期化) と `chkdsk b:` を打ち、書いたディスクがイメージと一致するかを見る。そのあと、書いたディスクから起動した画面と
元の A: (XVIA.XDF) から起動した画面が同じかを比べる (CONFIG.SYS の常駐と AUTOEXEC.BAT の `RX`、`ZMUX7`、`TIS -p`、`NPD -m` の表示まで。
RS-232C の相手が無いので、その先は比べない)。

`tests/xeij_boot.py RUNDIR XDF [秒]` は、起動フロッピーを XEiJ で起動して指定の秒数あとの画面の文字を出す。
