# net/ssh/dropbear

dropbear の `dbclient` を X68000 (Human68k) 用にクロスビルドして `ssh.x` を作る。

- ベース: dropbear 2022.83 (配布 tarball を `fetch.sh` が取得・SHA-256 検証して
  `work/` に展開。改変済みソースはリポジトリに置かない)。
- ツールチェーン: elf2x68k (`m68k-xelf-gcc`)。ソケットは elf2x68k 同梱の
  `libsocket.a` (xip.x / inetd.x 経由の BSD ソケット、`poll()` が `_CHANGE_PR`
  で CPU を譲る)。
- アルゴリズム: 鍵交換 curve25519、暗号 chacha20-poly1305、公開鍵 ed25519 のみ。
  RSA/DSS/ECDSA/AES を落として小さくしてある (`localoptions.h`)。

## ビルド

```
./fetch.sh              # dropbear-2022.83.tar.bz2 を取得・検証・展開
./build.sh              # ssh.x を作る (素の curve25519。遅い。既定の出力は ./ssh.x)
FAST=1 ./build.sh       # 68000 高速体演算を組み込む (XEiJ 計測で scalarmult 118s->10s。Z 実機の内訳は ../PROGRESS.md)
FAST=1 FAST_C=1 ./build.sh   # asm 無しの C 版だけ (22s)。mul68k.s 不要
FAST=1 CHACHA68K=0 ./build.sh  # 通信路の暗号を libtomcrypt のままにする (CHACHA68K の既定は FAST と同じ)
FAST=1 CHACHA68K_C=1 ./build.sh  # chacha68k の C 版だけ (asm 無し)
./build.sh /path/ssh.x  # 出力先を指定
DEBUG=1 FAST=1 ./build.sh /path/dbg.x   # 位相ごとの印 (経過秒付き) をテキスト画面に出す版
ZTERM=1 FAST=1 ./build.sh    # -Z (zterm: リモートのヘルパが画面を持ち、変わったセルだけ受け取る) が使える版
ZTERM=1 FAST=1 ACCEL=1 ACCELDIR=../../../../x68k-accel/net/accel ./build.sh   # + 暗号を Pico W に (-oAccel=...)
```

