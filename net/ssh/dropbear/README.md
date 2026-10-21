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
./fetch.sh          # dropbear-2022.83.tar.bz2 を取得・検証・展開
./build.sh          # ssh.x を作る (既定の出力は ./ssh.x)
./build.sh /path/ssh.x
```

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
  収まるよう `x68k_compat.h` で `FD_SETSIZE` を 256 に拡張)。
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

## 現状と性能

実機 (X68000 Z, 68000 @ 10MHz, b:pnetorig の素の xip MSS512) で動作:
接続 → バナー交換 → curve25519 鍵交換 → … と進むことを確認。
**ただし curve25519 が非常に遅い**。TweetNaCl の `i64[16]` 実装で、68000 には
32x32→64 乗算命令が無く全て libgcc の `__muldi3` 経由になるため、montgomery
ladder 1 ビットあたり約 1 秒、1 回の scalarmult (255 ビット) で約 5 分かかる。
SSH ハンドシェイクは scalarmult 2 回 + ed25519 検証 + ed25519 署名で、合計
十数分〜20 分規模になる。実用には curve25519/ed25519 の有限体演算を 68000 向けに
手最適化 (アセンブラの 32x32→64 等) する必要がある。接続先は IPQoS none の sshd
(xip が TOS≠0 の受信データを捨てるため。[../README.md](../README.md) 参照)。

デバッグ: `x68k_shim.c` の `x68dbg()` は既定 no-op。`-DX68K_SSH_DEBUG` でビルドすると
位相ごとの印をテキスト画面 (IOCS _B_PRINT) に出す (rcmd のファイル redirect を迂回し、
contee.sys 経由で Pi の /tmp/x68con.log に出る)。
