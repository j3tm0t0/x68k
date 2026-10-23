# HANDOFF: Pico 2 W ファームと SD カード (Alpha / x68k-tistack-2)

2026-10-23 20:15 時点。セッションが切れても続けられるように、状態・作り方・試し方・残りをここに置く。
秘密 (Wi-Fi のパスワード、SMB の資格情報) はここにもリポジトリにも書かない。

## 今の状態

| もの | 状態 |
|---|---|
| Z の中の sd 基板 (Pico 2 WH + microSD、192.168.1.4、ホスト名 pico-f350) | fork sddata 0bb206a まで (0036-0039) を build-p2w で作ったもの (uf2 c8de9191) を 20:00 に書き込み済み。SD は明示したときだけ |
| sd 基板の設定 (20:03 に設定画面で変更) | **RMTUNIT 1 → 2、REMOTE1 (空) → `sd:/`**。ほかは触っていない (Wi-Fi / SMB / HDS0 = x68000z/x68kdev.hds / REMOTE0 = x68000z/HFS/ / TZ JST-9 / TADJUST 2 / FASTCONN 0 / ネットワークの欄は空 = DHCP)。戻すには設定画面で RMTUNIT を 1 にする (REMOTE1 は残っていても使われない) |
| Z のドライブ | A: = HDS0、B: = SMB のリモートドライブ、**C: = SD カード**、D: E: = 内蔵 FD。REMOTE1 を外すと FD が C: D: に戻る (実機で確認) |
| パッチ | 0036 (SD の HDS、明示のみ)、0037 (SD をリモートドライブのユニットに + sdcopy)、0038 (Pico 2 W のビルド)、0039 (ホスト名と固定 IP。ホスト名は実機で確認、固定 IP は未試験)。main にマージ済み (6c661cb) |

実機で確かめたこと (自動で C: にする版): C: で dir / mkdir / ren / del / rmdir、B:→C:→B: のコピーが cmp で一致 (400KB)、
`sdcopy` で 64MB を約 86 秒、2 本目は `err sdcopy busy`、無いファイルは `err:smb-open`。
段階 1 (HDS1 = `sd:/x68000z/hds1.hds` で SCSI ID 1) は動くが、**B: を取ってリモートドライブが C: にずれる**ので明示したときだけ。

## ブランチとコミット

- fork (ローカルのみ、push しない): `/Users/moto/Dropbox/src/github.com/yunkya2/x68kzremotedrv-tsp`、ブランチ `sddata`
  - 0b1063c まで = `tsp` (パッチ 0001-0035 と同じ)
  - 483429f = 0036、369cda6 = 0037、ac0b102 = 0038、0bb206a = 0039、f6261a2 = 0040 (host= を返事の最初の語の直後に)、b467217 = 0041 (Wi-Fi の一覧を 10 秒で打ち切る)、4 つ目 = 0042 (SETCONFIG のつなぎ直しで SMB を外して開き直す、未試験、uf2 d94004ac)。0036-0041 の uf2 291c7c38 は scratchpad/p2w/release-291c7c38/ と raspi3:~/fw-fork-291c7c38.elf。Charlie の PZL の試験のあと、sd 基板にはこれが戻る
  - サブモジュールの dir (libsmb2, x68kserremote, pico-sdk, FreeRTOS-Kernel) は `../x68kzremotedrv-hid` への symlink。`lib/FatFs_SD` は no-OS-FatFS 6c644cf の clone への symlink
- x68k: `/Users/moto/Dropbox/src/github.com/j3tm0t0/x68k-tistack` (ブランチ `tistack`)。パッチは `net/x68kzremotedrv-netpipe/patches/`、説明は README の表
- パッチを作り直すとき: fork で `git format-patch --start-number 36 0b1063c..sddata -o <x68k>/net/x68kzremotedrv-netpipe/patches/`

## 作り方 (Pico 2 W)

要るもの (fork のサブモジュールは古いので別に用意): pico-sdk 2.1.1 (bddd20f)、raspberrypi/FreeRTOS-Kernel 4f7299d (RP2350 の port)、
no-OS-FatFS-SD-SDIO-SPI-RPi-Pico 6c644cf (`lib/FatFs_SD`)、arm-gnu-toolchain 14.3。今は session の scratchpad の `p2w/` にある (消えたら clone し直す)。

```sh
cd x68kzremotedrv-tsp && mkdir build-p2w && cd build-p2w
export PATH=~/opt/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin:$PATH CMAKE_POLICY_VERSION_MINIMUM=3.5
CFLAGS="-include time.h -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion" \
  cmake -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350 -DPICO_SDK_PATH=<pico-sdk> -DFREERTOS_KERNEL_PATH=<FreeRTOS-Kernel> -DHID_KEYBOARD=ON ..
make -j8      # x68kzremotedrv.elf / .uf2 (fork の build-p2w/ で作れる。0039 まで入れて c8de9191)
```

`-DPICO_PLATFORM=rp2350` を付けないと、最初の configure が rp2040 で止まることがある。Pico W の既定 (`pico_w`) は古い SDK のままで、SD のパッチ (0036 以降) とは組み合わせていない。

## 書き込みと試験 (Z の中の sd 基板)

