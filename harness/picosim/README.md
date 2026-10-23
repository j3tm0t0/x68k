# harness/picosim -- XEiJ + Mac の上の「Pico W の代わり」で X68000 Z の全体を試す

X68000 Z に挿した Pico W (x68kzremotedrv の netpipe ファーム) を、Mac の上のプロセス `picosim` で置き換える。
XEiJ の SCSI を Java エージェント (`harness/xeij/agent/PicoBridge.java`) が横取りして TCP で `picosim` に渡すので、
**Z も Pico も raspi も使わず**、Z 側のプログラム (npd.sys / npd.x / xip.x / rcmd.x / ssh.x / acceltest.x ...) を
そのまま動かせる。root も tun も要らない (LAN は libslirp)。

```
 XEiJ (X68000 SUPER, 68000)                          picosim (Mac, 1 プロセス)
 ┌──────────────────────────────┐   TCP 6868   ┌────────────────────────────────────────┐
 │ npd.x の zio()  ─ 0xEA0060 ──┼──┐          │ SCSI の対象 (ID 0 = HDS, ID 6 = netpipe) │
 │   (Z のメールボックス)        │  ├─ "PSQ1" ─▶│ Z の Linux の読み込みキャッシュ・先読み    │
 │ IOCS _SCSIDRV ─ SPC ─ ID 0/6 ┼──┘ ◀"PSA1"─ │ netpipe: X6NR / X6NP、0xA5 accel、0xA6 … │
 │   (PicoUnit)                  │             │ LAN: libslirp 192.168.1.0/24             │
 └──────────────────────────────┘             └──────────────┬─────────────────────────┘
                                                              │ Mac のソケット (NAT)
                                       ゲートウェイ 192.168.1.1 = Mac の 127.0.0.1
                                       Mac の 127.0.0.1:23268 -> Z (192.168.1.68):2323 (rcmd.x)
```

## 使い方

```sh
make -C harness/picosim                      # 初回は libslirp 4.8.0 を取ってきてビルドする (glib は Homebrew のもの)
make -C harness/picosim test                 # XEiJ なしの試験 (tests/bridgetest.py、0xA6 をそのまま返す試験用サービス入りの build/picosim-echo で)

# イメージ (npd 入り、起動すると netup → npd.x → rcmd -d -v まで自動)
(cd x68kcli && make image NET=npd NETZIP=192.168.1.68 IMG=build/picosim.hds && make agent)

harness/picosim/build/picosim -L 23268:2323 -s 30 &
IMG=build/picosim.hds XEIJ_JAVA_OPTS="-Dpicosim=127.0.0.1:6868 -Dpicosim.ids=6" \
  harness/xeij/run.sh -config=default -saveonexit=off -model=SUPER &
# 1 分ほどで rcmd.x が上がる (初回は rcmd.c のビルドで +1 分)
X68_HOST=127.0.0.1 X68_PORT=23268 harness/raspi/x68.sh 'ping.x -c 4 192.168.1.1'
```

- worktree の `x68kcli/` に `tools/` (xeij, rb-cli) と `dl/` が無ければ、本体の checkout のものへの symlink にする
- `-config=default -saveonexit=off` を付けると ~/XEiJ.ini を使わず書かない (ほかの XEiJ と干渉しない)
- XEiJ は GUI のセッションから起動する (`tmux -L gui`、だめなら `.command` を `open -a iTerm`)
- ID 0 も picosim から: `picosim -0 x68kcli/build/id0.hds -N 15` と `-Dpicosim.ids=0,6` (XEiJ の `-si0` は差し替えられる)。
  `-N 15` で最初の 15 秒は NOT READY (02/04/01、BOOTWAIT の真似)。XEiJ の SCSI ROM は TEST UNIT READY と
  REQUEST SENSE を繰り返して待ち、準備ができたら picosim の HDS から起動した (下記)
- Pico Z Loader の起動 (待ち画面・控え・設定画面) は下の「Pico Z Loader の起動」

### XEiJ 側の設定 (システムプロパティ、run.sh の `XEIJ_JAVA_OPTS`)

