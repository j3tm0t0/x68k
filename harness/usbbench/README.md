# usbbench: Z の真似をする USB ホスト

X68000 Z に挿す前に、Pico のファーム (Pico Z Loader や fork) が Z の USB の使い方に耐えるかを、机の上の Pico (p2w) で確かめる。
Z の中の Linux は見られないので、Z が何を送るかは **Pico の側で記録** し (capture/)、それを raspi3 から **libusb で送り直す** (zhost.py)。

## 道具

| もの | どこで | すること |
|---|---|---|
| `capture/build-pzl.sh <rev> <out.elf> [-D...]` | Mac | 採取用の PZL: TinyUSB のログ (CFG_TUSB_DEBUG=2) を UART に、行頭に起動からの ms、READ/WRITE(10) の番地 (`ZC R/W`)、名前の無い SCSI 命令の CDB (`ZC C`)。毎回出る決まった行は省く (UART 115200 は 1 行 3 ms で、Z への返事を遅らせる) |
| `capture/build-fork.sh <fork のツリー> <そのビルド dir> <out.elf>` | Mac | fork (x68kzremotedrv) の採取用。ツリーを一時ディレクトリに写し (元には触らない)、同じ ZC の行と TinyUSB のログを足して、指定のビルドの CMakeCache と同じ設定でビルド |
| `zcap2profile.py <picouart.log> -o p.json` | raspi3 / Mac | 記録 → profile。列挙の制御転送 (元のファームが STALL した要求には `"stall": true` を付ける)、SCSI 命令の並びと時刻。WRITE(10) は入れない (数だけ) |
| `zhost.py [-p p.json] [--speed N] [-t 秒]` | raspi3 (sudo、`~/venv-usbbench` の pyusb) | バスリセット → profile の列挙 → HID の IN を bInterval ごとに読み続けながら MSC (BOT) を時刻どおりに。どれかの転送が `-t` 秒を超えたら **HANG** (終了コード 2)、STALL や記録との違いは ERRORS (1)、問題なしは OK (0)。最後にバスリセットして Linux のドライバに返す |
| `profiles/z-guess.json` | | 記録が無いときの推測 (Linux 風の列挙と起動の読み込み) |

## 使い方 (p2w)

```sh
# raspi3: 一度だけ
python3 -m venv ~/venv-usbbench && ~/venv-usbbench/bin/pip install pyusb
# p2w を使う前に scratchpad/p2wlock を取る (zlock と同じ決まり)。控え: openocd の dump_image (下の注意)
swdflash <試すファーム.elf> p2w; sleep 25
sudo ~/venv-usbbench/bin/python zhost.py -p profiles/<記録>.json -t 5
```

## Z で記録を採る (zlock を取ってから、リードの許可で)

1. `capture/build-pzl.sh <rev> /tmp/x.elf <Z と同じ -D>` を raspi3 へ、`swdflash /tmp/x.elf sd`、Z の電源を入れ直す (`zpower`。ソフトリセットでは Z が USB を見つけ直さない)
2. `n0=$(wc -l < /tmp/picouart-sd.log)` → Z で操作 (起動、`dir`、`dir b:\*.bat`、30 秒待つ) → `tail -n +$n0 /tmp/picouart-sd.log > /tmp/zcap-<名前>.log`
3. 元のファームに戻す (リードが言うもの)。`zcap2profile.py /tmp/zcap-<名前>.log -o profiles/z-<名前>.json`

## 分かったこと・注意

