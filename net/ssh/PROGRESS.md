# SSH クライアント移植 進捗 (2026-10-22 09:30)

## 結論 2: 実機で `ssh.x -t moto@192.168.1.108` の対話シェルが使える

`-t` (pty) のセッションでは、受信データを term/vt100 の VT100 エミュレータ (テキスト VRAM 直描き、96x32) に
流し、キーボード (IOCS _B_KEYINP) を VT100 のバイト列にしてチャネルへ送る。実機 (b:pnetorg4、Pico キーボードで
遠隔打鍵) で確認したこと:

- `touch /tmp/from-z`、`echo`、`date`、`cat /etc/passwd` (スクロール)、`ls --color -la` (色・太字)、
  `sleep 100` → Ctrl-C で中断、`vi` (代替画面 1049・挿入・`ZZ` で保存)、`less` (space / q)、`exit` で
  Human68k のプロンプトに戻る (rcmd 経由の次のコマンドが通る)。bash の履歴と Pi 側のファイルで裏取り。
- 画面は memdump.x でテキスト VRAM 4 プレーンを吸い出して PNG にして確認 (contee.sys は IOCS 出力しか
  拾わないので VT100 の描画は /tmp/x68con.log に出ない)。
- ハンドシェイク〜プロンプトまで約 65 秒 (コマンドモードと同じ)。キー入力の往復は体感即時。

### 設計 (決めたこと)
1. **出力**: common-channel.c の `writechannel_fallback` (HAVE_WRITEV 無しなのでこれが実経路) で、
   ソケット以外の fd への write を `x68k_chan_write` に差し替え。VT が有効なら `vt_write`、無効なら素の write。
   VT の有効化は cli-chansession.c の `cli_tty_setup` / `cli_tty_cleanup` (pty 要求時だけ呼ばれ、
   リモート `exit` でも `dropbear_exit` でも必ず通る) から `x68k_tty_setup` / `x68k_tty_cleanup`。
   `-t` 無しのコマンドモードは vt_init を通らないので従来どおり (uname -a 65.4 秒で再確認)。
   スーパーバイザ切り替えは vt100.c 内で完結するので ssh.x はユーザモードのまま (libsocket も普通に動く)。
2. **入力**: select シムで fd 0 が要求されたら、poll を 50ms 刻みで回し、刻みの間に `vt_key` で IOCS の
   キーを読んでバッファに溜め、溜まっていれば fd 0 を「読める」と返す。`send_msg_channel_data` の read を
   `x68k_chan_read` にしてバッファから返す。**空のとき 0 や EAGAIN を返すと dropbear は EOF 扱いで stdin を
   閉じる**ので -1/EINTR。poll は libsocket のもの (内部で _CHANGE_PR) なので xip は止まらない。
   ソケットが無い select (`fd_read_pending(0)`) は nfds=0 の poll が EINVAL になるので poll を飛ばす。
3. **-t の pty 要求**: `ioctl(TIOCGWINSZ)` をシムで実装して `vt_get_size` (96x32) を返す。termios スタブの
   `tcgetattr` は **cooked な既定値** (ECHO/ICANON/ISIG/ICRNL/OPOST/ONLCR、VINTR=3、VERASE=0x7f …) を返す。
   全部 0 だと pty-req でそのまま sshd に送られ「エコー無し・階段状改行・Ctrl-C 不可」になる。
   TERM は Human68k の環境変数 (x68000) を使わず **xterm** を送る (環境変数 SSHTERM で上書き可)。
   vt100.c は xterm サブセット (色・1049 代替画面・DECCKM) なので vi/less がそのまま動く。
   SIGWINCH は newlib の signal() が通るが失敗しても無視 (画面サイズは変わらない)。
4. **終了**: リモート `exit` → チャネル close → `cli_tty_cleanup` → `vt_fini` (パレット・IOCS カーソル復帰)。
   画面は消さず最後の表示を残し、IOCS のカーソルを VT のカーソル行の次に置いて "Connection ... closed." が
   その下に出るようにする。ただし **Human68k のコンソールはプレーン 0,1 しか使わず、IOCS のスクロールも
   プレーン 0,1 しか動かさない** (実機の VRAM ダンプで判明: 色付き文字のプレーン 2 が 1 行ずれて重なった)。
   そこで戻す前に残った画面を「点灯ピクセルは全部プレーン 0,1、2,3 は空」に潰す (色は失うが以後の
   コンソールのスクロールでずれない)。コンソールの最終行は 30 (ファンクションキー行で 31 行) なので、
   VT のカーソルがそれより下なら自分で txrascpy で全プレーンを上げて空ける (IOCS に改行させない)。
   `VTKEEP=1 ./build.sh` の版は潰さない (VRAM ダンプで色を確かめる試験用)。

### vt100.c に入れた修正 (branch vt100 に戻すこと)
- `vt_write` の頭でソフトカーソルを消す。解析中の scroll() が VRAM を動かすので、カーソルの塊ごと
  スクロールして flush の消去 XOR が旧位置に塊を残していた (Enter でスクロールした行の行末に残骸)。
- `ESC[>...m` (xterm modifyOtherKeys) を SGR と誤読しない (`csi_priv == 0` のときだけ do_sgr)。
- `~` (0x7e) は CGROM だと JIS のオーバーラインなので自前のチルダグリフ。

### 実機で分かった周辺の事実
- rcmd の環境ブロックは満杯で `set` が「環境のためのメモリが足りません」。環境変数での切り替えは不可。
  Z の環境は TERM=x68000、HOME=B:/home、USER=x68k。
- IOCS _TXRASCPY は Z でも 4 プレーンとも正しくコピーする (rctest で確認。上の片寄りは IOCS コンソールの
  スクロールの話)。
- picokbd (US 配列 HID) で打つと X68000 側で `=`→`^`、`"`→`*`、`\`→`]`、`[`→`@`、`:`→`+`、`|`→`}` になる。
  試験で `ls --color=always` や `:wq` が打てなかったのはこれ (ssh.x のキー変換の問題ではない)。
  vi は `ZZ`、`ls --color -la` のように避けて打つ。

### 残り
- known_hosts、乱数、暗号の高速化 (下記「残り」) は未着手。
- キー: BS→DEL(0x7f)、矢印/HOME/DEL/INS/ROLL/F1-F10 は vt_key の表のとおり。Ctrl-C 動作確認済み。
  画面サイズ変更 (SIGWINCH) は無い。`~.` などの dropbear のエスケープは残してある。
- stderr の "Connection ... closed." は rcmd 経由だと TCP 側に行くので画面には出ない。Z のキーボードから
  直接起動したときは VT の最終行の下に出る (IOCS カーソルをそこに置いてある)。

---

# (以前) SSH クライアント移植 進捗 (2026-10-22 08:25)

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
1. ~~対話セッション~~ → 上記「結論 2」で完了 (09:30)。
2. **暗号のさらなる高速化**: ed25519 検証 26.7 秒が最大。署名 7.2 秒、ladder 19.6 秒。
3. known_hosts (`-y -y` で検証を飛ばしている)。HOME=B:/home の `.ssh/known_hosts` は
   テキストなので `fopen` のテキストモードで問題ないはず。未検証。
4. 乱数が弱い (タイマ撹拌のみ)。実運用には不十分。
