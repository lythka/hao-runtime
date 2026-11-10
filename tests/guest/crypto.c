/*
 * The A64 cryptographic extension as Apple silicon implements it:
 * FEAT_AES (AESE, AESD, AESMC, AESIMC), FEAT_PMULL (PMULL/PMULL2 .1q),
 * FEAT_SHA1 (SHA1C/P/M/H/SU0/SU1), FEAT_SHA256 (SHA256H/H2/SU0/SU1),
 * FEAT_SHA512 (SHA512H/H2/SU0/SU1) and FEAT_SHA3 (EOR3, BCAX, RAX1, XAR).
 * Each instruction runs on fixed pseudo-random vectors (also with the
 * destination aliasing a source), then in known-answer computations: AES-128
 * (FIPS-197 C.1) both ways, SHA-1, SHA-256 and SHA-512 of "abc" (FIPS 180-4
 * examples) and Keccak-f[1600] of the zero state (lane 0 of one state, a
 * patterned state in lane 1). Output is hex, compared with the
 * native arm64 build by tests/guest/crypto.sh.
 *   crypto [ROUNDS]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arm_neon.h>

__asm__(".arch_extension aes\n.arch_extension sha2\n.arch_extension sha3\n");

typedef uint8x16_t v16;

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t xs(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static v16 rv(void)
{
    uint64_t w[2] = {xs(), xs()};
    v16 v;
    memcpy(&v, w, 16);
    return v;
}
static void pv(const char *what, v16 v)
{
    uint64_t w[2];
    memcpy(w, &v, 16);
    printf("%-10s %016llx%016llx\n", what, (unsigned long long)w[1], (unsigned long long)w[0]);
}

/* d is read and written; n, m (and a) are read */
#define OP2D(name, asmstr) static v16 name(v16 d, v16 n) { __asm__(asmstr : "+w"(d) : "w"(n)); return d; }
#define OP3D(name, asmstr) static v16 name(v16 d, v16 n, v16 m) { __asm__(asmstr : "+w"(d) : "w"(n), "w"(m)); return d; }
#define OP2(name, asmstr) static v16 name(v16 n) { v16 d; __asm__(asmstr : "=w"(d) : "w"(n)); return d; }
#define OP3(name, asmstr) static v16 name(v16 n, v16 m) { v16 d; __asm__(asmstr : "=w"(d) : "w"(n), "w"(m)); return d; }
#define OP4(name, asmstr) static v16 name(v16 n, v16 m, v16 a) { v16 d; __asm__(asmstr : "=w"(d) : "w"(n), "w"(m), "w"(a)); return d; }

OP2D(aese, "aese %0.16b, %1.16b")
OP2D(aesd, "aesd %0.16b, %1.16b")
OP2(aesmc, "aesmc %0.16b, %1.16b")
OP2(aesimc, "aesimc %0.16b, %1.16b")
OP3(pmull1q, "pmull %0.1q, %1.1d, %2.1d")
OP3(pmull2q, "pmull2 %0.1q, %1.2d, %2.2d")
OP3D(sha1c, "sha1c %q0, %s1, %2.4s")
OP3D(sha1p, "sha1p %q0, %s1, %2.4s")
OP3D(sha1m, "sha1m %q0, %s1, %2.4s")
OP2(sha1h, "sha1h %s0, %s1")
OP3D(sha1su0, "sha1su0 %0.4s, %1.4s, %2.4s")
OP2D(sha1su1, "sha1su1 %0.4s, %1.4s")
OP3D(sha256h, "sha256h %q0, %q1, %2.4s")
OP3D(sha256h2, "sha256h2 %q0, %q1, %2.4s")
OP2D(sha256su0, "sha256su0 %0.4s, %1.4s")
OP3D(sha256su1, "sha256su1 %0.4s, %1.4s, %2.4s")
OP3D(sha512h, "sha512h %q0, %q1, %2.2d")
OP3D(sha512h2, "sha512h2 %q0, %q1, %2.2d")
OP2D(sha512su0, "sha512su0 %0.2d, %1.2d")
OP3D(sha512su1, "sha512su1 %0.2d, %1.2d, %2.2d")
OP4(eor3, "eor3 %0.16b, %1.16b, %2.16b, %3.16b")
OP4(bcax, "bcax %0.16b, %1.16b, %2.16b, %3.16b")
OP3(rax1, "rax1 %0.2d, %1.2d, %2.2d")
OP3(xar0, "xar %0.2d, %1.2d, %2.2d, #0")
OP3(xar1, "xar %0.2d, %1.2d, %2.2d, #1")
OP3(xar13, "xar %0.2d, %1.2d, %2.2d, #13")
OP3(xar63, "xar %0.2d, %1.2d, %2.2d, #63")

