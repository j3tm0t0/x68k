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
FAST=1 ./build.sh       # 68000 高速体演算を組み込む (scalarmult 118s->10s、~12x)
FAST=1 FAST_C=1 ./build.sh   # asm 無しの C 版だけ (22s)。mul68k.s 不要
./build.sh /path/ssh.x  # 出力先を指定
DEBUG=1 FAST=1 ./build.sh /path/dbg.x   # 位相ごとの印 (経過秒付き) をテキスト画面に出す版
```

`FAST=1` は `net/ssh/crypto68k/` (vt100 agent 作、`curve25519.patch` / `field_fast.h` /
`mul68k.s`) を使う。別の場所なら `CRYPTO68K=/path ./build.sh`。これでハンドシェイクが
素の sshd:22 (LoginGraceTime 120秒) にも収まる。素の遅いビルドは grace を上げた
sshd (例 ~/sshtest の :2222) が要る。

## このリポジトリに置くもの

| ファイル | 役割 |
|---|---|
| `localoptions.h` | dropbear の機能選択 (client のみ、curve25519/chacha20poly1305/ed25519) |
| `x68k_shim.c` | Human68k/newlib/libsocket に無い関数の補完 (下記) |
| `x68kinc/` | newlib に無いヘッダの最小スタブ (sys/ioctl.h, sys/un.h, sys/uio.h, syslog.h, termios.h, netinet/ip.h, netinet/tcp.h) と `x68k_compat.h` (AF_UNSPEC 等 + `FD_SETSIZE` 拡張) |
| `patches/` | dropbear 本体への最小パッチ (下記) |
| `fetch.sh` / `build.sh` | 取得・ビルド |

### x68k_shim.c が補うもの
- `select()` を libsocket の `poll()` で実装 (ソケット fd は 128..159。`fd_set` が
  収まるよう `x68k_compat.h` で `FD_SETSIZE` を 256 に拡張)。ソケット以外の fd
  (stdin/stdout/stderr) は `poll` に渡さず「書き込みは常に可、読み込みは不可」扱い。
  stdin をチャネルに流す仕組みはまだ無い (対話セッション未対応)。
- `x68dbg()`: 位相ごとの印。`DEBUG=1` ビルドでのみ「[ 12.34] 印」(IOCS _ONTIME の経過秒) を
  IOCS _B_PRINT でテキスト画面に出す (rcmd の redirect を迂回し、contee.sys 経由で Pi の
  /tmp/x68con.log に残る)。dropbear の `-v` は Z でバスエラーになるので使わない。
- `getaddrinfo`/`freeaddrinfo` を**数値 IP 専用**に上書き (libsocket 版は
  `gethostbyname` を呼び、xip の resolver が数値 IP でも返らずハングするため)。
- `x68k_connect`: **ブロッキング** connect した接続済み fd を返す。xip は
  非ブロッキング connect が不可 (EIO) なので。`cli-main.c` のパッチから呼ぶ。
- `x68k_get_entropy`: `/dev/urandom` が無いので MFP タイマ等を撹拌した種。**弱い**。
- `getuid/getpwuid/ioctl/pipe/fsync/execv/setsid/basename/dirname` のスタブ。

### patches/ (DROPBEAR_X68K で切り替え)
- `cli-main.c` : connect を `connect_remote` (非同期+select) から `x68k_connect`
  (同期ブロッキング) に差し替え。
- `common-session.c` : シグナル用 `signal_pipe` を無効化 (Human68k にシグナル無し)。
- `dbrandom.c` : `/dev/urandom` の代わりに `x68k_get_entropy` を使う。
- `dbutil.c` : `disallow_core` (setrlimit) を no-op に (Human68k に rlimit 無し)。
  `buf_readfile` (鍵ファイル) を `O_RDONLY | O_BINARY` で開く (newlib の既定はテキストモードで
  0x0d/0x1a を捨てるため、二進の秘密鍵が壊れて "Failed loading keyfile" になっていた)。
  `setnonblocking` はソケット以外の fd では何もしない (fcntl が無く "Couldn't set nonblocking" で落ちていた)。
- `common-kex.c` / `cli-kex.c` / `cli-auth.c` / `cli-authpubkey.c` / `cli-chansession.c` /
  `cli-session.c` : 位相の印 `x68dbg("...")` を入れるだけ (既定ビルドでは no-op)。

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
`-y -y` でホスト鍵検証を飛ばしている (known_hosts の扱いは未検証)。
対話セッション (stdin をチャネルに流す) は未対応。乱数源が弱いので実運用には不十分。
