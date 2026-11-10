/* Internal descriptor routing must preserve the complete interpreter contract. */
#include "hao/cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int hao_jit_exec_code(hao_cpu*, const uint32_t*, int, uint64_t);
static uint64_t seed=0x73a97251c883d0f1ULL;
static uint64_t random_word(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static void init(hao_cpu*c,unsigned control,unsigned sticky){
 uint64_t *p=(uint64_t*)c;for(unsigned k=0;k<sizeof *c/8;k++)p[k]=random_word();
 /* control low2=RMode, bit2=DN, bit3=FZ; all16 combinations. */
 a64_fpcr_write(c,((control&3)<<22)|((control&4)<<23)|((control&8)<<21));a64_fpsr_write(c,sticky?0x0800009f:0);
}
static int same(uint32_t word,unsigned control,unsigned sticky){
 hao_cpu before,a,b;init(&before,control,sticky);a=before;b=before;
 a64_fpcr_write(&a,before.fpcr);a64_fpsr_write(&a,sticky?0x0800009f:0);
 int ar=a64_exec(&a,word,0x123456780ULL);unsigned af=a64_fpsr_read(&a);
 a64_fpcr_write(&b,before.fpcr);a64_fpsr_write(&b,sticky?0x0800009f:0);
 int br=a64_exec_helper(&b,a64_helper_descriptor(word),0x123456780ULL);unsigned bf=a64_fpsr_read(&b);
 if(ar!=br||af!=bf||memcmp(&a,&b,sizeof a)){printf("routing mismatch %08x control%u sticky%u\n",word,control,sticky);return 1;}return 0;
}
int main(void){
 unsigned keys[]={0,1,2,3,4,6,7,0x20,0x21,0x22,0x26,0x27,0x43,0x44,0x45,0x47,0x62,0x64,0x65};
 unsigned regs[][3]={{0,1,2},{1,1,2},{2,1,2},{0,0,0},{31,30,31}};unsigned cases=0;
 for(unsigned k=0;k<sizeof keys/sizeof*keys;k++)for(unsigned shape=0;shape<3;shape++)for(unsigned pair=0;pair<5;pair++){
  unsigned key=keys[k],q=shape!=0,dbl=shape==2;uint32_t word=0x0e200400u|q<<30|(key>>6)<<29|(((key>>5)&1)*2+dbl)<<22|regs[pair][2]<<16|(0x18|(key&7))<<11|regs[pair][1]<<5|regs[pair][0];
  if(!a64_vector_fp_eligible(word)||a64_helper_descriptor(word)!=((uint64_t)HAO_HELPER_TAG_VECTOR_FP<<32|word))return 2;
  for(unsigned pattern=0;pattern<32;pattern++)for(unsigned mode=0;mode<16;mode++)for(unsigned sticky=0;sticky<2;sticky++){if(same(word,mode,sticky))return 1;cases++;}
 }
 uint32_t other[]={0,0xd4000001u,0xd4200000u,0xd4400000u,0x1e222820u,0x4e441483u,0x6e22d420u,0x0e62d420u,0x4f819802u,0xffffffffu};unsigned rejected=0;
 for(unsigned k=0;k<sizeof other/sizeof*other;k++){
  uint32_t word=other[k];if(a64_vector_fp_eligible(word)||a64_helper_descriptor(word)!=word)return 3;
  if(same(word,0,0))return 1;
  for(unsigned tag=1;tag<4;tag++){
   uint32_t tags[]={0,HAO_HELPER_TAG_VECTOR_FP,2,0xffffffffu};hao_cpu c,before;init(&c,0,0);before=c;
   uint64_t desc=(uint64_t)tags[tag]<<32|word;int rc=a64_exec_helper(&c,desc,0x887766554ULL);before.pc=0x887766554ULL;before.exit_insn=word;
   if(rc!=HAO_EXIT_UNDEF||memcmp(&c,&before,sizeof c)){printf("bad descriptor accepted %llx\n",(unsigned long long)desc);return 4;}rejected++;
  }
 }
 unsigned scratch=0;
 for(unsigned mode=0;mode<16;mode++)for(unsigned sticky=0;sticky<2;sticky++){
  hao_cpu a,b;memset(&a,0,sizeof a);a.x[0]=7;a.x[1]=3;hao_set_nzcv(&a,10);
  for(unsigned e=0;e<4;e++){a.v[0].f[e]=1.5f;a.v[1].f[e]=.25f;}
  a64_fpcr_write(&a,((mode&3)<<22)|((mode&4)<<23)|((mode&8)<<21));a64_fpsr_write(&a,sticky?0x0800009f:0);b=a;
  uint32_t code[98];code[0]=0xeb01001fu; /* CMP x0,x1 */
  for(unsigned k=1;k<=96;k++)code[k]=0x4e21d402u; /* independent FADD v2.4s,v0.4s,v1.4s */
  code[97]=0x9a818002u; /* CSEL x2,x0,x1,HI: consumes NZCV after helpers across96-insn blocks */
  for(unsigned k=0;k<98;k++)if(a64_exec(&a,code[k],0x70000000ull+4*k))return 5;
  unsigned af=a64_fpsr_read(&a);a64_fpcr_write(&b,a.fpcr);a64_fpsr_write(&b,sticky?0x0800009f:0);
  int result=hao_jit_exec_code(&b,code,98,0x70000000ull);unsigned bf=a64_fpsr_read(&b);
  if(result||b.rt_helper!=(uint64_t)(uintptr_t)a64_exec_helper||a.pc!=b.pc||memcmp(a.x,b.x,sizeof a.x)||memcmp(a.v,b.v,sizeof a.v)||hao_nzcv(&a)!=hao_nzcv(&b)||af!=bf||a64_fpcr_read(&a)!=a64_fpcr_read(&b)){
   printf("scratch routing/NZCV mismatch mode%u sticky%u result%d\n",mode,sticky,result);return 6;
  }
  uint32_t bad[]={0x4e21d402u,0xffffffffu};a64_fpsr_write(&b,0);
  if(hao_jit_exec_code(&b,bad,2,0x71000000ull)!=HAO_EXIT_UNDEF||b.pc!=0x71000004ull||b.exit_insn!=0xffffffffu)return 7;
  scratch++;
 }
 printf("helper descriptor: %u full-CPU/FP routing comparisons, %u malformed-tag/word rejections, %u scratch max-block/NZCV/exit controls PASS\n",cases,rejected,scratch);return 0;
}
