#include <stdbool.h>
#include <stdio.h>
bool rt_alias_bool(int);
int main(void)
{
    const int values[] = {0, 1, -1, 127, 128, 255, 256, 32767, 32768, 65535};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++)
        printf("%d: alias bool %d\n", values[i], (int)rt_alias_bool(values[i]));
    return 0;
}
