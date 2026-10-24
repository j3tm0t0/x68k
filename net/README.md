# net

X68000 / X68000 Z の TCP/IP まわり。TCP/IP 本体は ESP/X の xip.x (K.Shirakata 氏の改良版 β5) で、ここにはその下のドライバと相手側を置く。
イメージへの組み込みは [../x68kcli](../x68kcli) の `make` が行う (`scripts/xbuild.sh` がここをクロスビルドし、`scripts/stage.sh` が入れる)。

| ディレクトリ / ファイル | 中身 |
|---|---|
| [slip](slip) | `slip.sys`: xip.x から Ethernet ドライバ (`/dev/ep0`) に見える SLIP ドライバ (elf2x68k) |
| [slipd](slipd) | `slipd.x`: SLIP を運ぶ常駐スレッド。RS-232C か、Pico W の netpipe (`-z`、`-z IP` でブリッジ) |
| [pppwait](pppwait) | `pppwait.x`: Pi の pppd の LCP を受けてから ppp.x を起動するための待ち合わせ (X68000 上でビルド) |
| [rsspeed](rsspeed) | `rsspeed.x`: IOCS `_SET232C` で RS-232C の速度を設定する (Z の UART は IOCS 経由の設定だけ有効) |
| [patchxip.py](patchxip.py) | xip.x の MSS を 512 → 1460 に書き換える (元の SHA-256 を確かめてから。改変済みのバイナリは配布しない) |
| [raspi](raspi) | Raspberry Pi 側: `ppp-x68k.sh` / `.service` (MODE=slip / ppp / pico)、`slipudp.py` (netpipe の UDP を sl0 へ)、`x68mtutest.sh` |
| [x68kzremotedrv-netpipe](x68kzremotedrv-netpipe) | Pico W ファーム x68kzremotedrv に netpipe (SCSI ID 6 越しの SLIP) を足すパッチ |

X68000 側の使い方は `/etc/zshrc` の `netup` (`netup [速度]` / `netup pico` / `netup bridge IP`)。詳しくは [../x68kcli/README.md](../x68kcli/README.md) と [記事の下書き](../docs/_drafts/x68000z-wifi-tcpip.md)。
