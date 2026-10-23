# harness/picosim 引き継ぎ (2026-10-23 夜、x68k-picoharness)

詳しい中身は [README.md](README.md)。ここは「何ができて、どう回して、何が残っているか」だけ。

## 何を真似ているか

XEiJ (Java エージェント `harness/xeij/agent/PicoBridge.java`) が SCSI を TCP で picosim に渡す。XEiJ は無改造。

| 部分 | picosim の中 | 元 (ファーム) |
|---|---|---|
| SCSI の 2 つの道 | SPC (IOCS _SCSIDRV) の ID 差し替え + Z のメールボックス 0xEA0060 | Pseudo SCSI BIOS |
| ID 6 netpipe (npd.x の X6NR / slipd の X6NP) | netpipe.c | src/netpipe.c |
| 暗号アクセラレータ 0xA5 | net/accel/accel_ops.c + Monocypher をそのまま | src/accel.c |
| TSP 0xA6 (tis.x -p) | net/tistack の tsp_svc + lwIP (lwIP があれば既定で入る。LAN に出るのは最初の 0xA6 から) | patch 0031〜 |
| LAN | libslirp (192.168.1.0/24、.1 = Mac の 127.0.0.1、DNS .1/.3 は picosim が getaddrinfo で答える) | Wi-Fi + 代理 ARP |
| Pico Z Loader (待ち画面・控え・設定画面) | bootwait.c (`-T` `-B` `-F` `-C` `-K`) | src/bootwait.c ほか |
| B: リモートドライブ | remote.c + フォークの x68kserremote の remoteserv.c (ビルド時にそこから) | remoteserv |
| vdbuf のページ (rpage、patch 0030) | ドライバが bit 7 で起点を知らせれば LBA から。古いドライバは数え方 (B: の破損ありのまま) | virtual_disk.c |
| データの HDS (ID 1〜5) | `-H ID:file[:hold\|:after-remote]` | HDS1〜 / SD |
| Z の Linux の読み込みキャッシュ・16KB 先読み | picosim.c (`-c` `-r`) | Z |

## 回し方

```sh
make -C harness/picosim                    # 初回は libslirp を取ってきてビルド
make -C harness/picosim test               # 5 本 (下)。XEiJ の 2 本で 4 分ほど
make -C harness/picosim test NOXEIJ=1      # ホストだけの 3 本 (数秒)
make -C harness/picosim FW=<x68kzremotedrv の clone>/driver RS=<clone>/x68kserremote test
```

| 試験 | 何を | 要るもの |
|---|---|---|
| tests/bridgetest.py | 形式、Z のキャッシュ、slirp の ping / DNS / 転送、accel、サービスの口 (0xA6 echo)、口からの DNS | なし |
| tests/bootwaittest.py | 控え無し → NOT READY → 控えを作る、待ち IPL の WAIT / READY / CHANGED、設定画面の vdbuf、ID 1 のデータの HDS、`-Q` | FW (driver/ のバイナリ) |
| tests/remotetest.py | B: の com_cmdres を再現: 古いドライバの 16 / 24 ページの後の破損、rpage ドライバで正しい (ソフトリセット後の読み直しも) | RS、FW |
| tests/tsptest.sh | XEiJ で tis.x -p: 名前・echo・put・get・sink・src・ssh.x (鍵交換まで)・Mac からの rcmd.x (net/tistack/tests/xeij_pico.sh を回す) | lwIP、XEiJ (x68kcli/tools)、GUI のセッション |
| tests/zostest.sh | XEiJ で zos.x (ksh 2 ペイン)、右で ssh.x -t を TSP 越しに試験用 sshd へ、テキスト VRAM で判定 | 上 + x68k-zos の kernel/ (ZOS=)、x68kcli の stage、ssh.x |

