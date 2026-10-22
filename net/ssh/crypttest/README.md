# crypttest -- dropbear の chacha20-poly1305 を Z と Mac で比べる

ssh.x -Z のヘルパ転送で sshd が "message authentication code incorrect" を出したときに、68000 での大きなパケットの
暗号化を疑って作った。dropbear の chachapoly.c の encrypt と同じ手順で 16〜32780 バイトを暗号化し、暗号文とタグの
FNV を出す。**Z と Mac で全部一致** (2026-10-22)。Z では約 232 KB に 157 秒 = **約 1.5 KB/s** (暗号化と MAC)。

```sh
# dropbear を一度 build.sh で作った後 (work/ の libtomcrypt.a を使う)
D=../dropbear/work/dropbear-2022.83; LT=$D/libtomcrypt; INC="-I$LT/src/headers -I$D -DLTC_SOURCE -DDROPBEAR_SVR_PASSWORD_AUTH=0"
m68k-xelf-gcc -m68000 -O2 -fomit-frame-pointer $INC -I$D/../../x68kinc -include $D/../../x68kinc/x68k_compat.h -include $D/config.h crypttest.c $LT/libtomcrypt.a -o crypttest.x
cc -O2 -w $INC crypttest.c hostburn.c $LT/src/stream/chacha/chacha_{crypt,ivctr64,keystream,setup}.c $LT/src/mac/poly1305/poly1305.c $LT/src/misc/zeromem.c $LT/src/misc/crypt/crypt_argchk.c -o crypttest_host
```