/* Destination aliasing a source (the result must use the old values). */
static v16 eor3_alias(v16 n, v16 m, v16 a) { __asm__("eor3 %0.16b, %0.16b, %1.16b, %0.16b" : "+w"(n) : "w"(m)); (void)a; return n; }
static v16 bcax_alias(v16 n, v16 m, v16 a) { __asm__("bcax %0.16b, %1.16b, %0.16b, %2.16b" : "+w"(m) : "w"(n), "w"(a)); return m; }
static v16 sha256h_alias(v16 d, v16 n) { __asm__("sha256h %q0, %q0, %1.4s" : "+w"(d) : "w"(n)); return d; }
static v16 sha512h_alias(v16 d, v16 n) { __asm__("sha512h %q0, %q1, %0.2d" : "+w"(d) : "w"(n)); return d; }
static v16 aese_alias(v16 d) { __asm__("aese %0.16b, %0.16b" : "+w"(d)); return d; }
static v16 aesd_alias(v16 d) { __asm__("aesd %0.16b, %0.16b" : "+w"(d)); return d; }
static v16 aesmc_alias(v16 d) { __asm__("aesmc %0.16b, %0.16b" : "+w"(d)); return d; }
static v16 aesimc_alias(v16 d) { __asm__("aesimc %0.16b, %0.16b" : "+w"(d)); return d; }

/* EOR3 between a flag producer and its consumer: translated natively,
 * it must leave NZCV alone. */
static int eor3_flags(uint64_t x, uint64_t y, v16 *n, v16 m, v16 a)
{
    int r;
    __asm__("cmp %x2, %x3\n"
            "eor3 %1.16b, %1.16b, %4.16b, %5.16b\n"
            "bcax %1.16b, %1.16b, %5.16b, %4.16b\n"
            "cset %w0, lo"
            : "=r"(r), "+w"(*n) : "r"(x), "r"(y), "w"(m), "w"(a) : "cc");
    return r;
}

static void each(int rounds)
{
    for (int k = 0; k < rounds; k++) {
        v16 a = rv(), b = rv(), c = rv();
        printf("-- vectors %d\n", k);
        pv("a", a); pv("b", b); pv("c", c);
        pv("aese", aese(a, b)); pv("aesd", aesd(a, b)); pv("aesmc", aesmc(a)); pv("aesimc", aesimc(a));
        pv("pmull", pmull1q(a, b)); pv("pmull2", pmull2q(a, b));
        pv("sha1c", sha1c(a, b, c)); pv("sha1p", sha1p(a, b, c)); pv("sha1m", sha1m(a, b, c));
        pv("sha1h", sha1h(a)); pv("sha1su0", sha1su0(a, b, c)); pv("sha1su1", sha1su1(a, b));
        pv("sha256h", sha256h(a, b, c)); pv("sha256h2", sha256h2(a, b, c));
        pv("sha256su0", sha256su0(a, b)); pv("sha256su1", sha256su1(a, b, c));
        pv("sha512h", sha512h(a, b, c)); pv("sha512h2", sha512h2(a, b, c));
        pv("sha512su0", sha512su0(a, b)); pv("sha512su1", sha512su1(a, b, c));
        pv("eor3", eor3(a, b, c)); pv("bcax", bcax(a, b, c)); pv("rax1", rax1(a, b));
        pv("xar#0", xar0(a, b)); pv("xar#1", xar1(a, b)); pv("xar#13", xar13(a, b)); pv("xar#63", xar63(a, b));
        pv("eor3=", eor3_alias(a, b, c)); pv("bcax=", bcax_alias(a, b, c));
        pv("sha256h=", sha256h_alias(a, b)); pv("sha512h=", sha512h_alias(a, b)); pv("aese=", aese_alias(a));
        pv("aesd=", aesd_alias(a)); pv("aesmc=", aesmc_alias(a)); pv("aesimc=", aesimc_alias(a));
        uint64_t x = xs(), y = xs();
        v16 t = a;
        int lo = eor3_flags(x, y, &t, b, c);
        printf("eor3-flags %d (want %d)\n", lo, x < y);
        pv("eor3-flags", t);
    }
}

