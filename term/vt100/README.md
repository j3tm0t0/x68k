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

## 試験
## 画面と描画
- 既定 96x32 (768x512, 8x16 フォント)。`vt_init(cols,rows)` で指定可 (横最大 96・縦最大 32)。
- セルごとに論理画面 (`cur`) と VRAM の控え (`drawn`) を持ち、`flush` は変わったセルだけ描く。
- スクロールの上方向は IOCS TXRASCOPY (`_iocs_txrascpy`) でラスタをハードコピーする
  (1 ラスタ = 4 走査線、行 = 4 ラスタ)。下方向は降順フラグ未検証のため 32bit 転送。
- 色は 8 + 8 (bright) をテキストパレット (0xE82200) に割り当て、reverse/underline を含め
  グリフとプレーンの論理で表す。非 ASCII は `?` (全角相当は `??` で桁を保つ)、
  Unicode 罫線 (U+2500〜) は自前の DEC 罫線グリフに寄せる。

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
スタブ版 `vthost` と pyte (参照レンダラ) に食わせ、画面が一致するか比べる。実機は
`vtcat.x` をイメージに入れて XEiJ で動かし、出力を同じ形式で比較する
(shell+`ls --color` / vim / less / top の 4 本がホスト・実機とも pyte と完全一致)。
`tests/txtest.c` は TXRASCOPY の仕様確認、`tests/keyprobe.c` はスキャンコード確認用。
