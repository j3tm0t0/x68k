# net/ssh

X68000 / X68000 Z 用の SSH クライアント。Z から Linux ホストへ `ssh` して
コマンドを実行することを目指す (例: `ssh.x moto@192.168.1.108 uname -a`)。

クライアント本体 (dropbear 移植) は [dropbear/](dropbear) にある。
この階層 (tcptest*.c) は libsocket の下地確認と xip の挙動調査の記録。

TCP/IP は ESP/X の xip.x (常駐スタック)。ソケットは **elf2x68k 同梱の
`libsocket.a`** を使う。これは xip.x / inetd.x を `__sock_search_ti_entry`
で見つけて BSD ソケット API (`socket`/`connect`/`recv`/`send`/`poll`/
`read`/`write`/`fcntl` …) を提供し、newlib の fd と統合されている。
tcpb の古い libnetwork を自前でクロスビルドし直す必要はなかった。

`poll()` は内部で DOS `_CHANGE_PR` を呼んで CPU を譲るので、Human68k で
フォアグラウンドがブロックして xip.x の受信スレッドが止まる例の問題
(rcmd.c 参照) を poll 自身が面倒を見てくれる。ただし **ブロッキング
`read()` は ESTABLISHED + データ無しで `_TI_read_s` を `_CHANGE_PR`
せずに再試行し続けて固まる** (libsocket の read.c の仕様) ので使わない。
受信は常に `poll` / `socklen` で可否を見てからにする。

## ビルド

```
make            # elf2x68k で *.x を作る (-lsocket)
```

## 試験道具 (tcptest*.c)

libsocket が xip.x 相手に素で動くかの煙試験。`b:` (Pi の
`/srv/x68000z/HFS`) に置いて rcmd 経由で実行する。

| 試験 | 内容 | 結果 |
|---|---|---|
| tcptest.c  | socket/connect/poll/read、fcntl O_NONBLOCK、poll に stdin+sock | connect は成功。poll(POLLIN) がバナーを検出せず (後述) |
| tcptest2.c | getsockopt で状態と受信長を監視 (ブロッキング read で固まった) | 使わない |
| tcptest3.c | SOCK_NONBLOCK で connect | nonblock connect は errno=EIO で失敗 |
| tcptest4.c | write-first (接続後すぐクライアントバナー送信) | 送信は ACK されるが受信バナーで RST |
| tcptest5.c | connect 後に SO_SOCKMODE=BINARY を設定 | 効果なし。やはり RST |

## 真因: xip は TOS≠0 の受信 TCP データを捨てる

socket / connect / 送受信は動く。当初「能動オープンの受信が全部 RST される」
ように見えたが、切り分けの結果、真因は **IP ヘッダの TOS (DSCP)** だった。

- 素の Python リスナ (TOS=0) への能動オープンは **完全動作** (poll→POLLIN
  0.13s、banner 受信、nonblock=EAGAIN、stdin poll すべて OK)。
- 同じリスナで `setsockopt(IP_TOS, 0xb8)` を立てて送ると **RST・データ破棄**。
- sshd は IPQoS で送信データに tos 0xb8 (DSCP EF) を付ける。だから
  ハンドシェイク (tos 0) は通るのに最初のデータ到着で RST していた。
  純正 `telnet.x` が同症状だったのも同じ理由 (相手が sshd:22 だったため)。
- パッチの有無 (pnetny の xiply vs pnetorig の xiporig) は無関係。両方で再現。

→ **xip (KA9Q NOS 由来) が TOS≠0 の受信 TCP セグメントを拒否する** のが原因。

### 回避策 (採用・動作確認済み): IPQoS none の sshd

Pi に user-level の sshd を `IPQoS none` で立てれば、送信データの TOS が 0 に
なり Z が受信できる。中継は不要。実機で banner→自バナー→KEXINIT (複数
セグメント) 受信まで完走を確認。

- Pi: `~/sshtest/` (start.sh / stop.sh / sshd_config=IPQoS none / hostkey /
  authorized_keys)。ポート 2222。root 不要・systemd 不使用・削除可。
- 受入: `ssh.x moto@192.168.1.108 -p 2222 <command>`。

### 後でやる: xip の TOS 拒否をバイトパッチ (任意)

原因が TOS と特定できたので、xip が受信 IP ヘッダの TOS を見て弾いている箇所を
patchxip.py 方式で潰せば、素の sshd:22 や任意サーバにも通せる見込み。

### 開発・試験は b:pnetorig で

素の xiporig.x (MSS 512) を使う b:pnetorig に統一 (lead 方針)。リセット手順は同じ。
