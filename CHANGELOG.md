# 変更履歴

X68000 Z 向けに配っているもの (Pico のファーム、ssh.x / zterm、npd.sys / npd.x、tis.x) の変更をまとめます。
新しいものが上です。各版の中身と入れ方は [Releases](https://github.com/j3tm0t0/x68k/releases)、経緯は [日記](https://j3tm0t0.github.io/x68k/x68000z/) にあります。
ファームのパッチ番号は `net/x68kzremotedrv-netpipe/patches/` の番号です。

## 未公開 (次の版に入るもの)

### Pico のファーム
- 読み込みキャッシュの直し (0044)。書き込んだセクタを含む組が 2 つあると、古い中身を返すことがあった。
- Pico W (RP2040) でも SD カードを使える別ビルド (0045、0046)。`-DSD_CARD=ON` で有効。Pico W では sdcopy は入らない。既定は Pico 2 W で有効、Pico W で無効。
- `status` の返事を作るときのスタックのあふれを直した (0047)。ディスクが忙しい最中に UDP 6869 の `status` を何度も問い合わせると、メモリ切れで止まることがあった。
- 起動直後などに "Out of memory" で止まることがあったのを直した (0048)。HDS の読み書きと SMB の接続を保つ問い合わせ (keepalive) が同じ接続を同時に使っていた。SMB につながった直後、Z が HDS を読んでいる最中に重なりやすい。
- 仮想ディスクの中の HDS のファイルの FAT の鎖が、大きさが 32KB で割り切れるとき 1 クラスタ長かったのを直した (0049)。読むのには影響しない。
- SD カードのピンを、ビルドのときに Pico の USB の側 (GP2〜5) にも変えられるようにした (0050、`-DSD_PINS=usb`)。既定は今までどおり GP18〜21。

### Z-OS 関係 (term/vt100)
- ペインごとのスクロールバックの窓口 (`vt_sb`: 画面の上から流れ出た行を置き場に渡し、`vt_view` でさかのぼって見る)。置き場は zos.x の側 (主記憶、後で Pico の SD にも)。
- 小さい字のモード (6x12) の漢字を、CGROM の 16 ドットの字から形を崩さずに縮めて作るようにした。Z の実機の ROM の 12 ドットの字は潰れて読みにくかった。

### x68kcli のイメージ
- `/etc/zoshrc` (Z-OS のシェル zosh の別名と、`-lgcc2` を付ける gcc などの関数)。

## 2026.10.23

[リリース](https://github.com/j3tm0t0/x68k/releases/tag/v2026.10.23)。ファームは Pico 2 W 用 (パッチ 0001〜0043)。

### Pico のファーム
- Pico 2 W (RP2350) 向けのビルド。TCP の窓口 (TSP) を本来の大きさで入れた。
- 起動待ち画面。SMB が間に合わなくても「ディスクから起動できません」にならず待つ。
- USB キーボード (HID) を同じ Pico に混載。UDP 6869 の x68key でキーを送れる。返事は `ok host=<ホスト名> ...`。
- microSD (SPI0: GP18 SCK / GP19 MOSI / GP20 MISO / GP21 CS)。
  - REMOTEn を `sd:/` にすると、SD カードがリモートドライブのユニットになる (B: の次なら C:)。
  - HDS の欄に `sd:/...` と書くと、カードの上の HDS を SCSI ディスクにする。
  - UDP 6869 の `sdcopy` で、SMB の共有からカードへ写す。
- 基板ごとのホスト名 (既定 `pico-` + ボード ID の下 4 桁) と固定 IP (既定は DHCP)。
- TCP / UDP / DNS を Pico の lwIP に任せる窓口 (TSP、0xA6)。Z 側は `tis.x -p`。
- 直した不具合:
  - B: の読み込みが 8 ページずれる (上流に報告済み)。
  - pico-sdk 2.x のビルドで起動直後に落ちる。
  - 設定画面の Wi-Fi の一覧が終わらないことがある。
  - 設定を変えてつなぎ直したあと、古い SMB の接続が残る。
  - 設定画面の「ネットワーク設定」の見出しが枠に重なる。

### ssh.x
- パスワード認証 (password / keyboard-interactive)。
- 仮想コンソール。1 本の接続で最大 4 つのシェル (CTRL+F1〜F4)。
- 絵文字が実機の Z で白黒になっていたのを直した。
- Z-OS (zos.x) のペインの中で動く。端末の大きさがペインに合う。

### npd.sys / npd.x 1.2、tis.x
- `tis.x -p` に対応。
- npd.x が 5 秒止まっても、tis.x がつながっている接続を切らないようにした。

### 実機 (X68000 Z) での速さ (tis.x -p と xip.x)
| | tis.x -p | xip.x |
|---|---|---|
| 1 バイトの往復 | 53 ms | 149 ms |
| Z → Mac、1MB | 109 KB/s | 25.7 KB/s |
| Mac → Z、1MB | 145 KB/s | 16.5 KB/s |

### tis.x
- npd.x がしばらく回ってこなくても、つながっている TCP を切らないようにした (Z-OS の中で ssh.x が "Error writing: I/O error" で切れることがあった)。待っている connect と名前の問い合わせだけを "Network is down" で終わらせる。
- `tis.x -s` が Pico とのやり取りの数 (交換・送り直し・netdown・stall) も出す。

## 2026.10.22

[リリース](https://github.com/j3tm0t0/x68k/releases/tag/v2026.10.22)。ファームは Pico W 用。

- ssh.x (dropbear 2022.83 移植) の最初の公開版。
  - 68000 向けの速い暗号: X25519 / Ed25519 (crypto68k)、ChaCha20-Poly1305 (chacha68k)。
  - `-t` で VT100 (UTF-8 の日本語、絵文字 300 字)。
  - `-Z` で zterm (接続先に補助プログラムを送り込む、ローカルエコー付き)。
  - `-oAccel=verify|kex|all` で、暗号の計算を Pico W に任せる。
- zterm の補助プログラム (Linux aarch64 / x86_64、macOS arm64 / x86_64)。
- npd.sys / npd.x 1.1 (Pico W netpipe、暗号アクセラレータ)。
- Pico W のファーム (x68kzremotedrv + netpipe + 暗号アクセラレータ)。
