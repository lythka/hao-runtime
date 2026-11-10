/* Calls into /usr/lib/rdma/libccan.dylib, /usr/lib/libdpfu.dylib and
 * /usr/lib/rdma/libibverbs.dylib (which links libccan), libraries that exist
 * only in the arm64e shared cache (no x86_64 build, no file). */
#include <stdint.h>
#include <stdio.h>
struct list_node { struct list_node *next, *prev; };
struct list_head { struct list_node n; };
int ilog32(uint32_t v);
int ilog64(uint64_t v);
size_t strcount(const char *h, const char *n);
struct list_head *list_check(const struct list_head *h, const char *abortstr);
const char *DPFUGetDeviceModeString(int mode);
unsigned long DPFUSessionGetTypeID(void);
const char *ibv_wc_status_str(int status);
const char *ibv_node_type_str(int type);
const char *ibv_port_state_str(int state);
int main(void)
{
    printf("ilog32(1000)=%d ilog32(1u<<31)=%d ilog64(1ull<<40)=%d\n", ilog32(1000), ilog32(1u << 31), ilog64(1ull << 40));
    printf("strcount=%zu %zu\n", strcount("abcabcab", "ab"), strcount("aaaa", "b"));
    struct list_node a, b;
    struct list_head h;
    h.n.next = &a; a.next = &b; b.next = &h.n;
    h.n.prev = &b; b.prev = &a; a.prev = &h.n;
    printf("list_check=%s\n", list_check(&h, "bad") == &h ? "ok" : "bad");
    int modes[] = {0, 0x71, 0x78, 0x7f, 5};
    for (int k = 0; k < 5; k++) printf("DPFUGetDeviceModeString(%#x)=%s\n", modes[k], DPFUGetDeviceModeString(modes[k]));
    /* registers a CF runtime class whose descriptor lives in the cache image's data */
    unsigned long id = DPFUSessionGetTypeID();
    printf("DPFUSessionGetTypeID: %s, stable %s\n", id ? "nonzero" : "zero", DPFUSessionGetTypeID() == id ? "yes" : "no");
    /* string tables: pointers in the cache image's data, rewritten from the slide info */
    for (int k = 0; k < 3; k++) printf("ibv_wc_status_str(%d)=%s\n", k, ibv_wc_status_str(k));
    printf("ibv_wc_status_str(99)=%s node_type(1)=%s port_state(4)=%s\n", ibv_wc_status_str(99), ibv_node_type_str(1),
           ibv_port_state_str(4));
    return 0;
}
