# net/tistack -- xip.x の代わりになる常駐 TCP/IP 窓口 (tis.x)

ESP/X の xip.x (ソース無し、KA9Q NOS 由来) を置き換えるための常駐プログラム。
libsocket (elf2x68k) や ESP/X の libnetwork でビルドしたプログラムが **無修正で** こちらにつながるよう、
xip.x と同じ `_TI_*` 入口表を持つ。

裏方 (backend) は 2 つ:

- **ループバック** (既定): 127.0.0.0/8 と、`-a` で指定した自分のアドレス。同じ X68000 の中のソケット同士を
  TCP 風の信頼できるストリームでつなぐ。ネットワーク無しの XEiJ で API の層を固めた (下の「試験」)
- **Pico** (`-p`): TCP / UDP / DNS を Pico の lwIP に任せる (W5500 / ESP-AT 風)。ソケット操作を npd.x の
  SCSI 交換に載せて Pico に頼む (下の「Pico に任せる (tis.x -p、TSP)」)。127.0.0.0/8 はループバックのまま。
  **Mac の上の試験 (picosim、本物の lwIP) だけ。実機ではまだ動かしていない**

```
tis.x [-p] [-a a.b.c.d] [-b KB]   常駐する (-p: Pico に任せる、-a: 自分の IP もループバック扱い、-b: ソケット 1 個の受信バッファ、既定 8KB)
tis.x -s                     ソケット表を表示する
tis.x -t                     直近 128 回の呼び出し (命令・fd・長さ・戻り値) を表示する (調べもの用)
tis.x -r                     常駐を解く (開いているソケットがあれば断る。-rf で強制)
```

xip.x とは同時に常駐させない (libsocket は自分より前に常駐した "TCP/IP" を 1 つだけ見る)。tis.x は
起動時に先に常駐しているものを探し、あれば常駐しない。

## libsocket がスタックを見つける方法

`__sock_search_ti_entry()` (TCPPACKB の search_ti_entry.c に elf2x68k のパッチ) は:

1. 自分の PSP の直前のメモリブロック見出し (16 バイト) から `prev_mp` を順にたどる
2. `parent_mp` の上位 8 ビットが 0xFF (常駐ブロック) で、プログラム領域 (見出し + PSP 240 バイトの後ろ、
   つまり text の先頭) が `"TCP/IP"` で始まるものを探す。最初に見つかったものを使う
3. 入口は **text 先頭 + 8**。`d0 = 命令番号`, `a0 = 引数` で `jsr` し、`d0` が戻り値。d0-d2/a0-a2 は壊してよい
4. 入口の 8 バイト前を DOS `_MEMCPY` で読めなければ (スーパーバイザ保護領域) `SUPER_JSR` で呼ぶ
5. (FAST_API) 最初に命令番号 **-1** で呼び、0〜7 が返れば以後 `trap #n` で呼ぶ。tis.x は -1 を返して jsr のまま

ESP/X の libnetwork (rcmd.x がこれ) も同じ探し方で、FAST_API が無いだけ。

## _TI_ 入口表 (libsocket / libnetwork が使うもの)

命令番号は tcpipdrv.h の列挙順 (`_TI_get_version` = 0 ... `_TI_rip` = 76)。引数欄の `a0=fd` は
**a0 そのものが値** (ポインタではない)、`arg[]` は a0 が long 配列を指す。ソケット番号は **128〜159** (32 個)。
AF_INET = 0、SOCK_STREAM = 0、SOCK_DGRAM = 1 (KA9Q の値)。sockaddr_in のポートとアドレスはビッグエンディアン。

| 命令 | 引数 | 戻り値 | libsocket 側の扱い |
|---|---|---|---|
| socket | arg[3] = domain, type (SOCK_NONBLOCK は外して渡す), proto | fd / -1 | SOCK_NONBLOCK なら usock[].noblock=1 を直接書く |
| bind | arg[3] = fd, sockaddr*, len | 0 / -1 | -1 は EIO |
| listen | arg[2] = fd, backlog | 0 / -1 | 成功すると __sock_listen_fds に立てる |
| accept | arg[3] = fd, sockaddr*, socklen_t* | 新 fd / -1 | -1 で状態が 'L' (LISTEN) なら EAGAIN、それ以外は EIO。新 fd は noblock=0 に直す |
| connect | arg[3] = fd, sockaddr*, len | 0 / -1 | **-1 で状態が 'S' か 'E' なら EINPROGRESS** (ブロッキングでも)。それ以外は EIO |
| read_s | arg[3] = fd, buf, len | バイト数 / 0 (EOF) / -1 | -1 のとき: ブロッキングなら状態が 'S' でない限り **譲らずに即再試行**、ノンブロッキングなら EAGAIN |
| write_s | arg[3] = fd, buf, len | バイト数 / -1 | -1 のとき: ブロッキングなら同上、ノンブロッキングで状態が 'E' なら EAGAIN、それ以外 EIO |
| recvfrom | arg[6] = fd, buf, len, flags, sockaddr*, socklen_t* | 同 read_s | recv() は arg[4], arg[5] = 0 |
| sendto | arg[6] = fd, buf, len, flags, sockaddr*, len | 同 write_s | send() は arg[4], arg[5] = 0 |
| close_s | a0 = fd | 0 / -1 | close(fd>=128) とプロセス終了時 (at_exit) |
| socklen | arg[2] = fd, 0=受信 / 1=送信 | バイト数 / -1 | poll: 受信 >0 で POLLIN、送信 ==0 で POLLOUT、<0 で POLLERR |
| getsockname / getpeername | arg[3] = fd, sockaddr*, socklen_t* | 0 / -1 | |
| shutdown | arg[2] = fd, how | 0 / -1 | |
| sockstate | a0 = fd | 状態文字列 / NULL | 先頭 1 文字だけを見る ('L' 'S' 'E')。NULL は EBADF / EIO |
| sock_top | - | struct usock[32] の先頭 | **noblock (+4) を直接読み書き**する。並びは libsocket の struct usock と同じでなければならない |
| sockmode | arg[2] = fd, SOCK_BINARY/ASCII/QUERY | 以前のモード / -1 | SO_SOCKMODE |
| sockkick / usflush | a0 = fd | 0 / -1 | SO_SOCKKICK / SO_SOCKFLUSH |
| sockerr | a0 = fd | エラー文字列 / NULL | SO_SOCKERR |
| get_version | - | 版 | SO_GETVERSION |
| gethostbyname | a0 = 名前 | struct hostent* / NULL | 数字表記は libsocket が自分で処理。先に res_search (DNS) を試す |
| gethostbyaddr | arg[3] = addr*, len, type | struct hostent* / NULL | |
| res_search | arg[5] = name, class, type, answer, anslen | 長さ / -1 | DNS。-1 なら gethostbyname に回る |
| getservbyname / getservbyport / getprotobyname / getprotobynumber / getnetbyname / getnetbyaddr | | 構造体 / NULL | |