- TinyUSB の BOT: 求められた長さより短いデータ (MODE SENSE(6) の 4 バイトなど) のあと IN を STALL し、CSW はその後。zhost は STALL を解いて CSW を読む (仕様どおり)。
- Device Qualifier の要求は full speed の機器なので STALL が正しい。記録で `"stall": true` の要求は、STALL が返れば合格。
- HID の「Z がどの間隔で読むか」はデバイス側では見えない (NAK は記録に出ない)。zhost は endpoint の bInterval (10 ms) で読む。
- raspi3 の /tmp は 453MB の tmpfs。いっぱいになると UART の記録係 (picouart-*.service、uartlog.py) が落ちる (10/23 21:06 に p2w の係が落ちていた)。採取の前に `systemctl is-active picouart-sd` を確かめる。
- p2w のフラッシュの控えを openocd で取るとき、`flash read_bank` や 4MB 一度の `dump_image` は途中の番地で読み損なうことがある。256KB ずつ、読み損ねた塊だけ読み直すと揃う (`reset run; sleep 2000; halt` のあと)。

## Z で採ったもの (2026-10-23 22:05〜22:25、profiles/z-*.json)

| 記録 | ファーム | Z での結果 (dir b:\ /w + ver を繰り返す) | p2w で送り直し |
|---|---|---|---|
| z-fork | fork 670fb84 (fw-fork-4fb2a2f5 の設定) | 8 回とも通る | OK |
| z-pzl60 | PZL 60a9db7、UDP_KEYS、マウス無し | **10 回とも通る** (素のビルドは Charlie の試験で固まる) | OK |
| z-pzl35m | PZL 35fd5d8、UDP_KEYS + マウス | **12 回とも通る** | OK |

分かったこと:
- **Z の Linux の列挙** (電源投入ごとに 2 回、約 15 秒あけて): Device (64) → Set Address → Device (18) → Device Qualifier ×3 (STALL が正しい) →
  Config (9 → 全部) → String 0/2/1/3 → SET_CONFIGURATION 1 → GET_MAX_LUN → HID ごとに **SET_IDLE(0) → Report 記述子** →
  キーボードに **SET_REPORT (LED、1 バイト)**。**SET_PROTOCOL も GET_REPORT も来ない** (boot protocol に切り替えない = report protocol のまま)。
  マウスがあると、マウスにも SET_IDLE と Report 記述子 (0x4d バイト)。fork と PZL で同じ。
- **TEST UNIT READY が約 2.1〜2.5 秒ごと** に来るのは fork でも同じ (正常に動いている間も)。固まったことを示すものではない。
- MSC の命令の種類も fork と PZL で同じ (MODE SENSE(6) は短い返事 + STALL が 12 回、Prevent/Allow、READ CAPACITY、INQUIRY)。
- **採取用ビルドでは PZL が固まらなかった**。違いは返事の遅さだけ (ログの行ごとに UART で 3 ms ほど)。返事が速すぎるときだけ起きる、
  時間に敏感な不具合を疑う材料。
- PZL の記録に `smb2_connect_share ... share:sd: STATUS_BAD_NETWORK_NAME`: sd 基板の設定の REMOTE1 = `sd:/` (fork の SD の
  リモートドライブ) を PZL は SMB の共有として開こうとしている。

## 状態 (2026-10-23 22:10)

- p2w で一巡を確認: 採取用 PZL (35fd5d8) と採取用 fork (670fb84 = 0043、build-p2w と同じ設定) がどちらも起動し、zhost.py の z-guess で OK、
  記録 → zcap2profile.py → 送り直しで OK。
- (済) Z の記録。raspi3 に置いたもの: `/tmp/pzlcap-35fd5d8-mouse.elf` (悪い方の候補)、`/tmp/pzlcap-35fd5d8-nomouse.elf`、
  `/tmp/zcap-fork-670fb84.elf` (良い方、Z に今入っている fork と同じ設定)。Z の順番は Bravo → Charlie (PZL の 2 つ) → Alpha → Delta (fork)。
- 次: 記録から profiles/z-*.json を作り、p2w で「悪い方だけ HANG」になるかを確かめる。ならなければ、Z の操作 (dir の中断キーの確認) を
  HID の側で真似る (GET_REPORT / SET_REPORT の頻度) ところを足す。
