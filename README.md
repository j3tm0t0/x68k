# x68k

X68000 / X68000 Z まわりのハックを置くリポジトリ。

| ディレクトリ | 中身 |
|---|---|
| [x68kcli](x68kcli) | CLI 環境: Human68k 3.02 + zsh / fish / vim / GNU ツール / gcc の HDD イメージを作る |
| [net](net) | ネットワーク: SLIP ドライバ (slip.sys / slipd.x)、xip.x の MSS パッチ、Pi 側、Pico W の netpipe ファームのパッチ |
| [harness](harness) | デバッグ・遠隔操作: rcmd.x、Pi / Mac 側の操作スクリプト、Pico W キーボード、contee.sys、Z 上の調査道具、XEiJ |
| [docs](docs) | GitHub Pages (https://j3tm0t0.github.io/x68k/)。記事は `docs/_posts/` |

`x68kcli` のイメージ作成は `net` と `harness` のソースも使う (相対パス `../net`、`../harness`)。
README の中のパスは、特に断りがなければリポジトリの根からのもの。

## ライセンス

このリポジトリで書いたもの (コード、スクリプト、テンプレート、記事) は [MIT](LICENSE)。
イメージ作成時に取得する Human68k、ESP/X、xip.x、zsh などの配布物はこのリポジトリに含めず、それぞれの配布条件に従う
(Human68k は非商用・無償配布のみ)。`net/x68kzremotedrv-netpipe` のパッチの当て先 [x68kzremotedrv](https://github.com/yunkya2/x68kzremotedrv) は yunkya2 氏の MIT。
