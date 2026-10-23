# usbbench: Z の真似をする USB ホスト

X68000 Z に挿す前に、Pico のファーム (Pico Z Loader や fork) が Z の USB の使い方に耐えるかを、机の上の Pico (p2w) で確かめる。
Z の中の Linux は見られないので、Z が何を送るかは **Pico の側で記録** し (capture/)、それを raspi3 から **libusb で送り直す** (zhost.py)。

## 道具

| もの | どこで | すること |
|---|---|---|
| `capture/build-pzl.sh <rev> <out.elf> [-D...]` | Mac | 採取用の PZL: TinyUSB のログ (CFG_TUSB_DEBUG=2) を UART に、行頭に起動からの ms、READ/WRITE(10) の番地 (`ZC R/W`)、名前の無い SCSI 命令の CDB (`ZC C`)。毎回出る決まった行は省く (UART 115200 は 1 行 3 ms で、Z への返事を遅らせる) |
| `zcap2profile.py <picouart.log> -o p.json` | raspi3 / Mac | 記録 → profile。列挙の制御転送 (STALL したものは印付き)、SCSI 命令の並びと時刻。WRITE(10) は入れない (数だけ) |
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
- Device Qualifier の要求は full speed の機器なので STALL が正しい。記録に `stall` の印がある要求は、STALL なら合格。
- HID の「Z がどの間隔で読むか」はデバイス側では見えない (NAK は記録に出ない)。zhost は endpoint の bInterval (10 ms) で読む。
- raspi3 の /tmp は 453MB の tmpfs。いっぱいになると UART の記録係 (picouart-*.service、uartlog.py) が落ちる (10/23 21:06 に p2w の係が落ちていた)。採取の前に `systemctl is-active picouart-sd` を確かめる。
- p2w のフラッシュの控えを openocd で取るとき、`flash read_bank` や 4MB 一度の `dump_image` は途中の番地で読み損なうことがある。256KB ずつ、読み損ねた塊だけ読み直すと揃う (`reset run; sleep 2000; halt` のあと)。
