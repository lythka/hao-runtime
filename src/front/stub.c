/*
 * hao-stub: the x86_64 slice added to an arm64e-only program. The kernel
 * picks this slice on Intel; it runs the same file's arm64e slice through
 * libhao with native x86_64 libraries. Signed per program with that
 * program's identifier and entitlements (tools/hao-embed).
 */
int hao_self_main(int argc, char **argv, char **envp, char **apple);
int main(int argc, char **argv, char **envp, char **apple) { return hao_self_main(argc, argv, envp, apple); }

/* The format libhao_identity sends a guest's composed os_log text with (src/oslog.c). It must be in the
 * main executable's __cstring: logd resolves a format from the uuidtext of the image the record names. */
__attribute__((used, visibility("default"))) const char *const hao_oslog_format = "%{public}s";