| プロパティ | 既定 | 意味 |
|---|---|---|
| `picosim` | (無し = 何もしない) | 接続先 `host:port` |
| `picosim.ids` | `6` | SPC (IOCS `_SCSIDRV`) で picosim に渡す SCSI ID。`0,6` など |
| `picosim.mailbox` | `on` | Z のメールボックス 0xEA0060 を模倣する |
| `picosim.log` | `0` | 1 で命令ごとに XEiJ の標準出力へ |
| `picosim.timeout` | `5000` | 応答を待つ ms。過ぎたら切ってつなぎ直し、だめなら NOT READY (止まった picosim で XEiJ が固まらない) |

数は `xeijcmd.sh get xeij.PicoBridge stCmds` (ほかに `stMailbox` `stFails` `stNanos`) で読める。

### picosim のオプション

| | 既定 | |
|---|---|---|
| `-l host:port` | 127.0.0.1:6868 | XEiJ を待つ口 |
| `-n net/len` `-g gw` `-D dns` | 192.168.1.0/24, .1, .3 | slirp の LAN。ゲートウェイは Mac の 127.0.0.1 に。DNS の口とゲートウェイの UDP 53 番 (zshrc の `NETDNS` の既定) は picosim が Mac の resolver (getaddrinfo / getnameinfo) で答える (A と PTR。仮想の LAN のアドレスの PTR は picosim が名付ける) |
| `-L [u]PORT:ZPORT` | `2323:2323` | Mac の 127.0.0.1:PORT → Z:ZPORT。`u` で UDP、`none` で無し。何度でも |
| `-0 file.hds` `-N 秒` | | ID 0 を HDS ファイルで答える / 最初の N 秒 NOT READY (`-T 0,0,N` と同じ) |
| `-T w,s,h` | | Pico Z Loader の起動待ち: 起動から w 秒で Wi-Fi、s 秒で SMB、h 秒で HDS が開く (下記) |
| `-B file` | `<HDS>.bootsnap` | 起動の控え (フラッシュ 0x1E0000 の代わり: 見出し 4KB + HDS の LBA 0-31) |
| `-F dir` | | フォークの `driver/` (bootwait1.bin・bootwait.bin・hdsboot.bin・settingui.bin を読む)。付けると ID 6 の LBA 0 に署名 |
| `-C file` | | 制御: 1 行 1 命令を書くと実行して消す。`ready` `close` `status N` `auto` `powercycle` `hds PATH` |
| `-R dir` | | B: (リモートドライブ) にする Mac のディレクトリ。何度でも (B:, C:, ...)。下記 |
| `-P` | | vdbuf のページを LBA から決める (ファームの不具合の直し方の試し。下記) |
| `-K file` | | 設定画面の設定 (config_data) を保存するファイル (FLASHCONFIG で書き、起動時に読む) |
| `-c MB` `-r KB` | 64, 16 | Z の Linux の読み込みキャッシュの大きさ (0 で無し) / 先読み (4KB ページ単位) |
| `-d µs` | 0 | 命令ごとの遅れ (USB の往復の真似。メールボックスの道では Z の CPU は待ちの間も動く。SPC の道では XEiJ ごと止まる) |
| `-a 倍率` | 1 | アクセラレータの所要時間 (RP2040 の見積り: X25519 100ms / verify 170ms / sign 50ms) の倍率。0 ですぐ |
| `-p lan.pcap` | | LAN を流れた Ethernet フレームを pcap に (`tcpdump -nn -r`) |
| `-s 秒` `-v` | | 統計を定期的に (`kill -USR1` でも) / `-vv` で命令ごと、`-vvv` でフレームごとの記録 |

## Pico Z Loader の起動 (bootwait.c)

フォーク (yunkya2/x68kzremotedrv の j3tm0t0 版、branch bootwait 8f91624) の `src/bootwait.c`・`virtual_disk.c`・
`vd_command.c` と同じ決まりで答える。Z の上で動く 68000 のバイナリ (待ち IPL、hdsboot、settingui) は git に入れず、
`-F` でフォークの `driver/` (そこで `make` したもの) から読む。

```sh
F=~/src/github.com/yunkya2/x68kzremotedrv-hid/driver
cp x68kcli/build/picosim.hds x68kcli/build/id0.hds
harness/picosim/build/picosim -L 23268:2323 -0 x68kcli/build/id0.hds -T 15,25,35 -F $F -C /tmp/pz.ctl -v &
IMG=build/picosim.hds XEIJ_JAVA_OPTS="-Dpicosim=127.0.0.1:6868 -Dpicosim.ids=0,6" \
  harness/xeij/run.sh -config=default -saveonexit=off -model=SUPER &
```

