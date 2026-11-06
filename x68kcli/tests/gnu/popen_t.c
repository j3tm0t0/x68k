/* popen_t.x "コマンド": popen で読んだ行と pclose の値を出す (tests/gnu/zosh.sh が src/xshim の popen を試すのに使う) */
#include <stdio.h>
int main(int argc, char **argv)
{
    char b[256];
    FILE *f;
    if (argc < 2) return 2;
    if (!(f = popen(argv[1], "r"))) { printf("popen failed\n"); return 1; }
    while (fgets(b, sizeof b, f)) fputs(b, stdout);
    printf("status=%d\n", pclose(f));
    return 0;
}
