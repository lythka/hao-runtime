#!/usr/bin/env python3
"""Indexed SQDMULH/SQRDMULH: native ARM values, aliases, clearing and QC."""
import json
import os
from pathlib import Path
import subprocess

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
OUT=Path(os.environ.get('OUT',ROOT/'build/indexed-mulhigh')).resolve()
HAO=Path(os.environ.get('HAO',ROOT/'build/hao')).resolve()
OUT.mkdir(parents=True,exist_ok=True)
env={k:v for k,v in os.environ.items() if not k.startswith('HAO_')}

def run(args,policy=env):
    return subprocess.run(list(map(str,args)),env=policy,capture_output=True,timeout=90,check=True)

def word(rounded,size,index,form,rd,rn,rm):
    scalar=form=='scalar'
    q=1 if scalar or form=='q1' else 0
    h=index//4 if size==1 else index//2
    l=(index//2)&1 if size==1 else index&1
    m=index&1 if size==1 else rm//16
    return (0x0f000000 | (int(scalar)<<28) | (q<<30) | (size<<22) |
            (l<<21) | (m<<20) | ((rm&15)<<16) | ((13 if rounded else 12)<<12) |
            (h<<11) | (rn<<5) | rd)

cases=[(0x4f63d273,19)]  # exact observed instruction, including Rd=Rn alias
for rounded in (False,True):
    for size in (1,2):
        for index in range(8 if size==1 else 4):
            for form in ('q0','q1','scalar'):
                regs=[(0,1,2),(1,1,2),(0,1,0),(0,0,0)]
                regs.append((0,1,15 if size==1 else 31))
                for rd,rn,rm in regs:
                    cases.append((word(rounded,size,index,form,rd,rn,rm),rd))

preamble=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t words[2]; } V;
typedef uint64_t (*operation)(V *, const V *, const V *, uint64_t, uint64_t);
'''
functions=[]
for n,(opcode,rd) in enumerate(cases):
    functions.append(f'''
