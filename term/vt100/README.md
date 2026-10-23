# vt100 -- X68000 用 VT100 (xterm サブセット) 端末エミュレータ

Human68k の CON デバイスが解釈する ANSI は一部だけ (スクロール領域・DEC 罫線・
アプリケーションカーソル/キーパッド無し、折り返しも独特) なので、vim / less / top の
ような全画面プログラムが崩れる。このライブラリはバイト列を自前で解釈し、X68000 の
テキスト画面 (0xE00000、4 プレーン) に直接描く。SSH クライアントから link して使う。

| ファイル | 中身 |
|---|---|
| `vt100.h` / `vt100.c` | エミュレータ本体。`vt_init` / `vt_write` / `vt_key` / `vt_fini` / `vt_get_size` |
| `vtdump.c` | 論理画面を UTF-8 で書き出す (テスト用) |
| `vtcat.c` | 記録した VT100 バイト列を読んで描画するテストプログラム (`vtcat.x`) |
| `tests/` | pyte を参照レンダラにした比較試験一式 |

## ビルド
```sh
make                 # vtcat.x (X68000, elf2x68k)
make vthost          # Mac 上でパーサを試すスタブ版 (-DVT_HOSTTEST)
```

## SSH クライアントからの使い方
```c
#include "vt100.h"
int cols, rows;
vt_default_size(&cols, &rows);     /* 96x32 */
vt_init(cols, rows);               /* 画面を占有 */
/* pty 要求のウィンドウサイズは vt_get_size(&cols,&rows) で */
for (;;) {
    n = ssh_recv(buf, sizeof buf);
    vt_write(buf, n);              /* 受信を描画 */
    m = vt_key(kbuf, sizeof kbuf); /* キー入力を VT100 バイト列に */
    if (m) ssh_send(kbuf, m);
}
vt_fini();
```
スーパーバイザ切り替えはライブラリ内で完結する。クライアントはユーザモードのままでよい。
`vt_init`〜`vt_fini` の間は DOS のコンソール出力に触れないこと。

### 仮想コンソール (`-DVT_VCONS`)
VRAM 1 枚で複数の端末を切り替える (ssh.x の Ctrl+F1..F4)。端末の状態 (カーソル・属性・モード・解析の途中・応答キュー・
行ポインタ表、約 0.9 KB) を `vt_con_save` / `vt_con_load` で入れ物と写し合い、画面 2 面 (24 KB) は `vt_con_new` に渡した
置き場に置く (最初の端末は内蔵)。裏の端末は `vt_write` しても `flush` と `scroll` の VRAM・`drawn` に触らない (解析と論理画面
だけ)。`vt_con_visible(1)` で `drawn` を「分からない」にして全面を描き直す。`vt_con_status` は最上行の右端に反転の短い表示を
重ねる (論理画面は変えない)。`vt_reply` は DSR/DA の応答だけを取り出す (裏の端末の応答をそのチャネルへ)。`vt_keyhook` は
`vt_key` が読んだ IOCS のキーを先に見る。vt_init 済みの間の `vt_init` (zterm の HELLO) はいまの端末の状態だけ初期化する。
`VT_VCONS` 無しのオブジェクトは変わらない。試験は `tests/vctest.c` (run.sh の 6)。

### ペイン (Z-OS の zos.x の中)
`vt_init` は IOCS `_B_CONSOL` (d1 = d2 = -1) で今のコンソールの枠を読む。幅が 96 桁未満か、原点が左端でなければ
**その枠の中だけに描く**: TVRAM の原点を枠の左上にずらし (`vt_base`)、桁数・行数を枠に丸め、スクロールは TXRASCOPY
(全幅) をやめて枠の幅だけ 32bit で写す。Z-OS (j3tm0t0/x68k-zos の kernel/zos.x) はペインごとに `_B_CONSOL` が
ペインの位置と大きさを返すので、ssh.x がそのままペインに収まる。普通の Human68k の `_B_CONSOL` は 96 桁の全面なので、
今までどおり全面 (96x32) に描く。

- `vt_get_origin(&col, &row)`: 描いている枠の原点 (全面なら 0,0)。ssh.x は終わるときの「プレーン 0,1 に潰す」をその枠だけにする
- `vt_get_size` は `vt_init` の前でも、これから描く枠の大きさを返す (ssh.x の pty 要求は画面を切り替える前)
- 枠の左端は 4 桁の倍数のときだけ枠と見なす (32bit の写しがはみ出すのは右の仕切りの桁で、どの行も同じ)
- パレットは全面のときと同じく 16 色を設定する (zos.x のほかのペインの色も変わる。zos.x 側で合わせる)
- 試験: run.sh の 7 (vthost `-P 桁,行,幅,高さ` で枠を与え、全面で描いた VRAM をずらしたものと一致し、枠の外に描かないこと。`tests/panecmp.py`)