- **状態**: 起動 (picosim の起動か `powercycle`) からの時間で sysstatus を 1 (Wi-Fi 接続中) → 3 (SMB 接続中) → 4 → 5 と進め、
  5 で HDS を開く。待ち IPL の答えの Pico の IP は 192.168.1.118 ということにしている
- **控え**: HDS を開いたら LBA 0-31 (16KB) を控え (`-B`、見出しは "X6Z BootSnap v1"・FNV-1a・HDS の大きさ・パス) と比べ、
  違えば書き直す。開く前は、控えがこの HDS (パスが同じ) のものなら LBA 0-31 を控えから返し、LBA 2 を待ち IPL の前半
  (bootwait1)、3 を 0 にする。控えが無ければ NOT READY、sysstatus 5 で開けなければ MEDIUM ERROR
- **ID 0 (開いたあと)**: LBA 2 は hdsboot (F1 で設定画面)、LBA 0x20/0x21 は本物の LBA 2/3 (ファームと同じ)
- **ID 6**: LBA 0 = "X68SCSI1"/"X68000ZRemoteDrv"、LBA 4 = "X68K" (リモートドライブ 0 個)、0x80〜 = 待ち IPL の後半、
  0x100〜 = settingui、0x40000〜0xfffff = 待ち IPL の問い合わせ (X6BW の答え: WAIT / READY + 本物の IPL と和 /
  CHANGED / NOHDS)、それ以外の 0x200〜 = 設定画面との vdbuf ("X68Z")
- **設定画面の命令** (vd_command): GETINFO (版 "picosim") / GETCONFIG / SETCONFIG (HDS0 を変えると開き直し、時間は最初から) /
  GETSTATUS / FLASHCONFIG (`-K` に保存) / FLASHCLEAR / REBOOT (Pico だけ再起動。Z のキャッシュは残る) /
  WIFI_SCAN (SSID "picosim" 1 つ) / SMB2_ENUM (共有 "picosim" 1 つ) / SMB2_LIST (HDS のファイル名だけ)。
  scsiremote.sys (リモートドライブ B:) の命令は無い
- **Z のキャッシュ**: XEiJ の ROM は最初に ID 0 の LBA 0-3 を読むので、picosim の Z キャッシュの先読みで LBA 0-31 が
  まとめて入る。その後の読み込み (待ち IPL のあとの分割表・ドライバ) はキャッシュ (= 控え) から。Z で見えた
  「電源投入で 16KB を一括で読み、ソフトリセットでは読み直さない」の真似になっている。電源の入れ直しは picosim と
  XEiJ の両方を起動し直す (`-C` の `powercycle` は picosim 側だけ)

## B: (リモートドライブ、remote.c)

`-R DIR` で Mac のディレクトリを Z の B: (と続くドライブ) にする。ファームと同じく、ID 6 の分割表 (LBA 4) に
"Human68k" を数だけ並べ、SCSI のドライバ (LBA 6〜、`-F` の scsiremote.bin) を渡す。Human68k が起動時に組み込み、
vdbuf ("X68Z" を LBA 0x20 に書き、応答を 0x200000〜0x3fffff の帯から 8 ページずつ下へ読む) で要求してくる。
要求はフォークの x68kserremote (サブモジュール) の `service/remoteserv.c` に渡す。これは写さず、ビルド時にそこから
コンパイルする (`make RS=.../x68kserremote`、ファイル操作は同じ所の POSIX の fileop.h)。設定はファームの
include/config.h と同じ値 (`remote/config.h`: 1 回の READ / WRITE は 15KB、FILES は 10 件)。
`-vv` で要求ごとに記録する (`B: READ fcb=... pos=... len=... -> 返した長さ`)。`-vvv` で、Z が帯のどの LBA の
どのページを Pico から / キャッシュから受け取ったかも記録する。

