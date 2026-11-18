struct x8_l3 { long a, b, c; };
long x8_call_l3(struct x8_l3 (*f)(long), long k);
long x8_call_int(long (*f)(long), long k);
/* x8calls.S: x8s_ in an image with Swift metadata, x8c_ in one without */
long x8s_call_with(long (*f)(void), void *p);
long x8s_call_stale(long (*f)(void), void *p);
long x8c_call_with(long (*f)(void), void *p);
long x8c_call_stale(long (*f)(void), void *p);
