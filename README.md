# x68k

X68000 / X68000 Z まわりのハックを置くリポジトリ。

| ディレクトリ | 中身 |
|---|---|
| [x68kcli](x68kcli) | CLI 環境: Human68k 3.02 + zsh / fish / vim / GNU ツール / gcc の HDD イメージを作る |
| [net](net) | ネットワーク: SLIP ドライバ (slip.sys / slipd.x)、xip.x の MSS パッチ、Pi 側、Pico W の netpipe ファームのパッチ |
| [harness](harness) | デバッグ・遠隔操作: rcmd.x、Pi / Mac 側の操作スクリプト、Pico W キーボード、contee.sys、Z 上の調査道具、XEiJ |
| [docs](docs) | GitHub Pages (https://j3tm0t0.github.io/x68k/)。下書きは `docs/_drafts/` |

`x68kcli` のイメージ作成は `net` と `harness` のソースも使う (相対パス `../net`、`../harness`)。
README の中のパスは、特に断りがなければリポジトリの根からのもの。
