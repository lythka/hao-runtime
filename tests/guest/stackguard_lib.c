/* Native helper for tests/guest/stackguard.sh: functions no interface unit or table types, so hao's
 * bridge scans every argument slot (x0-x7 and the first stack slots) for guest code. They ignore what
 * the slots hold. */
long sg_sink8(long a, long b, long c, long d, long e, long f, long g, long h)
{
    (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h;
    return 42;
}

long sg_sink10(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j)
{
    (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h;
    return i == j ? 1 : 2;
}
