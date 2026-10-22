# zterm -- X68000 Z のためのリモート端末 (mosh / Orca 風の画面差分)

X68000 Z (68000 10MHz) は全画面の VT/xterm 出力を解釈・描画しきれない (Claude Code の再描画で実機 1 FPS 程度)。
そこで端末エミュレーションをリモート側に移す: 小さな Rust のヘルパ `zterm` がリモートでシェルを pty で動かし、
画面を自分で持ち、**Z が描ける形のセルの差分だけ**を ssh のチャネルで Z に送る。Z はセルを置いて描くだけ。
ヘルパは接続のたびに Z から押し込む (VS Code Remote のサーバや mosh-server と同じ考え) ので、リモートに何も入れておかなくてよい。

```
 Z (ssh.x -Z)                                   リモート (sshd)
 ┌──────────────────┐   1 本の exec チャネル    ┌──────────────────────────────┐
 │ ztclient.c        │ ── 起動スクリプト ──────▶ │ sh: キャッシュを見る          │
 │  ヘルパ転送       │ ◀─ ZTERM NEED/OK/GO ──── │  無ければ受け取って展開       │
 │ ztdraw.c          │ ◀─ フレーム (セル差分) ── │ zterm: pty + vt100 crate      │
 │  (vt100.c の画面) │ ── キー / ACK ─────────▶ │  Z のセルに変換・差分・間引き │
 └──────────────────┘                           └──────────────────────────────┘
```

| ディレクトリ | 中身 |
|---|---|
| `server/` | リモート側ヘルパ (Rust)。`src/main.rs` pty・ループ・問い合わせ応答、`src/zcell.rs` Z のセルへの変換、`src/proto.rs` 差分の符号化、`src/dec.rs` DEC 罫線の前処理、`build.rs` term/vt100 の表から字の表を作る、`dist.sh` 4 アーキテクチャのビルド |
| `client/` | Z 側。`ztclient.c` 起動スクリプト・転送・フレーム・ACK、`ztdraw.c` フレームを term/vt100 の論理画面に置いて描く、`zthost.c` Mac で動かす試験クライアント、`ztreplay.c` フレーム列の往復試験 |
| `tests/` | `replay.sh` (vt100.c との一致・往復)、`simbench.sh` (68000 の描画時間)、`play.py` / `spinner.py` (帯域試験の負荷) |
| `dist/` | `dist.sh` の出力 (git に入れない)。Z の `B:/zterm/` に写すもの |

## 使い方

```sh
cd term/zterm/server && ./dist.sh          # ../dist に linux-aarch64.gz ほか 4 本と VERSION
cd net/ssh/dropbear && ZTERM=1 FAST=1 ./build.sh   # ssh.x (-Z が使える版)
```
Z では `B:/zterm/` に `dist/` の `*.gz` と `VERSION` を置き、

```
b:ssh.x -Z -i b:\id_x68k moto@192.168.1.108          シェル
b:ssh.x -Z -i b:\id_x68k moto@192.168.1.108 top     コマンド
```
置き場所は環境変数 `ZTERMDIR` で変えられる (既定 `B:/zterm`)。ヘルパが無い・動かないときは同じ接続のまま
`-t` の VT100 セッションに切り替わる。

## 起動 (1 本の exec チャネル)

dropbear のクライアントは 1 接続 1 セッションの作りで、ハンドシェイクは Z で 13〜65 秒かかるので、
確認・転送・実行を 1 本の exec チャネルで済ませる。pty は要求しない (二進が通る)。
`ztclient.c` の `zc_command()` がコマンドを作る:

```
exec sh -c '<スクリプト>' zterm <版> <桁> <行> ['<コマンド>']
```

