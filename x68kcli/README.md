# x68kcli

X68000 (Human68k 3.02) 用の CLI 環境入り SCSI HDD イメージ (`.hds`) を、macOS 上で再現可能に生成する。

- シェル: **zsh 2.5.03** (既定、ログインシェル) / fish 0.8 (tcsh 系) / ksh 4.9
- エディタ: **Vim 3.0** (vi 互換, `vi` / `vim` で起動)
- UNIX コマンド: ITA TOOLBOX (ls cp mv rm mkdir rmdir cat head tail wc tr cut paste sort系 …)、GNU gawk 2.15 (`awk` でも可)、GNU sed 4.9 / grep 3.11 (elf2x68k でクロスビルド)、GNU make 3.79、GNU tar 1.11 (+gzip)、LHA 2.62
- ファイル名拡張: TwentyOne.x (18+3 文字、マルチピリオド、`/` 区切り)
- C コンパイラ: **GCC 1.42 真里子版** (`gcc`、LIBC 1.1.32A + HAS.X + HLK.X)、**XC 2.1** (`cc`、シャープ純正)、GCC 2.95.2 beta4 (`gcc295`、68020 以上専用)
- ターミナル: MuTERM 1.23 (`muterm` で起動。RS-232C 直結で Raspberry Pi などのシリアルコンソールにログインする設定済み。[シリアルログイン](#シリアルログイン-x68000-z--raspberry-pi) 参照)
- キー配置変更: KeyWitch.x 2.13 (+ US 配列用 `us.env`。X68000 Z に US 配列 USB キーボードをつなぐ場合向け)

bash / tcsh の Human68k 移植バイナリは現存アーカイブに見当たらなかったため、
それぞれに最も近い zsh / fish を収録している。

## 必要なもの

- macOS (Apple Silicon / Intel) または Linux。`curl`, `python3`, `lha` (`brew install lha`)
- 起動確認に XEiJ を使う場合: Java 26 以降 (`brew install openjdk`)

## 使い方

```sh
make image          # tools 導入 → アーカイブ取得 (sha256 検証) → 展開 → build/x68kcli.hds 生成
make run            # XEiJ で build/x68kcli.hds から起動 (SCSI ID 0)
make ls             # イメージ内のファイル一覧
```

`SIZE_MB=200 make image` でディスク容量を変えられる (既定 100MB、1 パーティション)。
`NET=ppp` / `NETSPEED=19200` でシリアル TCP/IP の方式と速度 (既定 SLIP 38400bps、下記) を、`HOMEDIR=B:/home` で HOME を変えられる
(A: はイメージを作り直すと上書きされるので、残したいファイルや `~/.zhistory` は別ドライブの HOME に置くとよい)。
`KEYMAP=us make image` にすると CONFIG.SYS で KeyWitch を US 配列設定 (`\ETC\KEYWITCH\us.env`) で常駐させる。
既定の `jis` でも zsh から `uskey` / `jiskey` で切り替えられる。XEiJ はホスト側でキー配置を変換するので `jis` のままでよい。
`make image PROFILE=dev` は開発用の `build/x68kdev.hds` を作る。中身は同じで、起動時の自動接続・rcmd・GraySaver を動かさず、
COMMAND.X のコンソールを RS-232C (38400bps) に出す (`CTTY AUX`)。Pi 側は `ppp-x68k` を止めて `screen /dev/ttyS0 38400` で操作し、Z の画面に戻すには `CTTY CON`。

起動すると CONFIG.SYS → AUTOEXEC.BAT → TwentyOne 常駐 → `zsh -l` と進み、`A:/%` プロンプトになる。
`exit` で COMMAND.X に戻る。fish / ksh は `fish.x` / `ksh.x -l` で起動 (`-l` で /etc/profile を読む)。

### C 開発

```sh
gcc hello.c -o hello.x      # GCC 1.42 真里子版 (/usr/include, /usr/lib = LIBC 1.1.32A)。68000 で動く
cc hello.c                  # XC 2.1 純正 (/xc/include, /xc/lib)。hello.x ができる
gcc295 hello.c -o hello.x   # GCC 2.95.2 beta4。68020 以上専用で X68000 (Z) では SIGBUS になる (XEiJ は -model=X68030)
```

vi からは `:!gcc %` や `:make` (GNU make 3.79、`MAKE_SHELL`=zsh) で呼べる。`:sh` / Ctrl-Z で zsh に降りて `exit` で戻る。
XC 付属の AS.X / LK.X / AR.X / LIB.X / DB.X / SCD.X は `/xc/bin` にある (PATH 末尾)。

## イメージの中身

```
A:\            HUMAN.SYS COMMAND.X CONFIG.SYS AUTOEXEC.BAT STARTUP.ENV  (Human68k 3.02 システム一式)
A:\SYS, \BIN   Human68k 標準ドライバ・コマンド
A:\USR\BIN     zsh.x fish.x ksh.x vim.x vim.hlp TwentyOne.x gawk.x make.x tar.x lha.x + ITA TOOLBOX
A:\USR\DOC     各ソフトの添付ドキュメント
A:\ETC         termcap zshenv zshrc fishrc profile  KEYWITCH\*.env
A:\HOME        ホームディレクトリ ($HOME)
A:\TMP         一時ファイル ($temp)
```

設定ファイルは `templates/` にある (UTF-8 で書き、ビルド時に Shift_JIS / CRLF へ変換)。

## 仕組み

| スクリプト | 役割 |
|---|---|
| `scripts/tools.sh` | rb-cli (Human68k FS にファイルを書けるツール) と XEiJ を `tools/` に導入 |
| `scripts/fetch.sh` | `sources.txt` の URL からアーカイブを `dl/` に取得し sha256 を検証 |
| `scripts/stage.sh` | アーカイブを展開し、イメージに入れるツリー `stage/` を作る |
| `scripts/mkhds.py` | 空の SCSI HDD イメージを作る (クラスタは 8KB: rb-cli はディレクトリが 1 クラスタを超えると FAT チェーンを壊すため)。X68SCSI1 ヘッダ、Sharp 純正の SCSI IPL / デバイスドライバ / パーティション IPL (FORMAT.X から抽出)、BPB、FAT を XEiJ のフォーマッタと同じレイアウトで書く |
| `scripts/mkbatch.py` | `stage/` を rb-cli の batch JSON に変換 |
| `scripts/fixfs.py` | rb-cli が空白で埋めるディレクトリエントリの拡張名フィールドを 0 埋めに直す (直さないと Human68k が HUMAN.SYS を見つけられない) |
| `harness/xeij/run.sh` | XEiJ を起動 (貼り付け/制御パイプ有効、エージェント付き) |

### 起動確認の自動化 (XEiJ)

`harness/xeij/agent/XEiJAgent.java` は XEiJ に `-javaagent` で載せる小さなエージェントで、
`harness/xeij/xeijcmd.sh screenshot` (→ `capture/N.png`)、`harness/xeij/xeijcmd.sh textcopy` (テキスト画面をクリップボードへ → `pbpaste`)
などをファイル経由で実行できる。`harness/xeij/xeijtype.py 'ls -l\n'` で制御パイプからキー入力を送れる。
`make run` 後にこれらを組み合わせると、画面を見ずに Human68k 上のコマンド実行結果を確認できる
(`run.sh` は `-usecanvas=off` で起動する。キャンバス描画のままだと screenshot が黒くなる)。

動作確認は XEiJ 0.26.10.08 (IPLROM 1.6) と X68000 Z (Pseudo SCSI, USB メモリの `X68000Z/` に HDS と `pscsi.ini` を置く)
で行った (2026-10-19)。X68000 Z では `Ctrl + F11 + Del` のソフトリセットで OS だけ再起動できる。XM6 / 実機は未検証。

## シリアルログイン (X68000 Z → Raspberry Pi)

X68000 Z の UART 端子 (3.3V TTL) と Raspberry Pi の GPIO UART を 3 線で直結し、MuTERM から Linux のシリアルコンソールにログインする。
RS-232C レベル (±12V) ではないので USB シリアル変換やレベル変換は入れない。

| X68000 Z UART ケーブル | Raspberry Pi 40pin |
|---|---|
| 赤 (RX) | pin 8 TXD (GPIO14) |
| 青 (TX) | pin 10 RXD (GPIO15) |
| 緑 (GND) | pin 6 GND |

Raspberry Pi 側は `enable_uart=1` で `/dev/ttyS0` (serial0) に `serial-getty@ttyS0` が立ち上がっている状態を前提にする。
X68000 Z は 38400bps までしか対応せず、ハードウェアフロー制御も無いので、getty のボーレートを 38400 に固定し端末種別を `ansi` にする
(`harness/raspi/serial-getty-override.conf` を `/etc/systemd/system/serial-getty@ttyS0.service.d/override.conf` に置いて `systemctl daemon-reload && systemctl restart serial-getty@ttyS0`)。
カーネルの起動メッセージも読みたければ `cmdline.txt` の `console=serial0,115200` を `38400` にする。

X68000 Z 側は zsh から `muterm` を起動するだけ (`\USR\BIN\MUTERM.CNF` に 38400bps / XON-XOFF / RTS-CTS なし / ANSI / カーソルキー = `ESC [ A` 等を設定済み)。
Enter を押して `login:` が出ればログインできる。文字化けするときは BREAK キー (agetty がボーレートを切り替える) を押すか、SPEED を 19200 / 9600 に下げる。
SHIFT+BREAK で MuTERM を終了、HELP キーで設定ウィンドウ、F3 で行入力モード (通常は 1 文字ずつ送るモード)。
MuTERM は VT100 エミュレータではなく NEC / Human68k / ANSI 系のエスケープシーケンス (カーソル移動・消去・行挿入削除・色) を処理するだけ。
TERM=ansi なら terminfo にスクロール領域が無いので vim も行挿入/削除で描画されるが、TERM=vt100 / vt220 だと `ESC [ r` を使うので崩れる。X68000 LIBRARY には VT100 を名乗るターミナルは無い
(Ko-Window 用の KX_Term / mtelnet.win が vt100 系対応を謳うが Ko-Window が必要)。
KeyWitch (`KEYMAP=us`) 併用時は MuDial + 小人さんパッチが必要とされているので未検証。
Raspberry Pi 側の bash は bracketed paste の `ESC [ ? 2004 h` を MuTERM が表示してしまうので、`harness/raspi/bashrc-serial.sh` を `~/.bashrc` に足して TERM=ansi のときだけ切る。

実機なしで確認するには XEiJ の RS-232C を TCP/IP に接続し (`harness/xeij/run.sh -tcpiphost=127.0.0.1 -tcpipport=54321 -tcpiputf8=off -rs232cconnection='TCP%2FIP+%E2%87%94+AUX'`)、
`harness/xeij/xeijserial.py 127.0.0.1 54321 raspi3 build/serial.log` で `ssh -tt raspi3` に橋渡しする。XEiJ 上の `muterm` から Raspberry Pi の bash が使えることを確認済み
(ログイン後の表示、ls、Ctrl+C、カーソルキー、vi の表示崩れなし (簡易確認)、SHIFT+BREAK で zsh に戻る)。X68000 Z 実機 + UART 直結でもログインできた (2026-10-19。最初は文字化けすることがある。BREAK を押すと直る)。

## SLIP で TCP/IP 接続 (X68000 Z → Raspberry Pi → インターネット)

同じ 3 線直結のまま、MuTERM の代わりに SLIP を張って X68000 側で TCP/IP (ping / telnet / ftp / rcmd) を使う。既定の方式。
Human68k に公開された SLIP ドライバは無いので、ESP/X (xip.x) 用のネットワークドライバを自作した (`net/slip`, `net/slipd`。elf2x68k でクロスビルド)。

| 部品 | 役割 |
|---|---|
| `\SYS\slip.sys` (CONFIG.SYS) | xip.x からは Ethernet ドライバ (`/dev/ep0`、ESP/X etherdrv API) に見える。送信は Ethernet 見出しを外して SLIP にし、送信リングに積む。SCC にも割り込みベクタにも触らない |
| `slipd.x` (常駐スレッド) | ppp.x と同じく IOCS (`_INP232C` / `_OUT232C`、ROM と RSDRV.SYS) でシリアルを読み書きする。受信した SLIP をほどいて slip.sys 経由で xip.x の受信ハンドラに渡し、送信リングを RS-232C に流す。RSDRV.SYS の受信バッファを 1KB から 32KB に付け替える |
| `xip.x` / ESP/X コマンド | PPP と同じ (下表) |

```sh
netup [速度]     # rsspeed.x (IOCS _SET232C) → xip.x → ifconfig ep0 → inetdconf +router/+dns → slipd.x
netdown          # xip.x -r (slipd.x のスレッドは残り、次の netup で受信を再開する)
```

速度は AUTOEXEC.BAT の `NETSPEED` (make の `NETSPEED=`、既定 38400)。Pi 側は `net/raspi/ppp-x68k.service` を `MODE=slip` (既定) で常駐させ、
`/etc/default/ppp-x68k` の `SPEED=` を揃える (slattach が sl0 = 192.168.68.1 を作り、NAT で外へ出す)。

X68000 Z 実機で計測 (x68put で Pi → X68000、100KB):

| 方式 | 速度 | 実効 | TCP 再送 |
|---|---|---|---|
| SLIP | 38400bps | 2.7KB/s (21.9kbps) | 0% |
| SLIP | 19200bps | 1.7KB/s (13.6kbps) | 約 1.4% |
| PPP (ppp.x) | 9600〜38400bps | 約 1KB/s | |

38400bps では X68000 側の処理が頭打ちになる (xip.x の TCP ウィンドウを 8192 にしても変わらない)。
xip.x は 1 パケットごとに 100〜150ms かかり、MSS が 512 に固定されているので、イメージには MSS を 1460 にした
xip.x を入れる (`make XIPMSS=1460`、既定。`net/patchxip.py` が元の xip.x の SHA-256 を確かめてから書き換える。
改変済みのバイナリは配布しない)。Pico W の netpipe では 4KB/s → 8.5KB/s になった。`XIPMSS=512` で元のまま入れる。
はじめは SCC の受信・送信割り込みを slip.sys が自前で横取りしていたが、xip.x のヒープやスタックが壊れて
バスエラー / アドレスエラー / 画面のスクロール崩れが起きた。ROM / RSDRV の割り込み処理に任せる今の形 (ppp.x と同じ) にしてから起きない。
XEiJ では `harness/xeij/xeijslip.py 127.0.0.1 54322 raspi3 build/slip.log` (Pi の pty に slattach して sl1 = 192.168.69.1 と橋渡し) で試せる。
XEiJ の SCC には受信 FIFO が無いため、19200bps 以上では実機より取りこぼしが多い (19200bps で約 5%)。

## Pico W 経由で TCP/IP 接続 (netpipe)

[x68kzremotedrv](https://github.com/yunkya2/x68kzremotedrv) (Pico W を X68000 Z の USB に挿してリモートドライブにするもの) に、
SCSI ID 6 越しに SLIP のバイト列を運ぶ netpipe を足したファームを使う (netpipe ブランチ、未公開)。
`slipd.x -z` が ID 6 の決まった LBA に要求を書き、応答を読む。シリアルケーブルは要らず、38400bps の SLIP の約 3 倍速い。

```sh
netup pico               # Pico → Pi の slipudp.py (UDP 6868) → sl0。アドレスはシリアルの SLIP と同じ 192.168.68.2、Pi で NAT
netup bridge 192.168.1.68  # Pico がこの IP を代わりに名乗り (代理 ARP)、Z を LAN 上の 1 台にする。Pi も NAT も不要
```

- bridge: ルーターは `NETGW` (既定 192.168.1.1)、DNS は `NETDNS` (既定は `NETGW`)。IP は DHCP の範囲外か予約したものを使う。
  外から Z にそのままつなげる (rcmd、自作のサーバ)。Pico は外からの ping (echo request) を Z に渡さない (ESP/X が止まったことがあるため)。
- pico: Pi 側は `/etc/default/ppp-x68k` を `MODE=pico` にする (`net/raspi/ppp-x68k.sh` が `slipudp.py` と slattach を起動する)。
- pico / bridge / RS-232C の切り替えと bridge の IP の変更は再起動が要る (slipd.x のスレッドが最初の設定のまま残る)。

X68000 Z 実機で計測 (x68put、200KB、MSS 1460 の xip.x): pico 8.4KB/s、bridge 8.6〜8.8KB/s。律速は xip.x。

Z の Linux は SCSI の読み込みをキャッシュし、X68000 をリセットしても消えない (電源を切ると消える)。
そのため応答は毎回違う LBA (0x110000〜0x1ffff7 の約 480MB を 8 セクタずつ) から読み、番号と読み始めの位置は起動のたびに時刻から決める。
0x200000〜0x3fffff は B: のドライバ (scsiremote.sys) が使う。slipd.x が残す記録 (`SLPDTAP_`、直近 64 パケットの先頭) は `ztap.x` (`make -C harness/ztools`) で見られる。

## PPP で TCP/IP 接続 (NET=ppp)

`NET=ppp make image` にすると SLIP の代わりに PPP (IIJ-PPP の Human68k 移植) を使う。ssh クライアントは存在しないので、どちらの方式でも Pi へは telnet で入る。

| 役割 | 収録物 |
|---|---|
| RS-232C ドライバ | `tmsio.x` 0.31 (既定。フロー制御をソフトで選べる) / `bsio.x` 0.21 (軽いが RTS/CTS 固定。3 線接続では未検証) |
| PPP | IIJ-PPP on Human68k Rel.5: `\SYS\ppp.sys` (CONFIG.SYS で常駐) + `ppp.x` + `/etc/conf.ppp` |
| TCP/IP | `xip.x` β5 (計測技研 ESP/X `inetd.x` の改良版。`/etc/hosts` `networks` `protocols` `services` を読む) |
| コマンド | ESP/X PACK-A (TCPPACKA): `ping.x` `telnet.x` `ftp.x` `inetdconf.x` `ifconfig.x` `netstat.x` `arp.x` |

CONFIG.SYS には `PROCESS = 16 10 20` (xip/ppp がバックグラウンドプロセスを作る。タイムスライスは 50 より 20 の方が ping が速く、速度は同じ) と `DEVICE = \SYS\ppp.sys`、
AUTOEXEC.BAT には `PPP=A:\ETC` (conf.ppp の場所) / `SYSROOT=A:\` (etc の親) / `HOST=x68k` を設定済み。
アドレスは `/etc/conf.ppp` の `raspi:` ラベルで X68000 = 192.168.68.2、Raspberry Pi = 192.168.68.1 (`/etc/hosts` も同じ)。

Raspberry Pi 側は `net/raspi/ppp-x68k.service` で常駐させる (手順はファイル先頭のコメント。`/etc/default/ppp-x68k` に `MODE=ppp` と `SPEED=`)。手で試すなら `net/raspi/ppp-x68k.sh` を `sudo` で起動しておく (serial-getty を止めて `pppd /dev/ttyS0 38400 local noauth nocrtscts novj noccp noipv6 ...` を待ち受け、
`ip_forward` + `iptables MASQUERADE` で X68000 からのパケットを外へ NAT する。Ctrl-C で getty に戻す)。
`cmdline.txt` の `console=serial0,...` は削除しておく (カーネルメッセージが PPP フレームを壊す)。`ppp` パッケージが必要。

X68000 Z 側は zsh から:

```sh
netup [速度]     # RSDRV + rsspeed.x (IOCS _SET232C) → pppwait.x → xip.x → ppp.x -dial raspi<速度> → inetdconf +dns 8.8.8.8
ping -n -c 3 8.8.8.8   # 外に出られれば成功 (-n: 逆引き DNS を回線に流さない)
telnet raspi     # Pi にログイン (telnetd を入れておく)。ftp raspi も可
netdown          # ppp.x (切断・常駐解除) → xip.x -r
```

`netup 9600 tmsio` / `netup 9600 bsio` で tmsio.x / bsio.x に切り替えられる。`ppp.x` の `show modem` で `speed: 38400($9)` と表示される (Web の作例にある `set speed $C` は 14400bps)。
**X68000 Z では tmsio が SCC に書く速度が UART に反映されず、常に 9600bps で通信する** (Pi 側 38400 で待つと Z からのデータが化ける。
IOCS `_SET232C` で先に 38400 にする `rsspeed.x 4c09` を試したが、その後 tmsio を載せると 1 バイトも出なくなった)。そのため netup の既定は標準 RSDRV.SYS + rsspeed.x (Z でも 19200 / 38400 が通る)。
実機 X68000 (RS-232C) なら 38400 が使え、ftp 3KB/s 台の報告がある。X68000 Z 実機で `netup` → `ping 8.8.8.8` が通ることを確認済み (9600bps)。
XEiJ では上記 TCP/IP 接続 (`-model=SUPER` を付ける。既定の X68030 25MHz では ppp.x が切断時に固まる: 添付 qa.doc の高速機問題) + `harness/xeij/xeijppp.py 127.0.0.1 54321 raspi3 build/ppp.log` (ssh 経由で Pi の `pppd notty` に橋渡し) で PPP が張れる (ping 8.8.8.8 応答、netdown で両側とも正常終了) ことを確認済み。X68000 Z 実機では tmsio で送受信できる (9600bps のみ)。bsio は未検証。

### Raspberry Pi から X68000 のコマンドを実行する (rcmd.x)

逆方向。X68000 側で `rcmd.x` (ESP/X ソケット API で書いた小さなサーバ、`harness/rcmd/rcmd.c`) を起動しておくと、
Pi から `x68 'ls -l'` のように zsh のコマンドを実行して出力を受け取れる。1 接続 1 コマンド、出力は一時ファイル経由
(Human68k に pty が無いので対話的なフルスクリーン操作はできない。vim などは不可)。

```sh
# X68000 側 (netup のあと)。ソースはイメージ内 /usr/src/rcmd にあり、X68000 上で make してビルドする
cd /usr/src/rcmd; make          # gcc.x rcmd.c -o rcmd.x -lnetwork -liocs -ldos -lgcc2
rcmd.x                          # port 2323 で待ち受け。リモートから "exit" を送ると終了
# Raspberry Pi 側 (harness/raspi/x68.sh を ~/bin/x68 などに置く。bash の /dev/tcp を使うので nc 不要)
x68 'ls -l /usr/bin'
x68 'gcc hello.c -o hello.x && hello.x'
x68 exit                        # rcmd.x を終える
```

rcmd.x の実行中は X68000 のコンソールはふさがる (ESC で終了。`-d` で接続ごとの状態を表示、`-B` で accept() ブロック待ち)。
X68000 Z 実機 + PPP 9600bps で `ls` / `echo` / `gawk` の実行と終了コードの受け渡しを確認済み。日本語を含むコマンドは化ける (未調査)。
XEiJ の PPP は外から来たパケット (Pi からの ping / 接続) に応答しないが、SLIP (xeijslip.py) なら rcmd も試せる。
`netup` は `pppwait.x` (net/pppwait) で Pi の pppd が送ってくる LCP を受信してから ppp.x を起動する。
ppp.x は相手が居ないと Phase: Dead で固まり BREAK も届かないため。pppwait は ESC / BREAK / Ctrl-C で中止できる (待ち時間は `NETWAIT=秒`、既定 30)。出力は Shift_JIS/CRLF のまま送られ、`x68` 側で UTF-8/LF に直す。
終了コードは `--rcmd-end rc=N--` の行で返り、`x68` の終了コードになる。ESP/X PACK-B (libnetwork.a, ヘッダ, サンプルソース) も `/usr/lib` `/usr/include` `/usr/src/espx` に入れてある。

## 既知の制限

- rb-cli の `new hd x68k` が作る FAT は Human68k に「管理領域が破壊されています」と判定されるため使っていない (mkhds.py で代替)。
  rb-cli の `get` は自分で書いた HDS から 4KB しか読み出せないバグがあるが、書き込み (batch) は正しい。
- zsh のジョブ制御、複数リダイレクトは Human68k 版の制限で使えない (添付 readme.x68 参照)。また `VAR=値 コマンド` の一時代入が親シェルに残るバグがある (`/etc/zshrc` の `cc` 関数は明示的に戻している)。
- 1 ディレクトリのエントリ数は 250 以下にすること (rb-cli の制限、`mkbatch.py` が警告する)。
- IOCS.X は IPLROM 1.3 以降 (XEiJ 含む) では組み込めない旨のメッセージが出るが無害。
- less は移植バイナリを確保できていない。sed / grep は `scripts/xbuild.sh` が elf2x68k (`brew install yunkya2/tap/elf2x68k`) でクロスビルドする (無ければイメージに入らない)。
- 収録ソフトのライセンスは各 `USR/DOC` 内のドキュメントに従う。Human68k は非商用・無償配布のみ可。
  このリポジトリには取得した配布物やイメージは含めない (`dl/`, `build/` は git 管理外)。

## 入手元

`sources.txt` を参照 (X68000 LIBRARY, archive.org)。
