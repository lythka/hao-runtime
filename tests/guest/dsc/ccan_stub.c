/* Link-time stand-in for /usr/lib/rdma/libccan.dylib, which exists only in
 * the arm64e shared cache (no file, no SDK stub). Never installed: at run
 * time dyld (native) and hao both take the cache's library. */
#include <stddef.h>
#include <stdint.h>
struct list_node { struct list_node *next, *prev; };
struct list_head { struct list_node n; };
int ilog32(uint32_t v) { (void)v; return -1; }
int ilog64(uint64_t v) { (void)v; return -1; }
size_t strcount(const char *h, const char *n) { (void)h; (void)n; return 0; }
struct list_head *list_check(const struct list_head *h, const char *abortstr) { (void)abortstr; return (struct list_head *)h; }