__attribute__((noinline)) static uint64_t op{n}(V *out,const V *a,const V *b,uint64_t flags,uint64_t control) {{
    uint64_t fpsr,before,after;
    __asm__ volatile(
        "ldr q0,[%[out]]\\n ldr q1,[%[a]]\\n ldr q2,[%[b]]\\n"
        "ldr q3,[%[b]]\\n ldr q15,[%[b]]\\n ldr q19,[%[a]]\\n ldr q31,[%[b]]\\n"
        "msr fpcr,%[control]\\n msr fpsr,%[flags]\\n mrs %[before],fpcr\\n"
        ".inst {opcode:#x}\\n str q{rd},[%[out]]\\n"
        "mrs %[fpsr],fpsr\\n mrs %[after],fpcr"
        : [fpsr] "=&r"(fpsr),[before] "=&r"(before),[after] "=&r"(after)
        : [out] "r"(out),[a] "r"(a),[b] "r"(b),[flags] "r"(flags),[control] "r"(control)
        : "v0","v1","v2","v3","v15","v19","v31","memory");
    if(before!=after) return UINT64_MAX;
    return fpsr;
}}
''')
table='static const operation operations[]={'+','.join('op'+str(n) for n in range(len(cases)))+'};\n'
main=r'''
static uint64_t seed=0x123456789abcdef0ull;
static uint64_t random_word(void) {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static const uint16_t half[]={0x8000,0x7fff,0xffff,0,1,0x4000,0xc000,0x7ffe};
static const uint32_t whole[]={0x80000000u,0x7fffffffu,0xffffffffu,0,1,0x40000000u,0xc0000000u,0x7ffffffeu};
int main(void) {
    for(unsigned n=0;n<sizeof operations/sizeof operations[0];n++)
        for(unsigned r=0;r<26;r++) for(unsigned sticky=0;sticky<2;sticky++) {
            V a,b,out={{0xa5a5a5a5a5a5a5a5ull,0x5a5a5a5a5a5a5a5aull}};
            if(r<8) {
                uint16_t av[8],bv[8];
                for(unsigned k=0;k<8;k++) {av[k]=half[(r+k)%8];bv[k]=half[r];}
                memcpy(&a,av,16);memcpy(&b,bv,16);
            } else if(r<16) {
                uint32_t av[4],bv[4];
                for(unsigned k=0;k<4;k++) {av[k]=whole[(r+k)%8];bv[k]=whole[r%8];}
                memcpy(&a,av,16);memcpy(&b,bv,16);
            } else if(r<22) {
                static const uint64_t av[]={0xffffffffffffffffull,0x0000000100000001ull,
                    0x8000000080000000ull,0xffffffffffffffffull,0x0001000100010001ull,0x8000800080008000ull};
                static const uint64_t bv[]={0x4000000040000000ull,0x4000000040000000ull,
                    0x8000000080000000ull,0x4000400040004000ull,0x4000400040004000ull,0x8000800080008000ull};
                a.words[0]=a.words[1]=av[r-16];b.words[0]=b.words[1]=bv[r-16];
            } else {a.words[0]=random_word();a.words[1]=random_word();b.words[0]=random_word();b.words[1]=random_word();}
            if(!(r%4)) out=a;  /* Alias cases also receive edge values. */
            uint64_t flags=sticky?0x0800009full:0;
            uint64_t fpsr=operations[n](&out,&a,&b,flags,sticky?0x03000000ull:0);
            printf("%u %u %u %016llx%016llx %08llx\n",n,r,sticky,
                   (unsigned long long)out.words[1],(unsigned long long)out.words[0],(unsigned long long)fpsr);
            if(fpsr==UINT64_MAX) return 1;
        }
    return 0;
}
'''
source=OUT/'fixture.c'
source.write_text(preamble+''.join(functions)+table+main)
for arch in ('arm64','arm64e'):
    binary=OUT/('fixture.'+arch)
    run(['xcrun','clang','-arch',arch,'-O1','-mmacosx-version-min=27.0',source,'-o',binary])
    run(['codesign','-s','-','-f',binary])
reference=run([OUT/'fixture.arm64']).stdout
(OUT/'native.out').write_bytes(reference)
assert len(reference.splitlines())==len(cases)*52
cache=OUT/'aot'
cache.mkdir(exist_ok=True)
aot_env=dict(env,HAO_CACHE=str(cache),HAO_VERBOSE='1')
p=run([HAO,'--aot',OUT/'fixture.arm64e'],aot_env)
(OUT/'aot-build.log').write_bytes(p.stdout+p.stderr)
results=[]
for mode,args,policy in [
    ('jit',[HAO,OUT/'fixture.arm64e'],dict(env,HAO_NO_AOT='1')),
    ('interp',[HAO,'--interp',OUT/'fixture.arm64e'],dict(env,HAO_NO_AOT='1')),
    ('aot',[HAO,OUT/'fixture.arm64e'],aot_env)]:
    p=run(args,policy)
    (OUT/(mode+'.out')).write_bytes(p.stdout)
    (OUT/(mode+'.err')).write_bytes(p.stderr)
    loaded=mode!='aot' or any(b'AOT cache ' in line and b' loaded' in line for line in p.stderr.splitlines())
    ok=p.stdout==reference and loaded
    results.append(dict(mode=mode,matches=ok,loaded_aot=loaded if mode=='aot' else None))
    print(mode,len(cases),'encodings',len(reference.splitlines()),'value/state cases','PASS' if ok else 'FAIL',flush=True)

# Decoder rejection does not change architectural vectors or cumulative QC.
host=OUT/'reserved.c'
invalid=[]
for base in (0x0f40c000,0x0f40d000,0x5f40c000,0x5f40d000):
    invalid += [(base&~0x00c00000), (base|0x00c00000), (base|0x20000000)]
host.write_text(r'''
#include "hao/cpu.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    unsigned bad[]={'''+','.join(hex(v) for v in invalid)+r'''};
    for(unsigned k=0;k<sizeof bad/sizeof bad[0];k++) {
        hao_cpu c,before;memset(&c,0xa5,sizeof c);before=c;
        int rc=a64_exec_simd(&c,bad[k],0x1000);
        if(rc!=-1 || memcmp(c.v,before.v,sizeof c.v) || c.fpsr!=before.fpsr || c.fpcr!=before.fpcr) {
            printf("invalid %08x rc%d changed state\n",bad[k],rc);return 1;
        }
    }
    puts("reserved size/U encodings rejected without vector/FPSR/FPCR changes");return 0;
}
''')
run(['xcrun','clang','-arch','x86_64','-mmacosx-version-min=27.0','-I'+str(ROOT/'include'),host,
     HAO.parent/'libhaocore.a','-o',OUT/'reserved'])
run(['codesign','-s','-','-f',OUT/'reserved'])
p=run([OUT/'reserved'])
print(p.stdout.decode().strip(),flush=True)
(OUT/'results.json').write_text(json.dumps(dict(encodings=len(cases),cases=len(reference.splitlines()),results=results,reserved=p.stdout.decode()),indent=2)+'\n')
raise SystemExit(not all(r['matches'] for r in results))