ESP/X libnetwork (rcmd.x) が追加で使うもの: `recvline` (arg[3] = fd, buf, len。改行まで)、`recvchar` /
`rrecvchar` (a0 = fd、1 文字か -1)、`seteol` (arg[2] = fd, 文字列)、`setflush` (arg[2])、`usesock` (a0 = fd、参照数 +1)、
`psocket`。rcmd.x の流儀:

- `listen(ls, 0)` (**backlog 0**): 接続が来ると listen したソケット自身が SYN RECEIVED → ESTABLISHED になり、
  `accept()` は同じ fd を返す。rcmd は `sockstate()` が "LISTEN" でなくなるのを _CHANGE_PR しながら待つ
- `sockstate()` は **大文字** ("LISTEN" "SYN RECEIVED" "ESTABLISHED" ...)。rcmd は先頭を大文字小文字無視で比べる
- `sockmode(c, SOCK_ASCII)` → `recvline()` → `sockmode(c, SOCK_BINARY)` → `write_s` を返り値で繰り返す → `usflush` → `close_s`
- 接続 1 本ごとに listen ソケットを閉じて **同じポートに bind し直す** (TIME WAIT で断ると "cannot re-listen")

その他の命令 (ARP / iface / rt / dns / MIB / rip、ifconfig.x / inetdconf.x / ping.x / netstat.x が使う) は
ループバックでは -1 か NULL を返す。

### xip.x で分かっている問題と tis.x の振る舞い

| 場面 | xip.x | tis.x |
|---|---|---|
| ブロッキング read で ESTABLISHED・データ無し | -1 を返し、libsocket が譲らずに再試行して固まる | 入口の中で _CHANGE_PR しながら待ち、データか EOF (0) で戻る |
| ノンブロッキング connect | EIO で失敗 | -1 + "SYN SENT" (→ EINPROGRESS)。次の tick で ESTABLISHED、poll の POLLOUT で分かる |
| ブロッキング connect | 0 | 0 (ESTABLISHED になるまで中で待つ。-1 を返すと libsocket が EINPROGRESS にする) |
| ノンブロッキング write > 4096 | 全部送るのに -1 (EAGAIN) を返す | 受信側の空きまで受け取ってその数を返す (部分書き込み)。0 は返さない |
| ブロッキング write | | 全部受け取るまで中で待つ (相手が切れたらそこまでの数) |
| 受信 TOS ≠ 0 | TCP データを捨てる | (ループバックには無関係) |
| EOF の通知 | | 受信バッファが空で相手が送信を閉じたら socklen(fd,0) = **-1** (poll は POLLERR、続く read が 0)。そのため EOF 後の getsockopt(SO_SOCKLENRECV) は EBADF になる |

本当のエラー (未接続・相手が消えた) で -1 を返すときは、続く **sockstate を 1 回だけ NULL** にする。libsocket は
ブロッキングでも状態が 'S' 以外なら譲らずに再試行し続けるが、状態が NULL なら EIO で止まる。そのため、エラーの
直後に別のプログラムが同じ fd の sockstate を読むと 1 回だけ NULL が見える。閉じ済みの fd (128〜159) の close は 0
(dropbear が同じ fd を 2 回閉じるため)。

ブロッキングの待ちは、libsocket に -1 を返して再試行させず、tis.x の中で `_CHANGE_PR` しながら待つ。
Human68k のスレッド (xip の受信、npd、テストのスレッド) はこの間に動く。

ノンブロッキングかどうかは usock[fd-128].noblock だけで決める (libsocket が直接書くので写しを持たない)。

## 設計

**Z の上に TCP は作らない。** tis.x は `_TI_*` の薄い窓口 (ソケット表・受信バッファ・状態の文字列・ブロッキングの待ち)
だけで、再送・ウィンドウ・タイマーは持たない。TCP は後の段階で Pico W の lwIP (pico-sdk / cyw43) が話し、
tis.x は「ソケット操作を npd の郵便受け越しに Pico に頼む」だけになる。ループバックの backend は同じ X68000 の
ソケットのバッファ同士でバイトを動かすだけ (TCP ではない)。backend の口はこの「頼む」形に合わせてある。

```
  プログラム (libsocket / libnetwork)
        │ jsr text+8 (d0=命令, a0=引数)
  ┌─────▼──────────────────────────────┐
  │ tis.c  API 層                      │  ソケット表 (usock[32] + 内部状態)、受信リング、状態機械、
  │   ti_dispatch()                    │  ブロッキングの待ち、名前解決、ASCII モード
  │   wait: leave → _CHANGE_PR → enter │
  └─────┬──────────────────────────────┘
        │ struct backend (be.h)
  ┌─────▼─────────┐   ┌──────────────────────────────────────┐
  │ lo.c ループ   │   │ (次) pico.c: npd の X6NR 交換に        │
  │ バック        │   │ ソケット要求レコードを載せ、Pico の     │
  └───────────────┘   │ lwIP が TCP を話す                     │
                      └──────────────────────────────────────┘
```

