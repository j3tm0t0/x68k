# SSH クライアント移植 進捗 (2026-10-22 08:25)

## 結論: 実機で `ssh.x moto@192.168.1.108 uname -a` が通った

X68000 Z (68000 @ 10MHz, b:pnetorg4 = 素の xip.x MSS512 + npd4.x) から Pi の
**素の sshd (ポート 22)** に ed25519 鍵で公開鍵認証し、`uname -a` の出力を Z 側に表示。
**接続から切断まで 65.0 秒** (ssh.x 本体の起動を含む rcmd 側の実測は約 70 秒)。

```
B:\dbcf.x: Caution, skipping hostkey check for 192.168.1.108
Linux raspberrypi3 6.18.50+rpt-rpi-v8 #1 SMP PREEMPT Debian 1:6.18.50-1+rpt1 (2026-09-11) aarch64 GNU/Linux
Connection to moto@192.168.1.108:22 closed.
```

Pi 側 `journalctl -u ssh`: `Accepted publickey for moto from 192.168.1.68 port 2052 ssh2: ED25519 SHA256:WXcB9...`

## 位相別の所要時間 (DEBUG=1 ビルド、sshd:22、IOCS _ONTIME による Z 側実測)

| 位相 | 区間 (印) | 秒 |
|---|---|---|
| 起動 (乱数種・crypto_init・鍵ファイル読み込み・引数解析) | main → getopts-done | 0.6 |
| TCP connect | connect-start → connected | 0.2 |
| curve25519 鍵生成 (固定ベース scalarmult、comb) | kexdh-init-start → kexdh-init-sent | 5.6 |
| サーバの KEXINIT / KEXDH_REPLY 待ち (ネットワーク) | kexdh-init-sent → kexdh-reply-recv | 1.3 |
| curve25519 共有秘密 (可変ベース scalarmult、ladder) | shared-secret-start → done | 19.6 |
| ホスト鍵の ed25519 署名検証 | hostkey-verify-start → done | 26.7 |
| NEWKEYS・service request 往復 | hostkey-verify-done → sign-start | 1.2 |
| 認証用 ed25519 署名 | sign-start → sign-done | 7.2 |
| userauth 成功 → exec → 出力 → 切断 | auth-success → finished | 2.0 |
| **合計** | main → finished | **65.0** |

暗号 (scalarmult ×2 + 検証 + 署名) が 59 秒で全体の 9 割。ネットワークと配管は 5 秒程度。
ポート 2222 (IPQoS none の試験用 sshd) でも同じ 64.8 秒。クロックの差は無い。

## 今日直したこと

1. **鍵ファイルが読めなかった真因**: elf2x68k の newlib は `open()` の既定が**テキストモード**で、
   読み込み時に 0x0d と 0x1a を捨てる。dropbear 形式の秘密鍵 (83 バイト) に偶然その 2 バイトが
   含まれていて 81 バイトになり `buf_get_ed25519_priv_key` の長さ検査で失敗していた
   (Z 上で試験プログラムを走らせて確認: `O_RDONLY` で 81、`O_RDONLY|O_BINARY` / `fopen "rb"` で 83)。
   → `dbutil.c` の `buf_readfile` を `O_RDONLY | O_BINARY` に (patches/dbutil.c.patch)。
2. **認証後の "Couldn't set nonblocking"**: セッション開始時に stdin/stdout/stderr を
   `fcntl(O_NONBLOCK)` するが、Human68k の非ソケット fd では失敗する。
   → `setnonblocking` はソケット以外 (fd < 128) では何もしない (patches/dbutil.c.patch)。
   併せて `select` シムで非ソケット fd を「書き込みは常に可、読み込みは不可」に
   (x68k_shim.c)。libsocket の `poll` に fd 0〜2 を渡さない。
3. **位相の印に経過秒を付けた** (`DEBUG=1 ./build.sh`、`x68dbg()`、IOCS _ONTIME)。dropbear の
   `-v` (TRACE) は Z でバスエラーになった (原因未確認、`%f` 書式の疑い) ので使わない。

## 環境メモ
- TOS≠0 受信破棄は lead の npd4.x (TOS クリア) で解決済み → **IPQoS none の試験用 sshd (~/sshtest, :2222) は不要**。
  stop.sh で停止済み。削除してよい。
- ssh.x は `FAST=1 ./build.sh` (crypto68k 組み込み) で 272,648 バイト。Pi の B: 共有では dbcf.x。
- 鍵は dropbear 形式 (`dropbearkey -t ed25519` / `dropbearconvert`)。OpenSSH 形式はそのままでは読めない。

## 残り (実用化)
1. **対話セッション**: stdin (キーボード) をチャネルに流す仕組みが無い (select シムが stdin を
   読めない扱い)。IOCS _B_KEYSNS で可否を見て 1 文字ずつ送る形にし、VT100 (branch vt100 の
   vt100.c) に出力を繋ぐ。`-t` (pty 要求) と termios スタブの確認も要る。
2. **暗号のさらなる高速化**: ed25519 検証 26.7 秒が最大。署名 7.2 秒、ladder 19.6 秒。
3. known_hosts (`-y -y` で検証を飛ばしている)。HOME=B:/home の `.ssh/known_hosts` は
   テキストなので `fopen` のテキストモードで問題ないはず。未検証。
4. 乱数が弱い (タイマ撹拌のみ)。実運用には不十分。
