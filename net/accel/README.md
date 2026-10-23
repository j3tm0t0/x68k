# net/accel -- Pico W に X25519 / Ed25519 を肩代わりさせる (SSH クライアントの暗号アクセラレータ)

X68000 Z の `ssh.x` (dropbear、[../ssh](../ssh)) は 68000 10MHz で、ハンドシェイク 65 秒のうち 59 秒が
Curve25519 / Ed25519 (keygen 5.6 / shared 19.6 / hostkey verify 26.7 / sign 7.2 秒、crypto68k 組み込み後)。
Z のネットワークを運んでいる Pico W (RP2040、x68kzremotedrv の netpipe) にその計算を頼む。
**既定はオフ** (Z 単体で計算)。`ssh.x -oAccel=verify|kex|all` か環境変数 `ACCEL=...` で段階を選んで有効にする。末尾に `+log` (`-oAccel=all+log`) か `ACCEL_LOG=1` で、Pico の caps と 1 回ごとの所要時間を stderr に出す (既定では出さない)。

| 段階 | 任せる演算 | Pico に渡るもの | 信頼の変化 |
|---|---|---|---|
| `verify` | ed25519 verify (ホスト鍵の署名検証) | 公開鍵・署名・交換ハッシュ (すべて公開情報) | **なし** (Pico が嘘をついても「検証 OK」を返せるだけ。`-y -y` で検証を飛ばすのと同じ程度) |
| `kex` | + X25519 keygen / shared | + 一時秘密鍵 → **Pico がセッション鍵を知る** | Pico (と Wi-Fi 越しにそのファーム) を信頼する |
| `all` | + ed25519 sign (公開鍵認証) | + **署名鍵の seed** | Pico に秘密鍵を渡す |

見込み (Z 実機 65 秒 → `verify` 約 38 秒、`kex` 約 13 秒、`all` 約 6 秒 + 往復)。Pico 側の計算は数十〜数百 ms (下記)。

## 仕組み

```
ssh.x ──accel.c──> npd.sys (ETD 20/21/22) ──ETD 19──> npd.x ──SCSI ID6 (X6NR 交換)──> Pico netpipe.c ──> accel.c (FreeRTOS タスク) ──> Monocypher
      <─ 結果 ──        (ETD 23) <── rec_in ──────── 応答レコード <────────────────── np_exchange <── 結果
```

- **記録形式** ([`accel_proto.h`](accel_proto.h)、Z と Pico で同じファイル):
  npd.x の X6NR 交換 (IP パケットを `[長さ 2][中身][詰め物]` で並べる) に **制御レコード** を混ぜる。
  中身の先頭 1 バイトが `ACCEL_MARK` (0xA5) なら制御レコード。IPv4 のバージョン nibble (4) と重ならないので、
  制御レコードを知らない側 (古い npd.x / 古いファーム) はそれを不正な IP として捨てて次のレコードに進む。
  - Z → Pico: `[0xA5][op][id 2][固定引数][メッセージ]`
  - Pico → Z: `[0xA5][op][id 2][status][reserved][usec 4][結果]` (枠は IP と同じ `[長さ 2][空き 14][中身][詰め物]`)
  - op: `PING` (caps を返す) / `X25519` (scalar 32 + point 32 → 32) / `X25519_BASE` (scalar 32 → 32、基点 9) /
    `ED25519_VERIFY` (pk 32 + sig 64 + msg → status) / `ED25519_SIGN` (seed 32 + pk 32 + msg → 64)
  - status: `OK` / `BADSIG` (署名が違う) / `BADREQ` / `BADOP` / `BUSY` (計算中に別の要求) / `DISABLED` (ファームで無効)
  - **メッセージの上限 1024 バイト** (`ACCEL_MSG_MAX`)。ssh クライアントが署名 / 検証するのは、ホスト鍵の検証 = 交換ハッシュ
    (curve25519-sha256 で 32 バイト、`cli-kex.c` の `buf_verify(..., ses.hash)`) と、公開鍵認証の署名 = `string(session_id) +
    USERAUTH_REQUEST` (`cli-authpubkey.c`、ユーザ名が短ければ 150〜250 バイト)。要求は最大 1124 バイト (3 セクタ)、
    1 回の交換 (8 セクタ = 4080 バイト) に収まるので複数の交換に分ける仕組みは無い。
  - 同時に飛ばせる要求は 1 つ。id は npd.sys が振り、応答の id が違えば捨てる。値はすべて明示的なバイト列 (BE) なので、
    68000 (BE) と RP2040 (LE) で同じヘッダがそのまま使える。