- **排他**: 入口に入ったら DOS `_SUPER` でスーパーバイザに移る。Human68k は割り込まれた側がスーパーバイザなら
  スレッドを切り替えない (Timer-D の切替えを保留する) ので、これだけで 1 CPU の上の排他になる。
  呼ばれたときすでにスーパーバイザ (SUPER_JSR、スーパーバイザのスレッド) ならそのまま。
  待つときはユーザーモードに戻ってから `_CHANGE_PR` し、戻ったら状態を調べ直す (待っている間に
  別のスレッドが同じソケットを閉じることがある)
- **スレッドは使わない** (ループバックは呼ばれたときに全部済む)。Pico 版でも、まずは呼び出し側の文脈で
  郵便受けを交換する (npd.x と郵便受けを共用するので、交換は npd.x のスレッドに頼む形が有力。下記)
- **tick**: 入口に入るたびと、待ちの 1 周ごとに `be->tick()` を呼ぶ。ループバックは SYN SENT → ESTABLISHED の
  昇格だけ (ノンブロッキング connect の非同期の道を本物と同じに通すため)。Pico 版は郵便受けの交換
- **バッファ**: ソケットごとに受信リング (既定 8KB × 32 = 256KB、常駐時に BSS の後ろに確保して `_KEEPPR` に含める)。
  ループバックの送信は相手の受信リングに直接書く (送信バッファ無し)。Human68k では常駐後に `_MALLOC` すると
  呼んだプロセスの持ち物になり、そのプロセスの終了で消えるので、動的確保はしない
- **フロー制御**: 送れる量 = 相手の受信リングの空き。空きが 0 なら socklen(fd,1) はリングの使用量 (≠0) を返して
  POLLOUT を止める
- **ポート**: bind(0) は 1024 から順に空きを探す。閉じたポートはすぐ再利用できる (TIME WAIT 無し)

### backend の口 (be.h)

API 層が持つもの: ソケット表、受信リング、状態 (文字列は API 層が作る)、待ち、名前解決。backend が持つもの:
相手との間の「接続を開く・データを運ぶ・閉じる」と非同期の進行 (tick)。

| 操作 | ループバック | Pico (予定) |
|---|---|---|
| tick() | SYN SENT を ESTABLISHED に | 郵便受けを 1 回交換: 溜まった要求と送信データを書き、応答 (状態変化・受信データ・空き) を読む |
| listen(s) | 何もしない (ポート表は API 層) | Pico の lwIP で tcp_listen。接続が来たら応答で子ソケットを作る |
| connect(s, addr, port) | 相手の listen を探して子を作る / 拒否 | 要求を積むだけ (SYN SENT)。結果は後の tick で |
| send(s, p, n) → 受け取った数 | 相手の受信リングに書く | 送信リングに積む。tick が Pico の空き (credit) の分だけ運ぶ |
| sendspace(s) | 相手のリングの空き | 送信リングの空き |
| shutdown(s, how) / close(s) | 相手に EOF を立てる / 相手から外す | 要求を積む (FIN)。close は応答を待たずに表から外し、Pico 側の後始末は Pico に任せる |

Pico 版 (pico.c) で決めたことは次の章。以下は最初の見込み (2026-10-22、ほぼこのとおりにした):

- **運ぶ形**: npd の X6NR 交換の 1 レコードとして「ソケット要求」(先頭 0xA6 など、アクセラレータの 0xA5 と同じ流儀)
  を載せる。要求: [命令][Z 側 fd][引数 / 送信データ]、応答: [事象][fd][状態 / 受信データ / Pico 側の受信空き]。
  IP パケットのレコードと同じ交換に混ぜられるので、xip.x と並べて移行できる
- **バッファの持ち主**: Z → Pico の送信データは Z の送信リング (tis.x) が持ち、Pico が受け取った (応答で ack) 分だけ
  捨てる。Pico → Z の受信データは Pico の lwIP の pbuf が持ち、Z が受信リングの空きを要求に毎回書き、
  Pico はその分だけ送る。Z がアプリに渡したら次の要求で空きが増え、Pico は tcp_recved する
  (端から端までのフロー制御になり、TCP のウィンドウが Z の受信リングに直結する)
- **譲り方**: 郵便受けの命令は数 ms かかる (USB の往復)。呼び出し側の文脈で待つと、その間 _CHANGE_PR で
  他スレッドに譲る npd.x の方式がそのまま使える。npd.x と同じ郵便受けを使うので、交換は 1 か所 (npd.x のスレッド)
  に集め、tis.x は要求を共有リングに積んで npd のスレッドを起こす形にするのが安全 (SCSI の命令を 2 か所から
  出さない)
- **状態**: Pico から来る状態変化 (connected / refused / peer FIN / reset) を API 層の状態に写す。
  ブロッキング connect / read は tick を回しながら待つ (ループバックと同じ道)

## Pico に任せる (tis.x -p、TSP)

2026-10-23。Z の上に TCP は作らず、ソケット操作そのものを Pico に頼む。Pico は lwIP で本物の TCP / UDP / DNS を話し、
Z (tis.x) はソケット表・受信リング・送信リング・ブロッキングの待ちだけを持つ。

### どこまでできているか

| | 状態 |
|---|---|
| 記録形式 `tsp.h` (Z と Pico で同じファイル) | 実装・ホストで試験済み |
| Z 側 backend `pico.c` + `tis.c` の手直し | 実装・ホストで試験済み (tis.x は m68k でビルドが通る、XEiJ / 実機では未試験) |
| Pico 側の窓口 `pico/tsp_svc.c` (lwIP raw API) + `tsp_plat.h` + `tsp_lwipopts.h` | 実装・**本物の lwIP 2.2** (pico-sdk の lib/lwip) の上でホストで試験済み。Cortex-M33 向けにコンパイルが通る (text 5.3KB / bss 7.9KB) |
| npd.x / npd.sys 1.2 の TSP レコードの受け渡し | 実装・m68k でビルドが通る。**試験していない** (実機が要る) |
| Pico のファーム (netpipe.c) への組み込み | 設計だけ (下)。新しいファーム (Pico Z Loader、Pico 2 W) に入れる |
| TLS (mbedTLS)、名前で connect して IPv6、ICMP (ping / traceroute) | 設計だけ。記録形式に場所を取ってある |

