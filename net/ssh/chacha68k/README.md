# chacha68k -- 68000 向けの ChaCha20 / Poly1305 (SSH の chacha20-poly1305@openssh.com)

dropbear の通信路の暗号は chacha20-poly1305 だけにしてある (`../dropbear/localoptions.h`)。
libtomcrypt のままだと Z 実機で **約 1.5 KB/s** (`net/ssh/crypttest`、232 KB に 157 秒) で、
平文の TCP (9〜11 KB/s) や VT100 の描画 (~14 KB/s) より遅く、SSH セッション全体の上限になっていた。
ここではその鍵ストリームと MAC を 68000 向けに書き直し、dropbear の `chachapoly.c` から呼ぶ。
結果は libtomcrypt とバイト単位で同じ。

## 速さ

Musashi (68000 のサイクル数、MULU は 38+2n) で 16 KB のパケット 1 つ (クロック/バイト):

| | ChaCha20 | Poly1305 | 合計 | 10MHz で |
|---|---|---|---|---|
| libtomcrypt | 394 | 1990 | 2384 | 4.2 KB/s |
| C 版 (cp68k.c) | 394 | 882 | 1276 | 7.8 KB/s |
| asm (MULU の comba) | 233 | 412 | 646 | 15.5 KB/s |
| **asm (Poly1305 を表引き)** | **233** | **193** | **426** | **23 KB/s** |

XEiJ (`-model=SUPER`、10MHz、`_iocs_ontime`) の `tests/cpbench.x`、16 KB x 8 パケット:

| | B/s |
|---|---|
| libtomcrypt | 4000 |
| cp68k (MULU の Poly1305 だけ) | 14515 |
| **cp68k** | **22103** (5.5 倍) |

52 バイトのパケット (打鍵・エコー) 100 個: libtomcrypt 2.23 s → 0.93 s。
libtomcrypt の Poly1305 は 26 ビットのリムの積を `ulong64` で取るので、68000 では `__muldi3` /
`__mulsi3` がパケットの時間の 69% を占めていた (`tests/csim` の関数別)。

**Z 実機は未計測** (計測しようとした時点で Z の rcmd に届かなかった)。libtomcrypt で Z は XEiJ の
約 2.7 倍遅い (1.5 KB/s 対 4.0 KB/s) ので、その比のままなら Z では ~8 KB/s。

## Poly1305

h (< 2^132) を 16 ビットのリム h0..h8、r を r0..r7 に分ける。積 P = h*r (< 2^256) を求め、
2^130 = 5 (mod p) で畳む:

    P = L + 2^128 Q            (L = P mod 2^128)
      = L + 2^128 (Q & 3) + 5 (Q >> 2)
      = L + 2^128 (Q & 3) + M + (M >> 2)      (M = Q & ~3)

h' < 2^130 + 2^129 なので次のブロックを足しても h < 2^132 に収まる。最後の整約とパッドの加算は C
(`poly68k_finish`)。C 版 `poly68k_blocks` がこの手順の仕様で、asm はブロックごとに同じ h を返す。

### MULU の comba (`poly68k_blocks_asm`、短いパケット用)
- h の 128 ビットを `%d4-%d7` に 32 ビットずつ。1 本のレジスタの下位語と上位語がリム 2 つで、
  MULU.W は下位語しか見ないので上位語のリムは `swap` で下ろす (生成器が状態を追う、28 回/ブロック)。
- r0..r6 は `%a0-%a6` (`move.w %aj,%d0 ; mulu.w %dh,%d0`)、r7 と h8 はスタック。h8 (< 16) をソースに
  すると MULU が速い (38+2n の n が小さい)。
- 列ごとに 64 ビット (`%d1:%d2`、`add.l` + `addx.l`) で足し、下位 16 ビットを P[k] に出す。
- 1 ブロック ~6600 クロック。