- **Z 側**
  - [`../npd/npdsys.c`](../npd/npdsys.c) (npd.sys 1.1): ETD 命令 **20** 預ける (要求を常駐バッファに写す、id を返す) / **21** 結果を取る
    (0 = まだ、>0 = 応答の長さ) / **22** 取り消す / **23** (npd.x から) Pico の応答 / **24** 統計。**19** (npd.x が送信レコードを取り出す)
    は、預かった要求があれば IP レコードの後ろに制御レコードとして載せる (入らなければ次回)。要求も結果も npd.sys のバッファに
    写すので、要求側 (ssh.x) が途中で死んでもスレッドが解放済みメモリに書くことは無い。
  - [`../npd/npd.c`](../npd/npd.c): `rec_in` が先頭 0xA5 のレコードを ETD 23 に回すだけ。送信は ETD 19 の出力に載るので、
    次のループで (`n > 0` になり) すぐ交換される。
  - [`accel.c`](accel.c) / [`accel.h`](accel.h): ユーザプログラム用の小さなライブラリ。`accel_open()` で npd.sys を探して PING し
    caps を得る。`accel_x25519` / `accel_x25519_base` / `accel_ed25519_verify` / `accel_ed25519_sign`。ETD 呼び出しは
    `_dos_super` で囲み (ユーザモードから呼ぶとバスエラー)、待つ間は `_dos_change_pr` で譲る。時間切れ (既定 15 秒) で
    取り消して -1 を返す → 呼び出し側は自前の計算に落ちる。直前の呼び出しの Pico 計算時間 (µs) と Z の往復時間 (1/100 秒) を読める。
  - [`acceltest.c`](acceltest.c) → `acceltest.x [-n N]`: RFC 7748 (5.2 scalarmult、6.1 keygen / shared) と RFC 8032 (7.1 TEST 1〜3 の
    sign / verify)、改ざん署名の拒否、1024 バイトのメッセージ (3 セクタ)、ssh の大きさ (32 / 200 バイト) を通して OK / FAIL と時間を出す。
- **Pico 側** (x68kzremotedrv の `accel` ブランチ = [`../x68kzremotedrv-netpipe/patches/0011-*.patch`](../x68kzremotedrv-netpipe/patches/))
  - `src/netpipe.c`: `bridge_out_rec` が先頭 0xA5 のレコードを `accel_submit` に渡す。`np_exchange` は応答があれば応答レコードを
    **先頭** に載せる (最初のセクタに入る。`resbuf` を書くのは USB タスクだけなので `rxring` には触らない)。
  - `src/accel.c`: FreeRTOS タスク (優先度 1、スタック 6KB)。起動時に `accel: ready` (タスクを作れなければ `accel: cannot create`)、
    演算ごとに `accel: op N id ... status S ... us` を Pico のログ (UART / 設定画面のログ) に出す。`np_exchange` は USB タスクの MSC 書き込みの中で呼ばれ、戻るまで Z の
    SCSI 書き込みが終わらないので、そこでは計算しない。要求を写して通知 → タスクが計算 → 結果を置く (proxy echo の
    `echo_pkt` / `echo_len` と同じ受け渡し)。計算中に別の要求が来たら `BUSY` を即答。直前と同じ要求 (id と中身が一致) なら
    計算せずに同じ結果を返す (Z が交換を取りこぼして出し直したとき)。
  - `src/accel_ops.c` + `src/monocypher/`: 計算本体。[Monocypher 4.0.2](https://monocypher.org/) (CC0 / BSD-2、
    [`monocypher/`](monocypher/) に必要な 4 ファイルを改変せず同梱。SHA-256 は `monocypher/README.md`)。
    `crypto_x25519` (scalar の clamp は中でする)、`crypto_x25519_public_key`、`crypto_ed25519_check` (0 = OK)、
    `crypto_ed25519_sign` (秘密鍵 64 バイト = seed || pk で、dropbear の priv || pub と同じ並び)。
    Monocypher は小位数の点を弾かない (全零の鍵と署名を「正しい」とする)。dropbear の TweetNaCl 版も同じなので安全性は変わらない。
- **dropbear 側** ([`dropbear/`](dropbear/)、**ssh ブランチにはまだ当てていない**)
  - `dropbear-accel.patch`: 呼び出し元 4 か所だけ。`common-kex.c` の `dropbear_curve25519_scalarmult` 2 回 (keygen / shared)、
    `ed25519.c` の `buf_put_ed25519_sign` / `buf_ed25519_verify`、`cli-runopts.c` の `-oAccel=`。すべて `#if X68K_ACCEL` で囲み、
    Pico に任せられなければ (-1) 元の関数に落ちる。`curve25519.c` (crypto68k のパッチが入る) には触らない。
    ssh ブランチの `patches/*.patch` と重ならないことを dry-run で確認済み。
  - `x68k_accel.c` / `x68k_accel.h`: つなぎ。段階の選択 (`-oAccel=` > 環境変数 `ACCEL` > none)、初回に `accel_open()`、
    caps に無い段階は使わない、`+log` のときだけ経過を stderr に 1 行出す (`accel: ed25519 verify on the Pico in 180000 us (round trip 0.25 s)`)。使えない・失敗して Z で計算し直すときの警告は常に出す。
  - `build-snippet.sh`: `build.sh` に挿す 2 か所 (`ACCEL=1 FAST=1 ./build.sh` で組み込む。組み込んでも既定は使わない)。
    scratchpad に複製した dropbear で `ACCEL=1 FAST=1` のビルドが通り、ssh.x (326KB) に `accel_open` 等がリンクされることを確認した。

