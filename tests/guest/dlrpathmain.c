#include <mach-o/dyld.h>
#include <limits.h>
#include <string.h>

extern int leaf_value(void);

int main(int argc, char **argv)
{
    char path[PATH_MAX];
    uint32_t n = sizeof path;
    return argc != 2 || _NSGetExecutablePath(path, &n) || strcmp(path, argv[1]) || leaf_value() != 11;
}
