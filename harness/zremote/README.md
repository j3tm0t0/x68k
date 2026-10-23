# harness/zremote -- Z のリモートドライブを決まったドライブ名に

`zdrv.x` は、x68kzremotedrv のリモートドライブ (scsiremote.sys、デバイス名 `\x01ZREMOTE`) が付いたドライブを探し、
決まった名前 (既定 Z:) をその根の仮想ドライブにする (DOS _ASSIGN の mode 0x50、SUBST と同じ)。SD のデータディスクなどが
増えてリモートドライブが B: から C: にずれても、AUTOEXEC.BAT で `zdrv.x` しておけば z: で同じ所を指せる。

```
zdrv.x        Z: = リモートドライブ (見つからなければ 1 を返す)
zdrv.x X:     X: に
zdrv.x -l     ドライブごとのデバイスドライバの名前
```

`make` (elf2x68k)。XEiJ + picosim で、リモートドライブが C: のときに `zdrv: Z: = C:\`、z: の読み書きが Mac の
ディレクトリに届くのを確かめた (2026-10-23)。ドライバの見出しはスーパーバイザ領域にあるので _SUPER の中で読む
(ユーザモードで読むとバスエラー)。調べた経緯は [../picosim/HANDOFF.md](../picosim/HANDOFF.md) の「ドライブ名」。