スクリプト (ログインシェルが fish / tcsh でも通るよう `sh -c` で包み、本体に `'` と `!` を使わない):
1. `A=$(uname -s)-$(uname -m)` (例 `Linux-aarch64`)、`F=~/.cache/zterm/zterm-<版>-$A`
2. `F` が実行できれば `ZTERM OK $A`。無ければ `ZTERM NEED $A gz` (gzip が無ければ `raw`) を出して
   `read N` で長さの行を待ち、`head -c $N | gzip -dc > $F.tmp`。`$F.tmp --version` が通れば `mv` し、
   同じアーキテクチャの古い版を消す。失敗は `ZTERM FAIL <理由>`。
3. `ZTERM GO` を出して `exec $F --cols C --rows R [-- sh -c "<コマンド>"]`。

Z は `ZTERM ` で始まらない行 (.bashrc の出力など) を読み飛ばす。NEED には `B:/zterm/<arch 小文字>.gz` の
バイト数の行と中身を送り、GO が来るまで他は何も送らない (busybox の head が読み過ぎても困らないように)。
`sh` の `read` はパイプを 1 バイトずつ読むので本体を食わない。転送ファイルは `fopen "rb"` (newlib の既定の
テキストモードは 0x0d/0x1a を捨てる)。版 (`VERSION`) は「Cargo の版 + 4 本の .gz のハッシュ」なので、
作り直すとリモートのキャッシュが入れ替わる。

## 通信路 (GO の後)

多バイトの値はビッグエンディアン (68000 がそのまま読む)。

**サーバ → Z**: フレーム = `'Z'` (0x5a)、長さ u16、本体。本体の頭で書き込み位置は (0,0)、属性は (fg 7, bg 0, fl 0)。

| バイト | 引数 | 意味 |
|---|---|---|
| 0x20-0x7e | — | その ASCII を現在の属性で置き 1 桁進む (F_SPECIAL 属性なら DEC 罫線グリフ) |
| 0x01 HELLO | ver cols rows | 最初のフレームの頭。Z はここで `vt_init` (画面を占有) |
| 0x02 POS | row col | 書き込み位置 |
| 0x03 ATTR | fg<<4\|bg, fl | 属性 (fl は F_UNDERLINE 1 / F_SPECIAL 2)。色は 16 色に解決済み (bold・reverse も) |
| 0x04 WIDE | cp16 | 全角 (漢字・かな。Unicode BMP、Z の CGROM にある字だけ)。2 桁 |
| 0x05 EMOJI | idx | 絵文字 (term/vt100 の emojitab の番号)。2 桁 |
| 0x06 CP16 | cp16 | 半角の非 ASCII (半角カナ・ブロック要素 U+2580-259F・¥・‾) |
| 0x07 REP | n ch | 同じ ASCII を n 個 |
| 0x08 SCROLL | top bot n | 行 [top,bot] を n 行上へ (負なら下へ)。Z は TXRASCOPY で動かす。空いた行は空白 (7,0) |
| 0x09 CURSOR | x y flags | カーソル位置と表示 (bit0) |
| 0x0a MODE | flags | bit0 DECCKM、bit1 代替画面、bit2 DECKPAM、bit3 bracketed paste (情報のみ) |
| 0x0b BELL | — | |
| 0x0c CLEAR | bg | 全面を空白 (7,bg) に (帯域の節約。描画は差分なので変わらない) |
| 0x0d BYE | code | シェルが終わった (終了コード) |
| 0x0e SIZE | cols rows | 寸法が変わった |

**Z → サーバ**: `type` u8, `len` u8, 本体。`k` キー (vt_key が作った VT バイト列そのまま)、`a` ACK (1 フレーム描き終えた)、
`r` 寸法 (cols rows)、`f` 全部描き直して、`q` 終わり (シェルに SIGHUP)。

### サーバの仕事
- **Z が描ける形まで決める**: 字は term/vt100 の `cp_kind` / `fallback` / Unicode 罫線 → DEC 罫線の写像をそのまま移した
  (`zcell.rs`)。漢字の表 (kanjitab.h) と絵文字の表 (emojitab.h) は `build.rs` が term/vt100 のヘッダから読むので、Z と
  ずれない。色は vt100.c の `map256` / `nearest16` と同じ 16 色への丸め。Z は Unicode の判定を一切しない
  (漢字は Unicode のまま送り、Z の blit が既存の 64 件キャッシュで JIS に引く)。
