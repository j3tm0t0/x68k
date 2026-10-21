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

## 画面と描画
- 既定 96x32 (768x512, 8x16 フォント)。`vt_init(cols,rows)` で指定可 (横最大 96・縦最大 32)。
- セルは「コードポイント + 幅フラグ + 属性」(`cell_t`)。UTF-8 デコーダ (`utf8_feed`) と描画層 (`blit`) は分離。
- セルごとに論理画面 (`cur`) と VRAM の控え (`drawn`) を持ち、`flush` は変わったセルだけ描く。
- スクロールの上方向は IOCS TXRASCOPY (`_iocs_txrascpy`) でラスタをハードコピーする
  (1 ラスタ = 4 走査線、行 = 4 ラスタ)。下方向は降順フラグ未検証のため 32bit 転送。
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
ZWJ (U+200D) は幅 0 として無視。表に無い絵文字は `??` フォールバック (幅 2 を保つ)。

- 表 `emojitab.h` は `genemoji.py` が **Twemoji 15.1.0 (CC-BY 4.0, © 2023 Twitter/X, jdecked/twemoji)** の
  72x72 PNG を 16x16 に縮小し、**16 色 (ANSI テキストパレット) に量子化**して生成 (元 PNG は含めず変換後の表のみ収録)。
- **割り切り**: 色はテキストパレットの ANSI 8+8 色 (SGR と共用、パレットは変更しない) に量子化するので粗い。
  透明ピクセルはパレット 0 (黒) にする (黒背景の端末前提)。重なり・透過・アニメーションは無い。現在 28 文字。
- PCG スプライトを使わない理由: スプライト/BG は 256x256・512x512 モード専用で、CLI の 768x512 高解像度テキスト
  (CRTMOD 16) では `_iocs_sp_init()` が **-1** を返し使えない (`tests/pcgtest.c` で XEiJ 確認。4/6/8/12 のモードなら 0)。
  512x512 に落とせば使えるがテキストが 64 桁に半減する。テキスト VRAM 直描きなら高解像度のままスクロールにも乗る。

### スループット (計測: XEiJ、vt_write を 30 回。10MHz 68000 は SLIP ≒ 3KB/s に追いつく水準)
| モデル | top (スクロール少) | vim (スクロール多) |
|---|---|---|
| X68030 (XEiJ 既定) | 13916 B/s | 11658 B/s |
| X68000 10MHz (`-model=SUPER`) | 4045 B/s | 3495 B/s |

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
`tests/run.sh` が、記録した VT100 バイト列 (`tests/fixtures/*.rec`) を Mac 上の
スタブ版 `vthost` と pyte (参照レンダラ) に食わせ、画面が一致するか比べる。XEiJ では
`vtcat.x` をイメージに入れて動かし、出力を同じ形式で比較する。全 8 本:
shell+`ls --color` / vim / less / top / jp_cat (全角 cat) / jp_less (全角 + スクロール) /
emoji_cat (絵文字 cat) / emoji_less (絵文字 + スクロール) が、ホスト・XEiJ とも pyte と完全一致
(pyte は wcwidth で全角・絵文字の幅を追跡)。XEiJ では漢字・絵文字セルの VRAM が CGROM /
`emojitab` のビットマップと一致することも確認。`tests/txtest.c` は TXRASCOPY の仕様確認、
`tests/keyprobe.c` はスキャンコード確認、`tests/pcgtest.c` は PCG スプライト可否の確認用。
フィクスチャ生成: `gen_fixtures.sh` (英語) / `gen_jp.sh` (日本語) / `gen_emoji.sh` (絵文字)。