```
  プログラム (libsocket) ─jsr─> tis.x (tis.c + pico.c) ── TIS_NP_OUT / TIS_NP_IN ──> npd.x のスレッド
        ソケット表・受信リング・送信リング・待ち                            │ SCSI ID6 X6NR 交換 (8 セクタ)
                                                                            ▼
  LAN / Wi-Fi <── lwIP (tcp_* / udp_* / dns_*) <── tsp_svc.c <── netpipe.c (先頭 0xA6 のレコード)
```

### 記録形式

npd の X6NR 交換 (IP パケットの `[長さ 2][中身][詰め物]` の列) に、中身の先頭が **0xA6 (TSP_MARK)** のレコードを混ぜる。
アクセラレータの 0xA5 と同じ流儀で、IPv4 のバージョン nibble と重ならないので古いファームは不正な IP として捨てる。
**1 回の交換に要求 1 個と応答 1 個**。その中にメッセージを並べる (ソケットごとのレコードにしない: Pico → Z の
レコードには 1 個ごとに 14 バイトの空きが付くので、まとめた方が無駄が少ない)。

```
要求 (Z → Pico): [A6][ver 1][flags][0][zseq 2][0 2][zepoch 4]   + メッセージ…    ≦ 4078 バイト (8 セクタ - 16 - 長さ 2)
応答 (Pico → Z): [A6][ver 1][flags][0][zack 2][0 2][pepoch 4]   + メッセージ…    ≦ 4064 バイト (さらに空き 14)
メッセージ:      [type][slot][gen][0][len 2][本体][偶数詰め物]
```

| Z → Pico | 本体 | Pico → Z | 本体 |
|---|---|---|---|
| CONNECT | proto, flags (NAME / TLS), lport, rport, addr, [名前] | CONNECTED | laddr, lport, raddr, rport |
| LISTEN | backlog, lport, laddr | BOUND | lport (LISTEN / UDPOPEN の返事) |
| UDPOPEN | lport, laddr | ACCEPT | (listen の slot) handle, laddr, lport, raddr, rport |
| DATA | TCP のデータ (CREDIT まで) | DATA | TCP のデータ (WINDOW まで) |
| SENDTO | addr, port, データ | DGRAM | addr, port, データ (1 個) |
| WINDOW | 受信リングの空き, 送りたい量 | CREDIT | 次の要求で送ってよい量 (絶対値) |
| SHUT | how (1 = FIN) | EOF | 相手の FIN (データを渡し切ってから) |
| CLOSE | - | ERROR | code (refused / timeout / unreach / reset / netdown / notsup ...)。Pico 側はもう無い |
| ADOPT / REJECT | handle, flags (LISTENER), 空き | RESOLVED | (slot = 問い合わせ番号) code, addr |
| RESOLVE | (slot = 問い合わせ番号) 名前 | | |

- **slot と gen**: slot = Z のソケット番号 - 128。gen はその番号を Pico 側で使い直すたびに増やす数で、Pico からの
  メッセージの gen が今のものと違えば Z は捨てる。交換の途中 (npd.x のスレッドが SCSI を待っている間) にアプリが
  閉じて同じ番号を開き直すと、戻ってきた応答の前のソケットのデータが新しいソケットに入るため
- **accept**: Pico は Z の番号を決められないので、ACCEPT は Pico 側の handle を知らせ、Z が番号を割り当てて ADOPT で
  引き取る (子のデータは ADOPT の後から流れる。同じ要求の中で Z から送り始められる)。表が一杯なら REJECT (Pico が切る)。
  **backlog 0 (rcmd.x)** は listen した番号自身が接続になる: ADOPT に LISTENER の印を付け、Pico は listen をやめる
  (引き取りまでの 2 本目は Pico が断る)。lwIP の backlog (TCP_LISTEN_BACKLOG) は使わず、tsp_svc が数える
- **名前**: gethostbyname は RESOLVE (問い合わせ 4 個まで同時)。Pico の lwIP の DNS が引く。引いた名前とアドレスを
  4 個覚えておき、そのアドレスへの CONNECT には NAME の印と名前を添える (TLS の SNI と、IPv6 のために Pico が名前を
  知っておく)。addr が 0 で名前がある CONNECT は Pico が引いてからつなぐ (記録形式の試験で確認)
- **TLS**: CONNECT の flags の TLS (0x02) を予約。いまの Pico は ERROR NOTSUP を返す

### 信頼性: 止まって待つ (stop-and-wait)

npd.x の交換は書き込み / 読み込みの失敗でレコードを失うことがある (IP なら TCP が再送するので npd.x は捨てていた)。
ソケット要求は失えないので TSP の層で再送する:

- Z は要求に番号 zseq を振って手元に持ち、応答の zack が zseq と一致するまで **同じ要求をそのまま送り直す**
- Pico は最後の応答 (≦ 4064 バイト) を持っておき、zseq が前回と同じなら **適用せずに同じ応答を返す**
- 1 回の交換が要求 1 個と応答 1 個なので窓は 1 で足りる。次の要求は前の応答を全部 Z に写してから作るので、
  WINDOW / CREDIT の数は常に「直前の応答のあと」の値になり、ずれない (下のフロー制御がこれに頼る)
- Z の読み込みキャッシュから古い応答が返っても zack が違うので捨てる (npd.x の X6NR の番号の確かめの上にもう 1 段)

### ブロッキングの待ち

