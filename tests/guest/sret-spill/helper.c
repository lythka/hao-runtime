/* ARM HFA return; x86 counterpart deliberately spills its hidden result pointer. */
typedef struct { double a, b, c, d; } Bounds;
Bounds spill_bounds(const double *p) { return (Bounds){p[0], p[1], p[2], p[3]}; }
