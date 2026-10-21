# crypto68k -- 68000 向けの高速 Curve25519 / Ed25519 体演算

dropbear (`curve25519.c`, TweetNaCl 版) は 2^255-19 の体を `gf = i64[16]` (radix 2^16) で持ち、
乗算 `M` が毎回 16x16=256 回の **i64 積 (`__muldi3`)** を呼ぶ。68000 には 64bit 乗算が無いので
これが遅く、X25519 の scalarmult 1 回に 10MHz で約 2 分 (実測 118 s)、ハンドシェイク (scalarmult を
数回 + ed25519 sign/verify) に数分〜十数分かかる。

ここでは `M` / `S` を、68000 の **MULU.W (16x16->32)** 向けに書き直した `M_fast` / `S_fast` に
差し替える。外部関数・結果は dropbear のものと同一 (mod p で一致) なので、`curve25519.c` に小さな
パッチを当てて `field_fast.h` と `mul68k.s` を足すだけで使える。

## 速度 (XEiJ、`-model=SUPER` = 68000 10MHz。`_iocs_ontime` によるエミュレート時間 = 実機相当)

X25519 scalarmult 1 回:

| 版 | 時間 | 対 元 |
|---|---|---|
| 元 (TweetNaCl, `__muldi3`) | 118 s | 1x |
| C 版 `M_fast`/`S_fast` (MULU + car25519 排除 + 二乗専用) | 22 s | 5.4x |
| + 68000 asm comba 乗算 (`mul68k.s`) | 10 s | 12x |
| **+ 逆元/√ を加算鎖に (254S+252M → 254S+11M)** | **9.1 s** | **13x** |

操作別 (asm 版、`tests/bench_ops.c`):

| 操作 | 時間 | 構成 |
|---|---|---|
| X25519 shared (scalarmult) | 9.1 s | Montgomery ラダー 255 段 |
| X25519 keygen (base=9) | 9.0 s | 同上 (固定基点だが汎用ラダー) |
| Ed25519 sign | 19 s | Edwards scalarbase + SHA-512 |
| **Ed25519 verify** | **37 s** | Edwards scalarmult 2 本 + √ + 逆元 |

MULU だけで scalarmult 1 回の理論下限が ~3 s。10MHz でハンドシェイク (keygen+shared+verify ≈
55 s) は、Z 実機 (XEiJ SUPER の 2〜3 倍遅い) で ~130〜160 s → OpenSSH の 120 s LoginGraceTime に
ぎりぎり届かない。**あと ~2 倍**には固定基点の事前計算が要る (下記)。

## 残りの最適化 (未実装、効果の大きい順)
- **固定基点テーブル (comb/窓) で Ed25519 の `scalarbase`**: sign と verify の [S]B はベース B が固定なので
  倍数表を前計算すれば倍加を大幅に省ける (4〜8 倍)。verify を ~37 s → 20 s 台、sign を ~19 s → ~5 s に。
  ハンドシェイクが Z 実機で 120 s を切る見込み。表は符号付き窓で小さく保つ。
- **Straus の 2 重スカラー**: verify の [S]B − [h]A を 1 本のラダーで (倍加を共有)。
- **comba のアキュムレート最適化** (演算子スキャン・レジスタ常駐): M を 10MHz で ~5〜6 s へ。
いずれも同じ検証 (cryptography 差分 + asm は Unicorn) で進められる。

## 中身

| ファイル | 役割 |
|---|---|
| `field_fast.h` | `M_fast` / `S_fast` 本体。`FAST_FIELD` で有効、`USE_ASM` で asm 乗算を使う |
| `mul68k.s` | 68000 の手書き comba 乗算 `fe_mul16` / 二乗 `fe_sqr16` (完全展開) |
| `gen_mul_asm.py` | `mul68k.s` の生成器 |
| `curve25519.patch` | dropbear の `curve25519.c` に当てるパッチ (M/S を差し替え) |
| `tests/` | 差分テスト・ベンチ一式 (下記) |

