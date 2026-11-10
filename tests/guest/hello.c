#include <stdio.h>
int main(int argc, char **argv) {
    printf("hello from arm64e: argc=%d argv0=%s pi=%.5f %s\n", argc, argv[0][0] ? "set" : "empty", 3.14159265, "ok");
    return 7;
}