**見つけた不具合 (2026-10-23)**: Z の実機で、ある ssh.x を B: から起動すると _EXEC の中で落ちた (同じファイルが A: からは
動き、CRC も合う)。picosim で再現した (PC $3E7D0、実機は $3E7D2)。原因は、ファームの vdbuf のページの決め方
(`page = vdbuf_rcnt + lba % 8`、lba % 8 == 7 を読むたびに vdbuf_rcnt += 8) と、ドライバの com_cmdres と、Z の Linux の
16KB 先読みの組み合わせ:
1. ドライバは応答の 8 ページごとに 8 セクタ下へ進み、命令の始めにもさらに 8 下げる。前の応答が 8 ページの倍数
   (例 24 ページ = 11,409〜11,904 バイト) で終わると、2 回続けて下げるので、次の命令の始まりのすぐ上の 8 セクタは誰も読まない
2. 次の命令の最初の読み込みで Linux が上へ 16KB 先読みし、その空いた 8 セクタも Pico から読む。Pico はそこにページ 8〜15 を
   渡して vdbuf_rcnt を余分に 8 進める
3. ドライバが 2 組目 (8 下) を読むと、ページ 16〜23 が来る。見出しの比較は署名・session・seqno だけなので気づかず、
   ページ 8 以降が 8 ずれたまま使われる
_EXEC では text+data の最後のかけら ((text+data−15,296) mod 15,360 バイト) の応答の次が再配置表の読み込みになる。
問題の ssh.x は 11,612 バイト → 24 ページで、再配置表の 3,966 バイト目 (= ページ 8 の頭) から先が壊れ、再配置が
ずれて落ちた。リリースの ssh.x は 10,252 バイト → 21 ページで無事。`cat` など別の大きさで読むと正しい中身になる
(CRC が合ったのはこのため)。ファイルの大きさしだいでどのプログラムでも起きる。

`-P` はページを LBA から決める (その応答で最初に読まれた 8 セクタの組からの距離。先読みはそれより上しか読まない)。
-P では同じ ssh.x が B: から起動した。`tests/remotetest.py` が com_cmdres と同じ手順で、既定 (ファームと同じ) で
壊れることと -P で直ることを確かめる。直し方の候補: ファームで -P と同じ計算をする / ドライバが 2 回続けて下げない
(scsiremote.bin はファームに入っているので、どちらもファームの更新だけで済む) / ドライバが見出しのページ番号も比べる。

## 横取りの仕方 (XEiJ には手を入れない)

XEiJ のソースは変えず、Java エージェントの中で XEiJ の public なものを 2 か所差し替えるだけ。

1. **SPC の道**: `SPC.SCUnit` の子 `PicoUnit` (package `xeij`、エージェントの jar に入れる。XEiJ.jar は封印されていない) を
   `SPC.spcUnitArray[id]` に入れる。`scuCommand` を丸ごと上書きし、CDB を picosim に渡して、答えのデータを
   `spiDataInPhase` / 状態を `spiStatusPhase` で返す。書き込みは `spiDataOutPhase(buf, 0, n×512, 1)` で全部受け取ってから
   1 回で渡す。`spcReset` は配列の中身の `scuReset` を呼ぶだけなので、リセット後も残る。差し替えは XEiJ のフレームが
   出来た直後 (spcInit の後、MPU を起こす前) にしていて、起動時の SCSI の走査から picosim が答える。
2. **メールボックスの道**: Z の上の npd.x は IOCS を通らず、uchopon 氏の Pseudo SCSI BIOS のメールボックス (0xEA0060、
   +0 bit7 使用中 / +1 段階 = 4 で開始 / +2 ID / +3 命令 0x28・0x2a / +4 LBA / +8 バイト数 / +12 バッファ / +16・+18 状態) を
   直に叩く。XEiJ ではこの番地は拡張 SCSI の場所でふだんは何も無い (バスエラー)。エージェントが 0xEA0000〜0xEA0FFF の
   ページを主記憶 (`MMD_MMR`、`mmrM8` のその番地) にし、別スレッドで +0/+1 を見張って picosim に渡し、データと状態を
   書いてから bit7 を落とす (`VarHandle.releaseFence`)。Z と同じく、待っている間も MPU は動く (npd.x は `_CHANGE_PR` で譲る)。
   `spcReset` がリセットのたびにこのページを `MMD_NUL` に戻すので、見張りのスレッドが付け直す。

SPC の模倣はデータアウトの後に必ず GOOD にするので、SPC の道では **書き込みの失敗を Z に伝えられない** (picosim は
帯の外の書き込みもファームと同じく成功扱いにするので、今は困らない)。

