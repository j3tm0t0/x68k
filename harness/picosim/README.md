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
| `-n net/len` `-g gw` `-D dns` | 192.168.1.0/24, .1, .3 | slirp の LAN。ゲートウェイは Mac の 127.0.0.1 に、DNS は Mac の resolver に転送される。ゲートウェイの 53 番 (zshrc の `NETDNS` の既定) は DNS の口に付け替える |
| `-L [u]PORT:ZPORT` | `2323:2323` | Mac の 127.0.0.1:PORT → Z:ZPORT。`u` で UDP、`none` で無し。何度でも |
| `-0 file.hds` `-N 秒` | | ID 0 を HDS ファイルで答える / 最初の N 秒 NOT READY |
| `-c MB` `-r KB` | 64, 16 | Z の Linux の読み込みキャッシュの大きさ (0 で無し) / 先読み (4KB ページ単位) |
| `-d µs` | 0 | 命令ごとの遅れ (USB の往復の真似。メールボックスの道では Z の CPU は待ちの間も動く。SPC の道では XEiJ ごと止まる) |
| `-a 倍率` | 1 | アクセラレータの所要時間 (RP2040 の見積り: X25519 100ms / verify 170ms / sign 50ms) の倍率。0 ですぐ |
| `-p lan.pcap` | | LAN を流れた Ethernet フレームを pcap に (`tcpdump -nn -r`) |
| `-s 秒` `-v` | | 統計を定期的に (`kill -USR1` でも) / `-vv` で命令ごと、`-vvv` でフレームごとの記録 |

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
- 速度 (XEiJ の中、rcmd のデータだけ): Z → Mac 11.9KB/s (77,628 バイト 6.5 秒)、Mac → Z 5.4〜5.7KB/s (x68put 全体)。
  実機の Z (40〜56KB/s) より遅い。picosim の 1 命令は XEiJ 側から見て平均 0.12ms、Z → Mac の送りの間の交換は約 37 回/秒 × 約 340 バイトで、
  Z 側 (XEiJ の 68000 10MHz、または XEiJ の実時間への合わせ方) が律速と見ているが、確かめていない
- Z のキャッシュ: npd.x の読みのうち 4 割前後がキャッシュに当たる (先読みした 16KB の残りなど)。古い応答は番号で弾かれる

## 限り

- XEiJ の CPU が Z より遅く、USB の往復 (実機で 1 命令 0.2〜2.5ms) は -d で足さないと 0 に近い。速度の数字は実機と比べない
- SPC の道の書き込みの失敗は Z に伝わらない。メールボックスは READ(10)/WRITE(10) だけ
- ID 6 の LBA 0 / scsiremote.sys / 設定画面 / 起動待ち IPL (Pico Z Loader の BW_BAND 0x40000〜) は未実装。ID 0 の
  NOT READY は「最初の N 秒」だけ (ファームの 5 秒の保留と control の状態遷移は真似ていない)
- LAN は slirp の NAT。外 (インターネットや家の LAN) にも出られるので、試験はゲートウェイ (Mac) 宛てに留める。
  Mac から Z へは -L の転送だけ (Z を LAN の 1 台として外から見ることはできない)
- DNS はゲートウェイの 53 番を slirp の DNS の口に付け替えて Mac の resolver に送るだけ (中身は確かめていない)
