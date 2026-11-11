/* footprint_front: hao's command-line front end (src/front/cli.c) as
 * tests/guest/footprint.sh builds it, with one difference: every guest file
 * is mapped, the way hao takes a file that only root can change. The
 * test's budgets are about system daemons, whose files are such files; its
 * own guests are the user's files, which hao reads into anonymous memory
 * instead (include/hao/macho.h hao_slice), and what that costs the test
 * reports on lines of its own. */
extern int hao_slice_test_path;
int hao_cli_main(int argc, char **argv, char **envp);
__attribute__((used, section("__TEXT,__hao_cli"))) static const char hao_cli_marker = 1;
int main(int argc, char **argv, char **envp)
{
    hao_slice_test_path = 1;
    return hao_cli_main(argc, argv, envp);
}

/* as src/front/cli.c: the format libhao_identity sends a guest's composed os_log text with */
__attribute__((used, visibility("default"))) const char *const hao_oslog_format = "%{public}s";