## 形式 (XEiJ ⇄ picosim、TCP、ビッグエンディアン)

1 命令 = 1 往復。データの向きは命令で決まる (書き込み系は要求に、読み込み系は応答に付く)。

```
要求 32 + out_len:  "PSQ1" seq(4) id(1) path(1) cdblen(1) 0(1) out_len(4) cdb(16) data(out_len)
                    path: 0 = SPC (IOCS)、1 = Z のメールボックス (CDB は READ(10)/WRITE(10) にして渡す)
応答 16 + in_len:   "PSA1" seq(4) status(1) key(1) asc(1) ascq(1) in_len(4) data(in_len)
                    status: SCSI の状態 (0 = GOOD、2 = CHECK CONDITION)。key/asc/ascq は記録用で、Z には
                    REQUEST SENSE (これも picosim に渡る) の答えとして届く
```

INQUIRY / READ CAPACITY / TEST UNIT READY / REQUEST SENSE / MODE SENSE も含めて全部 picosim が答える (NOT READY を
出すかどうかも picosim が決める)。picosim に繋がらないときは、エージェントが CHECK CONDITION と NOT READY (02/04/01)
を返す。メールボックスの状態は +16 = 0, +17 = SCSI の状態 (繋がらなければ 0xff)、+18 = 0。

## picosim の中 (ファームとの対応)

- **ID 6**: 容量 0x400000 セクタ (ファームの 0x80000000 バイト)。LBA 0x100000 の書き込みが要求、0x110000〜0x1ffff7 の
  読み込みが応答 (どの LBA でも同じ応答の lba % 8 のセクタ)。ほかの LBA は 0 を返し、書き込みは捨てる。LBA 0 が 0 なので
  Human68k は ID 6 をディスクと見ない (ファームの scsiremote.sys / 設定画面 / 起動待ち IPL の配信は真似ていない)
- **netpipe** (`netpipe.c`): ファームの `np_exchange` と同じ見出し・リング (16KB、溢れたら捨てて stat の下位に数える)・
  レコードの並べ方。"X6NR" (npd.x) と "X6NP" (slipd.x -z a.b.c.d の SLIP) のブリッジ。UDP モード (stat = 0) は無し。
  Z 宛ての ping はファームと同じく Z に渡さず代わりに返す
- **暗号アクセラレータ** (0xA5): `net/accel/accel_ops.c` + Monocypher をそのままリンク。ファームの accel.c と同じく、
  計算中の別の要求は BUSY、同じ要求の出し直しは前の結果。結果は見積りの時間が経ってから次の交換で返す
- **Z の Linux の読み込みキャッシュ**: 読んだ (ID, LBA) を覚え、二度目は Pico に聞かずに前の中身を返す。外れたら
  4KB ページから 16KB 先読みする (Z で見えた振る舞い)。npd.x が応答帯を毎回違う所 (8 セクタずつ下へ、古い応答なら
  疑似乱数で飛ぶ) で読む理由がそのまま再現される。書き込みは素通しでキャッシュにも入る。電源断 = picosim の再起動
- **LAN** (`lan_slirp.c`): ファームの代理 ARP と同じく、Z の IP 宛ての ARP に Z の (架空の) MAC で答え、Z 宛ての IPv4 を
  netpipe に渡す。Z からの IP は Ethernet に包んで slirp へ。最初の交換で Z の IP を知ったら -L の転送を張る

## サービスを足す (tistack の Pico 側ソケット窓口など)

`picosim.h` の 2 つの口を使う。どちらも picosim の 1 本のスレッドから呼ばれるのでロックは要らない。

```c
/* netpipe のレコードのうち先頭が mark のものを受け持つ (tsp_svc.h の tsp_exchange と同じ形) */
struct picosim_svc { const char *name; uint8_t mark; void (*init)(void);
                     int (*exchange)(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more);
                     void (*poll)(void); };
void picosim_register_svc(const struct picosim_svc *svc);      /* __attribute__((constructor)) から */

/* LAN にもう 1 台つなぐ (Pico 自身の lwIP の netif の代わり) */
int lan_attach(const uint8_t mac[6], uint32_t ip_be, lan_rx_fn rx, void *arg);   /* その MAC 宛てと同報を rx に */
void lan_send(const uint8_t *frame, int len);                                      /* Ethernet フレームを LAN に */
uint64_t picosim_now_us(void);                                                     /* lwIP の sys_now 用 */
```