backend は待たない (be.h の約束のまま)。tis.c のブロッキングの道 (connect / read / write / accept) は状態を見ては
`_CHANGE_PR` するだけで、その間に npd.x のスレッドが交換し、TIS_NP_IN で ts[] と受信リングが書き換わる
(npd.x のスレッドはスーパーバイザで tis.x を呼ぶので、tis.c の排他 (_SUPER) と同じ仕組みで守られる)。
ループバックの試験で確かめた _OPEN_PR のスレッドでの待ち方がそのまま使える。足したもの:

- **listen / UDP の bind は Pico の返事 (BOUND / ERROR) を待つ**。待たないと listen 直後に来た接続を Pico が断り、
  使用中のポートも -1 にできない
- **gethostbyname** は RESOLVED を待つ (問い合わせの表が一杯なら空くまで)
- **close**: 送信リングにデータが残っていれば、番号を S_ZOMBIE にして出し切ってから CLOSE を送る
  (アプリから見ると閉じ済み。rcmd.x も dropbear も write の直後に close する)。Pico は残りを送ってから FIN
  (読み残しがあれば lwIP が RST)
- 急ぎの判定 (TIS_NP_OUT を最大 0 で呼ぶ): 制御メッセージがある、CREDIT のある送信データがある、WINDOW を送るべき、
  閉じる・SHUT を送るべき。npd.x はこれと Pico の MORE が無ければ 60ms ごとにしか交換しない

### フロー制御と公平さ

- **Pico → Z**: Z は受信リングの空きを WINDOW で知らせ (リングの 1/4 か 1KB 空くか、空になったとき)、Pico はその分
  だけ送る。Pico は受けた pbuf を Z に渡すまで持ち、**渡した分だけ tcp_recved** する。TCP の窓が Z の受信リングに
  つながる (Z のアプリが読まなければ相手が止まる)。Pico が持つのはソケットあたり TCP_WND まで
- **Z → Pico**: Pico は応答のたびに CREDIT (次の要求で送ってよい量、絶対値) を知らせる。上限は tcp_sndbuf、
  送信キューの空き (tcp_write 1 回は ceil(n / mss) + 1 個の pbuf を使い、Z は 1 要求で 1 ソケットにつき DATA を
  1 個しか送らない)、lwIP 全体の TCP segment の残り、送信の合計の上限 TSP_TXPOOL (12KB)。Z が WINDOW で
  「送りたい量」(送信リングが空でなければリングの大きさ、空なら 0) を申告し、Pico は申告の無いソケットにも
  1KB (CREDIT_FLOOR) を残す (打鍵が待たずに出る)。stop-and-wait なので CREDIT は減らしてもよい
- **公平さ**: どちらの向きも、データは毎回始まりのソケットをずらしながら、送れるソケットで等分してから (最低 512)
  余りを詰める。測った値: 4MB を受けている最中の打鍵の echo が平均 10ms、最悪 2 交換
- Pico の MORE (応答の flags) は「Z に渡せるものがまだある」ときだけ。Z の窓が 0 なら立てない (npd.x が空回りしない)
- 試験の中で tcp_write の ERR_MEM は 0 回 (CREDIT の計算が lwIP の限りの内側にある)

### Pico の RAM (Pico 2 W、520KB の中の見込み)

`pico/tsp_lwipopts.h` の値 (ファームの lwipopts.h から include する)。大きさは arm-none-eabi-gcc (Cortex-M33) で測った。

| もの | 大きさ |
|---|---|
| lwIP のヒープ MEM_SIZE (送信の pbuf: TSP_TXPOOL 12KB + 見出し・制御パケット) | 20KB |
| PBUF_POOL 16 個 × 約 1.6KB (受信。**cyw43 の受信と共用**) | 約 25KB |
| memp: TCP pcb 40 × 164 B、listen 8 × 28、segment 48 × 16、UDP 12 × 32 | 約 8KB |
| tsp_svc の表 (Pico 側ソケット 40 個 3.7KB + 最後の応答 4KB) | 7.9KB (bss) + text 5.3KB |
| 合計 | **約 61KB** (うち受信プールの 25KB は Wi-Fi と共用) |

- TCP_MSS 1460、TCP_WND 4 × MSS (5840)、TCP_SND_BUF 8 × MSS (1 交換分 4KB を書いた直後でも次の CREDIT が残る大きさ。
  4 × MSS では Z → LAN が 2040 バイト / 交換に落ちた)、TCP_SND_QUEUELEN 24
- **受信側の上限は TCP_WND × 同時に受けているソケットの数**。試験の 16 本同時 echo では Pico が Z に渡していない受信が
  最大 65KB になった (ホストの試験はプールに限りが無い)。実機では PBUF_POOL が尽きた分を cyw43 が捨て、相手が再送する
  (遅くなるが壊れない)。気になるなら、Z から開く接続は tcp_connect の前に pcb の rcv_wnd を小さくして始める
  (同時に受けている数で割る) 手がある (未実装)
- Pico 側ソケット 40 個 = Z の 32 番号 + 引き取り待ちの子 8

### Pico の再起動・Z のリセット (epoch)

- 応答の pepoch は Pico の起動ごとに変わる数。Z は値が変わったら Pico 側のソケットを全部無くしたことにする
  (つながっていれば EOF + "Network is down"、connect 中なら -1、ZOMBIE は即解放、UDP は次の sendto で開き直す、
  問い合わせ中の名前は失敗)
- 要求の zepoch は tis.x の常駐ごとに変わる数 (日付・時刻から)。Pico は知らない zepoch を見たら持っているソケットを
  全部捨てる (相手には RST)。Z をリセットして tis.x を入れ直したとき、前のソケットが Pico に残らない
- zseq の初めも時刻から決める (Z の読み込みキャッシュはリセットで消えない。npd.x と同じ理由)
- `tis.x -r` は npd.sys から入口を外す (npd.x が解放された入口を呼ばない)。Pico 側のソケットは次の tis.x の zepoch で捨てる
- Pico は Wi-Fi が切れたら `tsp_shutdown()` → 別の epoch で `tsp_init()` (Z は Network is down を見る)