## 試験 (ホスト、Mac)

```sh
cd net/accel/tests
python3 -m venv venv && venv/bin/pip install cryptography    # 1 回 (venv は .gitignore 済み。VENV=... で別の場所も可)
./run.sh 300
```

1. `accel_tool roundtrip 2000`: 記録形式の組み立て / 解読の往復 (乱数の op・id・長さ、短い要求・長すぎるメッセージ・違う印の拒否)
2. `npdsim`: 本物の `npdsys.c` を include して ETD 19〜24 の状態遷移 (預ける → 19 で IP の後ろに載る → Pico 側 (`accel_compute`) →
   応答を 23 で受ける → 21 で取る → 取り消し後の遅れた応答は捨てる → 1124 バイトの要求は入らなければ次回)
3. `difftest.py`: `accel_ops.c` + Monocypher を記録形式ごと通して、RFC 7748 5.2 / 6.1、RFC 8032 7.1 TEST 1〜3、乱数 (X25519 300 回、
   keygen 100 回、ed25519 sign / verify 84 本、メッセージ 0〜1024 バイト) を python `cryptography` と照合、改ざんした署名 / メッセージ /
   鍵の拒否
4. ファームウェア側に置いたコピー (`src/accel_proto.h` 等) が一致しているか

2026-10-22: すべて PASS (35 項目、0 失敗)。

## Pico の大きさと速さ

ファーム (netpipe ブランチ e3c98c9 → accel 0011 パッチ、arm-gnu-toolchain 14.3、`-O3`):

| | text | bss |
|---|---|---|
| 0010 (netpipe) | 883,964 | 138,296 |
| 0011 (accel) | 920,636 (+36.7KB) | 140,744 (+2.4KB) + タスクスタック 6KB (ヒープ) |

スタック: `-fstack-usage` + 呼び出しグラフで `accel_compute` → `crypto_ed25519_check` の最深経路 2,096 バイト (`-Os`)。6KB で約 3 倍の余裕。

速さは**実測していない** (Pico に書いていない)。同じソースを Cortex-M0+ 向け (`-O3`) に組んで Unicorn で命令数を数えた見積り
(RP2040 125MHz、CPI 1.0〜1.5):

| 演算 | 命令数 | 見積り |
|---|---|---|
| X25519 (shared / keygen) | 11.5M | 92〜140 ms |
| Ed25519 verify (2 バイト / 200 バイト) | 16.8M / 16.7M | 135〜200 ms |
| Ed25519 sign | 4.8M | 38〜57 ms |

ハンドシェイク 1 回分 (keygen + shared + verify + sign) で 0.4〜0.6 秒 + 交換の往復 (npd.x の問い合わせ間隔 60ms × 数回)。
Z 側の 59 秒に対して 100 倍以上速い。Monocypher の公式ページに M0+ の数字は無く、M0+ は 64 ビット積を持たないので
`fe_mul` の i64 積が効いている。`acceltest.x` が実測 (Pico の µs と Z の往復) を出す。

## 実機でやること (まだ何もしていない)

1. **Pico** (2 台目、netpipe 用): BOOTSEL で `picotool load -x <x68kzremotedrv の作業木>/build/x68kzremotedrv.uf2`。
   設定 (フラッシュ 0x1F0000) は残る。
2. **Z**: CONFIG.SYS が読み込む npd.sys を `net/npd/npd.sys` (1.1、起動時に `npd.sys 1.1 (accel)` と出る) に置き換える
   (A: の HDS イメージなら書き直し。npd.sys 1.0 のままだと ETD 20 が -1 を返し `accel_open` が -1 になる)。
   `net/npd/npd.x` と `net/accel/acceltest.x` を B: に置く。リセット。`npd.x 192.168.1.68` (B:PNETN.BAT 等) でスレッドを上げる。
3. `b:acceltest.x -n 3` → 全部 OK、Z の往復と Pico の µs を記録する。`FAIL accel_open = -2` なら Pico が答えていない
   (古いファーム / npd.x が動いていない)。`sst.x` に ETD 24 は無いので、必要なら ETD 24 で req/res/stale を見る道具を足す。
4. **ssh**: ssh ブランチの `build.sh` に `dropbear/build-snippet.sh` の 2 か所を挿し、`ACCEL=1 FAST=1 ./build.sh` →
   `b:ssh.x -oAccel=verify -y -y -i b:\id_x68k moto@192.168.1.108 uname -a` (65 → 約 38 秒)、次に `-oAccel=kex` (約 13 秒)、
   `-oAccel=all` (約 6 秒)。`-oAccel=all+log` なら stderr の `accel:` 行に Pico の時間が出る。実機の実測は verify 43 秒、kex 19 秒、all 12 秒 (2026-10-22、TCP の往復と 326KB の読み込みを含む)。
