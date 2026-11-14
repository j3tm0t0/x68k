# bsrmt.sys

BlueSCSI v2 (Pico 2 W) の SD カードの中のディレクトリを、X68000 の Human68k のドライブとして読み書きするデバイスドライバ。
設計は j3tm0t0/x68k-zos#188。

- X68000 の側: `bsrmt.sys` (このディレクトリ)。Human68k のリモートドライブで、ファイル単位の要求 (OPEN、READ、FILES など) を
  SCSI ID6 (BlueSCSI の netpipe の装置、INQUIRY の製品名が `X68NP`) に送る。
- BlueSCSI の側: BlueSCSI-v2 のフォークの `lib/X68kRemote` と `src/BlueSCSI_x68rd.cpp` (GPL-3、公開していない)。
- 要求と応答の形: `x68rd_proto.h` (MIT。ファームの側にも同じものがある)。要求 1 つを LBA 0x200000 に `_S_WRITEEXT` で書き、
  応答を LBA 0x210000 から `_S_READEXT` で読む。1 回の READ / WRITE は 15872 バイトまで、FILES は 1 回で 32 件まで。

## 使い方

```
DEVICE = \SYS\BSRMT.SYS [/uN] [/iN] [/cN]
```

| 引数 | 意味 | 既定 |
|---|---|---|
| `/uN` | ドライブの数 | BlueSCSI の bluescsi.ini にあるディレクトリの数 |
| `/iN` | SCSI ID | 6 |
| `/cN` | 読み書きのキャッシュ (15872 バイト) の数、1 か 2 | 2 |

常駐するのは約 75KB (`/c1` で約 59KB)。組み込むとき、ID の INQUIRY が `X68NP` で始まらないか、ファームが INIT に答えない
(リモートドライブの無いファーム、`X68kDrive=1` が無い) ときは組み込まない。

ドライブ名は CONFIG.SYS で前に組み込まれたドライブの後ろに付く。XVI を SCSI から起動すると A: が ID0、B: と C: が
フロッピーなので、ID0 だけなら D: から。

bluescsi.ini の `[SCSI]`:

```
X68kNetpipe=1
X68kDrive=1
X68kDrive0=/x68k         ; ユニット 0 のディレクトリ (既定 /x68k)
X68kDrive1=/home         ; ユニット 1。最大 4 つ
X68kDriveRO=0            ; 1 ですべて読み出し専用。ユニットごとは X68kDrive1RO=1
```

## 中身

`net/zmux/x68k/zrmt.c` (j3tm0t0/x68k#79 の直しの後) と同じ処理で、Pi との交換の代わりに SCSI を使う。元は yunkya2 氏の
x68kserremote (MIT) の `driver/remotedrv.c`。

- 小さな READ / WRITE はキャッシュ (dcache) でまとめる。キャッシュを送るときに失敗したら CLOSE / SEEK / READ / WRITE /
  FILEDATE がエラーを返す (ディスクがいっぱいのときに COPY が成功で終わらない)。
- FILES は 1 回で最大 32 件を受け取り、残りを NFILES で返す (remotedrv.c の fcache と同じ)。
- 要求の見出しに通し番号 (seq) と X68000 の時計を入れる。ファームはファイルの時刻にこの時計を使う。
- SCSI のエラーのときは同じ seq で 1 回だけ送り直す (ファームは同じ seq の要求を 2 回実行せず、前の応答を返す)。
  だめなら -15 (ドライブ指定が無効) で失敗する。

## 試験

- `make` (elf2x68k)。
- `make -C harness/picosim test-bsrmt`: XEiJ + picosim (`-X ディレクトリ`)。picosim の `x68rd.c` が同じ形の要求を受け、
  中身は remoteserv.c (x68kserremote) で Mac のディレクトリを読み書きする。DIR、TYPE、200000 バイトの COPY (両方向)、
  50 件の DIR、MKDIR、REN、DEL を確かめる。
- ファームの側の試験は BlueSCSI-v2 のフォークの `lib/X68kRemote/tests/x68rd_test.sh` (SdFat で FAT16 / FAT32 / exFAT の
  カードイメージ)。