### SLIP / xip.x の道との共存、Z の IP アドレス

- 同じ npd.x の交換に IP のレコード (xip.x)、0xA5 (アクセラレータ)、0xA6 (tis.x) が並ぶ。npd.x は ETD 19 の IP レコードの
  後ろに TSP レコードを置き (入らなければ次の交換)、受けた 0xA6 を tis.x に渡す。古い npd.sys (1.1、ETD 26 が無い) では
  tis.x -p は常駐を断り、npd.x は TSP を扱わない
- libsocket は先に常駐した "TCP/IP" を 1 つしか見ないので、xip.x と tis.x は同時には常駐させない (移行中は切り替えて使う)
- **アドレスが変わる**: Pico に任せたソケットは **Pico の IP** から出る。今の bridge モードは Z の IP (192.168.1.68) を
  Pico が代理 ARP で名乗り、IP パケットを xip.x に渡している。tis.x -p で Z が listen したポート (rcmd 2323 など) は
  Pico の IP で受けることになる。案: Pico Z Loader のファームでは **tis.x モードのとき Pico が Z の IP を自分の IP として
  名乗る** (LAN から見ると今までどおり 192.168.1.68 が Z)。Pico 自身のサービス (HID の UDP 6869、設定 / HDS の HTTP など)
  は同じ IP の決まったポートを予約し、Z がそのポートを listen すると INUSE で断る。xip.x モードでは今の代理 ARP のまま
- getsockname は Pico が実際に使ったアドレスとポートを返す (CONNECTED / ACCEPT が運ぶ)

### Pico のファームに入れるとき (設計)

1. `tsp.h`、`pico/tsp_svc.[ch]`、`tsp_plat.h`、`tsp_lwipopts.h` をファームに置き、lwipopts.h から tsp_lwipopts.h を include する
   (SO_REUSE、LWIP_DNS、TCP_LISTEN_BACKLOG 0 が要る)
2. 起動時 (lwIP の初期化と Wi-Fi の接続の後) に `tsp_init(乱数)`
3. netpipe の Z → Pico のレコードの読み出し (今の `bridge_out_rec`) で、先頭 0xA6 のものを覚えておく
4. 応答を作るところ (今の `np_exchange`) で `tsp_exchange(req, len, resbuf + 16 + 16, 空き, &more)` を呼び、
   `[長さ 2][空き 14][応答]` を **先頭に** 置く。more は X6NR の見出しの more に足す。要求が無い交換では何も載せない
5. FreeRTOS + cyw43 (今の fork) では np_exchange は USB タスクの MSC 書き込みの中なので、tsp_exchange の中の lwIP
   呼び出しは `cyw43_arch_lwip_begin/end` (TSP_LOCK) で囲む。重い仕事は無い (lwIP の呼び出しと 4KB の写し)。
   新しいファーム (NO_SYS、ポーリング) なら同じループから呼ぶだけ

### この先 (設計だけ)

- **TLS**: CONNECT の TLS の印で Pico が mbedTLS (altcp_tls) を話し、名前を SNI と証明書の照合に使う。Z には平文の
  ストリームに見える。信頼の段階はアクセラレータと同じ考え (Pico が平文を見る)。Pico 2 W の RAM で 1〜2 本
- **IPv6**: gethostbyname の答えが AAAA しか無い名前のとき、Pico が「名前の札」として使っていない IPv4 アドレス
  (240.0.0.0/4 から) を返し、その札への CONNECT を Pico が名前でつなぐ。Z は v6 アドレスを扱わずに済む
- **ICMP**: ping / traceroute は Pico の raw pcb に頼むメッセージを足す (Z の ping.x 相当を作る)
- 名前の逆引き (gethostbyaddr)、res_search (nslookup / dig) は UDP を Z で組めばそのまま通る (今の UDP で足りる)

### 試験 (picosim)

```
make -C tests picosim && tests/picosim        # -x 交換の時間 µs (既定 5000)、-c Z の写し ns/バイト、-q 速さの表を省く
```

`tests/picosim.c` が tis.c (API 層) + lo.c + pico.c をそのまま Mac で動かし、`tests/picoworld.c` が Pico と LAN になる。
Pico 側は **本物の lwIP 2.2** (pico-sdk の lib/lwip、`LWIP=` で場所を変えられる) を NO_SYS・仮想の時計で動かし、
tsp_svc.c をそのまま載せる。LAN の相手 (echo 7 / discard 9 / source 19 / sshd 22 / RST 70 / UDP echo / DNS 53) も
同じ lwIP の中 (Pico の IP 10.0.0.1 へのループバック)。10.0.0.0/24 のほかの番地は黒い穴、それ以外は経路なし。

管 (npd.x の代わり) の決まり: 交換は Z からだけ、1 回に要求 ≦ 4078・応答 ≦ 4064 バイト、Z に急ぎか Pico に MORE が
あればすぐ次、無ければ 60ms ごと、1 回 5ms (既定)。書き込みの失敗・読み込みの失敗・古い応答を混ぜられる。

2026-10-23: **103 項目すべて通過**。見ているもの:

- gethostbyname を Pico の DNS で (NXDOMAIN は NULL、localhost は Z のまま)、名前で引いたアドレスへのブロッキング
  connect、getpeername / getsockname (Pico のアドレス)
- 1MB ずつ両方向 (中身を照合、write の直後の close で残りを出し切ってから FIN)
- 16 本同時のノンブロッキング echo (32KB ずつ、socklen で poll)
- 公平さ (4MB を受けながら打鍵の echo: 平均 10ms、最悪 2 交換)
- listen / accept (backlog 2 で 3 本目は断られる、LAN のクライアントとやり取り、相手の close → read 0 と CLOSE WAIT)
- **rcmd.x の流儀** (backlog 0、sockstate が LISTEN でなくなるまで _CHANGE_PR、accept が同じ fd、ASCII の recvline、
  write → usflush → close、同じポートに 3 回 bind し直す、引き取り中の 2 本目は断られる)
