#include <stdio.h>
#include <string.h>
typedef struct { double a, b, c, d; } Bounds;
extern Bounds spill_bounds(const double *);
int main(void) {
    const double inputs[][4] = {{1.25, -2.5, 3.75, 4096}, {-0.0, 0.0, -1024, 0.125},
                               {65535, -32768, 0.0625, -0.03125}};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        Bounds b = spill_bounds(inputs[i]);
        if (memcmp(&b, inputs[i], sizeof(b))) return 1;
        printf("%u: %a %a %a %a\n", i, b.a, b.b, b.c, b.d);
    }
    return 0;
}