### 表引き (`poly68k_blocks_tab`、22 ブロック = 352 バイト以上)
r はパケットごとに決まる (鍵は ChaCha20 のブロック 0 から) ので、パケットの最初に
TA[b] = b*r と TB[b] = b*(r<<8) (b = 0..255) を作り、h のバイト k ごとに表の項を 8k ビットずらして足す。
MULU を使わない。

- 項の中身はビッグエンディアンで `+4: V4 +8: V3 +12: V2 +16: V1 +20: V0 +24: 0`。**+24 から `-(An)` で
  5 長語読むと V、+26 から読むと V << 16** (語境界なので 68000 でも読める)。長語の中の 4 つのバイト位置
  (0, 8, 16, 24 ビット) を TA / TB の 2 表で賄える。
- 表は 64KB の領域 1 つ。b の枠は `base + (b<<8 を符号付き語とみた値)` の 256 バイト (TA が +0、TB が +32)。
  h のバイトを語の上位バイトに置いておけば `move.w` 1 回で `b << 8` が取れ、そのまま
  `lea d8(base,%d5.w)` の添字になる。h の長語は `movep.l` で 1 バイトおきに散らす (下位バイトは 0 のまま)。
- 途中の和は `(h mod 2^8(k+1)) * r < 2^(8k+132)` なので、項を足す長語 q..q+4 の外へ桁上げしない。
  積の窓 A[q..q+4] を `%d0-%d4` で回し、確定した A[q] はスタックへ出してそのレジスタを A[q+5] に使う。
- 1 ブロック ~3000 クロック (バイトごとに添字 24 + 5 長語の add/addx 104)。表を作るのに ~6.7 万クロック
  (512 項)。打鍵のような短いパケットは MULU 版のまま。境目は `poly68k_tab_min` (22 ブロック)。

## ChaCha20 (`chacha68k_xor_asm`)

- 20 ラウンドを完全展開 (80 クォーターラウンド、~4.5KB)。16 語に対してレジスタは 15 本 (`%d0-%d7`,
  `%a0-%a6`) なので、`gen_chacha.py` がレジスタ割り当てを追う:
  - `a += b` / `c += d` は足される側が An でもよい (`adda.l` は Dn と同じ 8 クロック)。
  - `d = rotl(d ^ a, n)`: EOR のソースは Dn に限る。a が An なら空き Dn に写して `eor.l Dd,Df` とし、
    結果は Df に置く (旧 Dd が空く = 名前の付け替え)。
  - 回転は 16 = `swap`、12 = `swap` + `ror.l #4`、8 = `rol.l #8`、7 = `rol.l #7`。
  - 足りなければ次に使うのがいちばん先の語を空き An かスタックへ追い出す。
  クォーターラウンドの下限 ~150 クロックに対し、追い出しは半ラウンドに 2 回ほど (ラウンド部 13.3k/ブロック)。
- 鍵ストリームはリトルエンディアンなので、語ごとに `rol.w #8 / swap / rol.w #8` してから入力と XOR。
- 64 バイト単位は asm が in/out を直接読み書きする (偶数番地のとき)。端数 (Poly1305 の鍵 32 バイト、
  長さの 4 バイト、パケットの末尾) と奇数番地は asm で鍵ストリームを 1 ブロック作って C で XOR。
- カウンタは x[12..13] の 64 ビット (dropbear は `chacha_ivctr64`)。

## dropbear への組み込み

`../dropbear/build.sh` が `CHACHA68K=1` (既定は `FAST` と同じ) のとき:

1. `cp68k.h` / `cp68k.c` を dropbear のソースに置き、`chachapoly.patch` を当てる。
   鍵の展開 (`chacha_setup` / `chacha_ivctr64`) は libtomcrypt のまま、`chacha_crypt` /
   `chacha_keystream` / `poly1305_*` の呼び出しを `chacha68k_xor` / `poly68k_mac` に替えるだけ。
2. `chacha68k.s` / `poly68k.s` をアセンブルし、`cp68k.c` を `-DCP68K_ASM` で組んでリンクする。
   `CHACHA68K_C=1` なら asm 無しの C 版 (Poly1305 だけ速い、7.8 KB/s 相当)。

