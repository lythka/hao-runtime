struct x8_l3 { long a, b, c; };
long x8_call_l3(struct x8_l3 (*f)(long), long k);
long x8_call_int(long (*f)(long), long k);
