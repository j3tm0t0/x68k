# x68kzremotedrv-netpipe

[x68kzremotedrv](https://github.com/yunkya2/x68kzremotedrv) (yunkya2 氏、MIT) に netpipe を足すパッチ。
X68000 Z の SCSI ID 6 越しに SLIP のバイト列を運び、Z から Wi-Fi で TCP/IP を使えるようにする。
Z 側は [../x68kcli](../x68kcli) の `slipd.x -z` (`netup pico` / `netup bridge`)。

## 当て方

```sh
git clone https://github.com/yunkya2/x68kzremotedrv
cd x68kzremotedrv
git checkout 3afd408df99f3f9d1aafc7c090e54285cbeadba6   # "Show system statistics by default"
git am /path/to/x68k/x68kzremotedrv-netpipe/patches/*.patch
make
```

macOS (arm-gnu-toolchain 14.3) では次の設定でビルドした:

```sh
export PATH=~/opt/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin:$PATH
export CMAKE_POLICY_VERSION_MINIMUM=3.5
export CFLAGS="-include time.h -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion"
```

書き込みは BOOTSEL のまま `picotool load -x build/x68kzremotedrv.uf2`。設定 (フラッシュ 0x1F0000) は残る。

## 中身

| ファイル | 変更 |
|---|---|
| `src/netpipe.c` / `netpipe.h` | 新規。要求を LBA 0x100000 に書き、応答を 0x110000〜0x1ffff7 から読む。UDP モードとブリッジモード |
| `src/virtual_disk.c` | ID 6 の読み書きを netpipe に回す |
| `src/connect.c` | Wi-Fi の省電力を切る (応答が 50〜300ms 遅れて TCP が遅くなるため) |

- **UDP モード** (`slipd.x -z`): SLIP を UDP 6868 で `config.txt` の SMB サーバ (Raspberry Pi) に送る。
  Pi 側は `x68kcli/templates/raspi/slipudp.py` + slattach (`MODE=pico`)。
- **ブリッジモード** (`slipd.x -z 192.168.1.68`): Pico がその IP を代理 ARP で名乗り、IPv4 を Wi-Fi に転送する。Pi は要らない。
  外からの ping (echo request) は Z に渡さない。

## 注意

- Z の Linux は SCSI の読み込みをキャッシュし、X68000 をリセットしても消えない。応答は毎回違う LBA から読む。
- 0x200000〜0x3fffff は scsiremote.sys (B: ドライブ) が使うので、netpipe の帯はその下に置く。
- xip.x は送るフレームを 60 バイトまで詰めるので、ブリッジは IP の全長で切って送る。
