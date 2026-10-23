# harness/picosim/pzl -- Pico Z Loader のファームをホストで

x68k-zos の branch `pzl` の `pico/` (x68k-pwauth のファーム) のうち、SCSI の割り当て (`src/scsi/virtual_disk.c`)・
記憶装置の層 (`src/storage/storage.c`, `stor_ram.c`)・設定 (`src/config/config_file.c`) を**そのまま**コンパイルし、
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
- ホストの代わりにしたもの (`pzl_host.c`, `shim/`): main.c / net.c の大域変数、net.c の接続の状態機械 (`-T` の時間で
  sysstatus を進め、net.c と同じ順に HDS を開いて `vd_mounted()`)、stor_smb.c (共有 = 手元のディレクトリ)、フラッシュと
  watchdog。config_file.c の `.incbin` (ELF 用) は `host_config.py` が C の配列に置き換える (ほかは一字も変えない)
- ld -r で 1 個の .o にまとめ、外に出す名前は `pzl_*` だけ (picosim の名前とぶつからない)
- 番地: XEiJ の (ID, LBA) を `vd_read_block(0x803fa0 + ID × 0x800000 + LBA)` に (Z の Linux が pscsi.ini 越しにするのと同じ)。
  VD_NOTREADY → 02/04/01、VD_IOERR → 03/11/00、-1 は成功 (msc_disk.c と同じ)。ファームの 5 秒の保留は無い

確かめたこと (2026-10-23、pzl 3fdf00c): `tests/pzltest.py` 9 項目 (`make test` に入っている、PZL の作業木が無ければ SKIP)。
XEiJ: HDS0 = x68kcli のイメージ (共有の下のファイル) で、`-T 2,4,8` の間 TEST UNIT READY に NOT READY (約 1800 回) →
開いて Human68k・zsh・npd.x・rcmd.x まで起動した (PZL M1 には netpipe が無いので、ネットワークはつながらない)。
B: (リモートドライブ) は PZL の remoteserv (x68k-pwauth、作業中) が入ったら、vd_command → remote_serv で動くはず。
