# HANDOFF: Pico 2 W ファームと SD カード (Alpha / x68k-tistack-2)

2026-10-24 時点。セッションが切れても続けられるように、状態・作り方・試し方・残りをここに置く。
秘密 (Wi-Fi のパスワード、SMB の資格情報) はここにもリポジトリにも書かない。
**機材の配置 (番地・基板・試験台・手順の細かいところ) は非公開のメモにある** (x68k-zos `docs/lab-alpha.md`)。

## 今の状態

- パッチ 0001-0049 (`patches/`、説明は README の表)。リリース v2026.10.23 はファーム 0001-0043。
  v2026.10.23.1 (0001-0048) は実機の Z での確認が通り、机の上の試験台の結果を待って公開する (x68k #1)
- SD カード: HDS (`sd:/...` を HDS の欄に、明示したときだけ。SCSI ID が増えるのでリモートドライブの文字がずれる)、
  リモートドライブのユニット (REMOTEn = `sd:/`、B: の次なら C:、ずれない)、UDP 6869 の `sdcopy`
- ホスト名 (既定 `pico-` + ボード ID の下 4 桁) と固定 IP (既定は DHCP)。Pico Z Loader (Charlie) と同じ形 (項目名・NET1 の並び・返事の `host=`)
- 0049 (FAT の鎖の長さ) は PR #6 で試験待ち (x68k #5)

## 決まりごと

- 仕事は GitHub Project「X68000 Z」(https://github.com/users/j3tm0t0/projects/2) の issue で。私 (Alpha) の分: x68k #1、#4、#5。
  一覧: `GH_TOKEN=$(gh auth token -u j3tm0t0) gh project item-list 2 --owner j3tm0t0`。新しい仕事は issue を立てて project 2 に入れ、担当・分野・優先度・Status を付ける。
  人に見せられないもの (上流に伝えていない不具合、機材) は x68k-zos (#14 の下)、公開してよいものだけ x68k
- 変更はブランチと PR ("Fixes #N")。試験の結果を PR に書き、通ったら自分でマージ。小さな直しと HANDOFF / CHANGELOG は main に直接でよい。
  公開の x68k の PR に上流の未報告の不具合や機材のことを書かない
- 使う人に見える変更 (fork のパッチ、npd / tis / ssh) を入れたら `CHANGELOG.md` の「未公開 (次の版に入るもの)」に 1 行足す。リリースのときにその行を新しい版の見出しの下へ移す

## ブランチとコミット

- fork (ローカルのみ、push しない): x68kzremotedrv の作業木、ブランチ `sddata`
  - 0b1063c まで = パッチ 0001-0035、以降 1 コミット = 1 パッチ (0036 483429f … 0049 8680cea)
  - ブランチ `bisect` (PZL の Z が止まる件の切り分け用、パッチにしない)
- パッチを作り直すとき: fork で `git format-patch --start-number 36 0b1063c..sddata -o <x68k>/net/x68kzremotedrv-netpipe/patches/`

## 作り方

要るもの (fork のサブモジュールは古いので別に用意): pico-sdk 2.1.1 (bddd20f)、raspberrypi/FreeRTOS-Kernel 4f7299d (RP2350 / RP2040 の port)、
no-OS-FatFS-SD-SDIO-SPI-RPi-Pico 6c644cf (`lib/FatFs_SD`)、arm-gnu-toolchain 14.3。

```sh
mkdir build-p2w && cd build-p2w
CFLAGS="-include time.h -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion" \
  cmake -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350 -DPICO_SDK_PATH=<pico-sdk> -DFREERTOS_KERNEL_PATH=<FreeRTOS-Kernel> -DHID_KEYBOARD=ON ..
make -j8
```

- Pico W: `-DPICO_BOARD=pico_w -DPICO_PLATFORM=rp2040 -DTSP=OFF`、SD は `-DSD_CARD=ON` のときだけ (sdcopy と起動時の fetch は入らない)
- `-DPICO_PLATFORM=...` を付けないと、最初の configure が rp2040 で止まることがある
- 調べるとき: `-DMSC_LOG=ON` (USB の命令を UDP 6870 へ)、`virtual_disk.c` の `debuglevel = 1`、`sdfs.c` の `SDFS_LOG 1`、`-DPICO_DEBUG_MALLOC=1`

## 試し方 (要点)

- 実機の Z は排他 (zlock)。電源を切る前に team-lead に知らせる。Z の中の Pico をリセットしたら Z の電源も入れ直す (USB を数え直さない)
- Z の操作はシリアルのコンソール (CTTY AUX) に頼らない (Z では止まることがある)。netcon のあと rcmd で動かし、画面はキャプチャで見る。止まったかは rcmd / ping が答えるかで判断する
- 確かめること: 起動 (SMB を止めて起動待ち画面から起動する場合も)、B: / C: (SD) / A: の間のコピーを元と比べる、netcon と ping、sdcopy

## 分かっている問題・制約

- C: のファイルの日付は変えられない (FatFs の `FF_USE_CHMOD` が 0)。コピーしたファイルは書いた時刻。空き容量の表示は Human68k の上限 2GB
- `my_rtc.c` (FatFs の lib) が `time()` を置き換えている。0037 で always-on タイマーを起動時に 0 から動かし、SMB の時刻で 1 回合わせる
- sdcopy の「HDS として開いているファイルには写さない」は実機で試していない
- 設定画面は SMB の共有の一覧から選ぶ作り。`sd:` は ENUM の最後に足している (共有が 16 個あると出ない)
- Pico W の SD 入りビルドは実機で未試験 (x68k #4)。起動時の空きヒープ: SD なし 112,136 B、SD あり 107,872 B
- tis.x: npd.x が回ってこない間も接続は切らない (00cc0a6)。Z-OS の中で npd.x が回らないのは Human68k の時分割の割り込みの当たり方による (Bravo / Foxtrot が直す案)
- ID1 の HDS を B: をずらさずに使う案 (Delta): リモートドライブの起動部を ID1 より小さい ID で見せ、運びは ID6 のまま (harness/picosim/README.md「ドライブ名」)

## 次にやること

1. v2026.10.23.1 を公開 (試験台の表が通ったら)。CHANGELOG の行を 2026.10.23.1 の節へ (x68k #1)
2. PR #6 (0049) を試験台の FAT の鎖の検査のあとマージ (x68k #5)
3. Pico W の SD 入りビルドを実機で (x68k #4、予備の Pico W を挿してから)
4. Z-OS の Timer-D の直しが入ったら zostest を流し直し、npd.x の交換の間の長さを測って Bravo / Foxtrot に