### 設計
- 入力リムは A/Z の出力で ±2^17 まで振れ符号付きなので、まず **32bit の符号付きキャリー**で
  [0,2^16) に正規化 (`norm_in` / `norm_in_u16`)。元 `M` は i64 の `car25519` を 6 回呼ぶが、これが
  実は最大の律速だったので、ここを 32bit 化し car25519 を体演算から排除した。
- 乗算は radix 2^16 の comba (積スキャン)。積は `MULU.W`、64bit アキュムレータ (`d4:d3`) に足し、
  列ごとに下位 16bit を出して 16bit 右シフト。値は <2^37 なので `d4` は 5bit で、`swap` で畳める。
- 二乗は対称性で積を約半分 (i<j の項を 2 倍)。2 倍は積を 2 回足して行う (`add.l d0,d0` は ~2^32 の
  積で 33bit 目が溢れるため不可)。
- 2^256 ≡ 38 で 512bit 積を畳み込み (`reduce_fold`)、[0,2^16) の緩い形で返す (最終整約は元の
  `pack25519` が行う)。
- `movem.l` は使わず個別 push/pop (Unicorn の 68000 が `movem` を実行できないため。実機でも同じ)。

## dropbear への組み込み

1. `field_fast.h` と `mul68k.s` を dropbear のソースに置く。
2. `curve25519.c` に `curve25519.patch` を当てる (M/S の中で `M_fast`/`S_fast` を呼ぶだけ)。
3. ビルドで `-DFAST_FIELD -DUSE_ASM` を付け、`mul68k.s` をアセンブルしてリンクに加える
   (elf2x68k: `m68k-xelf-as -m68000 mul68k.s`)。asm を使わず C だけなら `-DFAST_FIELD` のみ (22s)。

`gf` / `u32` / `u64` / `i64` / `car25519` は `curve25519.c` のものをそのまま使う。
`field_fast.h` は `car25519` の定義より後ろで include される (パッチがそうする)。

## テスト

信頼できる参照 (Python `cryptography`) を正として、RFC 7748 (X25519) / RFC 8032 (ed25519) の
固定ベクタ + 多数のランダム入力で、元 `M` 版 (ref) と `M_fast` 版 (fast) の両方を照合する。
手書き asm は Mac 上の Unicorn (68000) で C 参照と直接突き合わせる。xiptest と同じ発想。

```sh
cd tests
# ホスト: ref と fast を cryptography と差分比較 (C 版 M_fast)
cc -O2 -I.. -o /tmp/ref  x25519_tool.c sha512.c
cc -O2 -I.. -DFAST_FIELD -o /tmp/fast x25519_tool.c sha512.c
venv/bin/python difftest.py /tmp/ref /tmp/fast 1000

# asm (fe_mul16/fe_sqr16) を Unicorn の 68000 で C 参照と照合
m68k-xelf-as -m68000 -o /tmp/mul68k.o ../mul68k.s
venv/bin/python unicorn_mul.py /tmp/mul68k.o 500

# asm 経路の norm/reduce もホストで差分比較 (fe_mul16 を C スタブに)
cc -O2 -I.. -DFAST_FIELD -DUSE_ASM -DHOST_ASM_STUB -o /tmp/astub x25519_tool.c sha512.c
venv/bin/python difftest.py /tmp/ref /tmp/astub 500

# X68000 実機 (XEiJ) ベンチ
m68k-xelf-gcc -O2 -m68000 -I.. -DFAST_FIELD -DUSE_ASM -DITERS=10 \
    -o bench.x bench68k.c sha512.c ../mul68k.s   # RFC7748 を確認しつつ計測
```

結果: 全差分テスト 0 失敗、Unicorn 500/500 一致、XEiJ で RFC 7748 RESULT OK。
`venv` は `pip install pyte pynacl cryptography unicorn Pillow` 済みのもの (term/vt100/tests/venv を流用可)。

ライセンス: dropbear の `curve25519.c` は MIT / TweetNaCl は public domain。ここで足したコードは MIT。
