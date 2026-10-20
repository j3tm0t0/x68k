#!/usr/bin/env python3
"""Human68k 用の空の SCSI HDD イメージ (.hds) を作る。
レイアウトは XEiJ の SCMedia.scmMakeFormatData() / HumanMedia と同じ:
  0x000  X68SCSI1 ヘッダ (512 バイト/レコード)
  0x400  SCSI ディスク IPL          } FORMAT.X (Human68k 3.02) に埋め込まれている
  0x800  X68K パーティションテーブル
  0xC00  SCSI デバイスドライバ      } Sharp 純正コードをそのまま取り出す
  32KB   パーティション "Human68k": パーティション IPL + BPB, FAT x2, ルートディレクトリ
rb-cli の `new hd x68k` が作る FAT は Human68k に「管理領域が破壊されています」と判定されるため使わない。
usage: mkhds.py FORMAT.X IMAGE.hds SIZE_MB [SECTORS_PER_CLUSTER]
SECTORS_PER_CLUSTER の既定は 8 (8KB クラスタ)。rb-cli はディレクトリが 1 クラスタを超えると
FAT チェーンを壊すため、1 クラスタに 256 エントリ入る大きさにしてある (XEiJ の自動計算なら 2)。
"""
import struct, sys

fmt_path, img_path, mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
spc_arg = int(sys.argv[4]) if len(sys.argv) > 4 else 8
fmt = open(fmt_path, "rb").read()
menu = fmt.find(b"X68000 SCSI DISK IPL MENU")
pboot = fmt.find(b"\x60\x24SHARP/KG    1.00")
drv = fmt.find(b"\xff\xff\xff\xff\x40\x00\x00\x00\x00\x6e\x00\x00\x00\x80\x01SCHDISK")
if min(menu, pboot, drv) < 0:
    sys.exit("mkhds: boot code markers not found in %s (FORMAT.X v2.31 expected)" % fmt_path)
disk_ipl = fmt[fmt.rfind(b"\x60\x00", 0, menu):pboot]
part_ipl = fmt[pboot:drv]
driver = fmt[drv:drv + 12771]
assert 0x380 <= len(disk_ipl) <= 0x400 and 0x280 <= len(part_ipl) <= 0x400

BPR = 512                     # SCSI レコード長
BPS = 1024                    # Human68k 論理セクタ長
PART_START = 32768            # パーティション先頭 (バイト)
NFATS, RESERVED, ROOTENTS, FATID = 2, 1, 512, 0xF7

disk_end = PART_START + (mb << 20)
records = disk_end // BPR
disk_sectors = disk_end // BPS
part_start_sector = PART_START // BPS
part_sectors = (disk_end - PART_START) // BPS

def b2s(n):  # バイト数 -> セクタ数 (切り上げ)
    return (n + BPS - 1) // BPS

root_sectors = b2s(ROOTENTS * 32)
fad = part_sectors - RESERVED - root_sectors          # FAT 領域 + データ領域
shift = 0
while b2s(2 * (65520 + 2)) * NFATS + (65520 << shift) < fad:
    shift += 1
if spc_arg > (1 << shift):
    shift = spc_arg.bit_length() - 1
spc = 1 << shift
fat12 = fad < b2s(2 * (4085 + 2)) * NFATS + (4085 << shift)
dcp2 = (fad >> shift) + 2
if fat12:
    dcp2 = min(4084, (fad - b2s((3 * dcp2 + 1) >> 1) * NFATS) >> shift) + 2
else:
    dcp2 = ((fad - b2s(2 * dcp2) * NFATS) >> shift) + 2
fat_sectors = (fad - ((dcp2 - 2) << shift)) // NFATS
root_start = RESERVED + fat_sectors * NFATS
assert fat_sectors < 256, "partition too large for 8-bit sectors-per-FAT"

img = bytearray(PART_START + (root_start + root_sectors) * BPS)
img[0:8] = b"X68SCSI1"
img[8:10] = struct.pack(">H", BPR)
img[10:14] = struct.pack(">I", records)
img[14:16] = struct.pack(">H", 0x0100)
img[16:16 + 35] = b"Human68K SCSI-DISK by Keisoku Giken"
img[0x400:0x400 + len(disk_ipl)] = disk_ipl
img[0x800:0x804] = b"X68K"
img[0x804:0x810] = struct.pack(">III", disk_sectors, disk_sectors, disk_sectors)
img[0x810:0x818] = b"Human68k"
img[0x818:0x820] = struct.pack(">II", part_start_sector, part_sectors)   # 上位バイト 0 = 自動起動
img[0xC00:0xC00 + len(driver)] = driver
assert 0xC00 + len(driver) <= PART_START
P = PART_START
img[P:P + len(part_ipl)] = part_ipl
img[P + 0x12:P + 0x26] = struct.pack(">HBBHHHBBII", BPS, spc, NFATS, RESERVED, ROOTENTS, 0,
                                     FATID, fat_sectors, part_sectors, part_start_sector)
marker = struct.pack(">I", FATID << 24 | (0x00FFFF00 if fat12 else 0x00FFFFFF))
for i in range(NFATS):
    o = P + (RESERVED + fat_sectors * i) * BPS
    img[o:o + 4] = marker
with open(img_path, "wb") as f:
    f.write(img)
    f.truncate(disk_end)
print("mkhds: %s  %d MB, partition %d sectors, FAT%d, %d sectors/cluster, %d sectors/FAT, %d clusters"
      % (img_path, mb, part_sectors, 12 if fat12 else 16, spc, fat_sectors, dcp2 - 2))