- **差分**: 送った画面 (= Z の論理画面) と新しい画面を比べ、変わった桁の連なりだけ送る (3 桁以下の無変化は含める。
  前景色だけ違う空白は送らない = vt100.c の same_look)。
- **スクロール検出**: 変わった行の範囲 [t,b] の中で、送った画面を n 行ずらすと合う行が「ずらさず合う行 + 2」以上なら
  SCROLL を先に送る (Z は TXRASCOPY でほぼただ)。半分以上が変わったら CLEAR を先に送る。
- **間引き (mosh と同じ)**: ACK の無いフレームは 1 つまで (`--inflight`)。Z が遅ければ間の画面は飛ばして
  最新との差分だけ送る。フレームの間隔は 50 ms 以上 (`--interval`)。書き途中の画面を送らないよう、pty が 8 ms 静かになるか
  最初の変化から 100 ms 待つ。`?2026` (同期更新) の間は送らない (最長 500 ms)。
- **端末への問い合わせに答える** (Z の vt_write は通らないので): DSR 5n/6n、DA1、DA2、`CSI 18 t`、DECRQM (?1 ?25 ?1049
  ?2004 ?2026)、OSC 10/11。答えは vt100.c と同じ。kitty キーボードの `CSI ? u` には答えない。
- **キー**: Z からは `vt_key` の VT バイト列がそのまま来る。DECCKM 中は `ESC [ A-D` を `ESC O A-D` に直して pty へ。
- シェルはログインシェル (`-bash`)、`TERM=xterm` (`--term` で変更)。LANG 等が無ければ `LANG=C.UTF-8`。

## Z 側の作り

`ztdraw.c` は **term/vt100/vt100.c を #include** して、その論理画面 (`cur`)・書き換え範囲 (`rlo/rhi`)・`scroll`・`flush`・
カーソルをそのまま使う (vt100.c 自体は変えない。ssh.x の ZTERM=1 ビルドと zthost は vt100.c の代わりに ztdraw.c を組む)。
フレームの命令でセルを書き、最後に `flush` が変わったセルだけ描く。ASCII の連なりは 1 ループで置く。

ssh.x (`net/ssh/dropbear`、`ZTERM=1`):
- `cli-main.c` のパッチ: `-Z` を getopts の前に抜き、getopts の後に `cli_opts.cmd` を起動スクリプトに、`wantpty=0`。
- `x68k_shim.c`: チャネルの stdout → `zc_input`、stdin ← `zc_output` (転送・ACK・キー)、select は送るものがあれば fd 0 を
  「読める」にする。HELLO で画面を占有したら DOS コンソールには触らない。
- `cli-chansession.c` のパッチ: チャネルが閉じたとき zterm が始まっていなければ、同じ接続で `wantpty=1` のセッションを
  開き直す (`-t` と同じ VT100)。

## 試験 (Mac)

```sh
cd term/vt100 && make vthost
cd term/zterm/server && cargo build --release && ./dist.sh
cd ../client && make zthost ztreplay
../tests/replay.sh                   # vt100.c との一致 + 往復
./zthost -d ../dist -g ../../vt100/tests/cgrom/CGROM.DAT -v out.bin -t out.txt -k keys.txt -- ssh raspi5
python3 ../../vt100/tests/vram2png.py out.bin out.png
```
`zthost` は ssh.x の代わりに起動スクリプトを付けてコマンドを起動し、同じ手順を回す (`-D ms` で 1 フレームごとに止まって
遅い Z の真似、`-V` でフレームごとに VRAM = 論理画面を確かめる、`-k` でキー台本)。

