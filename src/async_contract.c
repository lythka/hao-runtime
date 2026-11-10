/* Generic async publication transport for a separately versioned contract.
 * API names, metadata interpretation and receiver rules live in the provider.
 * Opt-in until release packaging supplies an accepted provider artifact.
 */
#include "hao/runtime.h"
#include "hao/async_contract.h"
#include "hao/opts.h"
#include "hao/aot.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct context { struct context *parent; void *resume; } context;
typedef void __attribute__((swiftasynccall)) resume_fn(
    context * __attribute__((swift_async_context)));
typedef void __attribute__((swiftasynccall)) throwing_fn(
    context * __attribute__((swift_async_context)), void * __attribute__((swift_context)));
typedef struct pending {
    context link;
    context *callee;
    hao_async_rule rule;
    uint64_t diagnostic_id;
    struct pending *next;
} pending;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static const hao_async_provider *provider;
static void *library;
static pending *active;
static uint64_t diagnostic_sequence;
static resume_fn publish;
static throwing_fn publish_throwing;

static void invalid(const char *why)
{
    fprintf(HAO_DIAG,"hao: async contract: %s\n",why);
    abort();
}

/* A release co-installs its selected own-code provider with libhao. Resolve
 * relative to the loaded runtime, never the working directory or an app name.
 * Reuse the cache path's ownership/ACL/no-symlink policy: guarded processes
 * require root-controlled ancestors and a root-owned regular provider file.
 * A present but untrusted package is an error, not an unadapted fallback.
 */
static int packaged_provider(char *path,size_t length,struct stat *identity)
{
    Dl_info info;
    char runtime[PATH_MAX],why[512];
    if (!dladdr((const void *)packaged_provider,&info) || !info.dli_fname ||
        !realpath(info.dli_fname,runtime)) invalid("cannot locate runtime package");
    char *slash=strrchr(runtime,'/');
    if (!slash) invalid("runtime package is not absolute");
    *slash=0;
    hao_aot_dir directory={.kind=hao_env_tier()==HAO_ENV_OPEN ? HAO_AOT_USER : HAO_AOT_SYS_RO};
    if (snprintf(directory.path,sizeof directory.path,"%s",runtime)>=(int)sizeof directory.path ||
        snprintf(path,length,"%s/libhao_async_contract.dylib",runtime)>=(int)length)
        invalid("runtime package path too long");
    struct stat present;
    if (lstat(path,&present)!=0) {
        if (errno==ENOENT) return -1;
        invalid("cannot inspect packaged provider");
    }
    int fd=hao_aot_open_trusted(&directory,"libhao_async_contract.dylib",identity,why,sizeof why);
    if (fd<0) invalid(why[0] ? why : "cannot open packaged provider");
    return fd;
}

static void initialize(void)
{
    char packaged[PATH_MAX];
    struct stat identity;
    int fd=-1;
    const char *path=hao_opt.async_contract_library;
    bool package=!path || !*path;
    if (package) {
        fd=packaged_provider(packaged,sizeof packaged,&identity);
        if (fd<0) return;
        path=packaged;
    }
    library=dlopen(path,RTLD_NOW|RTLD_LOCAL|RTLD_FIRST);
    if (!library) invalid(dlerror());
    if (fd>=0) {
        struct stat loaded;
        if (lstat(path,&loaded)!=0 || loaded.st_dev!=identity.st_dev || loaded.st_ino!=identity.st_ino ||
            loaded.st_size!=identity.st_size || loaded.st_mtimespec.tv_sec!=identity.st_mtimespec.tv_sec ||
            loaded.st_mtimespec.tv_nsec!=identity.st_mtimespec.tv_nsec)
            invalid("packaged provider changed during load");
        close(fd);
    }
    hao_async_provider_getter *get=(hao_async_provider_getter *)dlsym(library,"hao_async_contract_provider_v1");
    if (!get) invalid("missing provider entry");
    provider=get();
    if (!provider || provider->abi!=HAO_ASYNC_CONTRACT_ABI || provider->size!=sizeof *provider ||
        !provider->identifier || !provider->match) invalid("unsupported provider ABI");
    if (hao_opt.verbose || hao_opt.bridge_check) {
        hao_diag_prefix(HAO_DIAG);
        fprintf(HAO_DIAG,"async contract loaded: %s (%s)\n",provider->identifier,package ? "package" : "environment");
    }
}

