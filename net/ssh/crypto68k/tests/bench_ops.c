/* bench_ops.x -- X68000 で 4 操作を個別計測し BENCH.OUT に書く。
 *   X25519 shared (scalarmult), X25519 keygen (base=9), Ed25519 sign, Ed25519 verify。
 * FAST_FIELD / USE_ASM でビルドを切替。_iocs_ontime で 10MHz 相当の所要時間。 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdio.h>
#include "../nacl25519.c"

static const unsigned char SCAL[32] = {
  0xa5,0x46,0xe3,0x6b,0xf0,0x52,0x7c,0x9d,0x3b,0x16,0x15,0x4b,0x82,0x46,0x5e,0xdd,
  0x62,0x14,0x4c,0x0a,0xc1,0xfc,0x5a,0x18,0x50,0x6a,0x22,0x44,0xba,0x44,0x9a,0xc4 };
static const unsigned char POINT[32] = {
  0xe6,0xdb,0x68,0x67,0x58,0x30,0x30,0xdb,0x35,0x94,0xc1,0xa4,0x24,0xb1,0x5f,0x7c,
  0x72,0x66,0x24,0xec,0x26,0xb3,0x35,0x3b,0x10,0xa9,0x03,0xa6,0xd0,0xab,0x1c,0x4c };
static const unsigned char EXP[32] = {
  0xc3,0xda,0x55,0x37,0x9d,0xe9,0xc6,0x90,0x8e,0x94,0xea,0x4d,0xf2,0x8d,0x08,0x4f,
  0x32,0xec,0xcf,0x03,0x49,0x1c,0x71,0xf7,0x54,0xb4,0x07,0x55,0x77,0xa2,0x85,0x52 };
static const unsigned char ED_SEED[32]={0,17,34,51,68,85,102,119,136,153,0,17,34,51,68,85,102,119,136,153,0,17,34,51,68,85,102,119,136,153,0,17};
static const unsigned char ED_PUB[32]={87,190,244,87,213,119,255,143,222,169,208,189,164,62,231,223,127,184,45,109,122,120,161,27,135,169,64,223,197,214,44,217};
static const unsigned char ED_MSG[6]={222,173,190,239,202,254};
static const unsigned char ED_SIG[64]={143,78,86,232,224,49,132,209,170,204,253,101,107,79,70,41,175,34,143,11,183,63,149,81,150,140,36,147,11,218,33,52,25,61,26,84,189,123,73,81,144,148,159,129,104,162,15,68,42,41,92,43,124,182,68,195,68,131,29,117,249,251,4,4};

static long ela(struct iocs_time a, struct iocs_time b){ long c=(long)b.sec-(long)a.sec; if(c<0)c+=24L*60*60*100; return c; }

int main(void)
{
  struct iocs_time t0, t1;
  unsigned char q[32], sig[64]; unsigned long sl=0;
  long c_sh, c_kg, c_sg, c_vf; int i, ok=1, vr;

  t0=_iocs_ontime(); dropbear_curve25519_scalarmult(q, SCAL, POINT); t1=_iocs_ontime(); c_sh=ela(t0,t1);
  for(i=0;i<32;i++) if(q[i]!=EXP[i]) ok=0;

  { unsigned char base[32]; for(i=0;i<32;i++) base[i]=0; base[0]=9;
    t0=_iocs_ontime(); dropbear_curve25519_scalarmult(q, SCAL, base); t1=_iocs_ontime(); c_kg=ela(t0,t1); }

  t0=_iocs_ontime(); dropbear_ed25519_sign(ED_MSG, sizeof ED_MSG, sig, &sl, ED_SEED, ED_PUB); t1=_iocs_ontime(); c_sg=ela(t0,t1);
  { int sg_ok=1; for(i=0;i<64;i++) if(sig[i]!=ED_SIG[i]) sg_ok=0; if(!sg_ok) ok=0; }

  t0=_iocs_ontime(); vr=dropbear_ed25519_verify(ED_MSG, sizeof ED_MSG, ED_SIG, 64, ED_PUB); t1=_iocs_ontime(); c_vf=ela(t0,t1);
  if(vr!=0) ok=0;

  { FILE *f=fopen("BENCH.OUT","wb");
    if(f){ fprintf(f,"%s  X25519 shared=%ld.%02lds keygen=%ld.%02lds  Ed25519 sign=%ld.%02lds verify=%ld.%02lds  %s\n",
#ifdef USE_ASM
      "ASM",
#else
      "C  ",
#endif
      c_sh/100,c_sh%100, c_kg/100,c_kg%100, c_sg/100,c_sg%100, c_vf/100,c_vf%100, ok?"ALL OK":"WRONG");
      fclose(f); }
    printf("done %s\n", ok?"ok":"WRONG");
  }
  return 0;
}
