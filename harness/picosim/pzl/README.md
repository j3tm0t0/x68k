# harness/picosim/pzl -- Pico Z Loader のファームをホストで

x68k-zos の branch `pzl` の `pico/` (x68k-pwauth のファーム) のうち、SCSI の割り当て (`src/scsi/virtual_disk.c`)・
記憶装置の層 (`src/storage`)・設定 (`src/config`)・設定画面のコマンド (`src/remote/vd_command.c`)・リモートドライブ
(x68kserremote の `remoteserv.c`)・netpipe (`src/netpipe`)・暗号アクセラレータ (`src/accel`)・起動待ち (`src/bootwait`) を
**そのまま**コンパイルし、
picosim (`build/picosim-pzl --pzl=config.txt`) の SCSI の相手にする。XEiJ から見ると Z に PZL の Pico が挿さっている形。

```sh
make -C harness/picosim build/picosim-pzl            # PZLREPO= (x68k-zos の作業木)、PZLREV= (既定 pzl)
build/picosim-pzl --pzl=config.txt --pzl-root=DIR -T 2,4,8 -L none &
#   config.txt は Z の config.txt と同じ形 (WIFI_SSID / SMB2_SERVER / HDS0: share/path.hds ...)。
#   SMB の共有は DIR の下のディレクトリ (HDS0: share/pzl0.hds -> DIR/share/pzl0.hds)
IMG=... XEIJ_JAVA_OPTS="-Dpicosim=127.0.0.1:6868 -Dpicosim.ids=0,6" harness/xeij/run.sh -config=default -saveonexit=off -model=SUPER
```

- PZL のソースは作業木ではなく `PZLREV` のコミットから `git archive` で写し出す (書きかけに左右されない。
  サブモジュール x68kserremote も記録されたコミットで)。`build/rev` に使ったコミット。driver/*.inc は elf2x68k でここで作る
- ホストの代わりにしたもの (`pzl_host.c`, `shim/`):
  - main.c / net.c の大域変数、net.c の接続の状態機械 (`-T` の時間で sysstatus を進め、net.c と同じ順にリモートドライブの根と
    HDS を開き、HDS0 は起動待ちの控えも `bw_check`。boottime は Mac の時計)
  - SMB: HDS (stor_smb.c) とリモートドライブ (`REMOTEn: share/dir`) は `--pzl-root` の下のファイル・ディレクトリ。
    リモートドライブは x68kserremote の POSIX の fileop.h (Pico の src/remote/fileop.h は libsmb2)。設定画面の SMB の一覧
    (SMB2_ENUM / LIST) と Wi-Fi の一覧は失敗を返す
  - Wi-Fi (cyw43): pico-sdk の lwIP の netif を 1 つ picosim の LAN (libslirp) に (`--pzl-ip`、既定 192.168.1.118)。
    PZL の netpipe.c がそれを横取りして Z の IP (`--pzl-zip`、既定 .68) を名乗る。picosim は代理 ARP しない (-L の転送だけ)
  - core 1 (accel.c の計算) はスレッド。フラッシュは 2MB の配列 (`--pzl-flash=FILE` で残す。設定 0x1F0000・控え 0x1E0000)。
    `--pzl` だけで config.txt を渡さなければフラッシュの設定 (設定画面で保存したもの)
  - config_file.c の `.incbin` (ELF 用) とフラッシュの番地 (XIP 0x10000000) は `host_config.py` が置き換える (ほかは一字も変えない)
- ld -r で 1 個の .o にまとめ、外に出す名前は `pzl_*` だけ (picosim の名前とぶつからない)
- 番地: XEiJ の (ID, LBA) を `vd_read_block(0x803fa0 + ID × 0x800000 + LBA)` に (Z の Linux が pscsi.ini 越しにするのと同じ)。
  VD_NOTREADY → 02/04/01、VD_IOERR → 03/11/00、-1 は成功 (msc_disk.c と同じ)。ファームの 5 秒の保留は無い

確かめたこと (2026-10-23 夜、pzl 5c87bd6 = vd_command + remoteserv 44616f9、netpipe / accel 45512d7、起動待ち):
- `tests/pzltest.py` 15 項目: 上の 9 項目 + GETINFO (unit)、B: の INIT / OPEN / READ (rpage、24 ページの応答の後も正しい)、
  netpipe でゲートウェイへ ping、accel の PING
- XEiJ (16.7MHz、HDS0 = x68kcli のイメージ、REMOTE0 = Mac のディレクトリ): `drive.x` で A: HDS、**B: リモートドライブ**、
  `cat b:/hello.txt`、npd.x の netpipe で Mac → rcmd.x、`ping.x 192.168.1.1` (90〜110ms)、`acceltest.x` `0 failure(s)`、
  `ssh.x -y -oAccel=all+log` で Mac の sshd と鍵交換 (x25519 / verify は PZL の accel.c、`No auth methods` まで)
- 今のファーム (fork) との違い (picosim で見えたもの):
  - **リモートドライブの数**: GETINFO の unit は `boottime != 0 && TADJUST != 0` のときだけ設定される (上流から同じ)。
    boottime が 0 のまま (SMB のサーバの時刻が取れない) か TADJUST: 0 だと unit = 0 になり、XEiJ では B: と C: が
    「その他のタイプ」になって 2HD のドライブが消えた (C: は中身なし)。fork も同じ作りなので PZL だけの違いではないが、
    SMB を使わない SD の構成 (boottime の出どころが無い) では起こりうる
  - accel の所要時間は Mac の速さ (RP2350 の時間の真似はしていない。fork の真似の picosim は見積りの時間を待つ)

以前 (pzl 3fdf00c): `tests/pzltest.py` 9 項目 (`make test` に入っている、PZL の作業木が無ければ SKIP)。
XEiJ: HDS0 = x68kcli のイメージ (共有の下のファイル) で、`-T 2,4,8` の間 TEST UNIT READY に NOT READY (約 1800 回) →
開いて Human68k・zsh・npd.x・rcmd.x まで起動した (PZL M1 には netpipe が無いので、ネットワークはつながらない)。
B: (リモートドライブ) は PZL の remoteserv (x68k-pwauth、作業中) が入ったら、vd_command → remote_serv で動くはず。