- 要求に mark のレコードがあれば、その交換の中で `exchange` を呼び、戻りを Z へのレコード ([長さ 2][空き 14][応答]) として
  アクセラレータの応答の後ろ・IP パケットの前に載せる。`*more = 1` なら応答見出しの more を 1 足す (npd.x がすぐ次の交換をする)
- 組み込み: `make -C harness/picosim SVC="glue.c ../../net/tistack/pico/tsp_svc.c <lwIP の .c>" SVCFLAGS="-I... -DTSP_PLAT_HOST"`。
  glue.c で `tsp_init`/`tsp_exchange` を `picosim_svc` に包み、lwIP の netif の linkoutput を `lan_send`、`lan_attach` の rx を
  netif->input にし、poll で `sys_check_timeouts()` を回す。Pico の IP は LAN の空き (例 192.168.1.118) に。
  slirp から見るとその lwIP は LAN の別の 1 台で、外への TCP はゲートウェイ (slirp の NAT) を通る
- Z 側は、xip.x の代わりに tis.x を常駐させ、npd.x の交換に 0xA6 のレコードを載せれば、XEiJ の中の rcmd.x / ssh.x が
  そのまま picosim の lwIP 経由になる

## 確かめたこと (2026-10-23、XEiJ 0.26.10.08 -model=SUPER、MacBook Pro)

DNS: XEiJ の Z で `ping.x -c 2 localhost` → `PING localhost (127.0.0.1)` 2/2、`ping.x 192.168.1.1` の逆引きも
(`lan: DNS A localhost -> rcode 0, 1 answer(s)` / `lan: DNS PTR 1.1.168.192.in-addr.arpa`)。`tests/bridgetest.py` にも 1 項目

Pico Z Loader (`-T`、`-F` はフォークの driver/、HDS は 100MB の x68kcli イメージの写し):
- 控え無し (`-T 3,6,12`): ID 0 は 12 秒 NOT READY、開いたら控えを書いて (`snapshot ... written (updated)`) そのまま起動
- 電源を入れ直す (picosim と XEiJ を起動し直し、`-T 15,25,35`): 控えを返し (`serving the snapshot (waiting IPL)`)、
  待ち画面 (テキスト VRAM を textcopy で読んだもの):
  `Pico Z Loader ･ Raspberry Pi Pico W / x68kzremotedrv` / `Wi-Fi ○ 接続中` / `SMB ― (接続を試しています)` /
  `HDS ○ 待っています` / `経過 7 秒` / `S: 設定画面 (Wi-Fi / SMB / HDS)`。35 秒で `snapshot matches` →
  `state 1 sysstatus 5` (READY) → 本物の IPL に乗り換えて Human68k が起動し、npd.x・rcmd.x まで動いた
- HDS の LBA 31 の 1 バイトを変えて電源を入れ直す (`-T 2,4,8`): 古い控えを返したあと開いて `CHANGED`、待ち画面に
  `HDS が変わりました。Z の電源を入れ直してください` で止まる。もう一度入れ直すと新しい控えで READY → 起動
  (`x68 'echo r4 booted after HDS change + power cycle'` が返った)
- 待ち画面で S (`XEiJControl` に `typekey s`): settingui が ID 6 から読まれて設定画面 (`Ｒｅｍｏｔｅ Ｄｒｉｖｅ Ｓｅｒｖｉｃｅ
  ｆｏｒ Ｘ６８０００ Ｚ`、`SSID picosim`) が出た。GETINFO・GETCONFIG のあと GETSTATUS を 2 秒おきに問い合わせてくる
- XEiJ なしの試験 `tests/bootwaittest.py` (16 項目): NOT READY → 控えの作成 / 控えから待ち IPL / 後半 "BW2!" /
  WAIT → READY と IPL の和 / CHANGED と控えの書き直し / 入れ直して READY / ID 6 の署名・settingui・vdbuf の
  GETSTATUS と 4 ページの GETCONFIG

netpipe など (最初の日):