- **ssh.x (dropbear) の流儀** (名前を引いてブロッキング connect、ノンブロッキングにして socklen で poll、バナーと
  KEXINIT を往復、相手の close で read 0、同じ fd の 2 回 close が 0, 0)
- shutdown(SHUT_WR) のあと echo が 20000 バイト返して EOF・CLOSING、相手の RST → read 0 と write の "Connection reset"
- 拒否 ("Connection refused")、経路なし ("Network unreachable")、黒い穴 (ノンブロッキング connect → SYN SENT →
  lwIP の SYN の再送のあと 18 秒で "Connection timed out")
- UDP (sendto / recvfrom で echo、bind した受け口に LAN から 2 個 (区切りを保つ)、返事)
- 書き込みの失敗 5%・読み込みの失敗 5%・古い応答 5% を混ぜて 4 本 × 64KB と 1MB ずつが壊れない (送り直しを Pico が
  保存した応答で答える)
- Pico の再起動 (新しい pepoch → read 0、"Network is down"、そのあと新しい接続は通る)、tis.x の入れ直し
  (新しい zepoch → Pico が前のソケットを捨て、相手に RST)
- 記録形式を直に: 同じ要求 2 回に同じ応答、addr 0 + 名前の CONNECT を Pico が引いてつなぐ、TLS の印は NOTSUP
- 終わりごとに Z・Pico・lwIP にソケットが残っていない、Z の受信リングが溢れない、tcp_write の ERR_MEM が 0

速さ (1MB、仮想の時計。「Z の写し」は tis.x と npd.x が 1 バイトを写す時間で、送りは 3 回・受けは 2 回写す):

| 交換 1 回 | Z の写し | Z → LAN | LAN → Z |
|---|---|---|---|
| 2.5 ms | 0 | 1515 KB/s | 1509 KB/s |
| 5 ms | 0 | 772 KB/s | 769 KB/s |
| 10 ms | 0 | 390 KB/s | 389 KB/s |
| 2.5 ms | 0.6 µs (move.l) | 400 KB/s | 529 KB/s |
| 5 ms | 0.6 µs | 319 KB/s | 396 KB/s |
| 10 ms | 0.6 µs | 227 KB/s | 263 KB/s |
| 5 ms | 3 µs (1 バイトずつ) | 95 KB/s | 134 KB/s |

- 記録形式の効率: 1 交換あたりのデータ **Z → LAN 4033 バイト、LAN → Z 4018 バイト** (上限 4078 / 4064 の 99%)。
  1MB に 260 交換。速さは「交換 1 回の時間」で決まり、記録形式はほぼ限りまで詰めている
- **Z の側の律速は写し**。tis.c の memcpy は 1 バイトずつ (68000 で 1 バイト約 3µs) なので、今のままだと 100〜130KB/s
  (それでも xip.x の 45KB/s の 2〜3 倍)。long で写す memcpy (約 0.6µs / バイト) にすれば 300〜400KB/s の見込み
- SCSI の交換 1 回の時間は実機で測っていない (npd.x のメモでは SCSI の命令 1 回 0.2〜2.5ms、交換は書き込み + 読み込みの 2 回)
- 往復の遅れ: 交換のあと Pico に届いた答えは次の交換で来る。Z が何か送ったあとは、送信の残りの申告 (WINDOW) が
  急ぎになって続けて交換するので echo は 10ms、何も送っていないときに届いたものは最大 60ms (npd.x の問い合わせ間隔) 待つ。
  connect は 70ms (SYN を出した交換の次の問い合わせで CONNECTED)

### Pico 2 W のファームができたら確かめること

1. 実機の交換 1 回の時間 (SCSI の書き込み 8 セクタ + 読み込み 8 セクタ)。速さはほぼこれで決まる
2. tis.c の memcpy を long にして、Z の写しの時間を実機で測る (XEiJ でも測れる)
3. npd.x / npd.sys 1.2 の TSP の受け渡し (ETD 25 / 26、TIS_NP_OUT の問い合わせ、MORE による続けての交換)
4. USB タスクから tsp_exchange を呼ぶときのロック (cyw43_arch_lwip_begin) と所要時間 (MSC 書き込みの応答が遅れないか)
5. PBUF_POOL が尽きたときの振る舞い (多数の同時ダウンロード) と、TCP_WND / TCP_SND_BUF の値
6. Pico が Z の IP を名乗る形 (tis.x モード) と、予約するポート
7. Wi-Fi が切れたときの tsp_shutdown → tsp_init (Z に Network is down が出るか)
8. 無修正の rcmd.x / ssh.x を実機の tis.x -p で (XEiJ には Pico が無いので、XEiJ では TSP の相手を Z の中に置く試験台が要る)

## ファイル

- `start.S`: text 先頭の `"TCP/IP\0\0"` と入口 (text+8)、起動処理
- `tis.c`: API 層 (ti_dispatch)、常駐・表示・解除
- `lo.c`: ループバックの backend
- `pico.c`: Pico の backend (TSP の要求を組み、応答をほどく)
- `tsp.h`: TSP の記録形式 (Z と Pico で同じ)
- `pico/tsp_svc.[ch]`: Pico 側のソケット窓口 (lwIP raw API)、`pico/tsp_plat.h`: ファームとの間 (ロック・ログ)、
  `pico/tsp_lwipopts.h`: 前提にする lwIP の大きさ、`pico/host/lwipopts.h`: ホストの試験用
- `be.h`: API 層と backend の間の口
- `tests/`: libsocket の試験プログラムと XEiJ の試験台 (xeij_test.sh)

## 試験

3 段で試す。Human68k はシングルタスクなので、並行して動く相手が要る試験だけを XEiJ のスレッドに回す。
Pico の backend は 4 段目としてホストの picosim (上の「試験 (picosim)」) で試す。