### 結果 (2026-10-22)
- **画面の一致**: 記録済みの 19 本 (Claude Code 4 本・tmux・top・vim 日本語・less UTF-8・git log・絵文字・日本語 cat/less ほか)
  で、サーバの最終画面が vt100.c (vthost) と**文字・属性とも完全一致**。さらに入力を 1400 / 64 / 7 バイトずつに刻んで
  フレームを作り、Z 側 (ztdraw.c) で描いた画面がサーバの画面と一致し、フレームごとの VRAM 不変条件も保たれる。
  合成の 5 本 (`syn_*`) は vt100 crate の未対応 (?1047、DECAWM off、TBC、IL 後のカーソル桁、ZWJ 絵文字の幅) で違う。
- **帯域** (raspi5 へ実際の ssh、zterm の統計。D は 1 フレームごとの止まり = 遅い Z の真似):

| 負荷 | 素の VT (pty の出力) | zterm D=0 | zterm D=150ms |
|---|---|---|---|
| `top -d 0.2` | 8.5-8.8 KB/s | 1.3 KB/s | 1.1 KB/s |
| Claude Code の応答の流し込み (記録を 128B/20ms で再生) | 4.4 KB/s | 1.4 KB/s | 1.1 KB/s |
| Claude Code 風の全画面描き直し 20 FPS (`spinner.py`) | 43 KB/s | 0.72 KB/s | 0.38 KB/s |
| 長い文字の垂れ流し (`claude --help` の記録を 256B/10ms で再生) | 14.6 KB/s | 9.5 KB/s | 6.0 KB/s |

  どれも zthost の `-V` (フレームごとの VRAM 不変条件) は 0 件。垂れ流しは新しい文字そのものなので減らしようがなく、
  遅い Z (D=150) では画面から流れ去った分を送らない分だけ減る。
  Z の受信は暗号 (chacha20-poly1305) を 68000 で解くので、受け取るバイト数がそのまま CPU 時間になる。
- **Z の描画時間** (`tests/simbench.sh`、68000 10MHz のサイクル数の見積もり。入力を同じ 1400 バイトずつに区切り、
  素の VT は vt_write、zterm はその区切りごとの 1 フレームを ztd_frame):

| フィクスチャ | vt_write | zterm | 比 |
|---|---|---|---|
| app_claude_stream | 1.13 s | 0.56 s | 2.0x |
| app_claude | 0.70 s | 0.30 s | 2.3x |
| app_claude_help (垂れ流し) | 3.12 s | 2.02 s | 1.5x |
| app_tmux | 0.54 s | 0.15 s | 3.7x |
| app_lessu8 | 1.22 s | 0.28 s | 4.3x |
| jp_less | 1.35 s | 0.63 s | 2.1x |
| app_top / top / vim / app_vimjp | | | 1.1-1.2x |

  vt100.c の解析は既に速く、残りは VRAM への描画そのもの。zterm の効き目はこの表 (同じ区切り) より、実際の接続での
  間引き (Z が遅いほど間の画面を飛ばす) と受信バイト数の減少 (= 68000 での復号の時間) の方が大きい。

## ヘルパの大きさ (opt-level=z, LTO, strip, panic=abort。musl は静的)

| アーキテクチャ | 素 | gzip -9 |
|---|---|---|
| linux-aarch64 (aarch64-unknown-linux-musl) | 462 KB | 257 KB |
| linux-x86_64 (x86_64-unknown-linux-musl) | 515 KB | 266 KB |
| darwin-arm64 | 403 KB | 201 KB |
| darwin-x86_64 | 396 KB | 212 KB |

musl は rustup 同梱の self-contained な crt と libc.a を rust-lld で静的リンクする (`server/.cargo/config.toml`。zig や
musl-gcc は要らない)。`rustup target add aarch64-unknown-linux-musl x86_64-unknown-linux-musl x86_64-apple-darwin`。

## 割り切り・未対応
- vt100 crate の未対応: DEC 罫線の文字セット (`dec.rs` で前処理して補った)、?1047、DECAWM off、タブストップの設定、
  マウス (不要)。スクロールバックは持たない。
