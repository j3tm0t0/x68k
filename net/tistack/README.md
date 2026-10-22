# net/tistack -- xip.x の代わりになる常駐 TCP/IP 窓口 (tis.x)

ESP/X の xip.x (ソース無し、KA9Q NOS 由来) を置き換えるための常駐プログラム。
libsocket (elf2x68k) や ESP/X の libnetwork でビルドしたプログラムが **無修正で** こちらにつながるよう、
xip.x と同じ `_TI_*` 入口表を持つ。

いまの実装は **ループバックだけ** (127.0.0.0/8 と、`-a` で指定した自分のアドレス)。同じ X68000 の中の
ソケット同士を TCP 風の信頼できるストリームでつなぐ。ネットワーク無しの XEiJ で API の層を固めるのが目的で、
次の段階で Pico W に TCP を肩代わりさせる裏方 (backend) を足す (W5500 / ESP-AT 風、npd の SCSI 郵便受け越し)。

```
tis.x [-a a.b.c.d] [-b KB]   常駐する (-a: 自分の IP もループバック扱い、-b: ソケット 1 個の受信バッファ、既定 8KB)
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

ブロッキングの待ちは、libsocket に -1 を返して再試行させず、tis.x の中で `_CHANGE_PR` しながら待つ。
Human68k のスレッド (xip の受信、npd、テストのスレッド) はこの間に動く。

ノンブロッキングかどうかは usock[fd-128].noblock だけで決める (libsocket が直接書くので写しを持たない)。

## 設計

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

Pico 版の考え (次の段階):

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

## ファイル

- `start.S`: text 先頭の `"TCP/IP\0\0"` と入口 (text+8)、起動処理
- `tis.c`: API 層 (ti_dispatch)、常駐・表示・解除
- `lo.c`: ループバックの backend
- `be.h`: API 層と backend の間の口
- `tests/`: libsocket の試験プログラムと XEiJ の試験台 (xeij_test.sh)

## 試験 (XEiJ、ネットワーク無し)

```
tests/xeij_test.sh            # tistest rcmd ssh を全部。結果は $TIS_OUT/boot/*.TXT (既定 $TMPDIR/tistest)
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
  なる。echo (終わりの印まで)、**@put 300,000 バイト (115KB/s、中身を照合)**、exit → bye。
  backlog 0・大文字の sockstate・ASCII モードの recvline・1 接続ごとの bind し直しがそのまま通る
- **ssh.x** (無修正。x68k-ssh の dropbear 移植、09:26 版): `tisrun.x ssh` のスレッドが 127.0.0.1:22 の偽の
  SSH サーバになり、バナーを送る。ssh.x の connect (ブロッキング)・バナー `SSH-2.0-dropbear_2022.83`・
  KEXINIT (304 バイト) まで届く。サーバが切ったあと ssh.x は "Error closing fd 129, I/O error" で終わる
  (dropbear が同じ fd を 2 回 close し、libsocket が 2 回目の失敗を EBADF ではなく EIO にするため。tis.x は
  2 回目に -1 を返すだけ)
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
