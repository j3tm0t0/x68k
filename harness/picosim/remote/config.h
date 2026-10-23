/* picosim の B: (remote.c) で x68kserremote の remoteserv.c をビルドするときの設定。ファームの include/config.h と同じ値
   (Z 側の scsiremote.sys もこの値でビルドされている: 1 回の READ / WRITE は 15KB まで、FILES は 10 件ずつ) */
#ifndef _CONFIG_H_
#define _CONFIG_H_
#define CONFIG_ALIGNED
#define CONFIG_NFILEINFO    10
#define CONFIG_DATASIZE     (1024 * 15)
#define CONFIG_NDCACHE      4
#define CONFIG_NFCACHE      4
#endif