/* ---- known answers */
static v16 load(const uint8_t *p) { return vld1q_u8(p); }

static void aes128(void)
{
    /* FIPS-197 appendix C.1 */
    static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t want[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    uint32_t w[44];
    memcpy(w, key, 16);
    for (int k = 4; k < 44; k++) {
        uint32_t t = w[k - 1];
        if (k % 4 == 0) {
            /* SubWord(RotWord(t)) with AESE on a zero key: ShiftRows moves
             * bytes but the word duplicated into all columns comes through */
            uint32x4_t v = vdupq_n_u32(t);
            v16 s = aese(vreinterpretq_u8_u32(v), vdupq_n_u8(0));
            t = vgetq_lane_u32(vreinterpretq_u32_u8(s), 0);
            t = (t >> 8 | t << 24) ^ rcon[k / 4 - 1];
        }
        w[k] = w[k - 4] ^ t;
    }
    v16 st = load(pt);
    for (int r = 0; r < 9; r++) st = aesmc(aese(st, load((const uint8_t *)&w[4 * r])));
    st = aese(st, load((const uint8_t *)&w[36]));
    st = veorq_u8(st, load((const uint8_t *)&w[40]));
    uint8_t out[16];
    vst1q_u8(out, st);
    pv("aes128", st);
    /* and back with AESD/AESIMC (equivalent inverse cipher, FIPS-197 5.3.5) */
    v16 dk[11];
    for (int r = 0; r <= 10; r++) dk[r] = load((const uint8_t *)&w[4 * r]);
    v16 p = veorq_u8(st, dk[10]);
    for (int r = 9; r >= 1; r--) p = aesimc(aesd(p, vdupq_n_u8(0))), p = veorq_u8(p, aesimc(dk[r]));
    p = veorq_u8(aesd(p, vdupq_n_u8(0)), dk[0]);
    uint8_t back[16];
    vst1q_u8(back, p);
    printf("aes128 FIPS-197 C.1 encrypt %s, decrypt %s\n", memcmp(out, want, 16) ? "WRONG" : "ok", memcmp(back, pt, 16) ? "WRONG" : "ok");
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static void pad_abc(uint8_t *blk, int len)
{
    memset(blk, 0, (size_t)len);
    memcpy(blk, "abc", 3);
    blk[3] = 0x80;
    blk[len - 1] = 24;
}

static void sha1_abc(void)
{
    uint8_t blk[64];
    pad_abc(blk, 64);
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint32_t wv[16];
    for (int k = 0; k < 16; k++) wv[k] = be32(blk + 4 * k);
    uint32x4_t w[4], abcd = vld1q_u32(h);
    for (int k = 0; k < 4; k++) w[k] = vld1q_u32(wv + 4 * k);
    uint32_t e = h[4];
    static const uint32_t K[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
    for (int r = 0; r < 20; r++) {
        uint32x4_t wk = vaddq_u32(w[r % 4], vdupq_n_u32(K[r / 5]));
        v16 a = vreinterpretq_u8_u32(abcd);
        uint32_t e1 = vgetq_lane_u32(vreinterpretq_u32_u8(sha1h(a)), 0);
        v16 ev = vreinterpretq_u8_u32(vdupq_n_u32(e)), wkv = vreinterpretq_u8_u32(wk);
        if (r < 5) a = sha1c(a, ev, wkv);
        else if (r < 10 || r >= 15) a = sha1p(a, ev, wkv);
        else a = sha1m(a, ev, wkv);
        abcd = vreinterpretq_u32_u8(a);
        e = e1;
        /* message schedule for 4 rounds ahead */
        v16 n = sha1su0(vreinterpretq_u8_u32(w[r % 4]), vreinterpretq_u8_u32(w[(r + 1) % 4]), vreinterpretq_u8_u32(w[(r + 2) % 4]));
        w[r % 4] = vreinterpretq_u32_u8(sha1su1(n, vreinterpretq_u8_u32(w[(r + 3) % 4])));
    }
    uint32_t out[5];
    vst1q_u32(out, vaddq_u32(abcd, vld1q_u32(h)));
    out[4] = e + h[4];
    static const uint32_t want[5] = {0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d};
    printf("sha1(abc)   %08x%08x%08x%08x%08x %s\n", out[0], out[1], out[2], out[3], out[4], memcmp(out, want, 20) ? "WRONG" : "ok");
}

static void sha256_abc(void)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t blk[64];
    pad_abc(blk, 64);
    uint32_t wv[16];
    for (int k = 0; k < 16; k++) wv[k] = be32(blk + 4 * k);
    v16 w[4], s0 = vreinterpretq_u8_u32(vld1q_u32(h)), s1 = vreinterpretq_u8_u32(vld1q_u32(h + 4));
    for (int k = 0; k < 4; k++) w[k] = vreinterpretq_u8_u32(vld1q_u32(wv + 4 * k));
    for (int r = 0; r < 16; r++) {
        v16 wk = vreinterpretq_u8_u32(vaddq_u32(vreinterpretq_u32_u8(w[r % 4]), vld1q_u32(K + 4 * r)));
        v16 t = s0;
        s0 = sha256h(s0, s1, wk);
        s1 = sha256h2(s1, t, wk);
        w[r % 4] = sha256su1(sha256su0(w[r % 4], w[(r + 1) % 4]), w[(r + 2) % 4], w[(r + 3) % 4]);
    }
    uint32_t out[8];
    vst1q_u32(out, vaddq_u32(vreinterpretq_u32_u8(s0), vld1q_u32(h)));
    vst1q_u32(out + 4, vaddq_u32(vreinterpretq_u32_u8(s1), vld1q_u32(h + 4)));
    static const uint32_t want[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223, 0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
    printf("sha256(abc) ");
    for (int k = 0; k < 8; k++) printf("%08x", out[k]);
    printf(" %s\n", memcmp(out, want, 32) ? "WRONG" : "ok");
}

static v16 u64v(uint64_t lo, uint64_t hi) { uint64_t w[2] = {lo, hi}; v16 v; memcpy(&v, w, 16); return v; }
static uint64_t lane(v16 v, int k) { uint64_t w[2]; memcpy(w, &v, 16); return w[k]; }
static v16 ext8(v16 a, v16 b) { return vextq_u8(a, b, 8); }   /* lane 0 = a's lane 1, lane 1 = b's lane 0 */

static void sha512_abc(void)
{
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
        0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
        0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
        0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
        0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
        0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
        0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
        0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
        0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
        0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
        0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
        0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
        0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
    static const uint64_t H[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                  0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    uint8_t blk[128];
    pad_abc(blk, 128);
    v16 w[8];
    for (int k = 0; k < 8; k++) w[k] = u64v(be64(blk + 16 * k), be64(blk + 16 * k + 8));
    /* state pairs with the first-named word in lane 0: (a,b) (c,d) (e,f) (g,h).
     * Two rounds per step: SHA512H gives T1 of both rounds from
     * (g,h) + K + W (lanes swapped), (f,g) and (d,e); SHA512H2 the new a's
     * from those, (c,d) and (a,b). */
    v16 ab = u64v(H[0], H[1]), cd = u64v(H[2], H[3]), ef = u64v(H[4], H[5]), gh = u64v(H[6], H[7]);
    for (int r = 0; r < 40; r++) {
        v16 wk = u64v(lane(w[r % 8], 0) + K[2 * r], lane(w[r % 8], 1) + K[2 * r + 1]);
        wk = ext8(wk, wk);
        v16 hk = u64v(lane(gh, 0) + lane(wk, 0), lane(gh, 1) + lane(wk, 1));
        v16 t1 = sha512h(hk, ext8(ef, gh), ext8(cd, ef));
        v16 nef = u64v(lane(cd, 0) + lane(t1, 0), lane(cd, 1) + lane(t1, 1));
        v16 nab = sha512h2(t1, cd, ab);
        gh = ef; ef = nef; cd = ab; ab = nab;
        /* W[t+16], W[t+17] from W[t..t+1], W[t+2], W[t+9..t+10], W[t+14..t+15] */
        w[r % 8] = sha512su1(sha512su0(w[r % 8], w[(r + 1) % 8]), w[(r + 7) % 8], ext8(w[(r + 4) % 8], w[(r + 5) % 8]));
    }
    v16 st[4] = {ab, cd, ef, gh};
    uint64_t out[8];
    for (int k = 0; k < 4; k++) { out[2 * k] = lane(st[k], 0) + H[2 * k]; out[2 * k + 1] = lane(st[k], 1) + H[2 * k + 1]; }
    static const uint64_t want[8] = {0xddaf35a193617abaull, 0xcc417349ae204131ull, 0x12e6fa4e89a97ea2ull, 0x0a9eeee64b55d39aull,
                                     0x2192992a274fc1a8ull, 0x36ba3c23a3feebbdull, 0x454d4423643ce80eull, 0x2a9ac94fa54ca49full};
    printf("sha512(abc) ");
    for (int k = 0; k < 8; k++) printf("%016llx", (unsigned long long)out[k]);
    printf(" %s\n", memcmp(out, want, sizeof want) ? "WRONG" : "ok");
}

static void keccak(void)
{
    /* Keccak-f[1600] on lanes held two per register (same lane of two
     * independent states), theta/rho/pi/chi/iota with EOR3, RAX1, XAR, BCAX */
    static const uint64_t RC[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull, 0x000000000000808bull,
        0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull, 0x000000000000008aull, 0x0000000000000088ull,
        0x0000000080008009ull, 0x000000008000000aull, 0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull,
        0x8000000000008003ull, 0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull};
    static const int rho[25] = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};
    v16 A[25], B[25], C[5], D[5];
    for (int k = 0; k < 25; k++) A[k] = u64v(0, (uint64_t)k * 0x0101010101010101ull);
    for (int r = 0; r < 24; r++) {
        for (int x = 0; x < 5; x++) C[x] = eor3(eor3(A[x], A[x + 5], A[x + 10]), A[x + 15], A[x + 20]);
        for (int x = 0; x < 5; x++) D[x] = rax1(C[(x + 4) % 5], C[(x + 1) % 5]);
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++) {
                int s = rho[x + 5 * y], dst = y + 5 * ((2 * x + 3 * y) % 5);
                v16 v;
                /* XAR rotates right: ROL(a ^ d, s) = XAR(a, d, 64 - s) */
                switch ((64 - s) & 63) {
#define XC(n) case n: __asm__("xar %0.2d, %1.2d, %2.2d, #" #n : "=w"(v) : "w"(A[x + 5 * y]), "w"(D[x])); break;
                XC(0) XC(2) XC(3) XC(8) XC(9) XC(19) XC(20) XC(21) XC(23) XC(25) XC(28) XC(36) XC(37)
                XC(39) XC(43) XC(44) XC(46) XC(49) XC(50) XC(54) XC(56) XC(58) XC(61) XC(62) XC(63)
#undef XC
                default: fprintf(stderr, "rotation %d\n", s); exit(2);
                }
                B[dst] = v;
            }
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++) A[x + 5 * y] = bcax(B[x + 5 * y], B[(x + 2) % 5 + 5 * y], B[(x + 1) % 5 + 5 * y]);
        A[0] = veorq_u8(A[0], u64v(RC[r], RC[r]));
    }
    /* lane 0 of Keccak-f[1600] applied to the all-zero state (the Keccak
     * team's KeccakF-1600 intermediate values) */
    printf("keccak-f1600(0) lane0 %016llx %s\n", (unsigned long long)lane(A[0], 0), lane(A[0], 0) == 0xf1258f7940e1dde7ull ? "ok" : "WRONG");
    for (int k = 0; k < 25; k++) {
        char nm[16];
        snprintf(nm, sizeof nm, "keccak[%d]", k);
        pv(nm, A[k]);
    }
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 16;
    each(rounds);
    aes128();
    sha1_abc();
    sha256_abc();
    sha512_abc();
    keccak();
    return 0;
}
