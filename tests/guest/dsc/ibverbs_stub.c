/* Link-time stand-in for /usr/lib/rdma/libibverbs.dylib (arm64e shared cache
 * only; it depends on libccan, also cache-only). */
const char *ibv_wc_status_str(int status) { (void)status; return 0; }
const char *ibv_node_type_str(int type) { (void)type; return 0; }
const char *ibv_port_state_str(int state) { (void)state; return 0; }