1. **ホスト (Mac)**: `make -C tests host && tests/hostsim [回数]`。tis.c と lo.c をそのまま Mac でビルドし、
   _TI_ の入口を libsocket と同じ引数で呼ぶ (tests/hostsim.c、hostinc/ が x68k/dos.h の代わり)。`_CHANGE_PR` は
   差し込み口になっていて、「待っている間に相手が書く / 閉じる / 接続して来る / 自分が閉じられる」を作れる。
   戻り値・状態の文字列・errflag の約束を 50 項目ほどと、リングの折り返し (受信バッファ 1〜300 バイト) の
   乱数試験、乱数の命令列のあとの表の整合を見る。2026-10-22: 5000 回で 10049 項目すべて通過
2. **XEiJ、1 本のプログラム** (tistest.x): 両端をノンブロッキングで交互に動かす (下記)
3. **XEiJ、スレッド** (tisrun.x): `tisrun.x echo` はスレッドがブロッキングの echo サーバ (accept / read / write)、
   フォアグラウンドがブロッキングのクライアント (connect / write / read) で、poll を使わずに 100,000 バイトを
   往復させる (ブロッキングの待ちが _CHANGE_PR で相手に譲ることの確認。3.2 秒)。最後に無修正の rcmd.x と ssh.x

### XEiJ (ネットワーク無し)

```
tests/xeij_test.sh            # tistest echo rcmd ssh を全部。結果は $TIS_OUT/boot/*.TXT (既定 $TMPDIR/tistest)
tests/xeij_test.sh tistest    # 1 つだけ
```

XEiJ (SUPER、10MHz 68000) をホストのディレクトリから起動し (CONFIG.SYS に `PROCESS = 16 10 20`)、
AUTOEXEC.BAT が tis.x を常駐させて試験を動かす。2026-10-22 に全部通った:

- **tistest.x** (46 項目、1 本のプログラムでノンブロッキングのソケットを交互に動かす。スレッド無し):
  gethostbyname("localhost") / getservbyname、ノンブロッキング connect → EINPROGRESS → poll POLLOUT →
  SO_ERROR 0、listen の poll POLLIN → accept、getsockname / getpeername、エコー、空のノンブロッキング read →
  EAGAIN、20000 バイトの write → 8192 の部分書き込み、一杯で EAGAIN と POLLOUT 無し、SO_SOCKLENRECV、
  1〜7 バイトの write 5000 回、**300KB を両方向に同時に (中身を照合、20 秒)**、stdin とソケットの poll
  (300ms の時間切れ、ソケットだけ POLLIN)、shutdown(SHUT_WR) → 残りを読んで EOF・CLOSE WAIT・逆向きは使える、
  相手の close → read 0・write EIO、**ブロッキング read が EOF で 0**、拒否 (EIO)、ループバック以外 (-1)、
  ブロッキング connect → 0、閉じたポートへの bind し直し、使用中のポートへの bind は失敗、backlog 0 で
  accept が listen ソケット自身を返す、UDP の sendto / recvfrom (区切りを保つ)、ソケットが残らない
- **rcmd.x** (無修正。x68kcli/build/fix.hds の /USR/BIN/rcmd.x、ESP/X libnetwork でビルドされた少し前の版。
  @put はある): `tisrun.x rcmd` がフォアグラウンドで rcmd.x を起動し、スレッドが 127.0.0.1:2323 のクライアントに
  なる。echo (終わりの印まで)、**@put 300,000 バイト (115KB/s、中身を照合)**、exit → bye
  (返事を **ブロッキングの read** で受け、rcmd.x が書くまで tis.x の中で待って、EOF で 0 が返る)。
  backlog 0・大文字の sockstate・ASCII モードの recvline・1 接続ごとの bind し直しがそのまま通る
- **ssh.x** (無修正。x68k-ssh の dropbear 移植、09:26 版): `tisrun.x ssh` のスレッドが 127.0.0.1:22 の偽の
  SSH サーバになり、バナーを送る。ssh.x の connect (ブロッキング)・バナー `SSH-2.0-dropbear_2022.83`・
  KEXINIT (304 バイト) まで届く。初めは、サーバが切ったあと ssh.x が "Error closing fd 129, I/O error" で
  終わった (dropbear が同じ fd を 2 回 close し、libsocket が 2 回目の失敗を EBADF ではなく EIO にする)。
  実機の xip.x ではこのエラーが出ていないので、閉じ済みの fd の close は 0 を返すようにした。いまは偽のサーバが
  切ったあとの次の write で "Error writing: I/O error" (相手が消えた = EIO) で終わる (期待どおり)
- **常駐の解除**: `tis.x -r` のあと libsocket はスタックを見つけられず、tistest.x はきれいに失敗する

スレッドの試験台 (tests/tisrun.c): `_OPEN_PR` でユーザーモードのスレッドを作り (眠った状態で作られるので
`_SEND_PR` で起こす)、`_EXEC` で子 (rcmd.x / ssh.x) を動かす。子が `_CHANGE_PR` で譲るか、タイムスライスで
スレッドが動く。スレッドはこのプログラムのメモリで動くので、子の終了後にスレッドの終わりを待ってから終わる。
スレッドの中で malloc は使えない (子がすぐ後ろのメモリを取るので、ヒープを _SETBLOCK で伸ばせない)。

XEiJ のホストファイルシステムで分かったこと (tis.x とは無関係、tests の作りに効く):

- **dup2 で付け替えた標準出力に、子 (_EXEC) の出力が入らない**。rcmd.x の rcmd.out が空になり、
  コマンドの出力を返す試験ができない (300KB の Z → 相手の向きは tistest.x で見ている)。
  親のあとの printf も消えることがあるので、tisrun.x の結果は `-o` のファイルに書く
- 子の標準エラー出力は `_dos_dup2` で付け替えれば取れる (`tisrun.x -e`、ssh.x のエラーはこれで読んだ)