1. `scratchpad/zlock` を読み、無いか自分の名前のときだけ取る。**電源を切る前に team-lead に知らせる**。
2. raspi3 に elf を置いて `~/bin/swdflash <elf> sd` (1 回目は `Failed to write memory` になることがあり、2 回目で通る)。
   Pico をリセットすると Z は USB を数え直さないので、続けて `~/bin/zpower cycle 5`。
3. Z が起動したら (コンソールは AUX、`/tmp/x68con.log`) `~/bin/x68type "<コマンド>"` で 1 字ずつ送る。終わったら `x68type "b:netcon"`。
   CON のときに操作するには `x68key -t z type "ctty aux"` + `x68key -t z key enter`、戻すのは `x68type "ctty con"`。
4. `x68key -t z status`: `remote0=ok remote1=ok sdcopy=idle` など。sdcopy は UDP 6869 へ `sdcopy x68000z/sdcopy/hds1.hds /x68000z/test.hds`
   (返事は `ok sdcopy started`、様子は status の `sdcopy=run:N/M|done:N|err:<理由>`)。
5. UART のログ: raspi3 `/tmp/picouart-sd.log` (`picolog sd`)。remoteserv の 1 操作ずつのログは `virtual_disk.c` の `debuglevel = 1`、FatFs の失敗は `sdfs.c` の `SDFS_LOG 1` (どちらも遅くなる)。
6. 終わったら zlock を消し、x68k-zoskernel (Bravo) に知らせる。

## 実機の配置 (頼っているもの)

- sd 基板: Pico 2 WH、SPI0 GP18 SCK / GP19 MOSI / GP20 MISO / GP21 CS (25MHz。37.5MHz では mount しない)、Z の USB。Probe は raspi3 の E664A836A3167235 (`sd`)。
- p2w (机の上、192.168.1.3、raspi3 USB 1-1.3) は Charlie の Pico Z Loader の台。**使わない**。Probe E664A836A3178437 (`p2w`)。
- カードの中: `/x68000z/hds1.hds` (64MB、mkhds で作った Human68k の HDS)、`verify.bin`、`sdsmoke.bin`。raspi3 の `/srv/x68000z/sdcopy/hds1.hds` は sdcopy の試験の元。
- raspi3 の `~/bin`: swdflash, ocdserver, zpower, zshot, x68type, x68key (`-t z` = .4)。`pkill -f` は使わない (ssh ごと消える)。`pkill -x openocd`。

## 分かっている問題・制約

- C: のファイルの日付は変えられない (FatFs の `FF_USE_CHMOD` が 0、lib の ffconf.h)。コピーしたファイルは書いた時刻。
- 空き容量の表示は Human68k の上限 2GB。
- `my_rtc.c` (FatFs の lib) が `time()` を置き換えている。0037 で always-on タイマーを起動時に 0 から動かし、SMB の時刻で 1 回合わせる (時刻が 1 回前へ飛ぶ)。
- sdcopy の「HDS として開いているファイルには写さない」は実機で試していない (HDS1 にカードの HDS を指定していないため)。
- 設定画面の右の「ネットワーク設定」の見出しが、その上の「その他の設定」の枠の下の線と同じ行に重なって見える (実機の画面で。動きには関係ない)。
- tistack (TSP): zostest がたまに「Error writing: I/O error」で落ちていた (5 回に 1 回ほど)。原因は tis.x の「npd.x が 5 秒交換しに来ない」の判定で、つながっているソケットまで Z の側だけで死なせていた (tis.x -s で netdown 1・resend 0)。00cc0a6 で、待っている connect などだけを失敗にするよう直した。**なぜ Z-OS の中で npd.x が 5 秒回らないのか** (スレッドの割り当て) は未調査で、Bravo に伝える。
- ID1 の HDS を B: をずらさずに使う案 (Delta): リモートドライブの顔 (LBA 0/4/6.. の起動部) を ID1 より小さい ID で見せ、運び (vdbuf/netpipe) は ID6 のまま。harness/picosim/README.md「ドライブ名」。
- 設定画面は SMB の共有の一覧から選ぶ作り。`sd:` は ENUM の最後に足している (共有が 16 個あると出ない)。

## 次にやること

1. (済) 明示だけの版を書き、設定画面で RMTUNIT 2 / REMOTE1 = `sd:/` → C: = SD、FD は D: E:。status に `host=pico-f350`。
2. ホスト名と固定 IP は Charlie (PZL 186beb6) と同じ形に揃えた (項目名・既定 pico-XXXX・NET1 の並び・返事の host= の位置)。mDNS は今はしない。固定 IP は moto が頼むまで Z の基板では試さない。
3. (0042 で直した、実機は未試験) SETCONFIG で Wi-Fi をつなぎ直すと古い SMB の接続が残り、次の keepalive までリモートドライブが -14 を返していた。fork は enable_sta_mode を起動時の 1 回しか呼ばないので、つなぎ直しで固定 IP が DHCP に戻ることはない (netif を作り直さない) はず (未試験)。
3. tistack の実機の速さ (`tis.x -p`) は Pico 2 W でまだ測っていない。
4. パッチ 0036-0038 を main へ (ホストの試験は関係しないが、マージ前に走らせる)。