void hao_async_contract_fork_child(void)
{
    lock=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

static void *consume(context *callee,void *error,bool throwing)
{
    hao_crit_enter(); pthread_mutex_lock(&lock);
    pending **at=&active;
    while (*at && &(*at)->link!=callee->parent) at=&(*at)->next;
    pending *record=*at;
    if (!record || record->callee!=callee || record->rule.throwing!=(uint32_t)throwing ||
        callee->resume!=(throwing ? (void *)publish_throwing : (void *)publish))
        invalid("publication has no matching live invocation");
    *at=record->next;
    void *resume=record->link.resume;
    *callee=record->link;
    uint64_t before=0,after=0;
    if (!error) {
        memcpy(&before,(const void *)(uintptr_t)record->rule.result_address,sizeof before);
        after=before;
        if (before==record->rule.source_word) {
            after=record->rule.receiver_word;
            memcpy((void *)(uintptr_t)record->rule.result_address,&after,sizeof after);
        }
    }
    pthread_mutex_unlock(&lock); hao_crit_leave();
    if (record->diagnostic_id) {
        FILE *log=HAO_DIAG;
        flockfile(log); hao_diag_prefix(log);
        fprintf(log,"async contract end: {\"id\":%llu,\"outcome\":\"%s\"",
            (unsigned long long)record->diagnostic_id,error ? "error" : before==after ? "preserved" : "converted");
        /* An error publication has no initialized result. Do not read it, even
         * for diagnostics, and do not manufacture before/after values. */
        if (!error) fprintf(log,",\"before\":\"0x%llx\",\"after\":\"0x%llx\"",
            (unsigned long long)before,(unsigned long long)after);
        fprintf(log,"}\n"); funlockfile(log);
    }
    free(record);
    return resume;
}

static void __attribute__((swiftasynccall)) publish(
    context * __attribute__((swift_async_context)) callee)
{
    resume_fn *resume=consume(callee,NULL,false);
    __attribute__((musttail)) return resume(callee);
}

static void __attribute__((swiftasynccall)) publish_throwing(
    context * __attribute__((swift_async_context)) callee,void * __attribute__((swift_context)) error)
{
    throwing_fn *resume=consume(callee,error,true);
    __attribute__((musttail)) return resume(callee,error);
}

void hao_async_contract_prepare(hao_cpu *cpu,uint64_t target,const char *symbol)
{
    if (hao_verbose>2) fprintf(HAO_DIAG,"hao: async contract candidate %s ctx=%#llx provider=%s\n",symbol ? symbol : "?",
        (unsigned long long)cpu->x[22],hao_opt.async_contract_library);
    hao_thread *thread=hao_current();
    if (!thread || thread->async_contract_resolving || !cpu->x[22] || (cpu->x[22]&7)) return;
    thread->async_contract_resolving=true;
    pthread_once(&once,initialize);
    if (!provider) { thread->async_contract_resolving=false; return; }
    context *callee=(context *)(uintptr_t)cpu->x[22];
    context original=*callee;
    uint64_t guest=hao_guest_of_entry((uint64_t)(uintptr_t)original.resume);
    if (!guest) { thread->async_contract_resolving=false; return; }
    hao_image *image=hao_image_for_addr(guest);
    if (!image || !image->has_uuid) { thread->async_contract_resolving=false; return; }
    hao_async_call call={.abi=HAO_ASYNC_CONTRACT_ABI,.size=sizeof call,
        .symbol=symbol,.target=target,.swift_self=cpu->x[20],.context=cpu->x[22],
        .resume_entry=(uint64_t)(uintptr_t)original.resume,.resume_guest=guest,.resume_vmaddr=guest-image->slide};
    memcpy(call.arguments,cpu->x,sizeof call.arguments);
    memcpy(call.image_uuid,image->uuid,sizeof call.image_uuid);
    hao_async_rule rule={0};
    hao_cpu saved=*cpu;
    bool matched=provider->match(&call,&rule);
    *cpu=saved;
    thread->async_contract_resolving=false;
    if (!matched) return;
    if (rule.size!=sizeof rule || rule.result_bytes!=8 || !rule.result_address ||
        rule.throwing>1 || rule.reserved || rule.source_word==rule.receiver_word ||
        original.parent!=callee->parent || original.resume!=callee->resume)
        invalid("invalid publication rule");
    pending *record=calloc(1,sizeof *record);
    if (!record) invalid("allocation failed");
    record->link=original; record->callee=callee; record->rule=rule;
    hao_crit_enter(); pthread_mutex_lock(&lock);
    for (pending *p=active;p;p=p->next) if (p->callee==callee) invalid("overlapping context reuse");
    if (hao_opt.bridge_check) record->diagnostic_id=++diagnostic_sequence;
    record->next=active; active=record;
    callee->parent=&record->link;
    callee->resume=rule.throwing ? (void *)publish_throwing : (void *)publish;
    pthread_mutex_unlock(&lock); hao_crit_leave();
    if (record->diagnostic_id) {
        char uuid[33];
        for (unsigned i=0;i<16;i++) snprintf(uuid+2*i,3,"%02x",call.image_uuid[i]);
        FILE *log=HAO_DIAG;
        flockfile(log); hao_diag_prefix(log);
        fprintf(log,"async contract begin: {\"id\":%llu,\"uuid\":\"%s\",\"resume\":\"0x%llx\","
            "\"source\":\"0x%llx\",\"receiver\":\"0x%llx\",\"throwing\":%u}\n",
            (unsigned long long)record->diagnostic_id,uuid,(unsigned long long)call.resume_vmaddr,
            (unsigned long long)rule.source_word,(unsigned long long)rule.receiver_word,rule.throwing);
        funlockfile(log);
    }
}