## 画面と描画
- 既定 96x32 (768x512, 8x16 フォント)。`vt_init(cols,rows)` で指定可 (横最大 96・縦最大 32)。
- セル (`cell_t`) は 4 バイト: コードポイント 16bit + 属性語 16bit (`fg<<12 | bg<<8 | flags`、bold/reverse 解決済み)。
  比較・写し・消去は 32bit 1 語。字を置く属性語は SGR などで属性が変わったときだけ作り直す (`attr_update`)。
  UTF-8 デコーダ (`utf8_feed` / `put_run`) と描画層 (`blit`) は分離。
- セルごとに論理画面 (`cur`) と VRAM の控え (`drawn`) を持ち、`flush` は変わったセルだけ描く。
  行ごとに書き換えた桁の範囲 (`rlo`/`rhi`) を持ち、`flush` はその範囲だけ比べる。
- スクロールは IOCS TXRASCOPY (`_iocs_txrascpy`) でラスタをハードコピーする (1 ラスタ = 4 走査線、行 = 4 ラスタ)。
  下方向は d3 bit15 (降順)。上下とも XEiJ で実 VRAM がホスト版と一致 (`xeij_bench.sh`、app_lessu8)。
- 色は 8 + 8 (bright) をテキストパレット (0xE82200) に割り当て、reverse/underline を含め
  グリフとプレーンの論理で表す。Unicode 罫線 (U+2500〜) は自前の DEC 罫線グリフに寄せる。

### 日本語 (全角)
- UTF-8 → JIS X0208 変換表は `genkanji.py` が `kanjitab.h` に生成 (6879 文字、Unicode 昇順・二分探索)。
- 全角 (East Asian Width = W/F) は 2 セルを占める (左 `F_WIDE` + 右 `F_WIDECONT`)。カーソル送り・折り返し・
  右端 1 桁での繰り上げ・上書きで相方セルを割る処理を含む。
- 漢字は CGROM の 16x16 フォントを直接描く: `addr = 0xF00000 + ((ku-1)*94 + (ten-1))*32`
  (ku=JIS上位-0x20, ten=JIS下位-0x20、2 バイト/行)。実機で VRAM が CGROM グリフと一致することを確認。
- 変換表に無い全角は `??`、未対応の半角非 ASCII は `?`。半角カナは未対応。

### 絵文字 (テキスト VRAM に 16x16・16色)
絵文字は全角セル (`F_WIDE | F_EMOJI`) として 16x16・4bpp でテキスト VRAM に直接描く。cp にはコードポイントで
なく絵文字表のインデックスを入れる (絵文字は BMP 外で 16bit に収まらないため)。異体字セレクタ VS16 (U+FE0F)・
ZWJ (U+200D) は幅 0 として無視。表に無い全角の絵文字は `??` フォールバック (幅 2 を保つ)。

- 表 `emojitab.h` は `genemoji.py` が **Twemoji 15.1.0 (CC-BY 4.0, © 2023 Twitter/X, jdecked/twemoji)** の
  72x72 PNG を 16x16 に縮小し、**16 色 (ANSI テキストパレット) に量子化**して生成 (元 PNG は含めず変換後の表のみ収録)。
- **割り切り**: 色はテキストパレットの ANSI 8+8 色 (SGR と共用、パレットは変更しない) に量子化するので粗い。
  透明ピクセルはパレット 0 (黒) にする (黒背景の端末前提)。重なり・透過・アニメーションは無い。