- 漢字・絵文字・罫線の「描ける字」は term/vt100 と同じ (表に無い全角は `??`)。
- キーは vt_key のまま (Z 側で VT のバイト列にする)。画面サイズは 96x32 固定 (RESIZE の通信路はある)。
- ヘルパの転送中 (と GO の前) に打ったキーは捨てる。
- シェルの終わりは pty の read が EIO/0 になることで知る。バックグラウンドのジョブが pty を握っていると `exit` の後も
  続く (waitpid(WNOHANG) を見るのは今後)。
- DECRQM で ?2026 (同期更新) に対応と答えるので Claude Code は BSU/ESU を使う。ESU が来ないまま死んだときのため
  同期中の保留は最長 500 ms。

## 実機 (X68000 Z) の結果 (2026-10-22)

`b:dbz.x -Z [-oAccel=all] -i b:idx68k moto@192.168.1.122 top -d 1 -n 5` (raspi5):
- 最初の版は転送の途中で sshd が "Bad packet length" で切った。原因は xip: ノンブロッキングのソケットへの 4096 バイト超の
  write が、全部送ったのに -1 を返し、dropbear が送り直していた (net/ssh/dropbear/README.md の `__wrap_write`)。1 回 4096 に
  切って解決。
- 直した版: 転送 (sending → pushed → go → hello → top → 正常終了) と 2 回目のキャッシュ (cached → go → hello) が通った。
  ACCEL なし 1 回・ACCEL あり 3 回の転送のうち、ACCEL ありの最初の 1 回だけ転送の終わり近くで sshd が
  "message authentication code incorrect" を出して切った (再現せず。原因未確定)。
- macOS も通った (ACCEL、`top -l 3 -s 1 -n 8`): macmini-pro (Darwin x86_64、ログインシェル zsh) は転送 212 KB 2 分 51 秒 +
  キャッシュ 20 秒、motombp16 (Darwin arm64) は転送 201 KB 2 分 45 秒 + キャッシュ 22 秒。起動スクリプト (/bin/sh・head -c・
  gzip -dc・uname -s = Darwin) と Darwin の pty はそのまま動いた。終了後の Z のテキスト VRAM (プレーン 0) に top の出力が
  正しく残っていることも memdump で確認。
- **時間**: 転送 257 KB に約 3 分 (ACCEL ありで接続から終了まで 3 分 20 秒)、キャッシュありは ACCEL で 19 秒、なしで 78 秒。
  律速は Z の chacha20-poly1305 で **約 1.5 KB/s** (net/ssh/crypttest。TCP の送信だけなら 4096 ずつで 9〜11 KB/s)。
  受信も同じ速さでしか復号できないので、素の VT の Claude Code (4.4 KB/s) は Z では実時間に復号しきれない。zterm の
  1 KB/s 前後に減らすことがそのまま効く。転送を縮める (xz・ヘルパの小型化) と暗号の 68000 向け高速化 (poly1305 の
  64 ビット乗算) が次の課題。

### 全部入り (zterm + ssh の vt100 高速化 + chacha68k + ACCEL、2026-10-22)

`ZTERM=1 FAST=1 ACCEL=1 ./build.sh` = B:/dbza.x (353 KB)。B:/SSH5Z.BAT (`-Z`) と B:/SSH5T.BAT (`-t`) は raspi5 への接続。
rcmd から非対話で、接続から終了までの時間 (ACCEL の鍵交換込み、ヘルパはキャッシュ済み):

| コマンド | -Z | -t |
|---|---|---|
| `top -d 0.5 -n 20` (10 秒で終わる) | 23.8 s | 30.1 s |
| `seq 1 20000` (109 KB の出力) | 13.9 s | 249.8 s |

-Z は流れ去った行を送らないので垂れ流しが一瞬で終わる。-t は全部を復号して 1 行ずつスクロールして描く。ダイアログは無し。

