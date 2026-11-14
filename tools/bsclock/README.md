# bsclock.x -- BlueSCSI v2 の時刻を X68000 の時計に設定する

BlueSCSI v2 (Pico 2 W) のファームが Wi-Fi で SNTP サーバーから得た時刻を聞き、X68000 の時計 (RTC) に設定する小さなコマンド。X68000 Compact XVI のように、時計の電池が切れて起動のたびに日付がずれる機械向け。`AUTOEXEC.BAT` から `bsclock` を呼ぶ。

```
bsclock          BlueSCSI に時刻を聞き、X68000 の時計に設定する
bsclock -n       聞いて表示するだけ (設定しない)
bsclock -r       X68000 の時計を表示するだけ (IOCS の _DATEGET / _TIMEGET と、Human68k の DOS _GETDATE)
bsclock -i ID    BlueSCSI の netpipe の装置の SCSI ID (既定 6)
```

終了コードは 0 = 設定した、1 = netpipe の装置が無い、2 = BlueSCSI がまだ時刻を持っていない、3 = それ以外の失敗。`AUTOEXEC.BAT` では `IF ERRORLEVEL 2 ...` で分けられる。

## BlueSCSI の設定

ファームは j3tm0t0/x68k-zos の `docs/bluescsi.md` にある自前の版 (ローカルのブランチ `x68k-ota`)。`bluescsi.ini` の `[SCSI]` に:

```
WiFiSSID=...
WiFiPassword=...
X68kStage=1              ; Wi-Fi の受け取り (lwIP) を動かす。時刻の取得もこの上で動く
X68kNetpipe=1            ; SCSI ID 6 に netpipe の装置を出す (bsclock はここに聞く)
X68kNTP=pool.ntp.org     ; SNTP サーバー (名前か IP アドレス)。off で取らない。省くと pool.ntp.org
X68kTZ=+9                ; UTC との差。+9、-5、+5:30 など。省くと +9
```

- BlueSCSI は DHCP でアドレスを得たあとに SNTP サーバーに聞き、その後は 6 時間ごとに聞き直す。起動から数秒は時刻が無く、`bsclock` は終了コード 2 で終わる。
- 固定 IP (`X68kStageIP=`) のときは、名前の解決に `X68kStageDNS=` (省くとゲートウェイ) を使う。
- DaynaPORT (ネットワークの機器) をどこかの SCSI ID に置いていると、Wi-Fi の受け取りが動かないので時刻も無い。

## しくみ

netpipe の装置 (INQUIRY の製品名が `X68NP` で始まる SCSI ID) に、起動の制御のレコード `[0xA8][4]` (TIME) を 1 回の交換で送る。LBA 0x100000 に要求を書き、0x110000〜0x1ffff7 のどこかから答えを読む (読む位置は毎回変える)。答えは

```
[0xA8][4][status][年 2][月][日][時][分][秒][曜日 0=日][UTC との差 (15 分単位、符号付き)][1970 年からの秒 4 (UTC)]
```

で、日時は `X68kTZ` の時差の地方時。status 7 は「まだ時刻が無い」。INQUIRY が `X68NP` でない ID には書かない (普通のディスクなら LBA 0x100000 の中身を壊すため)。

時計に書く BCD は ROM の `_DATEBCD` / `_TIMEBCD` で作ってから `_DATESET` / `_TIMESET` に渡す。日付の BCD には曜日とうるう年の数え (年 % 4)、時刻の BCD には 24 時間制の指定が入る。自分で組んだ BCD を渡したところ、XEiJ では次に読んだ時刻が 8 時間ずれた (12 時が 20 時になった)。

## 試験

`tests/xeij_test.sh` (`make test`) が XEiJ と picosim (`harness/picosim`、ID 6 が TIME に答える) で、時刻の設定と、IOCS・DOS での読み戻し、装置の無い ID、まだ時刻が無いときを確かめる。XEiJ を起動する道具は j3tm0t0/x68k-zos の `kernel/tests/xeij_run.sh` を使う (`ZOS_TESTS=`)。実機 (XVI と BlueSCSI) ではまだ試していない。