FW / RS の部品の作り方は README の「ファームの部品」 (本家 3afd408 + net/x68kzremotedrv-netpipe/patches)。
手で XEiJ を動かすとき: `IMG=build/picosim.hds XEIJ_JAVA_OPTS="-Dpicosim=127.0.0.1:6868 -Dpicosim.ids=6" harness/xeij/run.sh -config=default -saveonexit=off -model=SUPER`
(イメージは `make -C x68kcli image NET=npd NETZIP=192.168.1.68 IMG=build/picosim.hds`)。XEiJ は GUI のセッション (`tmux -L gui`) から。
画面は textcopy かテキスト VRAM の dump (スクリーンショットは使わない)。ダイアログ (バスエラーなど) は textcopy に出ない。

## 分かっていること・限り

- 速度は MPU のクロックに比例 (Z の 68000 が律速): 10MHz で Z→Mac 29KB/s、16.7MHz で 50KB/s (実機の Z 並み)、33.3MHz で 100KB/s
- SPC の道の書き込みの失敗は Z に伝わらない。メールボックスは READ(10)/WRITE(10) だけ。USB の遅れは `-d` で足さないと 0
- XEiJ の ROM からの起動は Z の Linux 越しの起動とは違う (キャッシュと先読みの真似で近づけているだけ)。ファームの 5 秒の保留は無い
- Wi-Fi / SMB は時間で進むだけ。設定画面の SSID などは効かない (HDS0 のパスだけ)
- 電源の入れ直し = picosim と XEiJ を両方起動し直す
- **不安定なもの**: zostest が `make test` の通しで 1 回、TSP 越しの ssh.x が認証前に "Error writing: I/O error" で落ちた
  (すぐ回し直すと通った)。x68k-tistack-2 に報告済み。負荷のある時の TSP の write_s の失敗を疑っている (未確認)

## ドライブ名の調べもの (状況)

README の「ドライブ名」。Human68k 3.02 は SCSI を ID の小さい順に組み込む (ID0 = A:、ID1 のデータ = B:、ID6 のリモート = C:)。

- **ファームだけで B: をリモートに**: リモートドライブの顔 (LBA 0 署名・LBA 4 分割表・LBA 6〜 scsiremote.bin) をデータの HDS
  より小さい ID で出し、運ぶ道は ID 6 のまま。picosim `-Q 1 -H 2:data.hds` で A: ID0、B: リモート、C: データを確かめた。
  ファームへの実装は x68k-tistack-2 (SD を第 2 段で) に伝えた。まだ入っていない
- **Z だけで**: AUTOEXEC.BAT に `SUBST X: B:\` / `SUBST B: C:\` / `SUBST C: X:\` / `SUBST /D X:` (確かめた)
- **決まった高い名前 (Z: など)**: [../zremote](../zremote) の `zdrv.x` が、デバイス名 "\x01ZREMOTE" のドライブを探して
  Z: (引数で X: など) を仮想ドライブ (_ASSIGN mode 0x50) として割り当てる。リモートが C: のとき `zdrv.x` → `zdrv: Z: = C:\`、
  z: の読み書きが Mac のディレクトリに届いた。AUTOEXEC.BAT で zdrv.x してから z: を使えば、データの HDS の有無で
  ずれても同じ。mode 0x60 は -14 (パラメータ不正) で断られた (0x50 が SUBST と同じ「ドライブ → 根」)
- **できないこと**: ドライバが自分のドライブ名を選ぶ道は無い (Human68k が組み込み順に振る。LASTDRIVE は上限だけ)。
  起動の走査の間 NOT READY にしたディスクをあとから組み込む道具も 3.02 には無い (FORMAT.X の SCHDISK を DEVICE= しても
  ドライブは増えない)。SUSIE.X など外の道具は試していない

## 場所

- コード: x68k の main (`harness/picosim`、`harness/xeij/agent`、`harness/zremote`)。作業ブランチは picoharness2
- 読むだけで使っているもの: フォーク `~/src/github.com/yunkya2/x68kzremotedrv-hid` (FW / RS の既定)、
  `~/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip` (LWIP の既定)、x68k-zos の kernel/ (ZOS の既定)
- 実機 (Z / Pico / raspi) には触っていない。使うなら scratchpad/zlock の決まり (無いか自分の名前のときだけ取る、消して返す)