## 実機 (X68000 Z) での手順 (チェックリスト)

B: (Pi の /srv/x68000z/HFS) の配置:
```
B:/ssh.x 等            ZTERM=1 FAST=1 [ACCEL=1] ./build.sh の出力 (例 B:/sshz.x)
B:/zterm/VERSION        dist.sh が書く 1 行 (例 0.1.0-d488effa)
B:/zterm/linux-aarch64.gz   raspi3 / raspi5 用 (257 KB)
B:/zterm/linux-x86_64.gz    (必要なら)
B:/home/.ssh/known_hosts    既存 (-y を付けない)
```

1. ビルド: `term/zterm/server/dist.sh` → `dist/` を Pi の `/srv/x68000z/HFS/zterm/` へ。
   `ZTERM=1 FAST=1 ./build.sh /srv/.../sshz.x` (ACCEL も試すなら `ACCEL=1 ACCELDIR=...`)。
2. 素の動作確認 (ヘルパのキャッシュ無し) **[済: 上の結果]**: Pi で `rm -rf ~/.cache/zterm` → Z で `b:sshz.x -Z -i b:\id_x68k moto@<Pi>`。
   - ハンドシェイク後に `zterm: sending B:/zterm/linux-aarch64.gz (... bytes)`、`zterm: pushed`、`zterm: go` が出て画面が切り替わるか。
   - **転送時間を測る** (257 KB を Z の chacha20-poly1305 で送る。見積もり 20 秒前後)。Pi の `~/.cache/zterm/` に
     `zterm-<版>-Linux-aarch64` ができ、`--version` が通るか。
3. 2 回目 (キャッシュあり): `zterm: cached` → すぐ画面が出るか。**[済]**
4. 画面: プロンプト・`ls --color`・漢字 (`echo 日本語`)・絵文字・`top`・`vim`・`less`・`exit` で Human68k に戻り、
   画面がプレーン 0,1 に潰れてプロンプトが正しい行に出るか (x68k_tty_cleanup)。
5. キー: 矢印 (vim の通常モード = DECCKM で ESC O A に直るか)、Ctrl-C、BS、ROLL UP/DOWN、F1-F10。
6. **Claude Code**: `b:sshz.x -Z ... moto@<Pi>` → `claude`。入力欄の打鍵の反応、応答の流し込みの FPS を
   `-t` (VT100 直) と比べる。Pi 側で `ZTERM_STATS=1` を付けるには起動スクリプトを経ないで試すしかないので、
   帯域は Mac の zthost で測った値を参考にする。
7. フォールバック: B:/zterm の .gz の名前を一時的に変える → `zterm: no helper ... -- falling back to -t` の後、同じ接続で
   VT100 のシェルになるか (dropbear の closehandler の中でセッションを開き直す。**実機で一度も通していない**。
   新しいチャネルは OPEN_CONFIRMATION まで transwindow が 0 で、send_msg_channel_data は maxlen == 0 で何もしないので
   先走ってキーを送ることはない)。
11. 切断: 描画中に Pi の sshd を止める / LAN を抜く → dropbear_exit の経路でも画面が戻るか (cli_tty_cleanup の頭で
    x68k_tty_cleanup を呼ぶ。zterm は pty を要求しないので raw モードの確認より前に置いた)。
8. 壊れた転送: 転送の途中で ssh を切る → 次の接続で `.tmp` が残っても再送されるか。
9. `-Z host cmd` (例 `-Z host top`) が sh -c で動き、終わると戻るか。
10. 異常系: リモートの zterm を `pkill zterm` → BYE が来ずにチャネルが閉じる → 画面を戻して終わるか。

気をつけること: rcmd の環境ブロックは満杯なので ZTERMDIR は設定できない (既定 B:/zterm に置く)。
`-v` は使わない (Z でバスエラー)。DEBUG=1 のビルドなら位相の印に zterm の段階も出る。
