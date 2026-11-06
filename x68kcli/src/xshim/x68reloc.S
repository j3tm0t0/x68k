/* x68reloc.S: 再配置を自分で当てる入口 (-Wl,-e,__x68_fastreloc_start で crt0 の _start より先に動く)。
 *
 * Human68k の _EXEC は .x の再配置の表を 1 項目に約 33 マイクロ秒かけて当てる (vim 9 の 5 万項目で 1.7 秒)。
 * scripts/fastreloc.py が .x の再配置の表をデータの後ろ (bss の頭と重なる) に移し、ヘッダの再配置の大きさを 0 にして、
 * 下の __x68_reloc_table に表の位置 (テキストの頭から) を書く。ここでその表を 1 項目 5 命令で当て、
 * 表の場所から後ろを bss として crt0 に消させてから、本来の入口 (_start) へ飛ぶ。
 * 表の形: 前の位置からの差を符号付き 16 ビットで (1〜32767)。それより大きければ 0 の語の後に 32 ビット。0 の語と 0 の 32 ビットで終わり。
 * fastreloc.py を通していない .x では __x68_reloc_table が 0 なので、何もせず _start へ (_start の番地は loader が当てている)。
 * 起動時のレジスタ (a0 = メモリ管理ポインタ、a1 = 終わり、a2 = コマンドライン、a3 = 環境、a4 = 入口) はそのまま渡す */
    .text
    .globl  __x68_fastreloc_start
__x68_fastreloc_start:
    bra.s   1f
    .ascii  "X68RELO1"              /* fastreloc.py が探す目印 */
__x68_reloc_table:
    .long   0                       /* 表の位置 (テキストの頭から)。fastreloc.py が書く */
__x68_real_entry:
    .long   _start                  /* 表を当てると、ここも当たって本当の番地になる */
1:
    move.l  __x68_reloc_table(%pc),%d0
    beq.s   9f
    movem.l %d0-%d1/%a4-%a5,-(%sp)
    lea     256(%a0),%a4            /* テキストの頭 (crt0 と同じ) */
    move.l  %a4,%d1                 /* 足す値 = 読み込んだ番地 */
    lea     0(%a4,%d0.l),%a5        /* 表 */
    move.l  %a5,48(%a0)             /* crt0 はここから bss を 0 で消す (表も消える) */
    move.l  %a5,52(%a0)
2:                                  /* 1 項目 60 サイクルほど (表の形は fastreloc.py: 差を符号付き 16 ビットで。0 なら続く 32 ビット、それも 0 なら終わり) */
    move.w  (%a5)+,%d0
    beq.s   4f
    adda.w  %d0,%a4
    cmpa.l  %a1,%a4                 /* プログラムの終わり (a1) を越えたら、表が壊れているのでやめる */
    bcc.s   8f
    add.l   %d1,(%a4)
    bra.s   2b
4:
    move.l  (%a5)+,%d0
    beq.s   8f
    adda.l  %d0,%a4
    cmpa.l  %a1,%a4
    bcc.s   8f
    add.l   %d1,(%a4)
    bra.s   2b
8:
    movem.l (%sp)+,%d0-%d1/%a4-%a5
9:
    move.l  __x68_real_entry(%pc),-(%sp)
    rts
