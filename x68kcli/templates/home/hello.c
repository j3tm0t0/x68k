/* hello.c -- x68kcli の動作確認用サンプル
 *   gcc hello.c -o hello.x && hello.x      (GCC 1.42 真里子版)
 *   cc hello.c && hello.x                  (XC 2.1)
 *   gcc295 hello.c -o hello.x && hello.x   (GCC 2.95.2, 68020 以上のみ)
 */
#include <stdio.h>

int main(void)
{
    int i;
    for (i = 1; i <= 3; i++)
        printf("hello from X68000, %d\n", i);
    return 0;
}
