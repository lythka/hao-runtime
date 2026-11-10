#include <stdbool.h>
#include <stdio.h>
bool rt_bool(int);
signed char rt_char(int);
unsigned char rt_uchar(int);
short rt_short(int);
unsigned short rt_ushort(int);
bool rt_callback(int (*)(int), int);
bool rt_wrap(int);
__attribute__((noinline)) static int twice(int x) { return x * 2; }
int main(void)
{
    const int values[] = {0, 1, -1, 127, 128, 255, 256, 32767, 32768, 65535};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
        int v = values[i];
        printf("%d: %d %d %u %d %u callback=%d wrap=%d\n", v, (int)rt_bool(v), (int)rt_char(v),
               (unsigned)rt_uchar(v), (int)rt_short(v), (unsigned)rt_ushort(v), (int)rt_callback(twice, v), (int)rt_wrap(v));
    }
    return 0;
}
