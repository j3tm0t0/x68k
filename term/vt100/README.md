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
`tests/run.sh` が、記録した VT100 バイト列 (`tests/fixtures/*.rec`) を Mac 上の
スタブ版 `vthost` と pyte に食わせ、画面が一致するか比べる。実機は `vtcat.x` を
イメージに入れて XEiJ で動かし、出力を同じ形式で比較する。