BSS が 64KB 増える (Poly1305 の表)。

## ファイル

| ファイル | 役割 |
|---|---|
| `cp68k.h` / `cp68k.c` | 入口 (`chacha68k_xor`, `poly68k_mac`) と C 版。`CP68K_ASM` で asm を使う |
| `gen_poly.py` → `poly68k.s` | Poly1305 の MULU 版・表の生成・表引き版 |
| `gen_chacha.py` → `chacha68k.s` | ChaCha20 (生成器がレジスタ割り当て) |
| `chachapoly.patch` | dropbear の `chachapoly.c` (`CHACHA68K` で切り替え) |
| `tests/` | 下記 |

## テスト

正解は python `cryptography` (`tests/ref.py` が chacha20-poly1305@openssh.com を組む)。

| 道具 | 内容 |
|---|---|
| `tests/csim.c` | Musashi で素のバイナリ (`start.s` → `bench()`) を走らせ、クロック数・区間・関数別 (PROF=1) と出力を取る。奇数番地の語アクセスで止まる |
| `tests/mk.sh` | csim 用のバイナリを作る (libtomcrypt は work/ のものをリンク) |
| `tests/bench.c` / `benchcp.c` | libtomcrypt / cp68k で 1 パケット (区間 1 = chacha、2 = poly) |
| `tests/dbtest.c` | **パッチを当てた dropbear の `chachapoly.o` そのもの**: encrypt、getlength、その場の decrypt、1 バイト改ざんの拒否 |
| `tests/difftest.py N CMD...` | 長さ 4..35000 の乱数パケット (境目を多め) で CMD の出力を `ref.py` と比べる |
| `tests/rfc8439.c` | RFC 8439 2.3.2 / 2.4.2 (長さ 1..114、偶数・奇数番地) / 2.5.2、表引きと MULU の一致 |
| `tests/hosttool.c` | ホスト (C 版) で同じ計算 |
| `tests/cpbench.c` + `build_cpbench.sh` | X68000 用: libtomcrypt と同じ結果かを 16..32780 バイトで確かめ、速さを比べる |
| `tests/xeij_cpbench.sh` | XEiJ SUPER で cpbench.x を走らせて結果を出す |

```sh
cd tests
export S=/tmp/cc; mkdir -p $S
M=$TMPDIR/vttest/m68ksim/build   # Musashi (term/vt100/tests/simbench.sh が作る)
cc -O2 -w -I$M -o $S/csim csim.c $M/m68kcpu.c $M/m68kops.c $M/m68kdasm.c $M/softfloat/softfloat.c
V=../../../term/vt100/tests/venv/bin/python    # cryptography 入り
./mk.sh cpx -DCP68K_ASM benchcp.c ../cp68k.c ../poly68k.s ../chacha68k.s
$V -I difftest.py 200 $S/csim $S/cpx.bin $S/cpx.nm
D=../../dropbear/work/dropbear-2022.83     # FAST=1 ./build.sh の後
./mk.sh dbt -DCP68K_ASM dbtest.c $D/chachapoly.o ../cp68k.c ../chacha68k.s ../poly68k.s
$V -I difftest.py 100 $S/csim $S/dbt.bin $S/dbt.nm
./mk.sh rfc -DCP68K_ASM -DCSIM rfc8439.c ../cp68k.c ../chacha68k.s ../poly68k.s && $S/csim $S/rfc.bin $S/rfc.nm
cc -O2 -I.. -o $S/rfc rfc8439.c ../cp68k.c && $S/rfc
./build_cpbench.sh $S/cpbench.x && CP_OUT=$S ./xeij_cpbench.sh $S/cpbench.x
```

結果 (2026-10-22): difftest は csim (asm) 200 + dbtest 100 + ホスト (C) 300 パケットすべて一致、
RFC 8439 はホスト・csim (asm / C) とも OK、XEiJ の cpbench は 16..32780 バイトの全長で libtomcrypt と同一。

ライセンス: ここで足したコードは MIT。
