/* Standalone MAP_JIT/fork diagnostic, intentionally linked without hao.
 * Two parent writers emit and execute disjoint functions while the parent
 * forks. Rosetta makes a page r-x once it has run x86 code from it and lifts
 * that on the next write; a write to such a page while the process forks
 * ends it with SIGBUS instead. `mapjit_fork noexec` writes without running
 * (a control: no page is protected). The native arm64 build uses the
 * required per-thread JIT write-protection API.
 * Run with an external timeout; an intermittent signal is a diagnostic
 * failure, not an expected-pass gate result. See tests/README.md. */
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#ifdef __aarch64__
#include <libkern/OSCacheControl.h>
#endif
static unsigned char *code;
static unsigned next;
static int stop, noexec;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static void one(void) {
 pthread_mutex_lock(&lock);
 if(next>= (1<<24)-16){if(noexec)next=0;else{pthread_mutex_unlock(&lock);return;}}
 unsigned char *p=code+next;next+=16;
#ifdef __aarch64__
 pthread_jit_write_protect_np(0);
 uint32_t insns[]={0x52800540,0xd65f03c0};memcpy(p,insns,sizeof insns);
 sys_icache_invalidate(p,16);pthread_jit_write_protect_np(1);
#else
 unsigned char insns[]={0xb8,42,0,0,0,0xc3};memcpy(p,insns,sizeof insns);
#endif
 pthread_mutex_unlock(&lock);
 if(!noexec&&((int(*)(void))p)()!=42)abort();
}
static void *worker(void *a){while(!__atomic_load_n(&stop,__ATOMIC_RELAXED))one();return 0;}
int main(int argc,char**argv){
 noexec=argc>1&&!strcmp(argv[1],"noexec");
 code=mmap(0,1<<24,7,MAP_ANON|MAP_PRIVATE|MAP_JIT,-1,0);if(code==MAP_FAILED)return 2;
 pthread_t ts[2];for(int k=0;k<2;k++)pthread_create(ts+k,0,worker,0);
 for(int f=0;f<300;f++){pid_t p=fork();if(!p){lock=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;one();_exit(0);}int s;if(p<0||waitpid(p,&s,0)!=p||s)return 3;}
 __atomic_store_n(&stop,1,__ATOMIC_RELAXED);for(int k=0;k<2;k++)pthread_join(ts[k],0);puts("PASS");return 0;
}
