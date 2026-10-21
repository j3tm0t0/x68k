# net/ssh

X68000 / X68000 Z 用の SSH クライアント。Z から Linux ホストへ `ssh` して
コマンドを実行することを目指す (例: `ssh.x moto@192.168.1.108 uname -a`)。

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

## 現状: 受信がブロックされている (xip.x の能動オープンの不具合)

socket / connect / **送信** は動く (Pi の sshd が Z のクライアントバナーを
ACK するのを tcpdump で確認)。しかし **能動オープン (connect したクライアント)
のソケットでサーバから最初のデータ (41 バイトの SSH バナー) が届いた瞬間に、
Z の xip が相手へ RST を返し、データを捨てる。** ソケットの状態は手元では
ESTABLISHED のまま、受信バッファは空のまま。

- 受動オープン (rcmd = listen/accept 側) は大量受信 (@put で ~500kbps) でき
  ているので、**能動オープンの受信固有の問題**。
- 配布物の純正 `telnet.x` (FCT TCP/IP 2.00、xip β5 と同じ) を Z から
  `telnet.x 192.168.1.108 22` で動かしても **まったく同じ RST** になる。
  → 自作コードや libsocket の不具合ではなく、xip 側。
- 線上はすべて整合 (バナーの seq == Z がアドバタイズした rcv_nxt、順序内)
  なのに RST する。SYN_SENT→ESTABLISHED の経路で何かのフィールドが初期化
  されていない疑い。

SSH は双方向の能動オープンが要るので、ここが直るまでクライアントは完成
できない。考えられる道:
1. xip.x の能動オープン受信を直す (patchxip.py と同じやり方のバイトパッチ)。
2. Z を受動側 (listen) にして、Pi 側の中継が sshd へ橋渡しする回避策。
3. netpipe/SLIP 経路固有かどうかを別リンク (XEiJ など) で切り分ける。