`ZTERM=1` は term/zterm/client の `ztclient.c` と `ztdraw.c` (vt100.c を #include する) を vt100.c の代わりに組む。
`ssh.x -Z host [cmd]` で pty を要求せず 1 本の exec チャネルでヘルパを確かめ・押し込み・起動する。ヘルパは
`B:/zterm/` (環境変数 `ZTERMDIR`) の `<uname -s>-<uname -m>.gz` と `VERSION` (term/zterm/server/dist.sh が作る)。
始まらなければ同じ接続で `-t` のセッションに切り替わる。詳細は [term/zterm/README.md](../../../term/zterm/README.md)。

`CHACHA68K=1` は `net/ssh/chacha68k/` (68000 向けの ChaCha20 / Poly1305、`chachapoly.patch`) を組み込む。
XEiJ SUPER で chacha20-poly1305 が 4.0 KB/s → 22 KB/s (16 KB のパケット)。結果は libtomcrypt と同じ。

`VTKEEP=1` は終了時に VT100 の画面をプレーン 0,1 に潰さない試験用の版 (VRAM ダンプで色を見る)。

`FAST=1` は `net/ssh/crypto68k/` (vt100 agent 作、`curve25519.patch` / `field_fast.h` /
`mul68k.s`) を使う。別の場所なら `CRYPTO68K=/path ./build.sh`。これでハンドシェイクが
素の sshd:22 (LoginGraceTime 120秒) にも収まる。素の遅いビルドは grace を上げた
sshd (例 ~/sshtest の :2222) が要る。

## このリポジトリに置くもの

| ファイル | 役割 |
|---|---|
| `localoptions.h` | dropbear の機能選択 (client のみ、curve25519/chacha20poly1305/ed25519) |
| `x68k_shim.c` | Human68k/newlib/libsocket に無い関数の補完 (下記) |
| `x68k_vcons.c` | 端末まわり: VT100 画面の占有・復帰、チャネルの読み書き、zterm (-Z)、仮想コンソール (x68k_shim.c が #include) |
| `tests/vcshim.sh` | 仮想コンソールのホスト試験 (`tests/vcshim.c`: dropbear のチャネルと IOCS のキーを真似て x68k_vcons.c を動かす) |
| `x68kinc/` | newlib に無いヘッダの最小スタブ (sys/ioctl.h, sys/un.h, sys/uio.h, syslog.h, termios.h, netinet/ip.h, netinet/tcp.h) と `x68k_compat.h` (AF_UNSPEC 等 + `FD_SETSIZE` 拡張) |
| `patches/` | dropbear 本体への最小パッチ (下記) |
| `fetch.sh` / `build.sh` | 取得・ビルド |

### x68k_shim.c が補うもの
- `select()` を libsocket の `poll()` で実装 (ソケット fd は 128..159。`fd_set` が
  収まるよう `x68k_compat.h` で `FD_SETSIZE` を 256 に拡張)。ソケット以外の fd は `poll` に渡さず
  「書き込みは常に可」。stdin (fd 0) はキーボード: poll を 50ms 刻みで回し、間に `vt_key` (IOCS
  _B_KEYSNS/_B_KEYINP) でキーを読んでバッファに溜め、溜まっていれば「読める」と返す。
- `x68k_tty_setup` / `x68k_tty_cleanup`: `-t` のとき `cli_tty_setup` / `cli_tty_cleanup` から呼ばれ、
  `vt_init` / `vt_fini` (term/vt100) で画面を占有・復帰する。復帰時は残った画面をプレーン 0,1 に潰し
  (Human68k のコンソールはプレーン 0,1 しかスクロールしない)、IOCS のカーソルを VT のカーソル行の次に置く。
- `x68k_chan_write` / `x68k_chan_read` (x68k_vcons.c): チャネル番号付きの stdout/stderr 書き込みと stdin 読み出し。
  そのチャネルのコンソールの `vt_write` / 送信待ち (キー・DSR の応答) / zterm、VT 無効なら素の write / read。
  空のときは -1/EINTR (0 や EAGAIN は EOF 扱いされる)。
- `ioctl(TIOCGWINSZ)`: `vt_get_size` (96x32) を返す (pty 要求のウィンドウサイズ)。
- `x68dbg()`: 位相ごとの印。`DEBUG=1` ビルドでのみ「[ 12.34] 印」(IOCS _ONTIME の経過秒) を
  IOCS _B_PRINT でテキスト画面に出す (rcmd の redirect を迂回し、contee.sys 経由で Pi の
  /tmp/x68con.log に残る)。dropbear の `-v` は Z でバスエラーになるので使わない。
- `getaddrinfo`/`freeaddrinfo` を**数値 IP 専用**に上書き (libsocket 版は
  `gethostbyname` を呼び、xip の resolver が数値 IP でも返らずハングするため)。
- `x68k_connect`: **ブロッキング** connect した接続済み fd を返す。xip は
  非ブロッキング connect が不可 (EIO) なので。`cli-main.c` のパッチから呼ぶ。
- `x68k_get_entropy`: `/dev/urandom` が無いので MFP タイマ等を撹拌した種。**弱い**。
- `getuid/getpwuid/ioctl/pipe/fsync/execv/setsid/basename/dirname` のスタブ。

- `__wrap_write` (リンクに `-Wl,--wrap=write`): **ソケットへの write は 1 回 4096 バイトまで**。xip はノンブロッキングの
  ソケットに 4096 を超えて write すると、全部送ったのに -1 (libsocket が EAGAIN にする) を返す。dropbear は同じパケットを
  送り直すので sshd は "Bad packet length" で切る。`net/ssh/sendtest.c` + `sendsink.py` で実機確認 (2026-10-22): 4096 は
  300 KB まで正確 (受信を混ぜても)、4097 / 8192 / 32768 は毎回この重複。ブロッキングなら 8192 も正常。それまで Z は打鍵しか
  送らなかったので表に出なかった (ssh.x -Z のヘルパ転送で発覚)。

### patches/ (DROPBEAR_X68K で切り替え)
- `cli-main.c` : connect を `connect_remote` (非同期+select) から `x68k_connect`
  (同期ブロッキング) に差し替え。
- `common-session.c` : シグナル用 `signal_pipe` を無効化 (Human68k にシグナル無し)。
- `dbrandom.c` : `/dev/urandom` の代わりに `x68k_get_entropy` を使う。
- `dbutil.c` : `disallow_core` (setrlimit) を no-op に (Human68k に rlimit 無し)。
  `buf_readfile` (鍵ファイル) を `O_RDONLY | O_BINARY` で開く (newlib の既定はテキストモードで
  0x0d/0x1a を捨てるため、二進の秘密鍵が壊れて "Failed loading keyfile" になっていた)。
  `setnonblocking` はソケット以外の fd では何もしない (fcntl が無く "Couldn't set nonblocking" で落ちていた)。
  `m_close` は fd 0/1/2 を閉じない (セッションのチャネルは全部 fd 0/1/2 を共有する。1 本目が閉じても他が使う)。
- `cli-chansession.c` : `cli_tty_setup` / `cli_tty_cleanup` から `x68k_tty_setup` / `x68k_tty_cleanup` を呼ぶ。
  仮想コンソール: `x68k_open_session` (同じ接続にセッションチャネルをもう 1 本)、`cli_initchansess` の頭で
  `x68k_vc_initchan` (2 本目以降の pty 要求・コマンド)、`cli_closechansess` は `x68k_vc_closed` が 1 なら画面を戻さない
  ("Waiting for other channels" も出さない)。zterm が始まらなかったときの -t への開き直しもここから (旧 x68k_zterm_closed)。
- `cli-session.c` : `SESSION_RUNNING` で `x68k_vc_loop` (頼まれたチャネルを開く。チャネル 0 本の終了判定より前)。
  pty 要求の TERM は Human68k の TERM (x68000) でなく **xterm** (環境変数 `SSHTERM` で上書き)。
  SIGWINCH の signal() 失敗は無視。
- `common-channel.c` : チャネルの fd への write / read を `x68k_chan_write` / `x68k_chan_read` に (チャネル番号付き)。
- `x68kinc/termios.h` : `tcgetattr` は cooked な既定値 (ECHO/ICANON/ISIG/ICRNL/OPOST/ONLCR、VERASE=0x7f …)
  を返す。pty-req の termcodes はこれをそのまま sshd に送るので、全部 0 だとエコー無しの pty になる。
- `common-kex.c` / `cli-kex.c` / `cli-auth.c` / `cli-authpubkey.c` / `cli-chansession.c` /
  `cli-session.c` : 位相の印 `x68dbg("...")` (既定ビルドでは no-op)。

## 現状と性能 (2026-10-22 実機)

X68000 Z (68000 @ 10MHz, b:pnetorg4 = 素の xip MSS512 + npd4.x) から Pi の素の sshd:22 に
ed25519 鍵で公開鍵認証し、`uname -a` の出力を表示できた。

```
b:ssh.x -y -y -i b:\id_x68k moto@192.168.1.108 uname -a
Linux raspberrypi3 6.18.50+rpt-rpi-v8 #1 SMP PREEMPT Debian 1:6.18.50-1+rpt1 (2026-09-11) aarch64 GNU/Linux
Connection to moto@192.168.1.108:22 closed.
```

接続から切断まで **65 秒** (`FAST=1` = crypto68k 組み込み)。内訳は [../PROGRESS.md](../PROGRESS.md):
curve25519 鍵生成 5.6s、共有秘密 19.6s、ホスト鍵 ed25519 検証 26.7s、認証署名 7.2s、
ネットワークと配管が合わせて 5s 程度。sshd の既定 LoginGraceTime (120s) に収まる。
npd4.x が受信 TOS をクリアするようになったので IPQoS none の sshd は不要になった
(素の `FAST` 無しビルドは 1 回 15〜20 分かかり、grace を上げた sshd が要る)。

鍵は dropbear 形式 (`dropbearkey -t ed25519 -f id_x68k`、OpenSSH 形式は `dropbearconvert`)。
`-y -y` でホスト鍵検証を飛ばしている (known_hosts の扱いは未検証)。乱数源が弱いので実運用には不十分。

### 対話セッション (`-t`)

```
b:ssh.x -y -y -i b:\id_x68k -t moto@192.168.1.108
```

pty を要求すると term/vt100 の VT100 エミュレータ (xterm サブセット、96x32、テキスト VRAM 直描き) に
出力が流れ、キーボードがチャネルに送られる。実機で bash のプロンプト、`ls --color` (色)、`cat` の
スクロール、Ctrl-C、`vi` (代替画面)、`less`、`exit` で Human68k のプロンプトに戻るところまで確認
(../PROGRESS.md「結論 2」)。キー変換は term/vt100/README.md の表 (BS は DEL 0x7f、矢印は CSI/SS3)。
画面サイズ変更は無い。終了時は最後の画面を (色を落として) 残し、その下にメッセージとプロンプトが出る。

### 仮想コンソール (`Ctrl+F1`..`F4`、2026-10-22)

Linux のコンソールの Alt+F1.. と同じく、**1 本の接続 (ハンドシェイク 1 回) にシェルを最大 4 つ**開き、画面ごと切り替える。
`ssh.x -t host` / `ssh.x -Z host` はコンソール 1 で始まる。

| キー | 動き |
|---|---|
| `CTRL` + `F1`..`F4` (`OPT.1` + `F1`..`F4` でも) | コンソール 1..4 へ。無ければ新しいセッションチャネルを開く (`-t` は pty + ログインシェル、`-Z` は zterm の起動スクリプト。ヘルパはキャッシュ済みなので速い) |
| `Ctrl-]` の後に `1`..`4` | 同じ (前置キー。修飾キーの状態に頼らない) |
| `Ctrl-]` `Ctrl-]` | `Ctrl-]` を 1 つ送る (vim のタグジャンプなど)。`Ctrl-]` の後のその他のキーは `Ctrl-]` とそのキーを送る |
| `-oConsoleEscape=none` / `-oConsoleEscape=^X` | 前置キーを使わない / 別のキーに (ZTERM=1 ビルドのオプション解析で読む) |

- キーの判定: IOCS `_B_KEYINP` のスキャンコード F1..F4 = `0x63`..`0x66` と、そのときの `_B_SFTSNS` の bit1 (CTRL) / bit2 (OPT.1)
  (BIOS ワーク `$0811`: SHIFT|CTRL|OPT.1|OPT.2|…。IPLROM の `_B_SFTSNS` は `move.w $810,d0`)。前置キーの次のキーは
  `_B_KEYSNS` で先読みする (IPLROM で確認: `$0001xxxx`、下位ワードは `_B_KEYINP` と同じキーコード)。`_B_KEYINP` の値には修飾キーが入らないので、キーを読んだ時点の状態を見る
  (描画で読むのが遅れて先に CTRL を離すと普通の F2 になる。そのときは前置キーで)。今までも `Ctrl+F1` は修飾無しの F1 (`ESC O P`)
  を送っていたので、リモートのアプリから取り上げるキーは無い。X68000 Z の USB キーボードでは OPT.1 = F11。
- 切り替えると最上行の右端に ` 1 [2] 3 ` (表は括弧) が 1.5 秒出る (論理画面は変えず、消すときその桁を描き直す)。
- 裏のコンソールも受信を続ける: `-t` は VT100 の解析と論理画面 (VRAM には描かない)、DSR/DA の応答はそのチャネルへ。
  `-Z` はフレームを論理画面に置いて ACK を返す (サーバが止まらない)。表に出すとき全面を描き直す (96x32 で最大 0.3 秒ほど)。
- キーは表のコンソールのチャネルにだけ送る。どのコンソールも 96x32。
- サーバがセッションを開かなかった (MaxSessions など) ときは ` open failed ` を出してそのコンソールを捨てる。
- シェルが終わったコンソールはそのチャネルだけ閉じ、表なら前に表だったコンソールに戻る。最後の 1 つが閉じたら今までどおり
  画面を (色を落として) 残して終わる。`~.` は接続ごと切る。
- `-Z` でヘルパが始まらなかったコンソールは同じ番号で `-t` に開き直す。以後の新しいコンソールも `-t`。
- メモリ (コンソール 1 つ増えるごと): 端末の状態 0.9 KB + 画面 2 面 24 KB + dropbear のチャネルの受信バッファ 2 x 24 KB、
  `-Z` は + zterm の状態 38 KB (フレーム 32 KB) + 予測 2 KB。4 つで `-t` は +220 KB、`-Z` は +340 KB ほど。開く前に 60 KB
  残っているか確かめ、足りなければ ` no memory ` を出して開かない (dropbear の m_malloc は足りないと接続ごと終わるため)。
- 作り: `x68k_vcons.c`。term/vt100 の `vt_con_*` (端末の状態の持ち替え・裏の端末は VRAM に触らない、`-DVT_VCONS`)、
  term/zterm の `zc_ctx_use` / `ztd_ctx_use` (チャネルごとの通信路・予測の状態)。新しいチャネルはパケットを作っている途中
  (キーを読むのは `send_msg_channel_data` の中) では開けないので、`cli_sessionloop` (`x68k_vc_loop`) で開く。
- 試験 (Mac): `tests/vcshim.sh` (-t / -Z / zterm の開き直しの筋書き)、term/vt100 の `tests/run.sh` (vcons: 4 つの端末に
  フィクスチャを混ぜて流す)、term/zterm の `tests/replay.sh` (ztvctest: フレーム列を 4 本のチャネルに混ぜて流す)。
  **実機 (Z) では未確認。**