- 起動: PicoUnit は MPU が動き出す前に入り、起動の途中で ID 6 に TEST UNIT READY → REQUEST SENSE → INQUIRY → READ CAPACITY → READ(6) LBA 0 (2KB) が来て、
  0 なのでディスクとしては扱われなかった。npd.x が
  メールボックスで交換を始めた (`netpipe: bridge 192.168.1.68 (records)`)
- rcmd: Mac → 127.0.0.1:23268 → slirp → picosim → メールボックス → npd.x → xip.x → rcmd.x → zsh。
  `x68 'echo hello from Z'` が rc=0 (zsh のコールド起動込みで 15 秒)
- ping: `ping.x -c 4 192.168.1.1` → 4/4、70〜130ms (npd.x の問い合わせ間隔 60ms が効く)
- アクセラレータ: Z の上の `acceltest.x` (RFC 7748 / 8032) が 17/17 OK (0xA5 が picosim の accel_ops で往復)
- ssh: リリースの `ssh.x -y -oAccel=all+log -l nobody 192.168.1.1` が Mac の OpenSSH と鍵交換・ホスト鍵の検証まで
  (X25519 と ed25519 verify は picosim で)、`No auth methods could be used` で終わる (鍵を置いていないので期待どおり)
- ID 0: `-0 id0.hds -N 15` で XEiJ の ROM は 15 秒 TEST UNIT READY を繰り返し (約 5000 回)、準備ができたら picosim の
  HDS から起動し、そのまま npd・rcmd まで動いた
- 速度は MPU のクロックにほぼ比例する (Z の 68000 が律速)。`-model=SUPER -clock=N`、77,628 バイト、ほかに負荷のある Mac で:

  | クロック | Mac → Z (x68put 全体、3 回) | Z → Mac (rcmd のデータ) | ping 192.168.1.1 平均 |
  |---|---|---|---|
  | 10 MHz | 10.8 / 10.9 / 18.1 KB/s | 29.2 KB/s | 62ms |
  | 16.7 MHz | 55.4 / 55.4 / 59.7 KB/s | 50.1 KB/s | 40ms |
  | 33.3 MHz | 97.0 KB/s (3 回とも) | 99.5 KB/s | 52ms |

  16.7MHz の数字は実機の Z (npd + xip、45〜57KB/s) に近い。最初の日に測った 10MHz の 5.6 / 11.9KB/s は、別の XEiJ が
  動いていて Mac が混んでいたときのもの (10MHz の x68put は 1 回 1KB/s まで落ちたこともあり、ばらつきが大きい)
- Z のキャッシュ: npd.x の読みのうち 4 割前後がキャッシュに当たる (先読みした 16KB の残りなど)。古い応答は番号で弾かれる

## 限り

- XEiJ の CPU が Z より遅く、USB の往復 (実機で 1 命令 0.2〜2.5ms) は -d で足さないと 0 に近い。速度の数字は実機と比べない
- SPC の道の書き込みの失敗は Z に伝わらない。メールボックスは READ(10)/WRITE(10) だけ
- ファームの起動ローダ (LBA 2 の bootloader、remoteboot) は無い。
  NOT READY の前のファームの 5 秒の保留 (TinyUSB に呼び直させる) は真似ていない (XEiJ の ROM はすぐ NOT READY を見る)。
  Wi-Fi / SMB は時間で進むだけで、設定画面で変えた SSID などは効かない (HDS0 のパスだけ効く)
- XEiJ の ROM の起動は Z の Linux の起動と違う (Z は Linux が USB の HDS を読んでから X68000 の ROM が動く)。
  picosim の Z キャッシュの先読みでそれらしくしているだけ
- LAN は slirp の NAT。外 (インターネットや家の LAN) にも出られるので、試験はゲートウェイ (Mac) 宛てに留める。
  Mac から Z へは -L の転送だけ (Z を LAN の 1 台として外から見ることはできない)
- DNS は A と PTR の 1 問だけ。slirp の DNS の転送は使わない (picosim からは Mac の DNS サーバ (Tailscale の
  100.100.100.100) への UDP が "No route to host" になった。python からは届くので、macOS のローカルネットワークの
  許可のせいと見ている (未確認)。libslirp の macOS の
  `res_ninit` に初期化していない構造体を渡す不具合も Makefile で直している)。名前を引く間は picosim が止まる
