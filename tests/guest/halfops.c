/* FP16 scalar/vector decoder coverage and native full-FPSR comparison. */
#include <stdint.h>
#include <stdio.h>
typedef uint64_t vec __attribute__((vector_size(16)));
static uint64_t hash=0xcbf29ce484222325ULL;
static void mix(uint64_t x) { hash=(hash^x)*0x100000001b3ULL; }
#define CASE(name, instruction) \
static void name(vec a, vec b, uint64_t cr, uint64_t sr) { \
    vec out; uint64_t fpsr; \
    __asm__ volatile("msr fpcr, %[cr]\n\tmsr fpsr, %[sr]\n\tldr q0, [%[a]]\n\tldr q1, [%[b]]\n\t" \
        instruction "\n\tstr q0, [%[out]]\n\tmrs %[fpsr], fpsr\n\tmsr fpcr, xzr" \
        : [fpsr] "=r"(fpsr) : [a] "r"(&a), [b] "r"(&b), [out] "r"(&out), [cr] "r"(cr), [sr] "r"(sr) \
        : "v0", "v1", "memory"); \
    mix(out[0]); mix(out[1]); mix(fpsr); \
}
CASE(c0, "fmulx v0.4h, v0.4h, v1.4h")
CASE(c1, "fmulx v0.4h, v1.4h, v0.4h")
CASE(c2, "fmulx v0.8h, v0.8h, v1.8h")
CASE(c3, "fmulx v0.8h, v1.8h, v0.8h")
CASE(c4, "fmulx h0, h0, h1")
CASE(c5, "fmulx h0, h1, h0")
CASE(c6, "fcmeq v0.4h, v0.4h, v1.4h")
CASE(c7, "fcmeq v0.4h, v1.4h, v0.4h")
CASE(c8, "fcmeq v0.8h, v0.8h, v1.8h")
CASE(c9, "fcmeq v0.8h, v1.8h, v0.8h")
CASE(c10, "fcmeq h0, h0, h1")
CASE(c11, "fcmeq h0, h1, h0")
CASE(c12, "fcmge v0.4h, v0.4h, v1.4h")
CASE(c13, "fcmge v0.4h, v1.4h, v0.4h")
CASE(c14, "fcmge v0.8h, v0.8h, v1.8h")
CASE(c15, "fcmge v0.8h, v1.8h, v0.8h")
CASE(c16, "fcmge h0, h0, h1")
CASE(c17, "fcmge h0, h1, h0")
CASE(c18, "fcmgt v0.4h, v0.4h, v1.4h")
CASE(c19, "fcmgt v0.4h, v1.4h, v0.4h")
CASE(c20, "fcmgt v0.8h, v0.8h, v1.8h")
CASE(c21, "fcmgt v0.8h, v1.8h, v0.8h")
CASE(c22, "fcmgt h0, h0, h1")
CASE(c23, "fcmgt h0, h1, h0")
CASE(c24, "facge v0.4h, v0.4h, v1.4h")
CASE(c25, "facge v0.4h, v1.4h, v0.4h")
CASE(c26, "facge v0.8h, v0.8h, v1.8h")
CASE(c27, "facge v0.8h, v1.8h, v0.8h")
CASE(c28, "facge h0, h0, h1")
CASE(c29, "facge h0, h1, h0")
CASE(c30, "facgt v0.4h, v0.4h, v1.4h")
CASE(c31, "facgt v0.4h, v1.4h, v0.4h")
CASE(c32, "facgt v0.8h, v0.8h, v1.8h")
CASE(c33, "facgt v0.8h, v1.8h, v0.8h")
CASE(c34, "facgt h0, h0, h1")
CASE(c35, "facgt h0, h1, h0")
CASE(c36, "frecps v0.4h, v0.4h, v1.4h")
CASE(c37, "frecps v0.4h, v1.4h, v0.4h")
CASE(c38, "frecps v0.8h, v0.8h, v1.8h")
CASE(c39, "frecps v0.8h, v1.8h, v0.8h")
CASE(c40, "frecps h0, h0, h1")
CASE(c41, "frecps h0, h1, h0")
CASE(c42, "frsqrts v0.4h, v0.4h, v1.4h")
CASE(c43, "frsqrts v0.4h, v1.4h, v0.4h")
CASE(c44, "frsqrts v0.8h, v0.8h, v1.8h")
CASE(c45, "frsqrts v0.8h, v1.8h, v0.8h")
CASE(c46, "frsqrts h0, h0, h1")
CASE(c47, "frsqrts h0, h1, h0")
CASE(c48, "fabd v0.4h, v0.4h, v1.4h")
CASE(c49, "fabd v0.4h, v1.4h, v0.4h")
CASE(c50, "fabd v0.8h, v0.8h, v1.8h")
CASE(c51, "fabd v0.8h, v1.8h, v0.8h")
CASE(c52, "fabd h0, h0, h1")
CASE(c53, "fabd h0, h1, h0")
CASE(c54, "fadd v0.4h, v0.4h, v1.4h")
CASE(c55, "fadd v0.4h, v1.4h, v0.4h")
CASE(c56, "fadd v0.8h, v0.8h, v1.8h")
CASE(c57, "fadd v0.8h, v1.8h, v0.8h")
CASE(c58, "fsub v0.4h, v0.4h, v1.4h")
CASE(c59, "fsub v0.4h, v1.4h, v0.4h")
CASE(c60, "fsub v0.8h, v0.8h, v1.8h")
CASE(c61, "fsub v0.8h, v1.8h, v0.8h")
CASE(c62, "fmul v0.4h, v0.4h, v1.4h")
CASE(c63, "fmul v0.4h, v1.4h, v0.4h")
CASE(c64, "fmul v0.8h, v0.8h, v1.8h")
CASE(c65, "fmul v0.8h, v1.8h, v0.8h")
CASE(c66, "fdiv v0.4h, v0.4h, v1.4h")
CASE(c67, "fdiv v0.4h, v1.4h, v0.4h")
CASE(c68, "fdiv v0.8h, v0.8h, v1.8h")
CASE(c69, "fdiv v0.8h, v1.8h, v0.8h")
CASE(c70, "fmax v0.4h, v0.4h, v1.4h")
CASE(c71, "fmax v0.4h, v1.4h, v0.4h")
CASE(c72, "fmax v0.8h, v0.8h, v1.8h")
CASE(c73, "fmax v0.8h, v1.8h, v0.8h")
CASE(c74, "fmin v0.4h, v0.4h, v1.4h")
CASE(c75, "fmin v0.4h, v1.4h, v0.4h")
CASE(c76, "fmin v0.8h, v0.8h, v1.8h")
CASE(c77, "fmin v0.8h, v1.8h, v0.8h")
CASE(c78, "fmaxnm v0.4h, v0.4h, v1.4h")
CASE(c79, "fmaxnm v0.4h, v1.4h, v0.4h")
CASE(c80, "fmaxnm v0.8h, v0.8h, v1.8h")
CASE(c81, "fmaxnm v0.8h, v1.8h, v0.8h")
CASE(c82, "fminnm v0.4h, v0.4h, v1.4h")
CASE(c83, "fminnm v0.4h, v1.4h, v0.4h")
CASE(c84, "fminnm v0.8h, v0.8h, v1.8h")
CASE(c85, "fminnm v0.8h, v1.8h, v0.8h")
CASE(c86, "fmla v0.4h, v0.4h, v1.4h")
CASE(c87, "fmla v0.4h, v1.4h, v0.4h")
CASE(c88, "fmla v0.8h, v0.8h, v1.8h")
CASE(c89, "fmla v0.8h, v1.8h, v0.8h")
CASE(c90, "fmls v0.4h, v0.4h, v1.4h")
CASE(c91, "fmls v0.4h, v1.4h, v0.4h")
CASE(c92, "fmls v0.8h, v0.8h, v1.8h")
CASE(c93, "fmls v0.8h, v1.8h, v0.8h")
CASE(c94, "faddp v0.4h, v0.4h, v1.4h")
CASE(c95, "faddp v0.4h, v1.4h, v0.4h")
CASE(c96, "faddp v0.8h, v0.8h, v1.8h")
CASE(c97, "faddp v0.8h, v1.8h, v0.8h")
CASE(c98, "fmaxp v0.4h, v0.4h, v1.4h")
CASE(c99, "fmaxp v0.4h, v1.4h, v0.4h")
CASE(c100, "fmaxp v0.8h, v0.8h, v1.8h")
CASE(c101, "fmaxp v0.8h, v1.8h, v0.8h")
CASE(c102, "fminp v0.4h, v0.4h, v1.4h")
CASE(c103, "fminp v0.4h, v1.4h, v0.4h")
CASE(c104, "fminp v0.8h, v0.8h, v1.8h")
CASE(c105, "fminp v0.8h, v1.8h, v0.8h")
CASE(c106, "fmaxnmp v0.4h, v0.4h, v1.4h")
CASE(c107, "fmaxnmp v0.4h, v1.4h, v0.4h")
CASE(c108, "fmaxnmp v0.8h, v0.8h, v1.8h")
CASE(c109, "fmaxnmp v0.8h, v1.8h, v0.8h")
CASE(c110, "fminnmp v0.4h, v0.4h, v1.4h")
CASE(c111, "fminnmp v0.4h, v1.4h, v0.4h")
CASE(c112, "fminnmp v0.8h, v0.8h, v1.8h")
CASE(c113, "fminnmp v0.8h, v1.8h, v0.8h")
CASE(c114, "fcvtns v0.4h, v0.4h")
CASE(c115, "fcvtns v0.8h, v0.8h")
CASE(c116, "fcvtns h0, h0")
CASE(c117, "fcvtnu v0.4h, v0.4h")
CASE(c118, "fcvtnu v0.8h, v0.8h")
CASE(c119, "fcvtnu h0, h0")
CASE(c120, "fcvtms v0.4h, v0.4h")
CASE(c121, "fcvtms v0.8h, v0.8h")
CASE(c122, "fcvtms h0, h0")
CASE(c123, "fcvtmu v0.4h, v0.4h")
CASE(c124, "fcvtmu v0.8h, v0.8h")
CASE(c125, "fcvtmu h0, h0")
CASE(c126, "fcvtas v0.4h, v0.4h")
CASE(c127, "fcvtas v0.8h, v0.8h")
CASE(c128, "fcvtas h0, h0")
CASE(c129, "fcvtau v0.4h, v0.4h")
CASE(c130, "fcvtau v0.8h, v0.8h")
CASE(c131, "fcvtau h0, h0")
CASE(c132, "fcvtps v0.4h, v0.4h")
CASE(c133, "fcvtps v0.8h, v0.8h")
CASE(c134, "fcvtps h0, h0")
CASE(c135, "fcvtpu v0.4h, v0.4h")
CASE(c136, "fcvtpu v0.8h, v0.8h")
CASE(c137, "fcvtpu h0, h0")
CASE(c138, "fcvtzs v0.4h, v0.4h")
CASE(c139, "fcvtzs v0.8h, v0.8h")
CASE(c140, "fcvtzs h0, h0")
CASE(c141, "fcvtzu v0.4h, v0.4h")
CASE(c142, "fcvtzu v0.8h, v0.8h")
CASE(c143, "fcvtzu h0, h0")
CASE(c144, "scvtf v0.4h, v0.4h")
CASE(c145, "scvtf v0.8h, v0.8h")
CASE(c146, "scvtf h0, h0")
CASE(c147, "ucvtf v0.4h, v0.4h")
CASE(c148, "ucvtf v0.8h, v0.8h")
CASE(c149, "ucvtf h0, h0")
CASE(c150, "frecpe v0.4h, v0.4h")
CASE(c151, "frecpe v0.8h, v0.8h")
CASE(c152, "frecpe h0, h0")
CASE(c153, "frsqrte v0.4h, v0.4h")
CASE(c154, "frsqrte v0.8h, v0.8h")
CASE(c155, "frsqrte h0, h0")
CASE(c156, "fabs v0.4h, v0.4h")
CASE(c157, "fabs v0.8h, v0.8h")
CASE(c158, "fneg v0.4h, v0.4h")
CASE(c159, "fneg v0.8h, v0.8h")
CASE(c160, "fsqrt v0.4h, v0.4h")
CASE(c161, "fsqrt v0.8h, v0.8h")
CASE(c162, "frintn v0.4h, v0.4h")
CASE(c163, "frintn v0.8h, v0.8h")
CASE(c164, "frintm v0.4h, v0.4h")
CASE(c165, "frintm v0.8h, v0.8h")
CASE(c166, "frintp v0.4h, v0.4h")
CASE(c167, "frintp v0.8h, v0.8h")
CASE(c168, "frintz v0.4h, v0.4h")
CASE(c169, "frintz v0.8h, v0.8h")
CASE(c170, "frinta v0.4h, v0.4h")
CASE(c171, "frinta v0.8h, v0.8h")
CASE(c172, "frintx v0.4h, v0.4h")
CASE(c173, "frintx v0.8h, v0.8h")
CASE(c174, "frinti v0.4h, v0.4h")
CASE(c175, "frinti v0.8h, v0.8h")
CASE(c176, "fmul v0.4h, v0.4h, v1.h[0]")
CASE(c177, "fmul v0.4h, v0.4h, v1.h[1]")
CASE(c178, "fmul v0.4h, v0.4h, v1.h[2]")
CASE(c179, "fmul v0.4h, v0.4h, v1.h[3]")
CASE(c180, "fmul v0.4h, v0.4h, v1.h[4]")
CASE(c181, "fmul v0.4h, v0.4h, v1.h[5]")
CASE(c182, "fmul v0.4h, v0.4h, v1.h[6]")
CASE(c183, "fmul v0.4h, v0.4h, v1.h[7]")
CASE(c184, "fmul v0.8h, v0.8h, v1.h[0]")
CASE(c185, "fmul v0.8h, v0.8h, v1.h[1]")
CASE(c186, "fmul v0.8h, v0.8h, v1.h[2]")
CASE(c187, "fmul v0.8h, v0.8h, v1.h[3]")
CASE(c188, "fmul v0.8h, v0.8h, v1.h[4]")
CASE(c189, "fmul v0.8h, v0.8h, v1.h[5]")
CASE(c190, "fmul v0.8h, v0.8h, v1.h[6]")
CASE(c191, "fmul v0.8h, v0.8h, v1.h[7]")
CASE(c192, "fmul h0, h0, v1.h[0]")
CASE(c193, "fmul h0, h0, v1.h[1]")
CASE(c194, "fmul h0, h0, v1.h[2]")
CASE(c195, "fmul h0, h0, v1.h[3]")
CASE(c196, "fmul h0, h0, v1.h[4]")
CASE(c197, "fmul h0, h0, v1.h[5]")
CASE(c198, "fmul h0, h0, v1.h[6]")
CASE(c199, "fmul h0, h0, v1.h[7]")
CASE(c200, "fmulx v0.4h, v0.4h, v1.h[0]")
CASE(c201, "fmulx v0.4h, v0.4h, v1.h[1]")
CASE(c202, "fmulx v0.4h, v0.4h, v1.h[2]")
CASE(c203, "fmulx v0.4h, v0.4h, v1.h[3]")
CASE(c204, "fmulx v0.4h, v0.4h, v1.h[4]")
CASE(c205, "fmulx v0.4h, v0.4h, v1.h[5]")
CASE(c206, "fmulx v0.4h, v0.4h, v1.h[6]")
CASE(c207, "fmulx v0.4h, v0.4h, v1.h[7]")
CASE(c208, "fmulx v0.8h, v0.8h, v1.h[0]")
CASE(c209, "fmulx v0.8h, v0.8h, v1.h[1]")
CASE(c210, "fmulx v0.8h, v0.8h, v1.h[2]")
CASE(c211, "fmulx v0.8h, v0.8h, v1.h[3]")
CASE(c212, "fmulx v0.8h, v0.8h, v1.h[4]")
CASE(c213, "fmulx v0.8h, v0.8h, v1.h[5]")
CASE(c214, "fmulx v0.8h, v0.8h, v1.h[6]")
CASE(c215, "fmulx v0.8h, v0.8h, v1.h[7]")
CASE(c216, "fmulx h0, h0, v1.h[0]")
CASE(c217, "fmulx h0, h0, v1.h[1]")
CASE(c218, "fmulx h0, h0, v1.h[2]")
CASE(c219, "fmulx h0, h0, v1.h[3]")
CASE(c220, "fmulx h0, h0, v1.h[4]")
CASE(c221, "fmulx h0, h0, v1.h[5]")
CASE(c222, "fmulx h0, h0, v1.h[6]")
CASE(c223, "fmulx h0, h0, v1.h[7]")
CASE(c224, "fmla v0.4h, v0.4h, v1.h[0]")
CASE(c225, "fmla v0.4h, v0.4h, v1.h[1]")
CASE(c226, "fmla v0.4h, v0.4h, v1.h[2]")
CASE(c227, "fmla v0.4h, v0.4h, v1.h[3]")
CASE(c228, "fmla v0.4h, v0.4h, v1.h[4]")
CASE(c229, "fmla v0.4h, v0.4h, v1.h[5]")
CASE(c230, "fmla v0.4h, v0.4h, v1.h[6]")
CASE(c231, "fmla v0.4h, v0.4h, v1.h[7]")
CASE(c232, "fmla v0.8h, v0.8h, v1.h[0]")
CASE(c233, "fmla v0.8h, v0.8h, v1.h[1]")
CASE(c234, "fmla v0.8h, v0.8h, v1.h[2]")
CASE(c235, "fmla v0.8h, v0.8h, v1.h[3]")
CASE(c236, "fmla v0.8h, v0.8h, v1.h[4]")
CASE(c237, "fmla v0.8h, v0.8h, v1.h[5]")
CASE(c238, "fmla v0.8h, v0.8h, v1.h[6]")
CASE(c239, "fmla v0.8h, v0.8h, v1.h[7]")
CASE(c240, "fmla h0, h0, v1.h[0]")
CASE(c241, "fmla h0, h0, v1.h[1]")
CASE(c242, "fmla h0, h0, v1.h[2]")
CASE(c243, "fmla h0, h0, v1.h[3]")
CASE(c244, "fmla h0, h0, v1.h[4]")
CASE(c245, "fmla h0, h0, v1.h[5]")
CASE(c246, "fmla h0, h0, v1.h[6]")
CASE(c247, "fmla h0, h0, v1.h[7]")
CASE(c248, "fmls v0.4h, v0.4h, v1.h[0]")
CASE(c249, "fmls v0.4h, v0.4h, v1.h[1]")
CASE(c250, "fmls v0.4h, v0.4h, v1.h[2]")
CASE(c251, "fmls v0.4h, v0.4h, v1.h[3]")
CASE(c252, "fmls v0.4h, v0.4h, v1.h[4]")
CASE(c253, "fmls v0.4h, v0.4h, v1.h[5]")
CASE(c254, "fmls v0.4h, v0.4h, v1.h[6]")
CASE(c255, "fmls v0.4h, v0.4h, v1.h[7]")
CASE(c256, "fmls v0.8h, v0.8h, v1.h[0]")
CASE(c257, "fmls v0.8h, v0.8h, v1.h[1]")
CASE(c258, "fmls v0.8h, v0.8h, v1.h[2]")
CASE(c259, "fmls v0.8h, v0.8h, v1.h[3]")
CASE(c260, "fmls v0.8h, v0.8h, v1.h[4]")
CASE(c261, "fmls v0.8h, v0.8h, v1.h[5]")
CASE(c262, "fmls v0.8h, v0.8h, v1.h[6]")
CASE(c263, "fmls v0.8h, v0.8h, v1.h[7]")
CASE(c264, "fmls h0, h0, v1.h[0]")
CASE(c265, "fmls h0, h0, v1.h[1]")
CASE(c266, "fmls h0, h0, v1.h[2]")
CASE(c267, "fmls h0, h0, v1.h[3]")
CASE(c268, "fmls h0, h0, v1.h[4]")
CASE(c269, "fmls h0, h0, v1.h[5]")
CASE(c270, "fmls h0, h0, v1.h[6]")
CASE(c271, "fmls h0, h0, v1.h[7]")
CASE(c272, "scvtf v0.4h, v0.4h, #1")
CASE(c273, "scvtf v0.4h, v0.4h, #16")
CASE(c274, "scvtf v0.8h, v0.8h, #1")
CASE(c275, "scvtf v0.8h, v0.8h, #16")
CASE(c276, "scvtf h0, h0, #1")
CASE(c277, "scvtf h0, h0, #16")
CASE(c278, "ucvtf v0.4h, v0.4h, #1")
CASE(c279, "ucvtf v0.4h, v0.4h, #16")
CASE(c280, "ucvtf v0.8h, v0.8h, #1")
CASE(c281, "ucvtf v0.8h, v0.8h, #16")
CASE(c282, "ucvtf h0, h0, #1")
CASE(c283, "ucvtf h0, h0, #16")
CASE(c284, "fcvtzs v0.4h, v0.4h, #1")
CASE(c285, "fcvtzs v0.4h, v0.4h, #16")
CASE(c286, "fcvtzs v0.8h, v0.8h, #1")
CASE(c287, "fcvtzs v0.8h, v0.8h, #16")
CASE(c288, "fcvtzs h0, h0, #1")
CASE(c289, "fcvtzs h0, h0, #16")
CASE(c290, "fcvtzu v0.4h, v0.4h, #1")
CASE(c291, "fcvtzu v0.4h, v0.4h, #16")
CASE(c292, "fcvtzu v0.8h, v0.8h, #1")
CASE(c293, "fcvtzu v0.8h, v0.8h, #16")
CASE(c294, "fcvtzu h0, h0, #1")
CASE(c295, "fcvtzu h0, h0, #16")
CASE(c296, "fcmeq v0.4h, v0.4h, #0.0")
CASE(c297, "fcmeq v0.8h, v0.8h, #0.0")
CASE(c298, "fcmeq h0, h0, #0.0")
CASE(c299, "fcmge v0.4h, v0.4h, #0.0")
CASE(c300, "fcmge v0.8h, v0.8h, #0.0")
CASE(c301, "fcmge h0, h0, #0.0")
CASE(c302, "fcmgt v0.4h, v0.4h, #0.0")
CASE(c303, "fcmgt v0.8h, v0.8h, #0.0")
CASE(c304, "fcmgt h0, h0, #0.0")
CASE(c305, "fcmle v0.4h, v0.4h, #0.0")
CASE(c306, "fcmle v0.8h, v0.8h, #0.0")
CASE(c307, "fcmle h0, h0, #0.0")
CASE(c308, "fcmlt v0.4h, v0.4h, #0.0")
CASE(c309, "fcmlt v0.8h, v0.8h, #0.0")
CASE(c310, "fcmlt h0, h0, #0.0")
CASE(c311, "frecpx h0, h0")
CASE(recpx_s, "frecpx s0, s0")
CASE(recpx_d, "frecpx d0, d0")
static void (*const cases[])(vec,vec,uint64_t,uint64_t)={
recpx_s, recpx_d,
c0,
c1,
c2,
c3,
c4,
c5,
c6,
c7,
c8,
c9,
c10,
c11,
c12,
c13,
c14,
c15,
c16,
c17,
c18,
c19,
c20,
c21,
c22,
c23,
c24,
c25,
c26,
c27,
c28,
c29,
c30,
c31,
c32,
c33,
c34,
c35,
c36,
c37,
c38,
c39,
c40,
c41,
c42,
c43,
c44,
c45,
c46,
c47,
c48,
c49,
c50,
c51,
c52,
c53,
c54,
c55,
c56,
c57,
c58,
c59,
c60,
c61,
c62,
c63,
c64,
c65,
c66,
c67,
c68,
c69,
c70,
c71,
c72,
c73,
c74,
c75,
c76,
c77,
c78,
c79,
c80,
c81,
c82,
c83,
c84,
c85,
c86,
c87,
c88,
c89,
c90,
c91,
c92,
c93,
c94,
c95,
c96,
c97,
c98,
c99,
c100,
c101,
c102,
c103,
c104,
c105,
c106,
c107,
c108,
c109,
c110,
c111,
c112,
c113,
c114,
c115,
c116,
c117,
c118,
c119,
c120,
c121,
c122,
c123,
c124,
c125,
c126,
c127,
c128,
c129,
c130,
c131,
c132,
c133,
c134,
c135,
c136,
c137,
c138,
c139,
c140,
c141,
c142,
c143,
c144,
c145,
c146,
c147,
c148,
c149,
c150,
c151,
c152,
c153,
c154,
c155,
c156,
c157,
c158,
c159,
c160,
c161,
c162,
c163,
c164,
c165,
c166,
c167,
c168,
c169,
c170,
c171,
c172,
c173,
c174,
c175,
c176,
c177,
c178,
c179,
c180,
c181,
c182,
c183,
c184,
c185,
c186,
c187,
c188,
c189,
c190,
c191,
c192,
c193,
c194,
c195,
c196,
c197,
c198,
c199,
c200,
c201,
c202,
c203,
c204,
c205,
c206,
c207,
c208,
c209,
c210,
c211,
c212,
c213,
c214,
c215,
c216,
c217,
c218,
c219,
c220,
c221,
c222,
c223,
c224,
c225,
c226,
c227,
c228,
c229,
c230,
c231,
c232,
c233,
c234,
c235,
c236,
c237,
c238,
c239,
c240,
c241,
c242,
c243,
c244,
c245,
c246,
c247,
c248,
c249,
c250,
c251,
c252,
c253,
c254,
c255,
c256,
c257,
c258,
c259,
c260,
c261,
c262,
c263,
c264,
c265,
c266,
c267,
c268,
c269,
c270,
c271,
c272,
c273,
c274,
c275,
c276,
c277,
c278,
c279,
c280,
c281,
c282,
c283,
c284,
c285,
c286,
c287,
c288,
c289,
c290,
c291,
c292,
c293,
c294,
c295,
c296,
c297,
c298,
c299,
c300,
c301,
c302,
c303,
c304,
c305,
c306,
c307,
c308,
c309,
c310,
c311
};
int main(void) {
    static const uint64_t inputs[]={0,0x800000007c00fc00ULL,0x7d017e017fff7c01ULL,
        0x000103ff04000401ULL,0x3bff3c003c013e00ULL,0x7bfffbff3555b555ULL,
        0x800183ff84008401ULL,0x0001800100028002ULL,0x4000c0004c00cc00ULL};
    for (unsigned c=0;c<sizeof cases/sizeof *cases;c++) {
        for (unsigned cr=0;cr<16;cr++)
            for (unsigned a=0;a<sizeof inputs/sizeof *inputs;a++)
                for (unsigned b=0;b<sizeof inputs/sizeof *inputs;b++) {
                    vec x={inputs[a],~inputs[a]},y={inputs[b],~inputs[b]};
                    uint64_t fpcr=((cr&3)<<22)|((cr&4)<<22)|((cr&8)<<22);
                    cases[c](x,y,fpcr,0);cases[c](x,y,fpcr,0x0800009f);
                }
        printf("%u %016llx\n",c,(unsigned long long)hash);
    }
    return 0;
}
