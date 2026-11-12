/* Force a second refresh while the first reader holds the name-table pointer.
 * No fork is needed: this is the startup race exercised by test_fork workers. */
#include "hao/bridge.h"
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int paused, released;
static void pause_reader(void)
{
    pthread_mutex_lock(&lock);
    if (!paused) {
        paused = 1;
        pthread_cond_broadcast(&cond);
        while (!released) pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
}
static void *bind(void *arg)
{
    hao_bridge_bind("_hao_labels_test", "_hao_labels_test", (uint64_t)(uintptr_t)arg);
    return NULL;
}
int main(void)
{
    hao_bridge_init();
    void (**hook)(void) = dlsym(RTLD_DEFAULT, "hao_test_labels_reader");
    if (!hook) { fputs("missing TEST_SWITCHES hook\n", stderr); return 2; }
    *hook = pause_reader;
    pthread_t first;
    if (pthread_create(&first, NULL, bind, (void *)0x3000000000ULL)) return 2;
    pthread_mutex_lock(&lock);
    while (!paused) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    bind((void *)0x3000000010ULL);
    pthread_mutex_lock(&lock);
    released = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(first, NULL);
    puts("labels race: PASS");
    return 0;
}
