# x68k

X68000 / X68000 Z まわりのハックを置くリポジトリ。

| ディレクトリ | 中身 |
|---|---|
| [x68kcli](x68kcli) | CLI 環境: Human68k 3.02 + zsh / fish / vim / GNU ツール / gcc の HDD イメージを作る |
| [net](net) | ネットワーク: netpipe ドライバ npd.sys / npd.x (Pico W 越しの IP)、SLIP ドライバ、xip.x のパッチ (patchxip.py)、Pico W の netpipe ファームのパッチ |
| [net/ssh](net/ssh) | SSH クライアント `ssh.x` (dropbear 2022.83 の移植)。68000 向けの高速暗号 [crypto68k](net/ssh/crypto68k) (X25519/Ed25519) と [chacha68k](net/ssh/chacha68k) (ChaCha20-Poly1305)、`-t` で VT100、`-Z` で zterm |
| [net/accel](net/accel) | 暗号アクセラレータ: X25519/Ed25519 を netpipe の Pico W に肩代わりさせる (`ssh.x -oAccel=verify\|kex\|all`、既定はオフ) |
| [term/vt100](term/vt100) | VT100 (xterm サブセット) 端末エミュレータ。テキスト VRAM に直接描く。UTF-8 の日本語 (CGROM) と絵文字 |
| [term/zterm](term/zterm) | zterm: 接続先に Rust の補助プログラムを送り込み、画面を向こうで持って変わったセルだけ送る (mosh 風、ローカルエコー付き) |
| [harness](harness) | デバッグ・遠隔操作: rcmd.x、Pi / Mac 側の操作スクリプト、Pico W キーボード、contee.sys、Z 上の調査道具、XEiJ |
| [docs](docs) | [GitHub Pages](https://j3tm0t0.github.io/x68k/) の中身。記事は `docs/_posts/` |

Z から Linux / macOS に入って Claude Code まで動かした経緯は [日記](https://j3tm0t0.github.io/x68k/x68000z/) の第 10〜12 回。
ビルドは `net/ssh/dropbear` で `./fetch.sh && ZTERM=1 FAST=1 ACCEL=1 ./build.sh` (詳細は [net/ssh/dropbear/README.md](net/ssh/dropbear/README.md))。

`x68kcli` のイメージ作成は `net` と `harness` のソースも使う (相対パス `../net`、`../harness`)。
README の中のパスは、特に断りがなければリポジトリの根からのもの。

## ライセンス

このリポジトリで書いたもの (コード、スクリプト、テンプレート、記事) は [MIT](LICENSE)。
イメージ作成時に取得する Human68k、ESP/X、xip.x、zsh などの配布物はこのリポジトリに含めず、それぞれの配布条件に従う
(Human68k は非商用・無償配布のみ)。dropbear (MIT) は取得してパッチを当てる。vendor した Monocypher (CC0 / BSD-2) と
Twemoji から変換した絵文字の表 (CC-BY 4.0) はそれぞれの条件に従う。`net/x68kzremotedrv-netpipe` のパッチの当て先 [x68kzremotedrv](https://github.com/yunkya2/x68kzremotedrv) は yunkya2 氏の MIT。