- **収録は 300 字** (1 字 128 バイト + コードポイント 4 バイト ≒ 39 KB)。`selemoji.py` が `emojilist.txt` を作る。
  出所: 以前の 28 字 → 手で足した字 (🍣 🍺 🍻 🍶 🍜 などの飲食、顔・手・ハート、⏰ (Claude Code が出す) などの道具・記号、
  🟢 🔴 🟥 などの色、動物、天気) → **Unicode Consortium の「Emoji Frequency」(Ranked Order by Median Frequency、
  https://home.unicode.org/emoji/emoji-frequency/)** の上位から順に、300 字になるまで。各行の 3 列目が出所
  (`orig` / `extra` / `rank`)。入れるのは次をすべて満たす字だけ:
  - 1 コードポイント (U+FE0F は除いて数える)。**国旗 (🇯🇵 = 2 コードポイント)・ZWJ 合成・肌色違いは入らない**
  - **wcwidth が 2** (`widthtab.h` の全角)。❤ ⚠ ⚙ ☀ ☁ ➡ ⬅ ⬆ ⬇ ✔ ☑ ✏ ⌨ 🖥 🗂 🗑 🕹 🌧 などテキスト表示が既定の字は
    wcwidth 1 で、相手のアプリ・pyte・zterm の vt100 crate が 1 桁と数える (後ろの U+FE0F で 2 桁にする扱いは端末ごとに
    違い、ここでは U+FE0F を捨てる)。2 桁の絵文字として描くと桁がずれるので入れず、従来どおり 1 桁の代用字 (`?` `!` `v` など)
  - JIS X 0208 に無い (`kanjitab.h` にある字は CGROM の漢字で描く)
- 引き方: `emoji_index` は昇順の表の二分探索 (300 字で最大 9 回)。表の最小 (`EMOJITAB_MIN` = U+231B) 未満は引かないので、
  Claude Code が多用する ← → … などは探索しない。
- 作り直し: `python3 selemoji.py <emoji-frequency.html> <twemoji>/assets/72x72 300 > emojilist.txt` →
  `python3 genemoji.py <twemoji>/assets/72x72 emojilist.txt > emojitab.h` (Pillow が要る。PNG はコミットしない)。
  zterm のサーバは `build.rs` が `emojitab.h` を読むので、作り直せば番号がそろう (番号 256 以上は `0x10 EMOJI16`)。
- 一覧の画面: `tests/fixtures/emoji_grid.rec` (`tests/gen_emoji_grid.py`、96x32)。`tests/run.sh emoji_grid` の PNG で見られる。
- PCG スプライトを使わない理由: スプライト/BG は 256x256・512x512 モード専用で、CLI の 768x512 高解像度テキスト
  (CRTMOD 16) では `_iocs_sp_init()` が **-1** を返し使えない (`tests/pcgtest.c` で XEiJ 確認。4/6/8/12 のモードなら 0)。
  512x512 に落とせば使えるがテキストが 64 桁に半減する。テキスト VRAM 直描きなら高解像度のままスクロールにも乗る。

### スループット (XEiJ 実測)
`tests/xeij_bench.sh` (XEiJ -model=SUPER = 10MHz 68000、vt_write 1 回 1400 バイト ≒ TCP 1 セグメント)。
描画あり / blit を抜いた版 (`vtbnob.x`、解析と差分走査だけ) の bytes/s。ee8e8f9 → 現行:

| フィクスチャ | 描画あり | blit なし |
|---|---|---|
| Claude Code 応答の流し込み (app_claude_stream) | 6926 → 14615 | 9222 → 19377 |
| Claude Code 起動〜応答 (app_claude) | 6563 → 14475 | 9109 → 19568 |
| tmux 分割 (app_tmux) | 5263 → 12842 | 5890 → 14593 |
| vim 日本語 (app_vimjp) | 3222 → 6848 | 5451 → 15219 |
| top 96x32 (app_top) | 2790 → 10259 | 5165 → 23496 |
| top 80x24 (top) | 3045 → 10772 | 5593 → 23268 |
| less UTF-8 戻りページ (app_lessu8) | 2231 → 4838 | 2576 → 5658 |
| git log (app_gitlog) | 2271 → 6278 | 3732 → 10263 |

効いたもの (解析側):
- セルを 4 バイト (cp + 属性語) にし、字を置くときは属性を計算しない。比較・消去・スクロールの退避は 32bit 1 語。
- 字の連なり (`put_run`): 印字 ASCII とバッファ内で完結した 2・3 バイトの UTF-8 (罫線・かな漢字・代用字) を
  1 字ずつ `put_cell` を呼ばずに置く。コードポイント→置き方 (`cp_kind`) は 256 件のキャッシュ。行に収まる ASCII は
  `vt_write` の中でじかに置く (Claude Code は CSI の間に数字ずつ置くので呼び出しの手間も効く)。
- CSI の引数はまとめて読む (`csi_feed`、ESC [ からそのまま)。38;2;R;G;B → 16 色の割り算は 16 件のキャッシュ。
- 行ごとの書き換え範囲で `flush` の走査を絞る。1 行送りのスクロールは行ポインタ表を 1 つずつ詰める (`%` は
  68000 では `__modsi3` の呼び出しで、less の 2 割近くを食っていた。他にホットな libgcc 呼び出しは無かった)。

効いたもの (描画側):
- **テキスト VRAM の同時アクセス** (CRTC R21 bit8 + bit4-7): 1 セルの 4 プレーンは「0 塗り / 0xff 塗り / グリフ /
  反転グリフ」の 4 通りなので、同じ書き方のプレーンをまとめてプレーン 0 の位置に 1 回で書く。白字ならグリフ 1 回。
  前も今も同じ値の塗りつぶしのプレーン (`drawn` から分かる) は書かない。`row_fill` も 1〜2 回。
  `vt_init` が画面外のライン 1020 で効くか確かめ (`sa_probe`)、効かなければプレーンごとに書く。R21 は読まず、
  `flush` の最後と TXRASCOPY・カーソル反転の前に 0 に戻す。**XEiJ で実 VRAM がホスト版と一致 (app_top・
  app_claude_stream・app_vimjp)。X68000 Z 実機では未確認。**
- 68000 の手書き (`__mc68000__` のときだけ。ホスト版は C): 行の ASCII セルを同時アクセスで描く `vt_blitrow`、
  同じセルを飛ばす `cmpm.l` / `dbne`。simbench が 68000 版の最終 VRAM を C のホスト版と比べるので、そのフィクスチャ
  (`VT_BENCH_LIST`、既定 11 本) で手書き部分も確かめている (作ったときは全フィクスチャと fuzz.py の乱数入力 150 本を
  -c 64 / -c 1400 で流して 348 回とも一致)。

限界 (2026-10-22 時点): top は 4685 セル x 16 バイトの VRAM 書き込みだけで約 1.3M クロック、解析が約 2.7M クロック
かかるので、描画の付帯処理をゼロにしても 16〜17 KB/s 止まり。現状はセルあたり約 800 クロック (うちグリフ転送 ~290)。
解析側は CSI 1 つに 1300 クロック前後 (ESC [ B で 3 バイト)。大域変数の読み書きと関数の出入りが半分を占め、
これ以上は状態をレジスタに置いたままの手書きの解析器が要る。

simbench (Musashi の命令サイクル数) は XEiJ より 0〜10% 速めに出る。`-n` で blit なし、`-P` で関数ごとの正確な
クロック配分 (1 命令ずつ実行。命令アドレスごとのクロックは `VT_OUT/m68ksim/<名前>.pc`)。

### 実機 (X68000 Z) で分かって直したこと (branch ssh、2026-10-22)
- 解析中の `scroll()` が VRAM を動かす前にソフトカーソルを消す (`vt_write` の頭)。消さないとカーソルの塊ごと
  スクロールし、`flush` の消去 XOR が旧位置に塊を残す (Enter でスクロールした行の行末に残骸が出た)。
- `ESC[>...m` (xterm の modifyOtherKeys) を SGR と誤読しない。
- `~` (0x7e) は CGROM だと JIS X 0201 のオーバーラインなので自前のチルダグリフで描く。
- Human68k のコンソール (IOCS) はプレーン 0,1 しか使わず、スクロールもプレーン 0,1 しか動かさない。
  vt_fini 後に残した画面に色 (プレーン 2,3) があると以後のコンソールのスクロールでずれる。ssh.x 側は
  戻す前にプレーン 0,1 に潰している (net/ssh/dropbear/x68k_shim.c)。コンソールの最終行は 30 (31 行)。
- 漢字が 7 区ずれていた: CGROM には 9〜15 区が無く、16 区 (第 1 水準) は 0xF05E00 から (dd8e297)。
- 試験基盤で見つけて直したこと (2026-10-22): `\` が ¥ に見えた (CGROM 0x5c は ¥、バックスラッシュは 0x80) /
  BMP の絵文字 (⏰ ✅ ⚡ …) を 1 桁で数えて行の後ろがずれた (wcwidth 由来の `widthtab.h` に) / CSI 引数の桁あふれと
  大きな DCH・ICH・IL・DL で論理画面の外を書いた / `CSI ? u`・`CSI > 1 u` (kitty キーボード、Claude Code が送る) で
  カーソルを戻していた / 終端の無い OSC が次の CSI の `[` を食った / `38:5:N` の ':' で CSI が切れた /
  DA2・DSR 5n・DECRQM・`CSI 18 t`・OSC 10/11 の色問い合わせに答える (応答は vt_key から、丸ごと入るときだけ積む)。

## キー変換 (`vt_key`)
スキャンコード (IOCS `_B_KEYINP` の bit8-15) で判定する。実機で確認済み:

| キー | スキャン | 送出 (通常 / DECCKM) |
|---|---|---|
| ↑ ↓ → ← | 3c 3e 3d 3b | `ESC [ A/B/C/D` / `ESC O A/B/C/D` |
| HOME | 36 | `ESC [ H` |
| DEL | 37 | `ESC [ 3~` |
| INS | 5e | `ESC [ 2~` |
| ROLL UP / DOWN | 38 / 39 | `ESC [ 5~` / `ESC [ 6~` (PgUp/PgDn) |
| F1–F4 | 63–66 | `ESC O P/Q/R/S` |
| F5–F10 | 67–6c | `ESC [ 15~ … 21~` |
| BS | — | `0x7f` (DEL) |
| 印字・Ctrl・CR・TAB・ESC | — | ASCII をそのまま |

DSR (`ESC[6n`) / DA (`ESC[c`) への応答も `vt_key` から返る。

## 試験
`tests/run.sh` が全部流して PASS/FAIL を数える (Mac。要 `tests/venv` に pyte と Pillow、`tests/fetch_cgrom.sh` で CGROM)。
- **実フォントの描画**: vthost は実機と同じ blit でホスト上の 4 プレーン VRAM に描く (`-g` CGROM、`-v` で VRAM を書き出し、
  `tests/vram2png.py` で 768x512 PNG)。CGROM は XEiJ 同梱の CGROM_XEiJ.DAT を `tests/cgrom/` に写す (git には入れない)。
- **グリフ試験** (`tests/glyphtest.py`): 漢字・かな・1〜8 区の記号・ANK の VRAM を、独立に計算した CGROM の正しい位置
  (16 区以降は 7 区詰める) と比べ、さらにヒラギノで描いた字との最近傍で自分自身か確かめる。7 区詰めを外した変異版で
  落ちることも run.sh が確かめる。
- **pyte 比較** (`tests/compare.py`): 文字は桁ごと、属性 (前景/背景/下線) も比べる。こちらの代用表示 (未対応の字を ASCII/`??` に)
  は許し、pyte の誤読するシーケンス (CSI > 4;2 m を下線と読む、DCS の中身を表示する等) は渡す前に取り除く。pyte が xterm と
  違う場面 (右端の全角、代替画面、DEC 罫線、SU/SD) は `fixtures/*.expect` に期待画面を書く。問い合わせの応答は `*.reply`。
- **分割耐性**: -c 1 / -c 7 でも画面・VRAM が同じ。**VRAM 不変条件** (`vthost -V`): vt_write ごとに VRAM = 論理画面の描き直し。
- **頑健性**: ASan/UBSan 版で全フィクスチャと乱数で壊したシーケンス (`tests/fuzz.py`) を流す。
- フィクスチャ: `gen_fixtures.sh` / `gen_jp.sh` / `gen_emoji.sh` (80x24)、`gen_synth.py` (境界条件)、`gen_apps.sh`
  (96x32 TERM=xterm の tmux / Claude Code / vim / top / less / git log / 絵文字。個人情報は `scrub.py` で同じ長さに置換)。
- `tests/simbench.sh [-c N] [-n] [-p|-P] [REV...]`: 68000 版を Musashi で走らせて bytes/s と関数別の割合、最終 VRAM が
  ホスト版と同じか。`-n` は blit なし、`-p` は PC 標本 (インライン展開なし)、`-P` は 1 命令ずつの正確な配分。
  sim.c は TXRASCOPY と CRTC R21 の同時アクセスを演じる。
- 全角ペアの割れ (`break_pair`): 相方が本当に対の片割れのときだけ消す。ECH などで残った右半分の上に字を書いても、
  左の普通の字は消さない (旧版は消していた。乱数入力 400 本のうち 1 本だけ旧版と違うのはこれ)。

### 以前の試験 (XEiJ)

`tests/run.sh` が、記録した VT100 バイト列 (`tests/fixtures/*.rec`) を Mac 上の
スタブ版 `vthost` と pyte (参照レンダラ) に食わせ、画面が一致するか比べる。XEiJ では
`vtcat.x` をイメージに入れて動かし、出力を同じ形式で比較する。全 8 本:
shell+`ls --color` / vim / less / top / jp_cat (全角 cat) / jp_less (全角 + スクロール) /
emoji_cat (絵文字 cat) / emoji_less (絵文字 + スクロール) が、ホスト・XEiJ とも pyte と完全一致
(pyte は wcwidth で全角・絵文字の幅を追跡)。XEiJ では漢字・絵文字セルの VRAM が CGROM /
`emojitab` のビットマップと一致することも確認。`tests/txtest.c` は TXRASCOPY の仕様確認、
`tests/keyprobe.c` はスキャンコード確認、`tests/pcgtest.c` は PCG スプライト可否の確認用。
フィクスチャ生成: `gen_fixtures.sh` (英語) / `gen_jp.sh` (日本語) / `gen_emoji.sh` (絵文字)。
