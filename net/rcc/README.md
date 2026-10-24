# rcc: Z の gcc の代わりに、LAN の向こうの gcc でコンパイルする

Z の上の gcc (真里子版 1.42) は遅い。`rcc.x` は Z の `gcc` と同じ引数を受け取り、入力のファイルを TCP で LAN の別の機械に送る。
そこで elf2x68k の gcc 13 がコンパイル・リンクし、できたファイルを Z の gcc と同じ場所に書く。
相手につながらないときは、1 行知らせて Z の gcc でコンパイルする。

この先、受ける側を Pico 2 に移す (x68k-zos #18)。Z の `rcc.x` と要求・応答の中身はそのままで、運び方だけを替える
(今は TCP、Pico では SCSI ID 6 の交換の記録)。

| ファイル | 中身 |
|---|---|
| `rcc.x` (`rcc.c`) | Z で動かす。libsocket なので xip.x でも `tis.x -p` でも動く |
| `rccd.py` | 受ける側 (Linux / macOS、Python 3)。elf2x68k の `m68k-xelf-gcc` を呼ぶ |
| `tests/rccd_test.py` | `rccd.py` をホストで試す (出力の名前、パスの書き換え、エラーの名前と行) |
| `tests/xeij_rcc.sh` | XEiJ + picosim (`tis.x -p`) で Z から試す |

## 使い方

受ける側 (elf2x68k の `m68k-xelf/bin` に PATH を通しておく):

```sh
python3 rccd.py --port 7790 --allow 192.168.1.0/24
```

Z (zosh なら `/etc/zoshrc` の `gcc()` から呼ぶ):

```
set RCC_HOST=<受ける側の名前か番地>        (":ポート" も可。既定のポートは 7790)
rcc.x -O hello.c -o hello.x
rcc.x -c a.c b.c                           (a.o と b.o が今のディレクトリにできる)
```

- 引数は Z の gcc と同じ: `-O*`、`-o`、`-c`、`-S`、`-E`、`-I`、`-D`、`-U`、`-l`、`-L`、`-g`、`-W*` など。
  `-o` が無ければ `a.x`、`-c` / `-S` なら元の名前の `.o` / `.s` を今のディレクトリに書く。`-E` は標準出力に出す
- Z の gcc だけのライブラリは受ける側で読み替える: `-lgcc2`・`-ldos`・`-liocs`・`-lsignal` は付けない (elf2x68k の libc に入っている)、
  `-lnetwork` は `-lsocket`。`-L` は使わない
- 送るもの: 入力 (`.c` `.cc` `.cpp` `.s` `.o` `.a`)、`-include` のファイル、今のディレクトリ・ソースのディレクトリ・`-I` のディレクトリの `*.h`
- メッセージのファイル名と行番号は Z で書いたとおり (`A:\SUB\E.C:1: ...`)。失敗したら出力のファイルを作らない。終了コードはコンパイラのもの
- 大文字の `.C` は C として扱う (Human68k の名前は大文字になる。gcc は `.C` を C++ と見る)
- `RCC_HOST` が無いときは何も言わずに Z の gcc で。相手が居ないときは `rcc: no compile server, compiling here`、途中で切れたときは
  `rcc: lost the compile server while sending (receiving), compiling here` と出して、
  環境変数 `RCC_LOCAL` (既定 `gcc.x %s -lgcc2`。`%s` に引数が入る) を `system()` で動かす。`RCC_LOCAL` を空にすると 255 で終わる
- `-c` でできる `.o` は ELF (elf2x68k の形式) で、Z の HLK.X ではリンクできない。リンクも rcc.x で行う。
  つながらずに Z の gcc でリンクするとき、引数に ELF の `.o` があれば `rcc: x.o was made by the compile server (ELF); HLK can't link it, rebuild it here` と出す
- システムのヘッダ (`A:\usr\include` など) は送らない。受ける側が自分のヘッダとライブラリを持つ。
  環境変数 `RCC_SYS` を付けると、その名前を `SYS` の行で送り、受ける側の組と違えば断られる

## やり取り (版 2)

1 回の接続で 1 回のコンパイル。受ける側は何も覚えない (ファイルは一時ディレクトリに置き、返したら消す)。
HTTP (AWS Lambda など) や Pico の受け手に移すときも、同じ中身を運べばよい。行は LF で終わる。数は 10 進。

```
Z → 受ける側:  RCC2
               CWD <バイト数>\n<Z の今のディレクトリ>       (参考。"A:\work" など)
               SYS <名前>                                  省略可。ヘッダとライブラリの組 (rccd の既定は elf2x68k-newlib)
               ARG <バイト数>\n<引数>                      Z で打った引数をそのまま、順に (何回でも)
               FILE <バイト数> <Z のパス>\n<中身>           入力とヘッダ。パスは引数に書いたとおり (何回でも)
               END
受ける側 → Z:  RCC2 <終了コード> <標準出力のバイト数> <メッセージのバイト数> <出力のファイルの数>
               <標準出力><メッセージ (パスは Z の書き方)>
               OUT <バイト数> <Z のパス>\n<中身>            出力 (.x / .o / .s)。Z の gcc が書く場所 (何回でも)
```

- 受ける側が Z のパスを自分の置き場所に読み替える。相対パスは作業ディレクトリから、ドライブ付き (`A:\inc\x.h`) は
  その外の `_A_/inc/x.h` に置く。作業ディレクトリの外に出るパスは断る (終了コード 126)
- 受ける側は `--allow` (既定 192.168.0.0/16 と 127.0.0.0/8) の外からの接続を断る。1 ファイル 4MB、合わせて 16MB まで。
  コンパイラは `--timeout` 秒 (既定 120) で止める
- 長さが中身より先に来るので、受ける側は全体をメモリに溜めずに、届いた順にファイルへ書ける (RAM の少ない Pico 2 の受け手向け)。
  1 ファイルの上限は受け手が決めてよい (Pico 2 なら 1MB など)。細かく分けて送ることと流量の制御は、運ぶ層 (TCP、ID6 など) の仕事

## 試験

```sh
python3 -I net/rcc/tests/rccd_test.py      # ホストだけで (m68k-xelf-gcc が要る)
net/rcc/tests/xeij_rcc.sh                   # XEiJ の Z から tis.x -p 越しに
```

## 速さ (X68000 Z の実機、xip.x、受ける側は LAN の Raspberry Pi 5)

rcmd で 3 回ずつ動かした時間 (rcmd の往復の約 1.3 秒を含む)。Z の gcc は `gcc.x 〜 -lgcc2`、rcc は `rcc.x -O 〜`。

| | hello.c (6 行) | big.c (1,284 行) | zosh.c (1,034 行) |
|---|---|---|---|
| Z の gcc、道具とヘッダが A: (SMB の HDS) | 17.3 / 17.4 / 17.2 秒 | 247.4 / 247.5 / 245.9 秒 | コンパイルできない (`x68k/dos.h` が無い) |
| Z の gcc、道具とヘッダが C: (SD カード) | 26.0 / 26.1 / 26.0 秒 | 247.1 / 246.8 / 247.1 秒 | — |
| rcc.x → raspi5 | 6.5 / 6.6 / 6.5 秒 | 11.2 / 10.4 / 11.5 秒 | 8.7 / 8.6 / 8.6 秒 |

受ける側の gcc は 0.1〜0.6 秒。残りは Z からの送り (big.c は 29KB で 1.6 秒、zosh.c は 47KB で 2.1 秒) と、rcc.x と結果の受け取り。

