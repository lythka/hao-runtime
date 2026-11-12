# Runtime issues and measured scope

## JIT tail: integrated coverage and acceptance

Nine instruction groups remove 40,921 of 59,250 previously untranslated
static sites in a large FP/SIMD-heavy arm64 program. Coverage rises from
99.5283% to 99.8541% across the same 12,561,074 instructions; generated
code grows 13,558 KiB (2.1%). This falls short of the approximate 99.94%
target. Double fused arithmetic remains on the interpreter pending a
measured Intel speed advantage. The occupied reference laptop was not used.
Rosetta timing observations are informational, never an acceptance criterion
for retaining these changes.

Each row is cumulative static coverage after that group. Helper counts are
separate ordinary-input workloads, before -> after in both JIT and AOT;
they are not application execution frequencies. Conversion and half rows
use 5,000 iterations, the others 1,000, with 16 test operations per form
per iteration. The FPSR-read form clears FPSR before each read, accounting
for its extra system-register helper call.

| Group | Remaining sites | Native coverage | Code KiB | Helper calls before -> after |
| --- | ---: | ---: | ---: | ---: |
| Base | 59,250 | 99.5283% | 639,290 | — |
| 1. Scalar single fused and reciprocal steps | 47,034 | 99.6256% | 645,955 | 176,000 -> 0 |
| 2. Integer widening, narrowing and misc | 37,049 | 99.7050% | 646,110 | 3,504,000 -> 0 |
| 3. Conversions | 32,360 | 99.7424% | 647,368 | 2,080,000 -> 0 |
| 4. Multiple-structure memory | 27,066 | 99.7845% | 648,653 | 672,000 -> 0 |
| 5. Packed double and vector square roots | 25,673 | 99.7956% | 649,079 | 112,000 -> 0 |
| 6. Half arithmetic and extrema | 24,258 | 99.8069% | 651,281 | 2,960,000 -> 0 |
| 7. Conditional FP comparisons and FP state | 22,608 | 99.8200% | 651,546 | 144,000 -> 0 |
| 9. Reciprocal estimates | 19,701 | 99.8432% | 652,861 | 160,000 -> 0 |
| 10. AES | 18,329 | 99.8541% | 652,848 | 64,000 -> 0 |

The combined ordinary-input workload has 476 forms (364 added here). At
1,000 iterations, native ARM, base JIT/AOT and final JIT/AOT checksums all
agree. Instrumented helper calls fall from 5,840,000 to zero in both
engines; all baseline calls are classified as untranslated. The baseline
descriptor breakdown is three-different 1,344,000; indexed-vector
1,024,000; simd-misc 944,000; shift-immediate and load-store 672,000 each;
other 448,000; fp-scalar 320,000; integer-simd 144,000; fp-vector 112,000;
branch-system 80,000; crypto 64,000; fdiv 16,000. The final workload emits
no helper-group lines. Its JIT code grows 1,431 -> 4,482 KiB and AOT code
1,455 -> 4,506 KiB. These statistics runs contain no timing calls.

Combined complete-state validation passes 934,115 cases with zero
mismatches. Replay of the same five saved 200,000-record corpora used at
base has zero mismatches; IEEE-flag difference counts remain exactly
interpreter 0/0/0/0/0 and JIT 4/4,981/0/0/0 for simd/block/fma/mem/dp.

The final `make check` returns zero: 89 manifest passes, no unexpected
failures, one known failure (`guest/xinhab`) and three expected skips. It
includes `make test`, all five oracle modes and their sanitizer runs,
mutation controls and detection, packaging, cache-profile and guest checks.
The 1,265-line native instruction comparison agrees with JIT, interpreter,
helpers-only and AOT; the encoder check passes 41,847 instructions.
Cumulative groups 1–7 plus 9 also pass their full gate (87 manifest passes).
Earlier isolated attempts encountered expanded-control timeouts and
resource/logging failures; the final run uses the completed test harness
and passes those checks without changing any footprint budget. The
standard footprint test measures Objective-C overhead at 1,771 KiB for JIT
and 1,727 KiB for AOT against 2,048 KiB. Five additional paired baseline/
final samples all pass that budget (JIT medians 1,923/1,903 KiB); the earlier
excess was in allocator and page-table categories, with load causality not
established. Intel timing and application/release-image acceptance remain
separate and unmeasured.

The exact residual inventory is below. Its 8,552 intentional BRK/SVE/SME
exclusions imply a 99.9319% ceiling if all other sites were translated; the
brief's approximate 99.94% target underestimated SVE/SME by 1,325 sites.
The 3,781 smaller omitted sites are still implementation work, not claimed
to be impossible. Each individual ranked arrangement is below 0.3% of
the original tail (largest: SDOT.4S, 173 sites), though combined families
can exceed that share. Named families are not completely closed: indexed
SMULL/SMLAL, indexed double FMUL and scaled conversions remain among them.

| Remaining family | Sites | Reason / work still required |
| --- | ---: | --- |
| BRK | 3,477 | Deliberate trap; excluded. |
| SVE/SME | 5,075 | Unsupported and not advertised; complete-run evidence below, not counters. |
| Double fused | 5,996 | Retained helper; exact baseline implementation needs an Intel speed win. |
| Integer high/rounded narrowing | 1,079 | Needs high-half arithmetic, rounding and destination-half preservation. |
| SHA and PMULL | 588 | Outside AES scope; dedicated algorithms or another CPU feature/cache profile. |
| Integer dot/matrix products | 358 | Needs grouped byte products and index mapping; includes 80 SMMLA sites that remain UNDEF. |
| Integer reductions | 277 | Needs widening/minmax trees and scalar result clearing. |
| Indexed widening multiply | 240 | Needs H/L/M and register-bank decoding, source-half and accumulator alias tests. |
| Integer absolute differences | 230 | Needs signed/unsigned comparison before subtraction and widening/accumulation tests. |
| FP absolute difference | 198 | Needs subtraction-then-absolute special-value and FPSR handling. |
| Vector ADDP | 123 | Needs adjacent-pair packing from both source vectors. |
| Indexed double FMUL | 102 | Needs broadcast, admission and alias coverage. |
| Double vector comparisons | 86 | Needs unordered comparison and IOC/IDC handling. |
| Integer saturating add/sub | 77 | Needs exact clamps and sticky QC for active lanes. |
| Scaled fixed-point conversions | 77 | Needs fbits-aware rounding, saturation and exception checks. |
| Other scalar/count/reversal integers | 76 | Missing CLZ/RBIT/scalar comparison/add emitters and clearing tests. |
| Integer halving add | 63 | Needs widened intermediate and correct rounding before narrowing. |
| Other register shifts | 59 | Needs count-byte, oversized-count and rounding handling. |
| Saturating shifts/narrowing | 58 | Needs rounding/saturation ordering, width bounds and QC. |
| Single-lane/replicate structures | 58 | Outside multiple-structure scope; different decoding and partial-fault tests. |
| Half scalar move/compare | 12 | Needs half transfer/upper clearing and NaN comparison rules. |
| Double FNMUL | 12 | Needs post-multiply sign handling and special-value admission. |
| Double numeric min/max | 8 | Needs NaN preference and signed-zero selection. |

SMMLA is a pre-existing interpreter/capability gap; implementing its 80
sites would change the interpreter contract and is outside this JIT-only
change. No execution count is inferred for that family from static data.

The following sections record each group's admission bounds, focused
validation and remaining semantic limits. Static coverage counts a site as
translated even when exceptional operands can decline to its helper.
Temporary coverage caches were removed after every measurement.

## JIT tail: informational Rosetta ratios

These measurements are observations, not acceptance criteria. The baseline
is f893fdd and the candidate contains all nine groups above. Seven
interleaved repetitions per build and engine use 250,000 iterations, with
16 repeated test operations per iteration (four million per form). Loop,
reset, result-transfer and entry costs are included; the FPSR-read form
clears FPSR before each read. All 476 forms in all 28 uninstrumented runs
match native ARM checksums. Statistics were collected separately. Timing
started after correctness jobs finished; source and runtime hashes stayed
unchanged, and temporary AOT caches were removed.

Each value is candidate median divided by baseline median. The 364 new
forms range from 0.0061 to 0.9859 in JIT and 0.0058 to 0.9693 in AOT in
this run. Among the 112 prior control forms, 84 JIT and 39 AOT medians are
above one; their overall ranges are 0.8821–1.0724 and 0.9203–1.0579.
Those observations do not decide whether a change is kept and do not
establish application speedups or Intel performance.

The nearest-to-one new observations also illustrate the spread: FCVT H
from D in JIT has median ratio 0.9859, with baseline samples normalized
to its baseline median spanning 0.9731–1.1091 and candidate samples
0.8793–1.0311. FCVT H from S in AOT has ratio 0.9693, with normalized
ranges 0.9541–1.1475 and 0.8630–1.0324. The ranges overlap.

Group numbers match the coverage table; `prior` identifies existing
controls. Form names identify the exact operations in
[coverage_bench.c](../tests/guest/coverage_bench.c).

| Group | Form | JIT ratio | AOT ratio |
| --- | --- | ---: | ---: |
| 1 | `tail_fmadd` | 0.6972 | 0.6628 |
| 1 | `tail_fmsub` | 0.6654 | 0.6448 |
| 1 | `tail_fnmadd` | 0.6503 | 0.6223 |
| 1 | `tail_fnmsub` | 0.6565 | 0.6309 |
| 1 | `tail_fnmul` | 0.2890 | 0.2817 |
| 1 | `tail_frecpss` | 0.4752 | 0.4573 |
| 1 | `tail_frecps2s` | 0.2684 | 0.2584 |
| 1 | `tail_frecps4s` | 0.1533 | 0.1487 |
| 1 | `tail_frsqrtss` | 0.3048 | 0.2937 |
| 1 | `tail_frsqrts2s` | 0.1995 | 0.1940 |
| 1 | `tail_frsqrts4s` | 0.1146 | 0.1129 |
| 2 | `tail_saddl0_0` | 0.0218 | 0.0179 |
| 2 | `tail_saddl0_1` | 0.0228 | 0.0181 |
| 2 | `tail_saddl0_2` | 0.0128 | 0.0116 |
| 2 | `tail_saddl1_0` | 0.0307 | 0.0281 |
| 2 | `tail_saddl1_1` | 0.0306 | 0.0278 |
| 2 | `tail_saddl1_2` | 0.0194 | 0.0179 |
| 2 | `tail_uaddl0_0` | 0.0201 | 0.0176 |
| 2 | `tail_uaddl0_1` | 0.0212 | 0.0194 |
| 2 | `tail_uaddl0_2` | 0.0126 | 0.0115 |
| 2 | `tail_uaddl1_0` | 0.0298 | 0.0274 |
| 2 | `tail_uaddl1_1` | 0.0306 | 0.0277 |
| 2 | `tail_uaddl1_2` | 0.0195 | 0.0175 |
| 2 | `tail_saddw0_0` | 0.0205 | 0.0179 |
| 2 | `tail_saddw0_1` | 0.0212 | 0.0186 |
| 2 | `tail_saddw0_2` | 0.0122 | 0.0115 |
| 2 | `tail_saddw1_0` | 0.0207 | 0.0181 |
| 2 | `tail_saddw1_1` | 0.0215 | 0.0199 |
| 2 | `tail_saddw1_2` | 0.0125 | 0.0113 |
| 2 | `tail_uaddw0_0` | 0.0202 | 0.0183 |
| 2 | `tail_uaddw0_1` | 0.0205 | 0.0180 |
| 2 | `tail_uaddw0_2` | 0.0125 | 0.0116 |
| 2 | `tail_uaddw1_0` | 0.0207 | 0.0186 |
| 2 | `tail_uaddw1_1` | 0.0202 | 0.0185 |
| 2 | `tail_uaddw1_2` | 0.0121 | 0.0113 |
| 2 | `tail_ssubl0_0` | 0.0204 | 0.0183 |
| 2 | `tail_ssubl0_1` | 0.0196 | 0.0177 |
| 2 | `tail_ssubl0_2` | 0.0129 | 0.0125 |
| 2 | `tail_ssubl1_0` | 0.0299 | 0.0284 |
| 2 | `tail_ssubl1_1` | 0.0285 | 0.0282 |
| 2 | `tail_ssubl1_2` | 0.0188 | 0.0180 |
| 2 | `tail_usubl0_0` | 0.0192 | 0.0188 |
| 2 | `tail_usubl0_1` | 0.0193 | 0.0174 |
| 2 | `tail_usubl0_2` | 0.0124 | 0.0117 |
| 2 | `tail_usubl1_0` | 0.0297 | 0.0277 |
| 2 | `tail_usubl1_1` | 0.0298 | 0.0262 |
| 2 | `tail_usubl1_2` | 0.0188 | 0.0176 |
| 2 | `tail_ssubw0_0` | 0.0209 | 0.0185 |
| 2 | `tail_ssubw0_1` | 0.0216 | 0.0183 |
| 2 | `tail_ssubw0_2` | 0.0123 | 0.0112 |
| 2 | `tail_ssubw1_0` | 0.0209 | 0.0194 |
| 2 | `tail_ssubw1_1` | 0.0204 | 0.0179 |
| 2 | `tail_ssubw1_2` | 0.0126 | 0.0117 |
| 2 | `tail_usubw0_0` | 0.0206 | 0.0183 |
| 2 | `tail_usubw0_1` | 0.0195 | 0.0180 |
| 2 | `tail_usubw0_2` | 0.0124 | 0.0119 |
| 2 | `tail_usubw1_0` | 0.0212 | 0.0183 |
| 2 | `tail_usubw1_1` | 0.0184 | 0.0175 |
| 2 | `tail_usubw1_2` | 0.0122 | 0.0115 |
| 2 | `tail_smull0_0` | 0.0202 | 0.0186 |
| 2 | `tail_smull0_1` | 0.0201 | 0.0190 |
| 2 | `tail_smull0_2` | 0.0197 | 0.0173 |
| 2 | `tail_smull1_0` | 0.0289 | 0.0280 |
| 2 | `tail_smull1_1` | 0.0299 | 0.0292 |
| 2 | `tail_smull1_2` | 0.0261 | 0.0259 |
| 2 | `tail_umull0_0` | 0.0204 | 0.0179 |
| 2 | `tail_umull0_1` | 0.0207 | 0.0185 |
| 2 | `tail_umull0_2` | 0.0191 | 0.0175 |
| 2 | `tail_umull1_0` | 0.0292 | 0.0279 |
| 2 | `tail_umull1_1` | 0.0292 | 0.0290 |
| 2 | `tail_umull1_2` | 0.0261 | 0.0234 |
| 2 | `tail_smlal0_0` | 0.0572 | 0.0545 |
| 2 | `tail_smlal0_1` | 0.0566 | 0.0573 |
| 2 | `tail_smlal0_2` | 0.0362 | 0.0338 |
| 2 | `tail_smlal1_0` | 0.0511 | 0.0531 |
| 2 | `tail_smlal1_1` | 0.0534 | 0.0507 |
| 2 | `tail_smlal1_2` | 0.0336 | 0.0277 |
| 2 | `tail_umlal0_0` | 0.0546 | 0.0534 |
| 2 | `tail_umlal0_1` | 0.0561 | 0.0571 |
| 2 | `tail_umlal0_2` | 0.0354 | 0.0344 |
| 2 | `tail_umlal1_0` | 0.0557 | 0.0505 |
| 2 | `tail_umlal1_1` | 0.0563 | 0.0541 |
| 2 | `tail_umlal1_2` | 0.0301 | 0.0293 |
| 2 | `tail_smlsl0_0` | 0.0571 | 0.0591 |
| 2 | `tail_smlsl0_1` | 0.0575 | 0.0562 |
| 2 | `tail_smlsl0_2` | 0.0387 | 0.0356 |
| 2 | `tail_smlsl1_0` | 0.0540 | 0.0538 |
| 2 | `tail_smlsl1_1` | 0.0537 | 0.0504 |
| 2 | `tail_smlsl1_2` | 0.0326 | 0.0304 |
| 2 | `tail_umlsl0_0` | 0.0583 | 0.0559 |
| 2 | `tail_umlsl0_1` | 0.0551 | 0.0574 |
| 2 | `tail_umlsl0_2` | 0.0375 | 0.0351 |
| 2 | `tail_umlsl1_0` | 0.0516 | 0.0518 |
| 2 | `tail_umlsl1_1` | 0.0551 | 0.0520 |
| 2 | `tail_umlsl1_2` | 0.0319 | 0.0311 |
| 2 | `tail_sqxtn0_0` | 0.1529 | 0.1475 |
| 2 | `tail_sqxtn0_1` | 0.1826 | 0.1907 |
| 2 | `tail_sqxtn0_2` | 0.2216 | 0.2289 |
| 2 | `tail_sqxtn1_0` | 0.1531 | 0.1508 |
| 2 | `tail_sqxtn1_1` | 0.1855 | 0.1927 |
| 2 | `tail_sqxtn1_2` | 0.2185 | 0.2201 |
| 2 | `tail_uqxtn0_0` | 0.1560 | 0.1591 |
| 2 | `tail_uqxtn0_1` | 0.2102 | 0.2117 |
| 2 | `tail_uqxtn0_2` | 0.2395 | 0.2472 |
| 2 | `tail_uqxtn1_0` | 0.1635 | 0.1566 |
| 2 | `tail_uqxtn1_1` | 0.2017 | 0.2139 |
| 2 | `tail_uqxtn1_2` | 0.2322 | 0.2428 |
| 2 | `tail_sqxtun0_0` | 0.1549 | 0.1592 |
| 2 | `tail_sqxtun0_1` | 0.2077 | 0.2168 |
| 2 | `tail_sqxtun0_2` | 0.2371 | 0.2406 |
| 2 | `tail_sqxtun1_0` | 0.1544 | 0.1592 |
| 2 | `tail_sqxtun1_1` | 0.2016 | 0.2109 |
| 2 | `tail_sqxtun1_2` | 0.2299 | 0.2418 |
| 2 | `tail_shll0_0` | 0.0139 | 0.0122 |
| 2 | `tail_shll0_1` | 0.0164 | 0.0149 |
| 2 | `tail_shll0_2` | 0.0171 | 0.0153 |
| 2 | `tail_shll1_0` | 0.0150 | 0.0131 |
| 2 | `tail_shll1_1` | 0.0177 | 0.0158 |
| 2 | `tail_shll1_2` | 0.0198 | 0.0163 |
| 2 | `tail_sri0_0_1` | 0.0484 | 0.0491 |
| 2 | `tail_sri0_0_4` | 0.0497 | 0.0496 |
| 2 | `tail_sri0_0_8` | 0.0541 | 0.0543 |
| 2 | `tail_sli0_0_0` | 0.1694 | 0.1788 |
| 2 | `tail_sli0_0_4` | 0.0453 | 0.0459 |
| 2 | `tail_sli0_0_7` | 0.0463 | 0.0450 |
| 2 | `tail_rev160_0` | 0.0332 | 0.0302 |
| 2 | `tail_rev320_0` | 0.0313 | 0.0297 |
| 2 | `tail_rev640_0` | 0.0314 | 0.0293 |
| 2 | `tail_sri0_1_1` | 0.0549 | 0.0593 |
| 2 | `tail_sri0_1_8` | 0.0562 | 0.0594 |
| 2 | `tail_sri0_1_16` | 0.0582 | 0.0603 |
| 2 | `tail_sli0_1_0` | 0.2156 | 0.2102 |
| 2 | `tail_sli0_1_8` | 0.0583 | 0.0547 |
| 2 | `tail_sli0_1_15` | 0.0576 | 0.0571 |
| 2 | `tail_rev320_1` | 0.0397 | 0.0357 |
| 2 | `tail_rev640_1` | 0.0371 | 0.0382 |
| 2 | `tail_sqdmulh0_1` | 0.0971 | 0.0953 |
| 2 | `tail_sqdmulh0_1_0` | 0.1098 | 0.1019 |
| 2 | `tail_sqdmulh0_1_1` | 0.1066 | 0.1081 |
| 2 | `tail_sqdmulh0_1_2` | 0.1051 | 0.0983 |
| 2 | `tail_sqdmulh0_1_3` | 0.1040 | 0.1057 |
| 2 | `tail_sqdmulh0_1_4` | 0.1101 | 0.1140 |
| 2 | `tail_sqdmulh0_1_5` | 0.1138 | 0.1108 |
| 2 | `tail_sqdmulh0_1_6` | 0.1099 | 0.1166 |
| 2 | `tail_sqdmulh0_1_7` | 0.1116 | 0.1126 |
| 2 | `tail_sqrdmulh0_1` | 0.0986 | 0.0948 |
| 2 | `tail_sqrdmulh0_1_0` | 0.1067 | 0.1094 |
| 2 | `tail_sqrdmulh0_1_1` | 0.1100 | 0.1113 |
| 2 | `tail_sqrdmulh0_1_2` | 0.1082 | 0.1109 |
| 2 | `tail_sqrdmulh0_1_3` | 0.1075 | 0.1129 |
| 2 | `tail_sqrdmulh0_1_4` | 0.1120 | 0.1181 |
| 2 | `tail_sqrdmulh0_1_5` | 0.1132 | 0.1168 |
| 2 | `tail_sqrdmulh0_1_6` | 0.1128 | 0.1224 |
| 2 | `tail_sqrdmulh0_1_7` | 0.1147 | 0.1186 |
| 2 | `tail_sri0_2_1` | 0.0611 | 0.0661 |
| 2 | `tail_sri0_2_16` | 0.0625 | 0.0659 |
| 2 | `tail_sri0_2_32` | 0.0586 | 0.0634 |
| 2 | `tail_sli0_2_0` | 0.2442 | 0.2529 |
| 2 | `tail_sli0_2_16` | 0.0663 | 0.0670 |
| 2 | `tail_sli0_2_31` | 0.0650 | 0.0664 |
| 2 | `tail_rev640_2` | 0.0390 | 0.0433 |
| 2 | `tail_sqdmulh0_2` | 0.0711 | 0.0697 |
| 2 | `tail_sqdmulh0_2_0` | 0.0856 | 0.0845 |
| 2 | `tail_sqdmulh0_2_1` | 0.0863 | 0.0866 |
| 2 | `tail_sqdmulh0_2_2` | 0.0852 | 0.0888 |
| 2 | `tail_sqdmulh0_2_3` | 0.0867 | 0.0883 |
| 2 | `tail_sqrdmulh0_2` | 0.0801 | 0.0787 |
| 2 | `tail_sqrdmulh0_2_0` | 0.0980 | 0.0965 |
| 2 | `tail_sqrdmulh0_2_1` | 0.0944 | 0.0971 |
| 2 | `tail_sqrdmulh0_2_2` | 0.0959 | 0.0982 |
| 2 | `tail_sqrdmulh0_2_3` | 0.0952 | 0.0956 |
| 2 | `tail_sri1_0_1` | 0.0319 | 0.0337 |
| 2 | `tail_sri1_0_4` | 0.0330 | 0.0338 |
| 2 | `tail_sri1_0_8` | 0.0345 | 0.0381 |
| 2 | `tail_sli1_0_0` | 0.1000 | 0.1094 |
| 2 | `tail_sli1_0_4` | 0.0270 | 0.0306 |
| 2 | `tail_sli1_0_7` | 0.0297 | 0.0304 |
| 2 | `tail_rev161_0` | 0.0228 | 0.0228 |
| 2 | `tail_rev321_0` | 0.0216 | 0.0241 |
| 2 | `tail_rev641_0` | 0.0220 | 0.0224 |
| 2 | `tail_sri1_1_1` | 0.0389 | 0.0472 |
| 2 | `tail_sri1_1_8` | 0.0420 | 0.0482 |
| 2 | `tail_sri1_1_16` | 0.0448 | 0.0492 |
| 2 | `tail_sli1_1_0` | 0.1500 | 0.1521 |
| 2 | `tail_sli1_1_8` | 0.0357 | 0.0438 |
| 2 | `tail_sli1_1_15` | 0.0411 | 0.0418 |
| 2 | `tail_rev321_1` | 0.0301 | 0.0313 |
| 2 | `tail_rev641_1` | 0.0300 | 0.0314 |
| 2 | `tail_sqdmulh1_1` | 0.0858 | 0.0866 |
| 2 | `tail_sqdmulh1_1_0` | 0.0947 | 0.0980 |
| 2 | `tail_sqdmulh1_1_1` | 0.0950 | 0.0993 |
| 2 | `tail_sqdmulh1_1_2` | 0.0962 | 0.0987 |
| 2 | `tail_sqdmulh1_1_3` | 0.0962 | 0.0973 |
| 2 | `tail_sqdmulh1_1_4` | 0.0993 | 0.1036 |
| 2 | `tail_sqdmulh1_1_5` | 0.0991 | 0.1042 |
| 2 | `tail_sqdmulh1_1_6` | 0.0983 | 0.1041 |
| 2 | `tail_sqdmulh1_1_7` | 0.1008 | 0.1008 |
| 2 | `tail_sqrdmulh1_1` | 0.0875 | 0.0901 |
| 2 | `tail_sqrdmulh1_1_0` | 0.0963 | 0.0999 |
| 2 | `tail_sqrdmulh1_1_1` | 0.0988 | 0.1005 |
| 2 | `tail_sqrdmulh1_1_2` | 0.0971 | 0.1012 |
| 2 | `tail_sqrdmulh1_1_3` | 0.0975 | 0.1039 |
| 2 | `tail_sqrdmulh1_1_4` | 0.1041 | 0.1050 |
| 2 | `tail_sqrdmulh1_1_5` | 0.1042 | 0.1091 |
| 2 | `tail_sqrdmulh1_1_6` | 0.1041 | 0.1060 |
| 2 | `tail_sqrdmulh1_1_7` | 0.1018 | 0.1088 |
| 2 | `tail_sri1_2_1` | 0.0477 | 0.0563 |
| 2 | `tail_sri1_2_16` | 0.0482 | 0.0565 |
| 2 | `tail_sri1_2_32` | 0.0467 | 0.0519 |
| 2 | `tail_sli1_2_0` | 0.1825 | 0.1894 |
| 2 | `tail_sli1_2_16` | 0.0529 | 0.0541 |
| 2 | `tail_sli1_2_31` | 0.0519 | 0.0559 |
| 2 | `tail_rev641_2` | 0.0336 | 0.0343 |
| 2 | `tail_sqdmulh1_2` | 0.0461 | 0.0495 |
| 2 | `tail_sqdmulh1_2_0` | 0.0648 | 0.0672 |
| 2 | `tail_sqdmulh1_2_1` | 0.0659 | 0.0673 |
| 2 | `tail_sqdmulh1_2_2` | 0.0654 | 0.0673 |
| 2 | `tail_sqdmulh1_2_3` | 0.0658 | 0.0672 |
| 2 | `tail_sqrdmulh1_2` | 0.0533 | 0.0560 |
| 2 | `tail_sqrdmulh1_2_0` | 0.0755 | 0.0768 |
| 2 | `tail_sqrdmulh1_2_1` | 0.0751 | 0.0792 |
| 2 | `tail_sqrdmulh1_2_2` | 0.0760 | 0.0787 |
| 2 | `tail_sqrdmulh1_2_3` | 0.0750 | 0.0763 |
| 2 | `tail_sri1_3_1` | 0.0517 | 0.0576 |
| 2 | `tail_sri1_3_32` | 0.0523 | 0.0604 |
| 2 | `tail_sri1_3_64` | 0.0459 | 0.0591 |
| 2 | `tail_sli1_3_0` | 0.2077 | 0.2232 |
| 2 | `tail_sli1_3_32` | 0.0596 | 0.0579 |
| 2 | `tail_sli1_3_63` | 0.0594 | 0.0622 |
| 2 | `tail_addpd` | 0.0245 | 0.0232 |
| 3 | `tail_fcvtn_4h` | 0.5054 | 0.4988 |
| 3 | `tail_fcvtn_2s` | 0.1944 | 0.1873 |
| 3 | `tail_fcvtn2_8h` | 0.5105 | 0.4941 |
| 3 | `tail_fcvtn2_4s` | 0.1929 | 0.1870 |
| 3 | `tail_fcvtl_4s` | 0.1981 | 0.1905 |
| 3 | `tail_fcvtl_2d` | 0.1966 | 0.1871 |
| 3 | `tail_fcvtl2_4s` | 0.1983 | 0.1934 |
| 3 | `tail_fcvtl2_2d` | 0.1929 | 0.1934 |
| 3 | `tail_fcvtzu_4s` | 0.2395 | 0.2451 |
| 3 | `tail_fcvtns_4s` | 0.2302 | 0.2454 |
| 3 | `tail_fcvtps_4s` | 0.2414 | 0.2498 |
| 3 | `tail_fcvtms_4s` | 0.2422 | 0.2359 |
| 3 | `tail_scvtf_s` | 0.3385 | 0.3366 |
| 3 | `tail_scvtf_d` | 0.3433 | 0.3368 |
| 3 | `tail_scvtf_2d` | 0.2718 | 0.2644 |
| 3 | `tail_scvtf_8h` | 0.3516 | 0.3383 |
| 3 | `tail_ucvtf_s` | 0.3506 | 0.3403 |
| 3 | `tail_ucvtf_d` | 0.3531 | 0.3428 |
| 3 | `tail_ucvtf_2d` | 0.2823 | 0.2726 |
| 3 | `tail_ucvtf_8h` | 0.3622 | 0.3547 |
| 3 | `tail_fcvtzu_8h` | 0.3866 | 0.3639 |
| 3 | `tail_fcvtns_8h` | 0.3215 | 0.3333 |
| 3 | `tail_fcvt_hs` | 0.9788 | 0.9693 |
| 3 | `tail_fcvt_hd` | 0.9859 | 0.9627 |
| 3 | `tail_fcvt_sh` | 0.2551 | 0.2541 |
| 3 | `tail_fcvt_dh` | 0.2498 | 0.2470 |
| 4 | `ld2_8b` | 0.2520 | 0.2559 |
| 4 | `ld2_16b` | 0.3544 | 0.3424 |
| 4 | `ld2_4h` | 0.1512 | 0.1537 |
| 4 | `ld2_8h` | 0.2330 | 0.2404 |
| 4 | `ld2_2s` | 0.0672 | 0.0710 |
| 4 | `ld2_4s` | 0.1094 | 0.1085 |
| 4 | `ld2_2d` | 0.0503 | 0.0509 |
| 4 | `ld3_8b` | 0.3251 | 0.3293 |
| 4 | `ld3_16b` | 0.4520 | 0.4414 |
| 4 | `ld3_4h` | 0.2027 | 0.2100 |
| 4 | `ld3_8h` | 0.3154 | 0.3239 |
| 4 | `ld3_2s` | 0.0890 | 0.0943 |
| 4 | `ld3_4s` | 0.1442 | 0.1509 |
| 4 | `ld3_2d` | 0.0686 | 0.0768 |
| 4 | `ld4_8b` | 0.3772 | 0.3927 |
| 4 | `ld4_16b` | 0.4871 | 0.4835 |
| 4 | `ld4_4h` | 0.2434 | 0.2552 |
| 4 | `ld4_8h` | 0.3634 | 0.3800 |
| 4 | `ld4_2s` | 0.1029 | 0.1074 |
| 4 | `ld4_4s` | 0.1543 | 0.1666 |
| 4 | `ld4_2d` | 0.0812 | 0.0905 |
| 4 | `st2_8b` | 0.0726 | 0.0816 |
| 4 | `st2_16b` | 0.0865 | 0.0918 |
| 4 | `st2_4h` | 0.0607 | 0.0675 |
| 4 | `st2_8h` | 0.0737 | 0.0807 |
| 4 | `st2_2s` | 0.0646 | 0.0666 |
| 4 | `st2_4s` | 0.0642 | 0.0703 |
| 4 | `st2_2d` | 0.0568 | 0.0642 |
| 4 | `st3_8b` | 0.0886 | 0.0955 |
| 4 | `st3_16b` | 0.1013 | 0.1084 |
| 4 | `st3_4h` | 0.0739 | 0.0756 |
| 4 | `st3_8h` | 0.0854 | 0.0871 |
| 4 | `st3_2s` | 0.0758 | 0.0866 |
| 4 | `st3_4s` | 0.0823 | 0.0885 |
| 4 | `st3_2d` | 0.0661 | 0.0753 |
| 4 | `st4_8b` | 0.0952 | 0.0995 |
| 4 | `st4_16b` | 0.0990 | 0.1046 |
| 4 | `st4_4h` | 0.0777 | 0.0810 |
| 4 | `st4_8h` | 0.1060 | 0.0979 |
| 4 | `st4_2s` | 0.0776 | 0.0836 |
| 4 | `st4_4s` | 0.0892 | 0.0937 |
| 4 | `st4_2d` | 0.0698 | 0.0767 |
| 5 | `wide_fadd` | 0.1614 | 0.1679 |
| 5 | `wide_fsub` | 0.1640 | 0.1659 |
| 5 | `wide_fmul` | 0.1679 | 0.1727 |
| 5 | `wide_fdiv` | 0.1630 | 0.1587 |
| 5 | `wide_sqrtd` | 0.2609 | 0.2549 |
| 5 | `wide_sqrts` | 0.2045 | 0.1993 |
| 5 | `wide_sqrt2s` | 0.2795 | 0.2724 |
| 6 | `tail_fmul_4h` | 0.2849 | 0.2733 |
| 6 | `tail_fmul_8h` | 0.2910 | 0.2842 |
| 6 | `tail_fmul_h` | 0.6480 | 0.5852 |
| 6 | `tail_fadd_4h` | 0.2627 | 0.2554 |
| 6 | `tail_fadd_8h` | 0.2651 | 0.2565 |
| 6 | `tail_fadd_h` | 0.6201 | 0.6214 |
| 6 | `tail_fsub_4h` | 0.2670 | 0.2566 |
| 6 | `tail_fsub_8h` | 0.2663 | 0.2605 |
| 6 | `tail_fsub_h` | 0.6448 | 0.6274 |
| 6 | `tail_fmax_4h` | 0.0294 | 0.0277 |
| 6 | `tail_fmax_8h` | 0.0152 | 0.0143 |
| 6 | `tail_fmax_h` | 0.0837 | 0.0808 |
| 6 | `tail_fmin_4h` | 0.0299 | 0.0275 |
| 6 | `tail_fmin_8h` | 0.0153 | 0.0141 |
| 6 | `tail_fmin_h` | 0.0853 | 0.0731 |
| 6 | `tail_fmaxnm_4h` | 0.0276 | 0.0250 |
| 6 | `tail_fmaxnm_8h` | 0.0138 | 0.0127 |
| 6 | `tail_fmaxnm_h` | 0.0797 | 0.0770 |
| 6 | `tail_fminnm_4h` | 0.0274 | 0.0254 |
| 6 | `tail_fminnm_8h` | 0.0134 | 0.0129 |
| 6 | `tail_fminnm_h` | 0.0793 | 0.0757 |
| 6 | `tail_fmul_4h_e0` | 0.2646 | 0.2536 |
| 6 | `tail_fmul_8h_e0` | 0.2747 | 0.2650 |
| 6 | `tail_fmul_4h_e1` | 0.2777 | 0.2586 |
| 6 | `tail_fmul_8h_e1` | 0.2810 | 0.2679 |
| 6 | `tail_fmul_4h_e2` | 0.2764 | 0.2593 |
| 6 | `tail_fmul_8h_e2` | 0.2771 | 0.2680 |
| 6 | `tail_fmul_4h_e3` | 0.2701 | 0.2597 |
| 6 | `tail_fmul_8h_e3` | 0.2768 | 0.2661 |
| 6 | `tail_fmul_4h_e4` | 0.2653 | 0.2569 |
| 6 | `tail_fmul_8h_e4` | 0.2770 | 0.2655 |
| 6 | `tail_fmul_4h_e5` | 0.2763 | 0.2618 |
| 6 | `tail_fmul_8h_e5` | 0.2770 | 0.2748 |
| 6 | `tail_fmul_4h_e6` | 0.2739 | 0.2691 |
| 6 | `tail_fmul_8h_e6` | 0.2770 | 0.2696 |
| 6 | `tail_fmul_4h_e7` | 0.2678 | 0.2637 |
| 6 | `tail_fmul_8h_e7` | 0.2746 | 0.2707 |
| 7 | `tail_fccmp_s` | 0.1583 | 0.1828 |
| 7 | `tail_fccmpe_s` | 0.1543 | 0.1831 |
| 7 | `tail_fccmp_d` | 0.1548 | 0.1861 |
| 7 | `tail_fccmpe_d` | 0.1603 | 0.1822 |
| 7 | `tail_mrs_fpcr` | 0.7642 | 0.7766 |
| 7 | `tail_msr_fpcr` | 0.8067 | 0.8339 |
| 7 | `tail_mrs_fpsr` | 0.7473 | 0.7747 |
| 7 | `tail_msr_fpsr` | 0.7831 | 0.7938 |
| 9 | `tail_frecpe_2s` | 0.0613 | 0.0639 |
| 9 | `tail_frecpe_4s` | 0.0827 | 0.0894 |
| 9 | `tail_frecpe_2d` | 0.0557 | 0.0578 |
| 9 | `tail_frecpe_s` | 0.0433 | 0.0436 |
| 9 | `tail_frecpe_d` | 0.0417 | 0.0428 |
| 9 | `tail_frsqrte_2s` | 0.0077 | 0.0076 |
| 9 | `tail_frsqrte_4s` | 0.0083 | 0.0083 |
| 9 | `tail_frsqrte_2d` | 0.0072 | 0.0071 |
| 9 | `tail_frsqrte_s` | 0.0061 | 0.0058 |
| 9 | `tail_frsqrte_d` | 0.0061 | 0.0058 |
| 10 | `tail_aese` | 0.1049 | 0.1122 |
| 10 | `tail_aesd` | 0.1093 | 0.1118 |
| 10 | `tail_aesmc` | 0.0357 | 0.0341 |
| 10 | `tail_aesimc` | 0.0175 | 0.0159 |
| prior | `zip1_8b` | 0.9848 | 0.9978 |
| prior | `zip1_16b` | 1.0224 | 0.9888 |
| prior | `zip1_4h` | 0.9825 | 1.0579 |
| prior | `zip1_8h` | 1.0039 | 0.9327 |
| prior | `zip1_2s` | 0.9497 | 0.9742 |
| prior | `zip1_4s` | 1.0128 | 0.9977 |
| prior | `zip1_2d` | 1.0315 | 0.9654 |
| prior | `zip2_8b` | 1.0137 | 0.9760 |
| prior | `zip2_16b` | 1.0318 | 0.9618 |
| prior | `zip2_4h` | 1.0160 | 0.9754 |
| prior | `zip2_8h` | 0.9951 | 1.0034 |
| prior | `zip2_2s` | 0.9972 | 1.0000 |
| prior | `zip2_4s` | 0.9844 | 1.0339 |
| prior | `zip2_2d` | 1.0241 | 1.0102 |
| prior | `uzp1_8b` | 0.9940 | 0.9572 |
| prior | `uzp1_16b` | 0.9951 | 1.0236 |
| prior | `uzp1_4h` | 0.9785 | 0.9233 |
| prior | `uzp1_8h` | 1.0143 | 1.0278 |
| prior | `uzp1_2s` | 1.0648 | 0.9313 |
| prior | `uzp1_4s` | 1.0129 | 1.0242 |
| prior | `uzp1_2d` | 1.0026 | 0.9585 |
| prior | `uzp2_8b` | 0.9735 | 1.0295 |
| prior | `uzp2_16b` | 1.0406 | 0.9576 |
| prior | `uzp2_4h` | 1.0600 | 1.0161 |
| prior | `uzp2_8h` | 1.0347 | 0.9485 |
| prior | `uzp2_2s` | 1.0089 | 1.0516 |
| prior | `uzp2_4s` | 0.9768 | 0.9542 |
| prior | `uzp2_2d` | 1.0019 | 1.0111 |
| prior | `trn1_8b` | 1.0333 | 0.9641 |
| prior | `trn1_16b` | 1.0022 | 0.9954 |
| prior | `trn1_4h` | 1.0156 | 0.9946 |
| prior | `trn1_8h` | 0.9787 | 0.9988 |
| prior | `trn1_2s` | 0.9994 | 0.9744 |
| prior | `trn1_4s` | 1.0426 | 0.9726 |
| prior | `trn1_2d` | 1.0724 | 0.9708 |
| prior | `trn2_8b` | 1.0255 | 1.0020 |
| prior | `trn2_16b` | 0.9851 | 1.0106 |
| prior | `trn2_4h` | 0.9929 | 1.0034 |
| prior | `trn2_8h` | 1.0289 | 0.9994 |
| prior | `trn2_2s` | 1.0498 | 0.9679 |
| prior | `trn2_4s` | 1.0150 | 1.0126 |
| prior | `trn2_2d` | 1.0126 | 0.9713 |
| prior | `addv_8b` | 1.0263 | 1.0061 |
| prior | `addv_16b` | 1.0112 | 0.9941 |
| prior | `addv_4h` | 1.0076 | 0.9757 |
| prior | `addv_8h` | 1.0157 | 0.9755 |
| prior | `addv_4s` | 1.0215 | 0.9671 |
| prior | `uaddlp_4h` | 1.0120 | 1.0026 |
| prior | `uaddlp_8h` | 1.0018 | 1.0408 |
| prior | `uaddlp_2s` | 1.0212 | 0.9883 |
| prior | `uaddlp_4s` | 0.9694 | 1.0000 |
| prior | `uaddlp_1d` | 1.0331 | 0.9812 |
| prior | `uaddlp_2d` | 0.9977 | 0.9203 |
| prior | `saddlp_4h` | 1.0049 | 0.9805 |
| prior | `saddlp_8h` | 1.0176 | 1.0233 |
| prior | `saddlp_2s` | 1.0232 | 0.9740 |
| prior | `saddlp_4s` | 1.0176 | 0.9923 |
| prior | `saddlp_1d` | 1.0180 | 1.0026 |
| prior | `saddlp_2d` | 1.0216 | 0.9996 |
| prior | `uadalp_4h` | 1.0141 | 0.9973 |
| prior | `uadalp_8h` | 1.0143 | 0.9750 |
| prior | `uadalp_2s` | 1.0321 | 0.9807 |
| prior | `uadalp_4s` | 1.0169 | 1.0275 |
| prior | `uadalp_1d` | 1.0156 | 0.9915 |
| prior | `uadalp_2d` | 1.0159 | 1.0107 |
| prior | `sadalp_4h` | 1.0038 | 0.9773 |
| prior | `sadalp_8h` | 1.0059 | 0.9713 |
| prior | `sadalp_2s` | 1.0262 | 1.0013 |
| prior | `sadalp_4s` | 1.0034 | 0.9993 |
| prior | `sadalp_1d` | 0.8821 | 1.0050 |
| prior | `sadalp_2d` | 1.0052 | 0.9874 |
| prior | `faddp_4s` | 1.0074 | 0.9856 |
| prior | `faddp_2s` | 1.0171 | 0.9828 |
| prior | `faddp_s` | 1.0167 | 1.0008 |
| prior | `faddp_2d` | 1.0102 | 0.9956 |
| prior | `faddp_d` | 1.0126 | 1.0071 |
| prior | `fmaxv_4s` | 1.0037 | 1.0012 |
| prior | `fminv_4s` | 1.0098 | 0.9955 |
| prior | `fmaxnmv_4s` | 1.0182 | 0.9969 |
| prior | `fminnmv_4s` | 1.0177 | 0.9843 |
| prior | `fdiv_2s` | 1.0117 | 0.9976 |
| prior | `fdiv_4s` | 0.9952 | 1.0091 |
| prior | `fcmeq_zero_2s` | 1.0259 | 0.9995 |
| prior | `fcmeq_zero_4s` | 1.0381 | 1.0005 |
| prior | `fcmge_zero_2s` | 1.0006 | 0.9831 |
| prior | `fcmge_zero_4s` | 1.0092 | 0.9963 |
| prior | `fcmgt_zero_2s` | 1.0078 | 1.0226 |
| prior | `fcmgt_zero_4s` | 1.0277 | 0.9992 |
| prior | `fcmle_zero_2s` | 0.9874 | 0.9812 |
| prior | `fcmle_zero_4s` | 1.0007 | 1.0018 |
| prior | `fcmlt_zero_2s` | 1.0068 | 0.9925 |
| prior | `fcmlt_zero_4s` | 1.0031 | 0.9717 |
| prior | `ushl_2s` | 1.0279 | 0.9993 |
| prior | `ushl_4s` | 1.0117 | 1.0003 |
| prior | `ushl_2d` | 1.0231 | 1.0085 |
| prior | `sshl_2s` | 0.9978 | 0.9986 |
| prior | `sshl_4s` | 1.0089 | 0.9980 |
| prior | `sshl_2d` | 1.0014 | 1.0064 |
| prior | `fmul_s0` | 0.9941 | 1.0007 |
| prior | `fmul_s1` | 0.9984 | 0.9962 |
| prior | `fmul_s2` | 0.9889 | 0.9945 |
| prior | `fmul_s3` | 0.9971 | 1.0263 |
| prior | `fmla_s0` | 1.0070 | 1.0118 |
| prior | `fmla_s1` | 0.9958 | 1.0008 |
| prior | `fmla_s2` | 0.9968 | 0.9985 |
| prior | `fmla_s3` | 1.0019 | 0.9930 |
| prior | `fmls_s0` | 1.0035 | 0.9979 |
| prior | `fmls_s1` | 1.0066 | 0.9962 |
| prior | `fmls_s2` | 1.0053 | 1.0067 |
| prior | `fmls_s3` | 0.9983 | 0.9987 |
| prior | `fmul_d0` | 1.0149 | 0.9980 |
| prior | `fmul_d1` | 1.0054 | 0.9990 |

## JIT tail: scalar fused single and reciprocal steps

Scalar FMADD/FMSUB/FNMADD/FNMSUB S now share the packed single FMA
binary64 product, TwoSum and round-to-odd path, with independent addend
selection and both architectural sign controls. FRECPS/FRSQRTS S/.2S/.4S
use the same fused sum; the square-root step scales the binary64 sum by
one half exactly before its single-precision rounding. FNMUL S negates the
rounded product. Integer admission precedes floating arithmetic. Special
operands (including zero times infinity), tiny results and unsupported
controls retain the original helper with restored MXCSR and unmodified
registers. The interpreter is unchanged.

Validation: 771,142 complete-state cases, all 16 RMode/FZ/DN settings,
register aliases, directed fused near-ties, full FPSR and live NZCV; zero
mismatches. The 632-line native ARM instruction comparison matches JIT,
interpreter, helpers-only and AOT. Two new mutations are caught: scalar
addend sign (7,834 mismatches), reciprocal-square-root scaling (14,022).
The ordinary eleven-form workload at 1,000 iterations reports 176,000 -> 0
helper calls in both JIT and AOT (80,000 scalar, 64,000 vector and 32,000
scalar-step calls before); its native checksums match every engine.

AOT generation of the same large FP/SIMD-heavy arm64 program reports
12,561,074 instructions: 99.5283% -> 99.6256% translated natively (12,216
formerly untranslated sites removed), generated code
639,290 -> 645,955 KiB. Temporary caches were removed. Static coverage is
not executed-instruction coverage or application acceptance. Informational
Rosetta ratios are recorded above; Intel testing remains deferred.

Saved 200,000-record corpora in each of simd/block/fma/mem/dp replay with
zero mismatches. IEEE-flag difference counts are unchanged: interpreter
0/0/0/0/0 and JIT 4/4,981/0/0/0.

The supplied complete application-run record has 32 workload starts and
finishes computation before an offline-upload failure. Its advertised
instruction sets contain neither SVE nor SME; those instructions remain
UNDEF. SVE/SME was not executed in a complete run (fatal if executed), not
measured by counters. BRK remains a deliberate trap. This evidence does not
establish behavior on every possible application input.

## Release validation of optional AES cache profiles

Release validation now accepts exactly the runtime UUID and its AES-profile
variant (last byte XOR 1) in AOT cache headers, matching the image-payload
and installation tools. Previously it rejected valid AES-profile caches as
belonging to another runtime. Package dependencies still require the exact,
unmodified runtime UUID; runtime loading still requires the current CPU's
exact profile. Synthetic release preflight tests accept both valid header
profiles and reject unrelated UUID edits and a profile UUID substituted for
the runtime dependency. The 30 focused release tests pass.

## Exact reciprocal estimates in translated code

Cumulative static coverage reaches 99.8432% (2,907 additional sites), with
generated code 651,546 -> 652,861 KiB. Ten ordinary forms at 1,000
iterations reduce JIT/AOT helper calls from 160,000 to zero. Temporary
coverage caches were removed.

FRECPE/FRSQRTE S/D scalar and vector forms now use the ARM estimate tables
through integer operations. The tables are computed once from the same
integer RecipEstimate/RecipSqrtEstimate definitions as the interpreter and
reached through a runtime-bound CPU pointer, keeping JIT/AOT bytes position
independent. Admitted reciprocal inputs are normal with normal results;
square-root estimates require positive normal inputs. Zeros, subnormals,
NaNs, infinities, negative square roots and reciprocal output-denormal
boundaries keep the existing helper behavior and FPSR rules. The JIT stages
all active lanes before writing Vd, including source/destination aliases.
No interpreter semantics changed. Half estimates remain helpers.

The pointer occupies the CPU structure's trailing padding at 0x408; all
previous offsets are unchanged. AOT version 7 rejects old caches before
execution. Release validators, installer, image-payload and extension tools
use the new version; the 136-byte AOT header layout is unchanged.

Validation passes 777,292 complete-state cases; native JIT, interpreter,
helpers-only and AOT comparisons pass 926 output lines. Every estimate
index, exponent parity and sign is exercised under all FPCR controls in
the native test, with exceptional exponent boundaries and fraction bucket
edges. Independent table-rounding and exponent-parity mutations are caught
after a passing complete-state control. Encoder, AOT, helper-statistics,
image-payload, 28 release-validator tests and distribution checks pass.
Full runtime gates and cumulative coverage are recorded at integration.

At 1,000 iterations the ten ordinary estimate loops eliminate 160,000 helper
calls; native, base and changed result checksums agree. Together with the
integer and FP-environment loops, the workload drops from 3,808,000 helper
calls to zero and grows from 1,368 to 1,769 KiB of generated code. Intel
performance acceptance remains separate from development-host ratios.


## Conditional FP comparisons and FP environment moves

Cumulative static coverage reaches 99.8200% (1,650 additional sites), with
generated code 651,281 -> 651,546 KiB. Eight ordinary loops at 1,000
iterations remove 144,000 JIT/AOT helper calls. The combined groups 1–7
pass 899,805 complete-state comparisons after integration, including the
unchanged-control-word guard below. Temporary caches were removed.

FCCMP/FCCMPE S/D now test the condition before admitting operands. A false
condition installs the immediate NZCV without reading FP values or raising
exceptions; ordinary true comparisons use SSE, while NaNs, infinities and
subnormals retain the interpreter path. FPCR/FPSR reads and writes now emit
the interpreter's mapping directly, including native MXCSR rounding/flush
state, software QC/IDC, x87 rounding control and cumulative exceptions.
Guest NZCV is materialized before integer bit mapping. No FP instruction
executes before admission, and no interpreter behavior changed.

The complete-state test passes 749,126 cases, including all conditions,
input NZCV, S/D, signaling variants, FPCR controls, sticky FPSR and seeded
x87 exceptions. System-register tests also compare x87 rounding bits and
cover XZR plus masked writable values. Native comparisons pass across JIT,
interpreter, helpers-only and AOT (916 lines), and 41,143 encoder forms pass,
including all new x87 memory encodings. Independent mutations invert the
conditional branch and corrupt the FPCR rounding mapping. Full gates and
cumulative coverage are recorded at integration. Half FCCMP remains a helper.

At 1,000 iterations, eight new benchmark loops eliminate 64,000 scalar-FP
and 80,000 system helper calls. Their result checksums equal native ARM and
the base; together with the integer loops, the workload drops from 3,648,000
helper calls to zero. Development-host timing and Intel acceptance remain
separate measurements.


## Integer SIMD tail: widening, saturation and lane operations

The JIT now emits SSE2 through SSE4.2 for signed/unsigned widening add,
subtract, multiply and accumulate, SHLL, SQXTN/UQXTN/SQXTUN, SRI/SLI,
REV16/32/64, scalar ADDP and vector/indexed SQDMULH/SQRDMULH. Narrowing and
multiply-high preserve sticky FPSR.QC without changing live NZCV; each
source is loaded before an aliased destination is stored. Scalar SIMD
byte/halfword loads and stores now use the existing address/writeback path.
No interpreter semantics changed. Unallocated neighboring encodings keep
helper routing; scalar saturating and indexed widening forms remain outside
this change.

Validation: the complete-state differential test passes 729,670 cases,
including every new integer arrangement, selected halves, aliases, saturation
boundaries, full FPSR and live NZCV. Byte/halfword memory checks include
inaccessible pages and pre/post-index writeback. The native instruction
comparison passes in JIT, interpreter, helpers-only and AOT (229 added forms,
850 output lines total). The encoder check passes 40,811 forms. Independent
mutations remove QC publication and incorrectly widen the already-wide input;
the unchanged control must pass before the mutations run. Full runtime gates
are recorded separately. Cumulative coverage after this group is 99.7050%
(9,985 additional sites), with generated code 645,955 -> 646,110 KiB.
The temporary measurement cache was removed.

The 219 added ordinary benchmark loops execute 3,504,000 previously
untranslated helper calls at 1,000 iterations on the base; this translation
executes zero, with identical result checksums. Generated code for that
whole instruction workload grows from 1,317 to 1,611 KiB. Timing acceptance
on Intel remains separate from development-host measurements.


## Half arithmetic tail

Cumulative static coverage reaches 99.8069% (1,415 additional sites), with
generated code 649,079 -> 651,281 KiB. The 37 ordinary forms at 5,000
iterations reduce JIT/AOT helper calls from 2,960,000 to zero. Temporary
coverage caches were removed.

FMUL, FADD and FSUB .4H/.8H now widen ordinary operands exactly to binary64,
compute the exact product or sum, and round once with the same integer
binary16 machinery as FMLA. This includes all eight indexed FMUL lanes and
scalar H arithmetic. A shared pair-rounding emitter keeps fused and ordinary
half arithmetic aligned. Scalar and vector FMIN/FMAX/FMINNM/FMAXNM select
original operands using signed integer keys; -0 orders before +0, preserving
the architecture's tie rule without FP arithmetic.

Subnormal or nonfinite inputs, FZ16/AHP and other unsupported controls retain
the interpreter. Exact products and sums can produce subnormal half results,
overflow and inexact results inline, with full sticky FPSR. No interpreter
semantics changed. Half division and scalar half fused operations remain
helpers.

Focused validation passes 743,890 complete-state cases and 796 native
instruction hashes in JIT, interpreter, helpers-only and AOT. Cases include
all sixteen RMode/FZ/DN settings, signed zeros, normal/subnormal/overflow
boundaries, aliases, all indexed lanes, inactive scalar bits and live NZCV.
The half-subtraction mutation is caught (616 state mismatches), with a
passing control. A separate 37-form ordinary workload at N=5000 removes
2,960,000 helper calls in both JIT and AOT: 1,280,000 indexed-vector,
1,120,000 other and 560,000 scalar-FP calls. JIT workload code grows from
101 to 1,854 KiB. Every output matches native.
Full-gate and cumulative coverage measurements accompany integration;
Intel performance and application acceptance remain unmeasured.

## Integer and format conversion tail

Cumulative static coverage reaches 99.7424% (4,689 additional sites), with
generated code 646,110 -> 647,368 KiB. The temporary cache was removed.

The translator now handles the allocated H/S/D vector and scalar integer
conversions, explicit scalar-to-GPR rounding modes, and scalar H/S/D format
conversions, and vector FCVTN/N2 and FCVTL/L2. Ordinary lanes use SSE conversion and exact binary16 rounding;
results remain private until every lane succeeds. NaNs, infinities,
subnormal inputs, unsupported FPCR controls, saturated integer results and
unsigned 64-bit values above INT64_MAX keep the descriptor helper. GPR-to-H
also declines magnitudes above 65536 to avoid a second rounding through D.

The new complete-state cases found an older scalar D-to-S defect under FZ:
a tiny normal input could raise IXC along with UFC, and a subnormal input
could lose IDC. Shared integer admission and result checks now restore the
original MXCSR before falling back, including zero, tiny and minimum-normal
single results. The interpreter is unchanged. The interpreter does not
implement half-vector FCVTPS/FCVTMS; those encodings remain on its existing
UNDEF path. FZ16/AHP controls remain outside the inline contract.

Focused validation: 732,770 complete-state cases with zero mismatches,
including aliases, all sixteen RMode/FZ/DN settings, sticky FPSR, limits,
NaNs, rounding midpoints and live NZCV; 759 native instruction hashes agree
in JIT, interpreter, helpers-only and AOT. The expanded 138-form workload
also agrees with native in every engine; separate N=5000 JIT and AOT runs
show 2,080,000 -> 0 helper calls (640,000 other, 1,120,000 SIMD misc, and
320,000 scalar FP helper calls removed). JIT workload code grows from 796
to 1,062 KiB. The conversion-rounding mutation is caught (1,050 state mismatches);
the unmodified control passes. Full-gate, fixed-corpus and combined
coverage measurements are recorded with integration. Intel timings and
application acceptance remain separate checks.

## Optional AES-NI translation and AOT CPU binding

Cumulative static coverage reaches 99.8541% (1,372 additional sites), with
generated code 652,861 -> 652,848 KiB. Four ordinary forms at 1,000
iterations reduce JIT/AOT helper calls from 64,000 to zero. Temporary
coverage caches were removed.

AESE/AESD/AESMC/AESIMC now translate with AES-NI only when CPUID leaf 1
advertises it. The feature choice is captured once for translation, AOT
and fault recovery. The baseline build remains x86-64-v2. ARM's initial
AddRoundKey is applied before x86's last-round transform with a zero key;
AESMC cancels the inverse byte/row transforms before a forward mixed round.
All forms preserve NZCV and FPSR and support source/destination aliases.

The generator identity is the runtime image UUID with its last byte XOR 1
for the AES profile. Runtime cache validation requires the exact current
profile, rejecting an AES cache on a CPU without AES-NI. Packaging accepts
only the two explicitly derived identities for the installed runtime; all
other altered identities remain stale. Profiles are conservatively distinct
even when a particular guest contains no AES instructions.

Focused validation passes 786,248 complete-state cases, including reserved
size/opcode neighbours, and 774 native comparison lines in JIT, interpreter,
helpers-only and AOT. The crypto guest's 687 lines and five known answers
match native in JIT, interpreter and AOT. The encoder check passes 41,003
instructions. A private CPUID-off test verifies eight helper/state cases,
matching-cache acceptance, cross-profile cache rejection and identical
instruction bytes for JIT/AOT/recovery emission without executing forced
AES instructions. The installer's 16 tests pass, including removal of
arbitrarily modified generator identities, and the full AOT payload test
passes. Four isolated ordinary AES loops at 1,000 iterations remove
64,000 helper calls in both JIT and AOT; JIT workload code shrinks from
12 to 11 KiB. Baseline and candidate outputs equal native. Full gates,
representative coverage and uninstrumented timings are recorded at integration. Intel/application acceptance is
separate; CPUs lacking AES-NI retain the interpreter path.

## Packed double arithmetic and vector square roots

Cumulative static coverage reaches 99.7956% (1,393 additional sites), with
generated code 648,653 -> 649,079 KiB. Seven ordinary forms at 1,000
iterations reduce JIT/AOT helper calls from 112,000 to zero. Temporary
coverage caches were removed.

FADD/FSUB/FMUL/FDIV .2D and FSQRT .2S/.4S/.2D now use packed SSE
arithmetic after integer admission of every active operand. Nonfinite or
tiny results restore the prior MXCSR and enter the unchanged interpreter
helper. Exact zero results remain inline. All registers are published only
after admission, and integer/SSE checks preserve live NZCV. Reserved .1D
forms retain their existing undefined-instruction path.

Focused validation passes 780,104 full-state cases with zero mismatches,
covering all 16 FPCR RMode/FZ/DN combinations, register aliases, zero signs,
NaNs, subnormals, exact tiny results and overflow. The 766-line native
instruction test matches in JIT, interpreter, helpers-only and AOT. Seven
new per-form timing cases agree with native in every engine and report zero
helper calls for ordinary operands in JIT and AOT at 1,000 iterations. The
encoder check passes 40,747 instructions including divpd/sqrtpd/sqrtps.
The mutation suppressing result admission is caught, including an exact
tiny division whose helper route matters even when its bits agree.
Integrated full gates, saved-corpus flags, representative coverage and
uninstrumented timings are recorded separately. Intel/application
performance acceptance remains unmeasured.

## Interleaved SIMD multiple-structure memory operations

Cumulative static coverage reaches 99.7845% (5,294 additional sites), with
generated code 647,368 -> 648,653 KiB. The ordinary 42-form workload at
1,000 iterations drops 672,000 load/store helper calls to zero in JIT/AOT.
Temporary coverage caches were removed.

LD2/LD3/LD4 and ST2/ST3/ST4 previously always entered the interpreter.
They now translate every allocated 64/128-bit arrangement, with no offset
or immediate/register post-index, including wrapped register lists and SP.
The generated element accesses follow the interpreter's order. Loads keep
all destinations temporary until the last successful read; stores publish
each element as accessed. Base writeback occurs only after success. No
EFLAGS-changing instruction occurs while NZCV is live.

Focused validation passes 734,408 complete-state cases with zero mismatches,
including every byte boundary in a 64-byte interleaved transfer. The native
instruction comparison passes 759 lines in JIT, interpreter, helpers-only
and AOT, including partial-fault PC/NZCV/writeback recovery with skipped and
repaired faults. All 42 memory forms in the timing/statistics guest agree
with native in every engine; a 1,000-iteration run reports zero helper
calls in JIT and AOT. The encoder check passes 40,555 instructions. Full
gates and representative coverage/timing measurements are recorded with
the integrated change.

Native ARM may leave destination vector contents UNKNOWN on a data abort,
so the native skip tests compare PC, NZCV and base writeback; complete-state
fault tests establish the interpreter's stronger destination guarantee.
Interleaved stores with multi-byte elements conservatively decline any
transfer crossing a 4 KiB boundary before the first store, preserving the
interpreter's memcpy partial-store behavior exactly. Byte stores remain
inline. Single-structure LD2/3/4 lane and replicate forms remain helpers.
Application and Intel performance acceptance remain separate checks.

## MapKit 32-byte HFA functions in the aggregate-by-value table

Maps on the target crashed (SIGSEGV writing through `rdi`) in
`MKMapRectForCoordinateRegion`, which takes an `MKCoordinateRegion` and returns
an `MKMapRect`, four doubles each. On arm64 both are HFAs in `v0-v3`; on SysV
they are MEMORY class, the argument on the stack and the result through a hidden
pointer in `rdi`. No signature described the function, so the call took the
UNKNOWN route, whose inference cannot tell a 32-byte HFA from four separate
doubles: the native code wrote its result through the guest's first argument
register value.

`src/aggspi_gen.inc` (generator output, hao-research `tools/aggspi.txt`) now
types `MKMapRectForCoordinateRegion` and `MKMapRectBoundingMapPointsWrappingMeridian`
(no header; the first fixed by a native differential, the second generated from
both slices' code) and the four public `MKGeometry.h` functions of the same shape
that no table typed: `MKMapRectIntersection`, `MKMapRectDivide`,
`MKMapRectSpans180thMeridian` and `MKMapRectRemainder`. The other public ones were
already in `sigdb_gen.inc`. The header of the copied table is unchanged; its rows
are the generator's output for the updated spec.

`tests/guest/mkgeometry.sh` compares all six and two typed controls with native
arm64 in JIT, `--interp`, helpers-only and AOT; with the previous table it fails.

Limits: `MKMapRectUnionWrappingMeridian`, `MKMapRectContainsRectWrappingMeridian`,
`MKMapRectIntersectsMapRectWrappingMeridian` and `MKCoordinatesForRegion` take or
return the same aggregates but their argument order is not determined from the
code; they stay untyped. Other frameworks' C functions with a by-value aggregate
over 16 bytes and no signature have the same failure; a census is open in
hao-research. Measured on the 26B123 development host; the target's MapKit
exports the same symbols.

## SIMD coverage: combined validation and measurements

The seven instruction groups below remove interpreter calls for ordinary
operands while retaining the original descriptor helper on every decline.
AOT generation of the same large FP/SIMD-heavy arm64 program at base 678b97a
and the combined change reports 2,332,343 blocks and 12,561,074 instructions:
98.8% -> 99.5% translated natively. Generated code grows from 625,517 to
639,290 KiB (2.2%). Only cache generation was run, and both temporary caches
were removed; these figures do not establish application performance.

The final `make check` returns zero: 83 manifest passes, one expected failure
and three expected skips, with no unexpected failures or fork type-at crash.
This includes the full five-mode oracle, sanitizer oracle, unit, AOT, guest,
host and packaging gates. The combined vector test passes 714,822 complete
state cases with full FPSR and all 16 FPCR RMode/FZ/DN settings. The encoder
check passes 40,555 encodings. The 621-line native instruction test agrees
with JIT, interpreter, helpers-only and AOT. The expanded 112-form benchmark
also agrees in every engine. Its separate statistics runs at 50,000 iterations
count 88,000,000 -> 0 helper calls in both JIT and AOT; translated workload
code grows from 291 to 726 KiB.

All four mutation controls pass and all thirteen independent mutations are
caught. The new mutations produce 1,024 mismatches for ZIP order, 756 for
ADDV width, 3,000 for FADDP pair order, 404 for FDIV result admission and
4,610 for negative register shifts. The eight existing FP32/FP16 mutations
also pass their detection checks. FDIV's mutation includes an exact tiny
result whose required fallback is checked by routing even when result bits
agree. No interpreter behavior was changed. The already-documented reserved
permute/across-lane leniencies remain confined to the existing helper path.

Replay of the identical 200,000-record simd/block/fma corpora captured on
678b97a has zero mismatches and unchanged IEEE-flag difference counts:
interpreter 1/0/0, JIT 9/4,975/0. Separate fresh-corpus oracle runs also pass;
the fixed-corpus comparison isolates flag differences from random sampling.

The following are Rosetta development-host ratios, candidate/base, from
seven interleaved uninstrumented runs per build and engine, four million
instructions per form per run. Lower ratios mean less elapsed time in this
workload. Sixteen identical operations execute per loop; the accumulator
resets every sixteen operations, and loop/reset/entry costs are included.
Inputs are fixed ordinary values. Every measured run matches the native
arm64 checksum. Timing starts after all test jobs finish; statistics run
separately. These ratios do not decide Intel performance acceptance.

FCMGT #0 .4S in AOT is retained for Intel measurement: its median ratio is
1.063. Relative to the baseline median, its baseline observations span
0.944–1.017 and candidate observations 1.006–1.069; the ranges overlap.
Its JIT median ratio is 0.972. No form was discarded on Rosetta evidence.
Intel timing and application acceptance remain unmeasured for these changes.
Other precision variants, double fused arithmetic, reciprocal estimates,
AES, BRK and SVE keep their existing paths, as scoped below.

| Instruction / arrangement (scalar suffix = indexed lane) | JIT ratio | AOT ratio |
| --- | ---: | ---: |
| `zip1_8b` | 0.010 | 0.008 |
| `zip1_16b` | 0.006 | 0.005 |
| `zip1_4h` | 0.012 | 0.010 |
| `zip1_8h` | 0.009 | 0.008 |
| `zip1_2s` | 0.016 | 0.014 |
| `zip1_4s` | 0.013 | 0.011 |
| `zip1_2d` | 0.014 | 0.013 |
| `zip2_8b` | 0.046 | 0.045 |
| `zip2_16b` | 0.009 | 0.008 |
| `zip2_4h` | 0.055 | 0.056 |
| `zip2_8h` | 0.011 | 0.010 |
| `zip2_2s` | 0.069 | 0.067 |
| `zip2_4s` | 0.016 | 0.014 |
| `zip2_2d` | 0.017 | 0.016 |
| `uzp1_8b` | 0.031 | 0.029 |
| `uzp1_16b` | 0.020 | 0.020 |
| `uzp1_4h` | 0.043 | 0.040 |
| `uzp1_8h` | 0.031 | 0.028 |
| `uzp1_2s` | 0.050 | 0.048 |
| `uzp1_4s` | 0.040 | 0.040 |
| `uzp1_2d` | 0.048 | 0.045 |
| `uzp2_8b` | 0.031 | 0.030 |
| `uzp2_16b` | 0.021 | 0.017 |
| `uzp2_4h` | 0.042 | 0.041 |
| `uzp2_8h` | 0.029 | 0.028 |
| `uzp2_2s` | 0.050 | 0.050 |
| `uzp2_4s` | 0.042 | 0.037 |
| `uzp2_2d` | 0.048 | 0.047 |
| `trn1_8b` | 0.033 | 0.030 |
| `trn1_16b` | 0.024 | 0.024 |
| `trn1_4h` | 0.039 | 0.036 |
| `trn1_8h` | 0.035 | 0.033 |
| `trn1_2s` | 0.053 | 0.050 |
| `trn1_4s` | 0.050 | 0.049 |
| `trn1_2d` | 0.048 | 0.045 |
| `trn2_8b` | 0.032 | 0.031 |
| `trn2_16b` | 0.024 | 0.023 |
| `trn2_4h` | 0.041 | 0.039 |
| `trn2_8h` | 0.035 | 0.032 |
| `trn2_2s` | 0.052 | 0.050 |
| `trn2_4s` | 0.050 | 0.045 |
| `trn2_2d` | 0.049 | 0.046 |
| `addv_8b` | 0.106 | 0.108 |
| `addv_16b` | 0.089 | 0.088 |
| `addv_4h` | 0.081 | 0.079 |
| `addv_8h` | 0.077 | 0.078 |
| `addv_4s` | 0.051 | 0.050 |
| `uaddlp_4h` | 0.046 | 0.043 |
| `uaddlp_8h` | 0.033 | 0.032 |
| `uaddlp_2s` | 0.053 | 0.051 |
| `uaddlp_4s` | 0.041 | 0.038 |
| `uaddlp_1d` | 0.059 | 0.058 |
| `uaddlp_2d` | 0.049 | 0.047 |
| `saddlp_4h` | 0.049 | 0.047 |
| `saddlp_8h` | 0.031 | 0.030 |
| `saddlp_2s` | 0.055 | 0.053 |
| `saddlp_4s` | 0.039 | 0.041 |
| `saddlp_1d` | 0.059 | 0.057 |
| `saddlp_2d` | 0.049 | 0.045 |
| `uadalp_4h` | 0.047 | 0.045 |
| `uadalp_8h` | 0.032 | 0.031 |
| `uadalp_2s` | 0.055 | 0.053 |
| `uadalp_4s` | 0.038 | 0.037 |
| `uadalp_1d` | 0.065 | 0.062 |
| `uadalp_2d` | 0.052 | 0.050 |
| `sadalp_4h` | 0.051 | 0.050 |
| `sadalp_8h` | 0.035 | 0.043 |
| `sadalp_2s` | 0.059 | 0.056 |
| `sadalp_4s` | 0.044 | 0.042 |
| `sadalp_1d` | 0.065 | 0.064 |
| `sadalp_2d` | 0.054 | 0.052 |
| `faddp_4s` | 0.102 | 0.102 |
| `faddp_2s` | 0.160 | 0.158 |
| `faddp_s` | 0.305 | 0.301 |
| `faddp_2d` | 0.160 | 0.161 |
| `faddp_d` | 0.270 | 0.269 |
| `fmaxv_4s` | 0.158 | 0.153 |
| `fminv_4s` | 0.155 | 0.156 |
| `fmaxnmv_4s` | 0.158 | 0.156 |
| `fminnmv_4s` | 0.156 | 0.158 |
| `fdiv_2s` | 0.165 | 0.163 |
| `fdiv_4s` | 0.109 | 0.107 |
| `fcmeq_zero_2s` | 0.060 | 0.056 |
| `fcmeq_zero_4s` | 0.045 | 0.038 |
| `fcmge_zero_2s` | 0.068 | 0.065 |
| `fcmge_zero_4s` | 0.048 | 0.045 |
| `fcmgt_zero_2s` | 0.994 | 0.978 |
| `fcmgt_zero_4s` | 0.972 | 1.063 |
| `fcmle_zero_2s` | 0.071 | 0.066 |
| `fcmle_zero_4s` | 0.047 | 0.046 |
| `fcmlt_zero_2s` | 0.066 | 0.064 |
| `fcmlt_zero_4s` | 0.047 | 0.048 |
| `ushl_2s` | 0.132 | 0.126 |
| `ushl_4s` | 0.196 | 0.188 |
| `ushl_2d` | 0.110 | 0.107 |
| `sshl_2s` | 0.134 | 0.129 |
| `sshl_4s` | 0.191 | 0.182 |
| `sshl_2d` | 0.131 | 0.129 |
| `fmul_s0` | 0.216 | 0.218 |
| `fmul_s1` | 0.214 | 0.216 |
| `fmul_s2` | 0.213 | 0.214 |
| `fmul_s3` | 0.216 | 0.213 |
| `fmla_s0` | 0.496 | 0.486 |
| `fmla_s1` | 0.489 | 0.486 |
| `fmla_s2` | 0.484 | 0.482 |
| `fmla_s3` | 0.487 | 0.483 |
| `fmls_s0` | 0.484 | 0.486 |
| `fmls_s1` | 0.485 | 0.490 |
| `fmls_s2` | 0.488 | 0.484 |
| `fmls_s3` | 0.487 | 0.493 |
| `fmul_d0` | 0.210 | 0.210 |
| `fmul_d1` | 0.211 | 0.209 |


## Dynamic helper groups in translated code

`--stats` counts executed interpreter calls by fixed decoder group and reports
up to twenty group/kind entries in descending call count, each with a sample
encoding. Each emitted call carries a fourth ABI argument identifying a
never-translated instruction or a runtime decline from an inline path. The
interpreter descriptor and execution semantics are unchanged. Helpers-only
translations count as untranslated; interpreter execution and whole-block
interpreter fallback are outside these JIT-helper counters. AOT uses the same
call contract and its cache remains bound to the generating runtime.

Counters and representative encodings use relaxed lock-free atomics in a fixed
24-by-2 table; the helper path allocates nothing, takes no lock and uses no TLS.
Updates occur only with statistics enabled. Printing takes an approximate
snapshot if other threads are still executing; it does not synchronize guest
execution. Decoder groups intentionally aggregate arrangements and related
operations; static site counts are separate from these dynamic call counts.

A fixed-loop test in JIT and AOT subtracts N=0 startup counts and observes exactly
1000 each of untranslated reciprocal estimates, FABD and integer ADDP,
1000 subnormal FADD declines, and no
calls for 1000 ordinary FADDs. The initial 35-form SIMD workload matches native in JIT,
interpreter, helpers-only and AOT. Before further translations it makes
27,200,000 helper calls at 50000 iterations: permutes 4,800,000; FP pairwise
4,000,000; pair-long, FP reductions, zero compares, register shifts and scalar
indexed operations each 3,200,000; vector division 1,600,000; ADDV 800,000.
No input declines occur in that ordinary-operand workload.

Validation includes the full default oracle and sanitizer oracle, make test,
all eight existing FMA mutations, and the manifest's execution checks. The
chain test now selects the summary by its `aot_blocks=` prefix rather than
taking the last statistics line; its sites/race/JIT/AOT checks pass. Replay of
the saved 200000-record simd/block/fma corpora has zero mismatches and unchanged
IEEE-flag difference counts: interpreter 1/0/0 and JIT 9/4975/0.

The base AOT measurement of a large FP/SIMD-heavy arm64 program is 2,332,343
blocks and 12,561,074 instructions, 98.8% translated natively. Only cache
generation was run; this is coverage evidence, not application acceptance.


## Inline integer reductions and pairwise long sums

ADDV now translates the five allocated byte, halfword and word
arrangements, summing modulo the source lane width and clearing every
other destination bit. SADDLP/UADDLP and SADALP/UADALP translate all
24 signed/unsigned widening and accumulating arrangements. Byte shuffles
compact the even and odd source elements, SSE4.1 sign- or zero-extension
widens them before addition, and accumulating forms add the old destination
at the wider lane width. Both sources and the old destination are read
before commit, preserving aliases; 64-bit vector results clear the upper
half. These integer-only expansions leave NZCV, FPCR and FPSR unchanged.

The native instruction guest covers every new arrangement and matches
JIT, interpreter, helpers-only and AOT (580 output lines including the
preceding permutes). The complete-state unit test passes 565,318 cases,
including random states, aliases and live NZCV. A private-source mutation changes ADDV to byte-width
addition and must produce result mismatches after an unchanged control
passes (756 state mismatches); the ZIP-order mutation remains detected
(1,024 mismatches). The encoder check passes 40,171 encodings, including
all six signed/unsigned widening operations.

Reserved arrangements remain on the existing helper path; the across-lane
interpreter's known reserved-encoding leniency is unchanged. Saturating
pairwise forms, other across-lane operations and the interpreter reference
are outside this change. Intel timing and application acceptance remain
separate checks.

## Inline SIMD lane permutes

ZIP1/ZIP2, UZP1/UZP2 and TRN1/TRN2 translate every allocated byte,
halfword, word and doubleword arrangement. ZIP uses unpack operations;
UZP/TRN use constant byte-selection masks. Both operands are loaded before
the destination is committed, including every register-alias case. A
64-bit vector result clears the upper half. The expansion uses only
integer SSE instructions and immediate masks, with no EFLAGS writes,
absolute addresses, runtime allocation or new CPU requirement.

The complete-state unit test passes 557,894 cases, including all 42 forms
with random byte patterns, aliases, live NZCV and full FPSR. The native instruction guest adds all 42
forms and agrees in JIT, interpreter, helpers-only and AOT (551 lines).
The mutation check reverses ZIP1/ZIP2 half selection in a private source
copy; the unchanged control must pass before the mutated test is accepted
as detected (1,024 state mismatches). The encoder check passes 39,787
encodings, including all unpack-high forms.

Reserved size/Q combinations do not gain a translation. The interpreter
already accepts some reserved permute arrangements, listed as a known
leniency in `tests/oracle/known.h`; its behavior is unchanged here.
Performance on Intel and application acceptance remain separate checks.

## Inline floating-point comparisons with zero

FCMEQ/FCMGE/FCMGT/FCMLE/FCMLT #0 on .2S/.4S use integer admission and
CMPPS with the correct operand order. Both signed zeros compare equal;
NaNs, infinities and denormals retain the original helper, including its
IOC/IDC and FZ/DN behavior. The old FCMGT-specific branch in packed
conversions is replaced by this shared comparison path. Accepted code
leaves EFLAGS unchanged, and the helper path materializes and restores live
NZCV through the existing convention. Narrow destinations clear the upper
half; source/destination aliases are read before the result is stored.
Half and double comparisons with zero remain helpers.

Validation: 646,726 complete-state cases pass, covering full FPSR,
all 16 RMode/FZ/DN settings, aliases, signed zeros, special inputs and
live NZCV. Directed zero/subnormal cases assert inline/helper routing.
The 530-line native instruction test passes in JIT, interpreter,
helpers-only and AOT, now under every one of those 16 FPCR settings for
all of its floating-point forms. `make test` passes, including the full
all-mode oracle with zero mismatches and 39,787 encoder checks. Intel
performance and application acceptance remain separate checks.

## Inline single-precision vector division

FDIV .2S/.4S shares the vector arithmetic input/result admission and saved
MXCSR rollback. Every active input must be zero or normal. Results near
underflow, overflow or an invalid division go to the original helper with
the pre-attempt flags. An exact zero numerator divided by a nonzero ordinary
divisor may stay inline. The .2S expansion duplicates its active pair before
DIVPS so inactive 0/0 lanes cannot raise IOC; the destination still clears
its upper half. Guest rounding/FZ and live NZCV are preserved, with no VEX,
absolute addresses, allocation or TLS. Double and half vector division
remain helpers.

Validation: 605,766 complete-state cases pass with full FPSR and all 16
RMode/FZ/DN settings. Directed ordinary and exact-tiny divisions assert
respectively zero and one helper call. All 520 native instruction-test
lines agree in JIT, interpreter, helpers-only and AOT; 39,787 encoder
checks pass. Suppressing FDIV result admission produces 464 mismatches,
including result differences and required-helper-route failures, while
the unmodified mutation control passes. `make test` passes, including the
full all-mode oracle with zero mismatches. Intel speed and application
acceptance remain unmeasured.

## Inline floating-point pairwise sums and extrema reductions

FADDP .2S/.4S/.2D and scalar S/D now select adjacent pairs with integer
shuffles, admit every active operand before floating arithmetic, and issue
one ADDPS/ADDPD under the guest MXCSR. Nonordinary inputs and results with
exponent zero, one or all ones retain the original helper. This conservative
result rule includes exact zeros; it restores the pre-attempt MXCSR before
fallback, without committing a destination or changing live NZCV.

FMAXV/FMINV/FMAXNMV/FMINNMV .4S use the architectural pairwise tree.
Admitted zero/normal inputs share the same selection; bitwise zero handling
retains +0 for maximum and -0 for minimum of opposite-signed zeros. NaNs,
infinities and denormals stay with the original helper, retaining its NaN
selection, DN/FZ and IEEE flag behavior. Scalar outputs clear all unused
lanes, aliases load operands before writing, and no absolute addresses,
VEX instructions, allocation or TLS are introduced. Half forms stay helpers.

Validation at this change: 589,382 complete-state vector cases pass,
including all 16 RMode/FZ/DN controls, aliases, specials, full FPSR and live
NZCV; directed ordinary cases also assert that no helper is invoked. All
518 lines of the extended instruction guest match native arm64 in JIT,
interpreter, helpers-only and AOT. The encoder checks 39,723 encodings.
A passing mutation control and a deliberately wrong FADDP pair shuffle
produce respectively zero and 3,010 state mismatches. `make test` passes,
including the full all-mode oracle with zero mismatches. Performance on Intel
and application acceptance remain separate measurements.

## Scalar indexed floating-point multiplication in translated code

Scalar FMUL S/D and FMLA/FMLS S by element previously always called the
interpreter. FMUL now admits zero/normal operands by integer masks, performs
MULSS/MULSD under the guest MXCSR, and commits only a finite result outside
the zero/subnormal/minimum-normal exponent region. Other results restore
the pre-attempt MXCSR before calling the original descriptor helper. Exact
zero results currently take that conservative decline as well.

Single fused forms share the vector FP32 exact-double product, TwoSum and
round-to-odd expansion, with admission and result checks limited to the
one active lane. Indexed source loading follows H:L for S and H for D;
Vm uses all five register bits, and the reserved D/L=1 encoding retains its
helper. Upper 96/64 result bits are cleared, aliases read original values,
and unused NaN lanes do not affect scalar admission. Double fused, half
precision and FMULX indexed forms remain helpers. No new CPU feature,
EFLAGS write, absolute address, TLS or allocation is used.

The vector test adds 43,008 complete-state cases spanning all indices,
high Vm, aliases, all 16 RMode/FZ/DN settings, full FPSR, random/special
values and directed fused midpoint/underflow/overflow cases. Ordinary
scalar operations with signaling NaNs in inactive lanes explicitly assert
zero helper calls. The combined 607,814-case test passes with zero
mismatches, and native instruction comparison agrees for JIT, interpreter,
helpers-only and AOT (529 output lines). `make test` passes, including the
all-mode 200,000-record oracle and 39,787 encoder cases. The shared FP32
round-to-odd, negation and vector-index mutations are caught with a passing
unmodified control. Intel performance acceptance remains separate from
this correctness scope.


## Per-lane register shifts in translated code

USHL/SSHL .2S/.4S/.2D previously called the interpreter for each operation.
They now split the vector into SSE lanes, extract only each shift operand's
signed low byte, and combine left/right candidates without writing EFLAGS.
Counts at or beyond the lane width produce zero for logical shifts and
sign fill for signed right shifts, including -128. All lanes are staged
before storing Vd, so source/count/destination aliases retain original
operands; .2S clears the upper half. Byte and halfword register shifts,
rounding and saturating forms retain helpers. No VEX, allocation, TLS or
absolute addresses are introduced.

The vector unit test checks all 256 signed-byte counts in every supported
arrangement, random ignored count bits, aliases and live NZCV (12,288 new
full-state cases). The combined 564,806-case test has zero mismatches.
Native comparison agrees across JIT, interpreter, helpers-only and AOT
(515 instruction-test output lines), and the encoder check passes 39,787
encodings. Removing negative-count negation produces 4,618 mismatches;
the unmodified mutation control passes. `make test` passes, including the
all-mode 200,000-record oracle, unit, AOT and host gates. Performance acceptance on Intel
remains separate from correctness and Rosetta observations.


## Inline FP16 FMLA/FMLS in translated code

Vector FMLA/FMLS .4H/.8H, three-same and by element, emit an SSE2/SSE4
expansion with integer admission over every active operand before floating
arithmetic. Indexed half uses H:L:M and a four-bit Vm (V0..V15), including
upper indexed lanes with a .4H destination. Aliases read the complete input
before committing a result; .4H clears the destination's upper half.
Other half operations, scalar fused forms and double precision stay helpers.

The FPCR gate matches `fp_vec16_muladd`. Products are exact binary64; RN
TwoSum detects whether their sums are exact. A nonzero error declines the
whole instruction, restores the saved MXCSR and calls the original helper
descriptor without committing results or flags from earlier pairs. Unlike
the C helper's conservative span bound, this test can admit additional
actually exact sums. It does not round to odd. Accepted zero sums receive
the guest RM sign rule. Two normal-range lanes round together using integer
masks; zero, tiny and large results use the same integer rules per lane.
All four rounding modes and cumulative IXC/UFC/OFC match the C converter.
On success, the guest MXCSR is restored with accumulated flags at commit.
No EFLAGS writes, absolute addresses, VEX/AVX, F16C, TLS or allocations are
introduced by the expansion.

The block, oracle scratch and deterministic PC-recovery buffers share a
256 KiB allowance. Maximum 96-instruction half blocks emit 244,562 to
248,594 bytes, exceeding the preceding 128 KiB limit. Unit cases cover both
signs of the operation and both forms at that length. A native guest test
faults after ninety half operations in a 96-instruction block and verifies
the exact guest PC and live NZCV recovered by re-emission, in JIT,
interpreter, helpers-only and AOT. The wider allowance does not change
architectural state at helper or AOT boundaries.

The vector unit test checks 552,518 complete-state cases, including full
FPSR, all 16 RMode/FZ/DN controls, FZ16/AHP declines, every indexed lane,
register aliases, directed rounding boundaries and a late inexact pair
following admitted pairs. The 42-form native half test has 43 output lines
including recovery; the extended instruction test has 509 lines. All four
engines match native. The encoder check covers 39,595 encodings.

Mutation testing includes a passing unmodified control and independent
FP16 rounding-increment, subnormal-shift, index-decode, exactness-admission
and product-negation mutations, alongside the three existing FP32 mutations.
Removing the exactness guard did not alter the sampled architectural
results, so the unit-only build redirects the descriptor helper through a
counter: a known inexact double sum must call it, and an ordinary exact
case must stay inline. This directly tests the admission contract without
assuming that every lost double tail changes a half result. Production
helper routing is unchanged. The general `round_pack_mode` reference and
the C helper's span test are unchanged.

Replay of the same 200,000-record fma/simd/block corpora as 46054f7 has zero
mismatches and unchanged IEEE-flag difference counts: interpreter 0/0/0,
JIT 0/14/4,963. The nonzero counts are existing differences outside these
fused-operation tests.

The final full `make check` passes, including the all-mode oracle, sanitizer
oracle, unit, AOT, guest, host and packaging gates: 78 manifest passes, one
expected failure and three expected skips, with no unexpected failures or
fork type-at crash. The mutation control passes and all eight mutations
are caught. Separate `--stats` runs of four million half operations per
form report 8,000,000 -> 0 helper calls for both JIT and AOT; the C-only
build still makes 8,000,000. Measurements use separate uninstrumented runs.

Final Rosetta comparison, median ns/FMLA .8H over seven interleaved runs per
build and engine, sixteen million instructions per form per run:

| Engine / form | Base 46054f7 | C rounding | Inline |
| --- | --- | --- | --- |
| JIT three-same | 118.621 | 55.537 | 55.502 |
| JIT by element | 121.287 | 62.234 | 60.531 |
| AOT three-same | 120.912 | 54.325 | 60.747 |
| AOT by element | 127.737 | 61.150 | 60.996 |

All runs produce the native checksum. The half accumulator resets every
sixteen operations; that and loop/dispatch overhead are included. The C-only
step was also measured independently before inline implementation (below).
The incremental inline performance remains unresolved on this host: JIT
three-same ranges are 52.666–62.424 ns for C-only and 54.535–62.688 inline;
AOT three-same ranges are 52.811–60.766 and 54.438–62.255 respectively.
The higher AOT three-same median is recorded, but overlapping ranges and
uncontrolled host/Rosetta effects do not distinguish a persistent change
from noise. Eliminating helper calls does not by itself establish a further
throughput improvement over specialized C rounding.

On Intel hardware the inline path is clearly faster. On an i5-3210M (26B131,
a bundled hao, seven interleaved runs, sixteen million instructions per form),
the median ns/FMLA .8H is:

| Engine / form | c6657a0 | C rounding | Inline |
| --- | --- | --- | --- |
| JIT three-same | 329.3 | 177.9 | 95.3 |
| JIT by element | 354.8 | 204.8 | 95.4 |
| AOT three-same | not run | 178.5 | 95.1 |
| AOT by element | not run | 204.1 | 94.6 |

Inline run ranges (93.3–102.7) do not overlap the C-only ranges
(176.3–246.8). FP32 FMLA stays at 18.7–21.3 in every build. Rosetta re-translates
the inline expansion, so its timing does not predict Intel performance.

Limits: FZ16/AHP and other unsupported controls retain the existing helper;
the interpreter does not model FZ16/AHP, so those controls are tested for
fallback equality, not native correctness. Binary64-inexact sums and
nonordinary inputs retain helpers. The expansion has substantial code size:
the normal-input timing program grows from 53 KiB to 210 KiB of translated
code. The recovery probe covers a synchronous fault after completed
operations; every possible asynchronous interruption point is not
exhaustively tested. The exactness admission is shown by helper routing,
not by a result difference: no directed double-rounding value case exists
yet. Application latency and release-image acceptance remain unmeasured.

## FP16 fused multiply-add rounding

The ordinary-input `fp_vec16_muladd` path admits only binary64-exact
products and sums, then needs one rounding to binary16. Its final per-lane
call to the general soft-float converter is replaced by a finite-input
integer converter. Normal results use a fixed mantissa shift; subnormals
use an exponent-dependent shift. All four rounding modes, signed zeros,
overflow selection, and cumulative IXC/UFC/OFC match `round_pack_mode`,
which remains unchanged. Tininess is tested before rounding, including an
inexact tiny value that rounds up to the minimum normal. Values just above
max-finite do not raise OFC unless the rounded exponent overflows.
Flags are accumulated and raised once per vector. No new allocation, TLS,
CPU requirement or floating environment access is introduced by rounding.

Validation: 2,577,664 differential rounding cases cover every finite half
interval's endpoints and rounding midpoint (and adjacent doubles), both
signs and all modes, plus random finite doubles. This exhausts boundary
classes, not every possible interior double. The 42-form native guest test
compares complete vector results and full FPSR under 16 RMode/FZ/DN settings
with JIT, interpreter, helpers-only and AOT. Replay of the same 200,000-record
fma/simd/block corpora as 46054f7 has zero mismatches and unchanged IEEE-flag
differences: interpreter 0/0/0, JIT 0/14/4,963.

Seven interleaved uninstrumented runs on Rosetta, sixteen million operations
per form per run, give these median ns/FMLA .8H:

| Form | JIT base -> C rounding | AOT base -> C rounding |
| --- | --- | --- |
| Three-same | 121.303 -> 55.694 | 120.316 -> 56.193 |
| By element | 125.644 -> 63.007 | 125.188 -> 63.106 |

The normal-input loop resets its half accumulator every sixteen operations;
this overhead is included in both variants. Separate statistics runs of
four million operations per half form report 8,000,000 helper calls in both
variants and both engines. The C change does not yet inline half operations.
The full `make check` gate passes: 78 manifest passes, one expected failure
and three expected skips, with no unexpected failure or fork type-at crash.
On Intel hardware (i5-3210M, 26B131, a bundled hao with JIT, seven
interleaved runs), the median ns/FMLA .8H falls from 329 to 178 (three-same)
and from 355 to 205 (by element); FP32 FMLA stays at 19.5 as a control.
Application acceptance is not measured. FZ16/AHP still decline the fast path; the existing full
interpreter path does not model those controls, and is unchanged here.

## Nils of standard Swift types: pointer words, single inhabitants, tag bytes, Data

hao-research's walk of the Optional fields that 26B131 applications store
(inventory of 598 guest binaries and 187 arm64e-only cache images) found the
walk refusing the nils of common standard types. Since 596461f a refusal
passes the value through unpatched (logged), so these were unconverted, and
`Data?` the one whose nil differs at the first level: a native-made nil
reads as a present `Data` in guest code, and the reverse.

Rules now (`Recipe.c` `selected`, `collect_inner`):

- Pointer-word inhabitants on every walk, published values included (before,
  existentials and class references only for guest calls of native
  witnesses): Set and Dictionary storage (nominal descriptors `$sShMn`,
  `$sSDMn`), opaque and class-bound existentials (kind and flags, as
  before), `AnyHashable` (through its box existential), Swift class
  references (value witness table `$sBoWV` or `$sBOWV`), thick metatypes
  (kinds 0x304/0x306, table `$sBomWV`), `URL` and `DateComponents` (through
  their fields). XI0 is 0 on both, XI k is k on arm64 and 2k on x86_64, so
  the first nil needs nothing and the second (`AnyObject??`,
  `DateComponents?`) gets the existing two-consumed patch. Only the planned
  word is certified: a present existential's contents and an object's
  fields are not walked, as on the witness path. On the plain walk a live
  existential needs every witness table and its conformance descriptor in
  no guest image (a guest conformance's table is arm64 code), wherever it
  sits (before, only an Optional's leaf rule checked one), and a live Swift
  object word must be a heap pointer (`tests/guest/svconformance.sh` still
  refuses its guest conformances).
- One extra inhabitant, the zero word, identical on both: UnsafeRawPointer,
  UnsafeMutableRawPointer, OpaquePointer, ObjectIdentifier and Selector, by
  nominal descriptor. Only the first Optional uses it; the next one is a tag
  byte (the existing trailing-tag rule).
- Tag-byte inhabitants (`tag_byte_xi`): an Optional whose payload's
  inhabitants come from a multi-payload enum with one extra tag byte and no
  spare bit (Enum.c `tag_byte_layout`), for example `IndexPath` (its storage
  enum: size 17, xi 0xfc) and `Optional<SecondaryText>`. The nils are the
  byte's values 255, 254, ... on both; nothing is patched.
- Translated spare-bit inhabitants (`translated_xi_enum`): an Optional whose
  payload's inhabitants come from a native enum listed in `mpenum_gen.inc`
  without flag 4, whose row applies (live XI count = the row's x86_64 one, no
  extra tag bits on either side) and gives both masks. This Optional's nil
  is the enum's XI k, k the inhabitants inner Optionals consumed; the walk
  computes XI k for both layouts (Enum.c `spare_bit_xi`, IRGen's
  getFixedExtraInhabitantValue) and recognizes either. Each word in which
  they differ is a patch: the bits found, the receiving witness set's
  (materialized with the Optional's own inject witness) and arm64's
  (`empty_patch.arm_bits`, new: guest code's encoding, which
  `witness_to_guest` now writes instead of consumed - 1). An XI with no
  arm64 encoding (k at or past arm64's count; `Data`'s 12) and an outer
  Optional's nil are refused. Foundation's `Data` (`Data._Representation`:
  spare byte 15, ff on x86_64, f0 on arm64): XI 0, 1, 2 are word 1
  ff../bf../7f.. against f0../b0../70... SwiftUI's
  `ContainerBackgroundValue.Content` is translated the same way (both its
  words: ff00..07 against f000..07).
- Guest calls of `Data`'s own single-payload tag witnesses
  (getEnumTagSinglePayload, storeEnumTagSinglePayload with one empty case)
  are intercepted like an Optional's (`payload_witnesses_locked`): guest
  generic code asks a `T` = `Data` whether its `T?` is nil through them.
  `graph()` counts an Optional with a translated nil as affected.

`tests/host/swiftlayout.sh` section N measures on both architectures the
layout and XIs 0-2 of each type and its Optional, and walks `T?` nil and
some, `T??` outer and inner nil for 20 types (80 walks); N2 walks native and
arm64 nils of `Data?`, `Data??`, a struct holding `Data?` and its Optional
(patches equal to the measured words of both), and refuses the nil of 13
nested Optionals of `Data` (XI 12). `tests/host/swiftvalue.sh` now
expects `AnyObject??` (beside an affected enum) walked, with its second nil
a patch. On main c6657a0's libhao 69 of the 80
N walks were refused (`AnyObject?`, `Box?` and `URL?` nil and some and the
outer nils of the raw-pointer types' `T??` were walked); 67 of them walk
now, `DateComponents??` outer and inner nil stay refused (three consumed
inhabitants, past the bound). All 7 N2 walks were refused; 6 walk now. `tests/guest/optwitness.sh` runs `Data??` nil, some(nil) and a
value through native Optional witnesses and `Data`'s own: on c6657a0 a
Data-only build of it gave 2 lines that differ from native arm64 (a guest
nil read as present by native code); now all 179 lines of the test equal
native arm64 under JIT, `--interp` and AOT.

`src/mpenum_gen.inc` is now hao-research's r3 (e0c0f3d), row format 2
(`HAO_MPENUM_ROWS 2`): the r2 counts and flags plus arm64's XI count, both
architectures' extra tag bits, and for rows without flag 4 both common
spare-bit masks. `Tables.h` `mpenum_row` carries them.

Remaining limitations:
- Data?: only the nils are translated; a `Data??` deeper than 12 Optionals
  has no arm64 encoding and is refused. Other native single-payload types
  whose XI0 differs have no witness interception of their own tag witnesses
  (only Data's); their Optionals are translated when walked.
- Rows with flag 4 carry no masks: their XIs are identical, but an
  Optional of such an enum is still not planned (refused, passed through).
- Measured on the development host (26B123 runtimes), not on the target.

## Multi-payload enum rules from generated tables, and guest spare-bit enums

The Swift value walk (`src/swiftvalue/Recipe.c` `collect`, `Enum.c`
`discover_fixed_enum_case`) refused compiler-laid-out multi-payload enums
with several empty cases behind an extra tag byte (rule C allowed one), with
spare-bit tags outside bits 60-63 of a word or with any empty case (rule B),
and every guest-defined spare-bit enum (rule B needs native witnesses). A
refusal at publication ends the process; on a guest call of a native
generic witness it leaves the value unconverted, so an outer nil beside the
enum reaches the native witness in the arm64 encoding (`swift_retain(0x1)`).
Rule B also admitted an enum whose payload layout differs by architecture as
long as its x86_64 tags lay in bits 60-63, and every rule admitted a frozen
native type declared differently on arm64 inside a guest-laid-out value.

Two generated tables for macOS 27.2.1 (26B131, from hao-research; stamp in
`include/hao/gen_stamp.h`, format 1) are looked up by the defining image's
LC_UUID and the type descriptor's offset from its mach header
(`src/swiftvalue/Tables.c`, a per-descriptor cache). A row matches only the
image build it was generated from: on another build every lookup misses and
the rules fall back to what they do without a row. They are regenerated per
OS build.

- `src/mpenum_gen.inc`: 2525 native spare-bit multi-payload enums with
  payload and empty case counts, the x86_64 XI count of the spare-bit
  layout, and flags (1 payload-case encodings identical on arm64 and x86_64,
  2 empty-case ones too, 4 XIs identical): 1442 rows 1|2|4, 987 rows 1|2,
  91 rows 0 and 5 rows 4.
- `src/archdiff_gen.inc`: 43 native structs and enums whose representation
  differs from the arm64 build's type of the same image and name: fields or
  case indices differ (arm64-only `Float16` cases and fields, `Swift.Float16`
  and `Swift.Float80` themselves), arm64 adds trailing cases, or the
  multi-payload layouts differ (`ComputationalGraph.Output.Errors`: a tag
  byte on x86_64, spare bits on arm64).

Both are hao-research's r2 exports (6dff5fb), after its x86_64 slide-info
v2 parser fix (r1 had 1899 and 36 rows; no r1 row changed); mpenum_gen.inc
is since r3 (row format 2, below).

Rules now:

- Declared differently (`Recipe.c` `holds_archdiff`, checked first for every
  type the walk meets and in `selected()`): a listed type counts only where
  guest code can lay its bytes out, that is when its module froze it
  (`frozenstruct_gen.inc`, by qualified name). No listed type is defined in
  a guest image (the rows are x86_64 images'), and per hao-research's
  reachability check of 26B131 no guest image or application references any
  of them except `Swift.Float16` and `Swift.Float80`, the two frozen ones.
  A resilient listed type is held by guest code only through its native
  witnesses, its bytes are x86_64 throughout, and it is walked by its native
  layout as before. A value whose type is a frozen listed type, or holds
  one inline where guest code lays it out (struct fields, tuple elements,
  Optional payloads, an enum's inline payload cases, at most 16 levels and
  64 fields a level), is refused under every rule, rule D included. The
  search and the walk stop at a native struct or enum its module did not
  freeze (`native_opaque`): inside one everything is native-laid-out. So a
  guest container refuses where it would descend into such a field, and
  also for an inactive Optional payload or enum case it does not walk.
- C, any number of empty cases (layout only, guest types too): size =
  largest payload + 1 and xi = 256 - (payload cases + empty-case tags), where
  the empty cases take one tag when the payload area has 32 bits or more and
  ceil(empty / 2^bits) tags otherwise; at most 256 tags. A tag byte below the
  payload cases is walked as before. A tag byte for empty cases is admitted
  with nothing to walk when its empty case, (tag - payload cases) * 2^bits +
  the index in the payload's first (at most 4) bytes, is below the empty
  case count. Any other tag byte, size or xi is refused.
- B by table (native witnesses only): with a row for the type whose XI count
  is the live one, flags 1 and 2 admit the type wherever its tag bits lie and
  whatever its empty cases (size up to 128 bytes, any alignment; stride at
  least size). A row without flag 1 or 2, or with other case counts, refuses
  it even where the bit-60-63 test passes. A row with another XI count is
  ignored (the type's payloads are not loadable and it has a tag byte).
  The mechanics are B's: tag bits are the union of the native injections of
  each payload case and of empty case 0 (index 0, so only its tag) into
  zeroed storage, each read back with the native `getEnumTag`; the live
  case is the native `getEnumTag`; an empty case has nothing to walk; a
  payload is walked in a copy with the tag bits cleared, and a patch over a
  tag bit refuses. Without a row, B is unchanged.
- Guest spare-bit enums (`Enum.c` `guest_spare_bit_case`, no witness run):
  for an enum whose descriptor lies in a guest image, the common spare-bit
  mask comes from that image's `__swift5_mpenum` record. From the mask and
  the case counts the arm64 layout follows as IRGen computes it
  (GenEnum.cpp: empty-case tags as for C over the non-spare bits; tag bits
  all spare bits or the highest ones; empty case index in the non-spare bits
  from the lowest; XIs 2^spare bits - tags, at most 0x7fffffff). It is used
  only when the enum has no extra tag byte (size = largest payload) and the
  predicted XI count equals the live value witness table's. The case is
  gathered from the value's tag bits; a tag past the cases (an XI) or an
  empty index past the cases is refused, an empty case has nothing to walk,
  a payload case's payload is walked in a copy with the tag bits cleared.
  A tag that also needs extra tag bits is refused.

`tests/host/swiftlayout.sh` (section E) measures in native arm64 and x86_64
programs: `FiveEmpty` (Int / (Int, Int) / five empty cases: size 17, xi
0xfd, empty cases tag 2 with index 0-4 in the first payload byte), tags in a
`Bool`'s spare bits (`Low2`: byte 8 bit 7; `Low3E2` with two empty cases,
bits 6-7, empty case 1 index 1), and four native types of the host whose
26B131 rows apply (the host is 26B123; 1698 of the 2525 rows' images are the
same binaries here): `NWListener.State` (5 bytes, tags in bits 38-39, three
empty cases), `LocalizedStringResource.BundleDescription` (bits 62-63, one
empty case, XIs differ), AppIntents `ResizeBehavior` and `ZoomBehavior` (9
bytes, tags in an `Optional<Double>`'s tag byte). Every injected case of
each is the same on both architectures. Over the x86_64 values, 22 of the
94 walk verdicts of sections A-E changed: the C enums with two and five
empty cases (11 values, refused before), the AppIntents and
BundleDescription values (8, refused before; `NWListener.State` was already
walked by rule D), and the frozen listed `Swift.Float16` (0 bytes on
x86_64; walked before by rule D, alone, in a tuple and as an `Optional`
nil). The resilient listed `ComputationalGraph.Output.Errors` (with a
`String` payload) and `Feature.DataType` (1 byte) are walked as before by
rules C and D, also in a tuple, and `Float16` inside a native struct the
module did not freeze is walked. 18 fixed-enum cases with explicit rows (the exported
`hao_sv_discover_fixed_enum_case_row`) show: a row without flag 1, or with
flag 4 or 1 only, refuses the 16-byte enum rule B admits without one; flags
1|2 admit spare-bit enums with an empty case and Bool-byte tags that rule B
refuses; a row of another XI count is ignored; the 17-case enum (whose
measured cases differ, so its true row has flags 0) stays refused. 18 table
lookups check rows by UUID and offset, including the flags-0 FontParser
`GlyphMap` row and the first and last rows, and the script checks both
tables are sorted. Still refused: a bad tag, an empty index past the cases,
xi - 1.

`tests/guest/optwitness.sh` now also copies, assigns and drops tuples of
guest enums and a `C??` through the native tuple witnesses: a String-payload
tag-byte enum with two empty cases and an Int-payload one with five (rule
C), and spare-bit enums decoded from the guest record (class-reference
tags in bits 62-63 with an empty case, tags in a `Bool`'s spare bits with
two empty cases, and a 17-case enum whose arm64 tag uses bit 2). Before the
change JIT, `--interp` and AOT all crashed in `swift_retain(0x1)`; with only
the guest-record rule removed they still crash at the spare-bit enums; now
all 162 lines equal native arm64.

`tests/guest/svarchdiff.sh` (new) calls the walker from an arm64e guest:
a guest struct with a `Float16` field and an `NSObject??`, `Float16` itself
and an `Optional` nil of the struct are refused (the previous libhao walked
the first two), the same struct with an `Int16` field is walked with one
patch. Its inline-compiled `Float16` values equal native arm64.

`Swift.Float16` under hao, apart from the walk: libswiftCore's x86_64
`Float16` metadata has size 0 (no stored field), and guest code binds its
uses of the type to that native metadata. Inline arithmetic and the guest's
own struct layouts (size 2, offsets as on arm64) are right; anything that
goes through the native metadata is not. Under JIT and `--interp`, generic
`MemoryLayout<Float16>.size` is 0, a `Float16` printed through its native
description prints `Float16()`, and a generic copy through the native
witnesses copies nothing (`copied(1.5)` returned 2.25, a stale value).
The walk refusal does not cause this and does not fix it; the walk of a
guest struct holding `Float16` would itself be sound (the field holds no
pointer and the struct's offsets are the guest's). Not addressed here.

Remaining limitations:
- The tables describe 26B131. On another build a native type finds no row:
  B falls back to the bit-60-63 rule, and a type declared differently by
  architecture there is not refused.
- A listed type counts as guest-laid-out only when frozen; a resilient one
  is trusted to reach guest code only through native witnesses (true of
  every listed type on 26B131 by hao-research's reachability check, not
  checked at run time). They are measured on this host's
  26B123 runtimes for the images that are the same binaries, not on the
  target system or Intel hardware.
- A native C enum (no table: layout only) whose payload layout differs by
  architecture is walked by its x86_64 layout; that is right while guest
  code holds it only through native witnesses (`Output.Errors` on 26B131).
  `archdiff_gen.inc` lists the declaring types; the walk finds containers of
  a frozen one by containment.
- Unpaired native spare-bit enums (no row) keep rule B; their XIs and
  `Optional`s are still refused. Optional nils of listed enums whose XIs
  differ are translated since (section above).
- Guest spare-bit enums: the layout is IRGen's as modelled, checked only
  through the XI count, the size and these test enums; a layout that needs
  spare bits and extra tag bits, and a generic (runtime-laid-out) guest enum
  without a record, stay refused.
- An outer nil inside a guest enum's payload (`b(C, C??)`, either guest
  rule) is patched to the native encoding for a native tuple witness, which
  then hands the enum to the guest enum's own arm64 witnesses: the copy
  crashes (also before this change, for a guest tag-byte enum). Not covered
  by a test.
- The containment search for listed types stops at 16 levels and 64 fields;
  what lies beyond is not searched.

## Per-entry jump-cache base in translated code

Each translated indirect branch previously loaded `cpu.rt_jc`, then its
selected cache slot, then the entry PC and code. The cache mapping is
per-thread and stable for the lifetime of an entry and its chained blocks.
The entry stub now loads its base into host R12, which was already saved
and restored by that stub; the hot branch reads the slot directly through
R12. This removes one dependent memory load per BR/BLR/RET. The slot's
full-PC comparison, miss probe, PC store and publication rules are unchanged.
Helpers preserve R12 by the host ABI. Nested callback/signal entries save
and restore it, and a new dispatcher entry binds and reloads its own cache.
No guest integer/vector register is moved out of `hao_cpu`, no new fault
recovery location is introduced, and AOT code uses the same entry contract.

`state_bench` covers two alternating return sites with compact and
8-KiB-spaced table targets, BR tail dispatch, a single return site, and
short arithmetic blocks around XPACI. The helper loop has ordinary guest
register loads/stores around the call, not a full CPU-state flush. Native
arm64 and JIT/interpreter/helpers-only/AOT results agree. Separate `--stats`
runs count eight startup helper calls in the call/return workloads and
N+8 in the XPACI workload, both before and after. The host dispatcher is
not entered per hot call: its block count stays at the startup-scale count
while the iteration count grows.

Two-second, 1-ms `sample` captures on Rosetta put 1,016/1,463 main-thread
samples before and 1,041/1,483 after in `jit_helper` and its callees for the
short helper loop. Call/return samples stay in generated Rosetta code;
those addresses do not identify individual hao x86 state moves. AOT
inspection confirms the repeated cache-base load and the existing
per-instruction register stores. Guest-register copying and C dispatch
have not been removed. Cross-block guest-register caching and reduced
helper state materialization are not implemented: the profiles did not
establish a benefit that justified new signal/fault recovery rules.

Performance is provisional. Repeated Rosetta measurements vary materially
between processes, including the helper loop whose hot path this change
does not alter. The results do not establish either a general speedup or
a regression. The implementation is retained for review on the strength
of the completed correctness checks; further performance validation is
needed. Alternative cache-base and helper-pointer register sequences were
also measured, without a reliable basis for selecting them.


One measurement set after the test workload stopped: median ns/iteration,
seven interleaved runs, 100 million iterations per pattern, without
`--stats`. These are observations, not acceptance thresholds or established
performance effects.

| Pattern | JIT before -> after | AOT before -> after |
| --- | --- | --- |
| BLR, two return sites | 5.799 -> 5.675 | 5.957 -> 5.834 |
| BLR, spaced targets | 5.784 -> 5.803 | 5.971 -> 5.562 |
| BR tail dispatch | 5.163 -> 5.675 | 5.683 -> 5.706 |
| XPACI helper blocks | 9.945 -> 9.603 | 10.114 -> 9.709 |
| BLR, one return site | 5.783 -> 4.941 | 4.814 -> 5.466 |

For example, the seven baseline single-return AOT observations range
from 4.389 to 7.586 ns; the candidate ranges from 5.266 to 5.715 ns.
Uncontrolled host/Rosetta effects prevent interpreting the median
difference alone as a regression.

Validation includes the full all-mode oracle and sanitizer oracle,
`make check`, native equality in all five benchmark patterns, and
`signals.sh`, `uresume.sh`, `threads.sh`, `fullcache.sh`, `chain.sh` and
`jitlock.sh`. The manifest reports 76 passed, one expected failure and
three expected skips, with no unexpected failures or fork type-at crash.
Intel hardware, application frame latency and release-image
acceptance remain outside this measured scope.

## Inline FP32 FMLA/FMLS in translated code

Vector FMLA/FMLS .2S/.4S, three-same and by element, now emit the
`fp_vec32_muladd` fast path in `src/jit.c`. Integer admission precedes FP
arithmetic for every active lane. Exact binary64 products, an RN sum,
TwoSum error recovery and an integer round-to-odd step precede one
`cvtpd2ps` per pair under guest rounding/FZ. FMLS flips the multiplicand
sign exactly. The indexed single form uses H:L for the lane and M:Rm for
V0..V31; half precision's restricted Vm encoding is not reused for S.
A .2S result clears the upper half. Nonordinary operands, unsupported
FPCR controls and tiny/zero results retain the existing helper descriptor
and restore the pre-attempt FP environment. Scalar, half and double fused
forms remain helpers. No VEX/AVX instructions or absolute addresses are
emitted; integer branches preserve live NZCV and deterministic re-emission.

The common RN path saves MXCSR but does not reload it; other rounding
modes use the second word of the entry stub's eight-byte slot for RN and
restore guest RC before conversion. The block, oracle scratch and PC
recovery buffers have a common 128 KiB allowance: a maximum 96-instruction
block of these expansions exceeded the previous 64 KiB oracle scratch.
`--stats` adds `helper_calls` for executed interpreter-helper calls, enabled
only when statistics are requested; `helper` remains a translation count.

Validation: the vector unit test checks 303,682 complete-state cases,
including all 16 FPCR settings, full FPSR, directed near-ties, aliases,
every indexed lane, high Vm and maximum-sized blocks. Independent mutations
of the odd step, product negation and H:L decode produce 11,078, 46,930 and
37,404 mismatches respectively. The encoder check covers 39,307 encodings.
`fma32.sh` (226 output lines) and extended `jitinsns.sh` (489 lines) compare
native arm64 with JIT, interpreter, helpers-only and AOT. The million-record
fma/simd/block corpora replay with zero mismatches and unchanged IEEE-flag
difference counts versus 2207f5e: interpreter 0/0/0, JIT 0/53/24,592.
The nonzero counts are existing differences, not new fused-operation
exceptions. Full runtime gates include the all-mode oracle, sanitizer
oracle, unit, AOT, guest, host and packaging checks.

Median ns/FMLA on Rosetta, seven interleaved runs per variant, sixteen
million instructions per form per run, without `--stats`:

| Form | JIT before -> after | AOT before -> after |
| --- | --- | --- |
| Three-same .4S | 23.385 -> 13.123 | 23.228 -> 13.153 |
| By-element .4S | 25.071 -> 13.128 | 24.999 -> 13.047 |

Both runs use the same normal inputs and produce the native arm64 checksum.
The final runtime gate passes with only the documented skips and expected
failure; no `test_fork` type-at crash was observed. A fresh parallel build
exposed a test dependency race (`jitlocktest` started before `hao` existed);
its Makefile prerequisite now includes the executable.

The normal-input benchmark executes sixteen fused operations per loop.
Separate `--stats` runs of four million operations per form report
8,000,000 -> 0 executed helper calls for both JIT and AOT (the baseline
was instrumented with the same counter only). JIT translated helper sites
fall from 64 to 0; AOT execution has no JIT-site counts. The benchmark's
translated code grows from 6 KiB to 48 KiB, including fallback paths.

Limits: the arithmetic is still a multi-instruction SSE2 expansion, with
substantial code size and a helper for cancellation, tiny values and
special operands. Measurements are on the Apple silicon development host
running hao under Rosetta, not on Ivy Bridge. Instruction-cache effects
in larger applications and application/release-image acceptance are not
established by these microbenchmarks or runtime gates.

## Swift values with an Array-supplied nil, spare-bit enum tags or an extra tag byte

The Swift value walk (`src/swiftvalue/Recipe.c` `selected`/`collect`, run
on a published value and on a guest call of a native value witness) refused
three shapes, and a refusal at publication ends the process:

- An `Optional` of a struct whose extra inhabitants come from an `Array`
  field. Swift takes a struct's inhabitants from its first field with the
  largest count, and an `Array` ties with most pointer-sized fields and
  comes first. `selected()` planned an `Array` buffer word only for an enum
  Element (any Element on the witness path).
- A compiler-laid-out multi-payload enum whose tag is in the payloads'
  spare bits, for example `SwiftUI.ContainerBackgroundValue.Content`
  (16 bytes, flags 0x210007; a class-reference word, or two of them).
- A compiler-laid-out multi-payload enum with an extra tag byte, for
  example `String` / `(String, String)` / one empty case (33 bytes, stride
  40, flags 0x2230007, xi 0xfd).

`enum_payloads()` accepts only runtime-laid-out enums (the runtime's
multi-payload tag witness and a payload-size word in the metadata), which
the two enum shapes do not have.

Measured in one program built natively for arm64 and for x86_64
(`tests/host/swiftlayout.swift`, arm64 natively and x86_64 under Rosetta
on the development host):

- `Array<E>` is one buffer word, size 8, flags 0x10007, xi 0x7fffffff, for
  E a struct, an enum, a Swift class, `AnyObject`, `NSObject` and `Any`.
  XI0 is 0 on both; XI k is k on arm64 and 2k on x86_64. `Optional<S>.none`
  of a struct whose first field is the `Array` writes only that word.
- Spare-bit tags: for the 16-byte enum above (and its SwiftUI original)
  injecting case 0 into zeroed storage writes nothing and case 1 sets bit 63
  of word 1 on both; live values are `[p, 0]` and `[p, 0x8000000000000000|p]`
  on both. A five-case enum of one class reference uses bits 61-63 on both.
  A 17-case one uses bits 60-63 and 2 on arm64, 59-63 on x86_64. The empty
  encodings always differ: xi 0x3ffe against 0x3ffffe for the 16-byte enum,
  XI0 words f000000000000007 against ff00000000000007.
- Extra tag byte: payload cases leave the payload as it is and write the
  case index into byte 32; the empty case is a zero payload with tag 2; the
  inhabitants are tag bytes ff, fe, ... Identical on both, live values too.
  With two empty cases xi is still 0xfd (they share tag 2 and keep their
  index in the payload area).

Rules as implemented:

- A: the `Array` branch of `selected()` admits any Element on both paths.
  It plans the buffer word's empty encodings only. A live `Array` is not
  walked: its elements are still not certified, and an outer nil inside an
  element is no patch of the enclosing value.
- B (`Enum.c` `spare_bit_case`): an enum whose field descriptor is
  MultiPayloadEnum, with 2 to 64 direct payload cases that all resolve and
  no empty case; size a multiple of 8 up to 128 bytes, equal to its stride,
  alignment 8; every value witness (0-7, 11-13) native, in no guest image
  or thunk. Each payload case injected with the native witness into zeroed
  storage must read back as that case and set no bit outside bits 60-63 of
  a word. The live case is the native `getEnumTag`; that case's payload is
  walked as its type from a copy with every injected tag bit cleared
  (`destructiveProjectEnumData` is never run on the live value). A patch
  inside it that would overwrite a tag bit refuses the value.
- C (`Enum.c` `tag_byte_case`): an enum whose field descriptor is
  MultiPayloadEnum, with 2 to 64 direct payload cases that all resolve and
  at most one empty case; size = largest payload + 1 and xi = 256 - (payload
  cases + empty cases), which in Swift's layout means no spare bit carries
  the tag. The byte at the largest payload's size is the case: a payload
  case's payload is walked as its type at offset 0, the empty case needs
  nothing, any other byte is refused. This is layout only, so it applies to
  a guest enum whose witnesses are arm64 code as well.
- D (`Recipe.c` `collect_inner`): a value of fewer than 8 bytes is walked
  with no patch and no verdict on its case. A patch rewrites a pointer word,
  and such a value holds none. Before this rule every enum of that size
  other than a payload-free one was refused, for example a 1-byte guest
  enum with payload cases (flags 0x2200000) inside a SwiftUI value, and an
  `Optional<Bool>` published as a result. Either refusal ended the process.
  `swiftlayout` measures a single-payload enum (1 byte, xi 0xfc), a Bool
  multi-payload enum (2 bytes), a 6-byte struct of them and an `Optional` of
  the first. Each is identical on both architectures. Those 8 walks and 2
  of `Optional<Bool>` were refused before this rule.
- F (`Recipe.c` `function_value`): a function value other than a block, under
  any number of `Optional`s, is walked with no patch. Measured in native
  arm64 and x86_64 programs: a Swift closure is a code word and a context
  word (16 bytes, flags 0x10007), a thin or C function one code word; xi
  0x7fffffff, XI k is code word k on both, context untouched, and each
  `Optional` consumes the next. Its captures live in a separate heap object.
  A block is an ObjC reference whose XI k is k on arm64 and 2k on x86_64, so
  it is excluded. Before, every `Optional` of a closure was refused, for
  example `Optional<(Bool) -> ()>` in a SwiftUI value, which ended the
  process. `swiftlayout` section F: 8 walks refused before, all pass.
  An `Optional` of a struct or tuple whose extra inhabitants come from such
  a function (the first field with the most, down to the function's code
  word, each step checked against the value witness tables) has the same
  nil on both too: the code word equal to the inhabitants its payload
  already consumed (`function_xi_word`). Covers closure fields of every
  kind measured (`@MainActor`, `inout` parameters, generic and `Optional`
  results, `@Sendable async throws`).
- G (`Common.c` `c_integer_enum`): a Clang-imported enum (`__C`, foreign
  metadata, the witnesses of `Int8`/`Int16`/`Int32`/`Int`) is its integer.
  It has no extra inhabitants and holds no pointer word. This rule was
  limited to guest-defined descriptors, so a native framework's imported
  enum (`ComparisonResult`, `NSLineBreakMode`, a `UIInterfaceOrientation?`
  from native metadata) was refused. `swiftlayout` section G: 5 walks
  refused before.

Refusal policy (`swiftvalue.c` `decline`): a value the walk cannot certify,
and the ownership cases it cannot carry out, pass through unpatched by
default. The native witness runs on the guest's bytes, and a published result
stays as the guest wrote it. The first time a (type, reason) pair occurs in a
process, hao prints three things under the stable prefix `Swift value
survey:`: the value's innermost refusing types (`refused at offset N depth D:
... type=...`, from a second walk in survey mode), the schema of the value,
and one `passed through unpatched: REASON (... type=...) root=... guest lr=...
pc=...` line. Up to 1024 pairs are reported per process. This is the owner's
choice: refusing by default hides what ARM apps send.

The cost is that a nil in the guest's encoding inside such a value can make
native code misbehave or crash later, away from the boundary. Read a crash
after a survey line with that in mind. `HAO_SWIFT_VALUE_STRICT=1` (open tier
only, no boot argument; for tests) restores the refusal: the same lines, then
the `Swift value boundary refused:` line and abort. An activation lifetime
mismatch, an internal invariant, still aborts. Not covered by a guest test:
the publication and witness paths are reached from SwiftUI/AttributeGraph on
the target. `swiftlayout` section S checks the survey walk itself (a value
with two refusing fields prints both).

Laptop (26B131, MBP9,2, live swap): Weather.app, which aborted at
`Swift.Optional<(Swift.Bool) -> ()>` under the refusing default, now passes
`Optional<SwiftUI.Binding<Swift.Bool>>` and
`Optional<SwiftUI.Binding<Optional<SwiftUI.SwipeActionResultToken>>>` through
with survey lines and runs about 50 s. It then aborts in native code
outside hao: VFX → `-[MTLIOAccelDevice newIndirectArgumentEncoderWithLayout:]`
in the image's Metal driver. Its window has not been shown yet, so whether
the passed-through values render correctly is still unconfirmed.

Both enum rules are used by the walk only. The ownership transfers in
`Aggregate.c` and `graph()`, which picks the types whose native witnesses a
publication intercepts, still follow only runtime-laid-out enums.

`tests/host/swiftlayout.sh` (new in `tests/check.list`) checks the
measurements on each architecture, that every other measured line is equal
on both, and 46 walk verdicts over the x86_64 values. Before the change 36
of them were refused: every `Optional` of an `Array`-first struct except
an enum Element, every enum value of shape B and C, nil patches inside
their payloads included. Still refused, as expected: the 17-case enum, an
`Optional` nil of the B enum and of its SwiftUI original, a spare-bit enum
with an empty case, the extra-tag enum with two empty cases, copies of its
metadata with size + 1 or xi - 1, and a tag byte past the cases.
`tests/guest/optwitness.sh` now also copies, assigns and drops tuples of the
C-shaped guest enum and a `C??` through the native tuple witnesses (arm64e
guest under JIT, `--interp` and AOT against native arm64). The walk must get
through the enum to give the `C??` outer nil its native encoding. Before the
change all three modes crashed in `swift_retain(0x1)`; now all 111 lines are
equal.

Remaining limitations:
- Measured on the development host's native Swift runtimes (arm64, and
  x86_64 under Rosetta), not on the target system or Intel hardware.
- An `Optional` of a B or C enum is refused. For B its nil encodings
  differ. For C they are tag bytes that are the same on both, but not part
  of this rule.
- B: a guest-defined spare-bit enum (arm64 witnesses), an empty case, a tag
  that needs a bit outside 60-63 and an enum over 128 bytes are refused.
  The tag-bit patch refusal is not exercised: the payload words that held a
  nil in the measured enums had no spare bits, so no tag landed in them.
  (Changed since: table rows, guest records; see "Multi-payload enum rules
  from generated tables" above.)
- C: only one empty case was measured, and more are refused. (Changed
  since: any number; same section.)

## Table lookups, EXT, integer multiplies and round-to-integral in translated code

Translated code ran TBL/TBX, EXT, the integer vector multiplies (MUL, MLA,
MLS, vector and by element) apart from `MUL .8H/.4H`, every FRINT* (vector
and scalar), the 64-bit lane compares (CMGT, CMGE, CMHI, CMHS, CMTST and the
compares with zero on `.2D`), CMHI/CMHS on 32-bit lanes, SMIN/SMAX/UMIN/UMAX
other than signed halfwords and unsigned bytes, and ABS through the
interpreter helper. None of them has an SSE2 form; all have one in SSSE3,
SSE4.1 or SSE4.2, which hao now requires of the CPU (x86-64-v2, checked at
startup by `src/main.c`; no VEX encodings are used).

`src/jit.c` now emits them inline (encoders `x_sse38_rr`, `x_sse3a_rri` in
`src/x86_emit.c`):
- TBL/TBX with one to four table registers, `.8B`/`.16B`: one PSHUFB per
  table register on the indices minus 16k, a saturating add of 0x70 setting
  bit 7 (zero result) for indices outside that register; the results are
  ORed. TBX keeps Vd's byte where the index is at least 16 x len.
- EXT: PALIGNR on Vm:Vn; the 64-bit form rotates the two low halves joined.
  EXT with Q = 0 and imm4 >= 8 is reserved: the interpreter returned
  uninitialized bytes of its staging buffer there and now reports UNDEF as
  hardware does (the oracle's leniency entry for it is removed).
- MUL/MLA/MLS on bytes (two PMULLW on the even and odd bytes), halfwords
  (PMULLW) and words (PMULLD); MUL/MLA/MLS by element on halfwords and words.
- CMEQ/CMTST/CMGT/CMGE (PCMPEQQ/PCMPGTQ) and CMHI/CMHS (the same after
  flipping the sign bits) on `.2D`, CMHI/CMHS on `.4S`/`.2S`, the compares
  with zero on `.2D`; SMIN/SMAX/UMIN/UMAX in every lane size (PMINSB, PMINSD,
  PMINUW, PMINUD and the maxima); ABS (PABSB/W/D; `.2D` as (x ^ s) - s).
- FRINTN/M/P/Z/X/I and FRINTA, vector `.2S`/`.4S`/`.2D` and scalar S/D: the
  fast path of `fp_rint`. Operands with any lane that is subnormal, infinite
  or NaN go to the helper before any FP instruction runs, with nothing
  raised (FZ/IDC, FPProcessNaN and DN stay the interpreter's). For zero and
  normal lanes ROUNDPS/ROUNDPD is FPRoundInt exactly: the fixed modes for
  N/M/P/Z, MXCSR.RC (the guest's FPCR.RMode) for X and I, PE (IXC) only for
  FRINTX. FRINTA (ties away) is trunc(x) plus copysign(1, x) when
  |x - trunc(x)| >= 1/2, else plus x's signed zero; each step is exact for a
  normal x, so it raises nothing. No MXCSR save/restore is needed: a
  declined operand reaches no FP instruction.

Measured on the Apple silicon development host, where hao's x86_64 code
runs under Rosetta (not on Intel hardware), so these are ratios, not Intel
timings. `tests/guest/vbench2.c` (2000 repetitions, best of five, host
under load from other jobs):

| Kernel | Before, JIT | After, JIT | Before, AOT | After, AOT |
|---|---:|---:|---:|---:|
| TBL/TBX/EXT (`shuffle`) | 0.095 s | 0.006 s | 0.099 s | 0.005 s |
| MUL/MLA/MLS (`imul`) | 0.394 s | 0.020 s | 0.411 s | 0.020 s |
| FRINT* and `.2D` FP (`round`) | 2.010 s | 0.782 s | 2.074 s | 0.778 s |
| compares, min/max, ABS (`cmpmm`) | 0.700 s | 0.020 s | 0.730 s | 0.018 s |
| total | 3.22 s | 0.83 s | 3.31 s | 0.82 s |

Its static helper instructions went from 178 to 46 of 985 and its dynamic
helper calls (400 repetitions) from 20.1 M to 2.87 M, all of the remainder
double-precision vector FADD/FSUB. `tests/guest/fpscalar -b NAME 300000`
(ns per instruction under the JIT, best of three): `frintn v.4s` 31.7 to
3.6, `frintx s` 17.4 to 4.1, `frinti s` 16.1 to 3.7, `frintm d` 16.7 to
3.8, `frinta s` 18.7 to 8.1, `frinta v.2d` 23.9 to 7.3. `tests/guest/vbench.c`
is unchanged (0.39-0.40 s, JIT and AOT).

Validation: `tests/aot/jit_vector.c` (in `make unit`) now runs 200,000 cases
(120,000 before), adding the integer three-same compares/min/max/multiplies,
the compares with zero and ABS, TBL/TBX (indices mostly in or near range)
and EXT including the reserved encodings, the by-element multiplies with
their neighbours, and FRINT* vector and scalar (all keys, reserved ones
included) over values around the integers in single and double, alone,
before and after single-precision vector FP (the constants one leaves for
the next), under all 16 RMode/DN/FZ settings: translator and interpreter
agree on registers, NZCV, the full FPSR and memory. `tests/guest/jitinsns.sh`
compares 89 more lines (477) with native arm64 under JIT, `--interp`, helpers
only and AOT, FRINT* over halves, quarters and specials under six FPCR
settings among them. `tests/guest/fpscalar.sh` (every FRINT* over corner
values under each rounding mode with FZ and DN, against native arm64) passes
under JIT, `--interp`, helpers only and AOT. The oracle's simd and block
modes, one million records each, checked by the runtime before and after on
the same native records: no mismatches, and the same FPSR IEEE-flag
differences record for record (36 simd, 24,488 block; the known scalar
FZ/IDC gap); 365 more reserved EXT encodings now report UNDEF.

Remaining limitations:
- Double-precision vector arithmetic (FADD/FSUB/FMUL `.2D`, FMLA/FMLS) is
  still helper-bound; it is most of what is left in `vbench2`'s rounding
  kernel.
- FRINT32*/FRINT64*, half-precision FRINT*, PMUL, the pairwise integer
  min/max and the scalar (`D`-register) integer compares, ABS and NEG stay
  with the helper.
- A FRINT* operand with a subnormal, infinite or NaN lane pays the inline
  check and then the helper.

## Vector floating point and structure loads/stores in translated code

Translated code ran every Advanced SIMD floating-point instruction and every
structure load or store (LD1, ST1, LD1R, ...) through the interpreter
helper: an indirect call, `a64_exec`'s and `a64_exec_simd`'s decoders,
copies of the registers, and only then `fp_vec32`/`fp_vec32_select` or the
memory copies of `a64_ldst_simd`. Byte-lane and arithmetic 64-bit shifts,
the shift-and-accumulate and rounding shifts, SHRN, SSHLL/USHLL and XTN went
the same way. A single-precision kernel spent nearly all its time there.

`src/jit.c` now emits these inline, SSE2 only:
- FADD, FSUB, FMUL (vector and by element), FMAX, FMAXNM, FMIN, FMINNM,
  FCMEQ, FCMGE, FCMGT on `.2S`/`.4S`: the fast path of `fp_vec32` and
  `fp_vec32_select` instruction for instruction (integer admission of the
  operands, the SSE operation under the guest's MXCSR, the same result
  check, the MXCSR saved and restored around a declined result). A declined
  case calls the helper, which declines it again and takes the per-lane
  path, so values and FPSR are the interpreter's. The code does not touch
  EFLAGS; the declining path stores NZCV before the call and reloads it
  after only when it is live. FABS and FNEG (`.2S`, `.4S`, `.2D`).
- SHL, USHR, SSHR, USRA, SSRA, URSHR, SRSHR, URSRA, SRSRA by an immediate in
  every lane size (bytes and the arithmetic 64-bit forms emulated), SHRN,
  SHRN2, SSHLL/USHLL (UXTL/SXTL) and their "2" forms, XTN, XTN2.
- LD1/ST1 of one to four registers, LD1R, LD1/ST1 of one lane, without
  offset or post-indexed by an immediate or Xm, SP as base. Registers are
  transferred one at a time and the base written back last, as in
  `a64_ldst_simd`: a fault part way leaves the earlier registers transferred
  and Xn unchanged, at the instruction's pc.

Measured on the Apple silicon development host, where hao's x86_64 code
runs under Rosetta (not on Intel hardware): `tests/guest/vbench.c` (FADD,
FSUB, FMUL, FMUL by element, FMAX(NM), FMIN(NM), FCMGT, FCMGE, LD1/ST1 of
one to four registers, LD1R; 4000 repetitions) went from 1.40 s to 0.40 s
under the JIT and from 1.42-1.51 s to 0.39-0.40 s with an AOT cache (best of
five runs each, host under load from other jobs); its dynamic helper calls
from translated code went from 87,062,441 to 6,153 and its static helper
instructions from 124 to 21 of 436 (`--stats`). `tests/guest/bench.c`
(double-precision matrix code, FMLA) is unchanged: 53 to 45 static helper
instructions, 2.20 M to 2.15 M helper calls, wall time within noise.
`tests/aot/jit_vector.c` (in `make unit`) compares 120,000 cases of these
encodings, alone and between a CMP and a CSEL that reads NZCV, through the
translator and the interpreter under all 16 RMode/DN/FZ settings, with the
full FPSR (the IEEE flags the oracle only counts), all registers and memory,
including faulting accesses at a page end: no differences.
`tests/guest/jitinsns.sh` compares 107 more lines (281 before) with native
arm64 under JIT, `--interp`, helpers only and AOT: the vector FP forms over
a corner-value table under five FPCR settings with FPSR and NZCV, the
shifts, the loads and stores (post-index by immediate and register, SP
base) and a faulting post-indexed LD1 in a signal handler that skips or
repairs it.

Remaining limitations:
- Double-precision vector arithmetic, FMLA/FMLS (fused), FDIV, FMULX,
  FRECPS/FRSQRTS, FABD, FACGE/FACGT, the pairwise forms, half precision,
  conversions and the vector compares with zero still go through the
  helper, as do LD2-LD4/ST2-ST4 and LD2R-LD4R.
- A vector FP instruction with any special lane (NaN, infinity, subnormal,
  or a result that is zero, subnormal or near the minimum normal without
  being an exact zero) pays the inline check and then the helper.
- Translated loads and stores, like `t_ldst_reg`'s, use the address as
  given: a tagged (top-byte) address faults where the interpreter strips
  the tag.
- A 16- or 8-byte register store that straddles into an inaccessible page
  stores nothing in translated code; the interpreter's `memcpy` may store
  the bytes before the page end (Arm leaves them UNKNOWN). Registers before
  it in the list are stored by both.
- Each translated single-precision arithmetic instruction is about 250-350
  bytes of x86 code: `vbench`'s translations grew from 13 to 33 KB.

## Scalar FP conversions, max/min and round-to-integral cost

The translator emitted SSE code for scalar FADD/FSUB/FMUL/FDIV/FSQRT, FCVT
between single and double, FCMP, FCSEL and SCVTF. Every other scalar or
packed floating-point instruction ran through the interpreter helper, and
there the software model did all of the work: FP-to-integer conversions and
round-to-integral unpacked the operand to a 128-bit significand and rounded
it; integer-to-FP conversions went through the general `round_pack_mode`
even when the integer was exactly representable; max/min unpacked both
operands twice and repacked the winner; a compare with zero unpacked both
sides; and `nans()`, which every add, subtract, multiply, divide, max/min and
fused operation calls before anything else, classified each operand up to
four times (two ordered searches, signalling then quiet, each through the
format table) although almost no operation has a NaN operand.

What changed, in two layers.

Translated code (`src/jit.c`, single and double only; JIT and AOT emit the
same bytes). Each form is taken inline only for results the SSE instruction
can vouch for; anything else re-executes the instruction in the helper with
the operands untouched, and a declined attempt leaves in MXCSR only flags the
helper raises again:
- `FCVTZS`/`FCVTZU Wd|Xd, Sn|Dn`: `CVTTSS2SI`/`CVTTSD2SI`. Signed: a result
  equal to the minimum integer (the x86 integer indefinite) goes to the
  helper, which saturates and raises IOC where x86 raised IE. Unsigned: the
  operand's bits are admitted first (`0 <= x < 2^32` or `2^63`, one unsigned
  compare that also rejects negatives, infinities and NaNs), because a
  negative fraction or a double above 32 bits would raise PE where A64 raises
  only IOC. A zero result from a denormal operand goes to the helper (IDC
  instead of IXC under FZ).
- `UCVTF Sd|Dd, Wn|Xn`: `CVTSI2SS`/`CVTSI2SD` of the zero-extended value; an
  `Xn` with bit 63 set goes to the helper.
- `FMOV Sd|Dd, #imm`: a constant store.
- `FMAX`/`FMIN`/`FMAXNM`/`FMINNM`: `MAXSS`/`MINSS` for unequal ordered
  operands, AND (max) or OR (min) of the operands when they compare equal
  (the zero-sign rule). A denormal operand (tested on the bits) or a NaN
  (`UCOMIS` unordered) goes to the helper.
- Packed single (`.4S`, `.2S`): `SCVTF` is `CVTDQ2PS`; `UCVTF` the same when
  no lane has bit 31 set; `FCVTZS` is `CVTTPS2DQ` unless a lane is denormal
  or answers the integer indefinite; `FCMGT #0.0` is the signed integer
  compare of the bits with zero unless a lane is a NaN or denormal.

Interpreter and helper (`src/a64_fp.c`, single and double; half precision
keeps the reference path):
- `nans()` first asks, with one integer compare per operand, whether any
  operand is a NaN.
- `fp_to_int` (every FCVT*S/U rounding mode and the fixed-point forms) and
  `fp_rint` (every FRINT*) work on the bits of a normal operand with 64-bit
  shifts; zero, denormal, infinite, NaN and out-of-range operands keep the
  reference path, which owns IOC, IDC and saturation.
- `fp_from_int` uses the host conversion for an integer that fits `int64`
  with no fraction bits: one rounding in the MXCSR mode, PE for IXC.
- `fp_max`/`fp_min` and the NM forms return the selected operand for zeros,
  normals and infinities; `cmp3` compares sign-magnitude keys unless FZ could
  flush an operand.
- `fp_raise` writes MXCSR only when a flag is new.
- `FJCVTZS` judged its range before truncating: `-2^31 - 0.5` reported IOC,
  where A64 reports IXC (the truncated value is in range). It now tests the
  truncated integer.

Measured with `tests/guest/fpscalar -b NAME 300000` (a loop of eight copies
of the instruction, ns per instruction, best of three). The development host
is Apple silicon and runs hao under Rosetta, so these are ratios, not
Intel timings; `STMXCSR`/`LDMXCSR` in particular are much slower there than on
an Intel CPU.

| Instruction | Before, JIT | After, JIT | Before, AOT | After, AOT |
|---|---:|---:|---:|---:|
| `fcvtzs w, s` | 29.7 | 0.73 | 30.9 | 0.63 |
| `fcvtzu x, d` | 28.9 | 0.79 | 30.4 | 0.63 |
| `ucvtf s, w` | 25.3 | 0.32 | 26.3 | 0.23 |
| `fmov s, #1.0` | 13.7 | 0.38 | 13.8 | 0.29 |
| `fmaxnm s, s, s` | 32.6 | 3.49 | 34.8 | 3.45 |
| `scvtf v.4s` | 89.9 | 3.15 | 92.9 | 3.12 |
| `ucvtf v.4s` | 92.4 | 2.54 | 94.2 | 2.67 |
| `fcvtzs v.4s` | 31.2 | 3.80 | 32.3 | 3.42 |
| `fcmgt v.4s, #0.0` | 43.4 | 3.02 | 46.3 | 2.77 |
| `frintn v.4s` (helper) | 68.5 | 27.6 | 72.3 | 28.4 |
| `frintx s` (helper) | 26.2 | 15.4 | 28.8 | 15.6 |
| `ucvtf s, s` (helper) | 43.0 | 19.0 | 44.6 | 20.1 |
| `fcvtns w, s` (helper) | 31.1 | 23.0 | 31.8 | 23.3 |
| `fmaxnm v.2d` (helper) | 62.0 | 26.4 | 61.8 | 25.3 |
| `fmadd s` (helper) | 44.3 | 36.9 | 43.3 | 37.9 |
| `fdiv v.4s` (helper) | 88.3 | 85.1 | 84.2 | 81.1 |

`tests/guest/fpscalar.sh` runs 147 instruction forms (the ones above, every
FCVT* rounding mode, fixed point, every FRINT*, compares, max/min, pairwise
and NaN-selection cases, half-precision controls) over corner values, all
pairs of them for two-operand forms, and random values, under each rounding
mode with FZ and DN off and on and FPSR clear or full beforehand. Results,
FPSR (IOC, DZC, OFC, UFC, IXC, IDC, QC) and NZCV equal native arm64 under JIT,
`--interp`, helper-only JIT and AOT.

Remaining limitations:
- The non-truncating `FCVT*` modes, fixed-point conversions, `FCVTZU` and
  `UCVTF` with large lanes, the double packed conversions, the other
  compares with zero and packed max/min still go through the helper; its
  dispatch is then most of their cost. (`FRINT*` is now inline: see "Table
  lookups, EXT, integer multiplies and round-to-integral" above.)
- Inline SSE arithmetic that predates this change (FADD, FSUB, FMUL, FDIV,
  FSQRT, FCVT single<->double, FCMP) differs from A64 in FPSR under FZ: a
  flushed denormal operand does not raise IDC, and a result flushed to zero
  raises IXC together with UFC where A64 raises UFC alone. Values agree. The
  interpreter (`--interp`) is exact. `tests/guest/fpscalar.c` runs those
  instructions with FZ clear.
- FPCR.AH, FIZ and NEP are not modelled; the test leaves them clear.
- The packed tests for denormal lanes also send a lane equal to the minimum
  normal number to the helper (exact, slower).

## Single-precision fused multiply-add cost

The release CPU profile has no FMA instruction, so every fused
multiply-add ran in the exact software path (`fp_muladd` in
`src/a64_fp.c`: unpack, 128-bit exact sum, `round_pack`), once per lane.

Single precision now takes a host path when every operand that takes part is
a normal number or a zero (`fma32`, `fp_vec32_muladd`):

1. the product is computed in binary64, where it is exact (24 + 24 bits);
2. the addend is added in round-to-nearest and Knuth's TwoSum gives the
   exact error of that sum;
3. if the error is nonzero and the sum's last significand bit is 0, the sum
   moves one unit in the last place toward the error (round to odd);
4. one `cvtpd2ps` under the guest's MXCSR rounds to single in FPCR.RMode.

Rounding to odd at 53 bits and then to 24 bits in any mode equals rounding
the exact value once (Boldo and Melquiond; needs 53 >= 24 + 2). The odd
last bit also makes the conversion inexact exactly when the fused result
is, so the conversion's PE and OE are FPSR.IXC and OFC with no further
bookkeeping. The path covers FMLA and FMLS `.4S`/`.2S`, both by element,
the scalar by-element forms, and FMADD, FMSUB, FNMADD and FNMSUB on
singles, in the interpreter and therefore in the JIT's and AOT code's
helper calls. It uses SSE2 only.

It declines, leaving MXCSR as it was, and the software path runs unchanged
when an operand is a NaN, an infinity or a subnormal, when the
result is zero or below 2^-125 (ARM detects tininess before rounding and
SSE after, FZ flushes, and an exact zero takes its sign from the rounding
mode), or when FPCR has a bit set other than RMode, FZ and DN. A zero
operand with a nonzero result needs no rule of its own: the binary64 sum
is then the other term exactly. A vector
whose lanes are not all admitted is computed lane by lane, and each lane
is admitted or declined alone.

Without step 3 the path is wrong. `0xbc000008 * 0xc03ffff4 + 0x483bb5c8`
under round-to-nearest is `0x483bb5c9`; the binary64 sum lands on the
midpoint of two singles and a second rounding gives `0x483bb5ca`.
`0xde800000 * 0x5e800000 + 0x5e800000` under round-toward-zero is
`0xfd7fffff`; the binary64 sum absorbs the addend and converts to
`0xfd800000` with no inexact flag from the conversion.

The software path also no longer calls `nans()` unless `unpack` classified
an operand as a NaN; the call had no effect otherwise. This applies to half
and double precision as well.

Checks against native arm64. `make oracle` has an `fma` mode
(`tests/oracle/fma_cases.h`): every form above with distinct and aliased
registers, all sixteen RMode/FZ/DN settings, clear and preset FPSR, on
ordinary, arbitrary, near-tie, wide exponent gap, 53 to 57 bit sum,
overflowing, tiny, exactly cancelling and special operands, with one lane
in six from another class. For these records FPSR's cumulative flags must
match as well as the registers. 1,000,000 records matched in the
interpreter and through the JIT; 868,009 of them had only normal or zero
operands and results of at least 2^-125 in every lane, and for 68,427 of
those
`(float)((double)a * b + c)` differs from native. With step 3 removed,
9,699 of 100,000 records mismatched. `tests/guest/fma32.sh` runs 25
instruction forms on the same operand classes (675,000 instructions) and
requires the JIT, `--interp` and AOT runs to print native arm64's result
and FPSR hashes. `make oracle ORACLE_MODES="simd dp" ORACLE_N=1000000` had
no mismatch, and `make check` passed (`make test`, `oracle-asan` with the
`fma` mode, and the `TEST_SWITCHES` guest harness: 68 passed, 0 failed, 1
known failure in `guest/xinhab`, 8 expected skips).

Measured on the Apple silicon development host, where x86_64 hao runs
under Rosetta (not on Intel hardware), ns per instruction on normal
operands, JIT and AOT alike since both call the interpreter helper:

| Instruction | Before | After |
|---|---|---|
| FMLA/FMLS `.4S` | 128 | 22 |
| FMLA/FMLS `.4S` by element | 130 | 25 |
| FMADD/FMSUB single | 41 | 23 |

Remaining limitations:
- Each fused instruction is still a helper call from translated code; the
  path is not emitted inline.
- A rounding mode other than round-to-nearest needs two extra MXCSR writes
  per instruction (the sum must run in round-to-nearest). Under Rosetta
  `.4S` under RZ went from 167 to 86 ns rather than to 30 in the same run;
  the cost on Intel hardware has not been measured.
- A vector with a subnormal, infinite or NaN lane, or a tiny or zero
  result in a lane, is computed lane by lane. (`.4S` with one zero
  multiplicand lane and nonzero results is admitted whole: 159 to 23 ns.)
- Double precision stays in the software path: there is no wider host type
  to hold the product. It would need error-free transformations in
  binary64 itself (Dekker's product with Veltkamp splitting, TwoSum, and
  the Boldo-Melquiond three-addition scheme with an emulated round-to-odd
  addition), with guards for splitting overflow, an inexact low product
  part near the subnormal range, and separate handling of the directed
  modes.
- Half precision vectors keep `fp_vec16_muladd`, which admits a lane only
  when the exact sum fits binary64. The round-to-odd step would remove
  that bound (53 >= 11 + 2); the final rounding would stay in `fp_cvt`,
  since the profile has no half-precision conversion instruction. Scalar
  half fused operations are always computed in software.
- Not measured on Intel hardware or in an application.


## Guest calls of private-library functions that take a `va_list`

A guest `va_list` is a pointer to 8-byte stack slots; an x86_64 `va_list` is
a pointer to `{gp_offset, fp_offset, overflow_arg_area, reg_save_area}`. hao
converts one only for functions in its `va_list` tables (`src/interpose.c`
`vlists`): hand rows, plus `src/vlist_gen.inc`, which is generated from SDK
declarations. A private library's functions are in neither, so a guest call
of CoreUtils `NSErrorV(domain, code, format, va_list)` handed the native
function the guest pointer itself. CoreUtils' `PrintFCoreVAList` then read
the guest's slots as an x86_64 `va_list` and faulted at the first pointer
conversion (`%@` through `CFGetTypeID`, `%s`).

The hand rows now include the 26 CoreUtils exports that take a `va_list`.
They were found by register flow in the arm64e code: an export whose argument
register reaches the `va_list` position of `PrintFCoreVAList` (index 2), or
of `CFPropertyListBuildFormatted` and `VSNScanF`, which read it in place
(`ldr`, `add #8`, `str` on its spill slot), followed to a fixpoint. Variadic
functions, which make their own `va_list` with `va_start`, are excluded. In
the 26B131 x86_64 cache all 26 names are exported, and only by CoreUtils.
The conversion is the one the SDK `v*` functions use: register save areas
marked exhausted, overflow area at the guest's slots, so integer and floating
arguments are read in the guest's order with no format typing.

`tests/guest/cuvalist.sh` has a guest variadic wrapper hand its `va_list` to
`VSNPrintF`, `VASPrintF`, `NSPrintV` and `NSErrorV` with string, integer,
double, `%.6a` and `%@` conversions. Before the change it crashed with
SIGSEGV under JIT, `--interp` and AOT; after it, all 6 lines equal native
arm64.

`src/vlist_gen.inc` now also has rows derived from code: both slices of the
26B131 caches, x86_64 `__va_list_tag` reads, arm64e `va_arg` loads and
argument forwarding to a fixpoint, kept only when the two slices agree on the
index. Without seeds, that derivation reproduced 55 (x86_64) and 30 (arm64e)
of the 76 SDK and hand rows and never gave a wrong index. It confirms the
CoreUtils hand rows: 23 from both slices, `CFCreateV` and
`CFPropertyListCreateFormattedVAList` from x86_64 only, and none contradicted.
`VSNScanF`, a scanf consumer, is outside what it can derive. Over all 17,745
C/C++ guest imports of 26B131, the only `va_list` parameters beyond the SDK
and hand rows are `NSErrorV`, `NSErrorNestedV` (hand rows) and `_ISDVLog`
(SyncServices, index 3, the one new generated row).

Remaining limitations:
- The derivation does not cover a `va_list` passed as the seventh or a later
  argument, scanf consumers, or ObjC methods (those go through their type
  encoding in `src/objc.c`). A private function of those kinds needs a hand
  row.
- A function with a floating-point argument before its `va_list`, or with more
  than five integer arguments before it, cannot go through `call_with_va`. No
  CoreUtils row has either.

## Guest calls of native Optional, tuple and array witnesses

A nested Optional whose outer nil is a pointer's extra inhabitant (`C??`,
`[Int]??`, `AnyObject??`, `(any P)??`, the storage of a `lazy var` of
Optional type) is written by guest code inline with the arm64 encoding:
inhabitant 1 is the value 1. libswiftCore's x86_64 witnesses read 2. This
was measured in native programs of both architectures; for an existential
the inhabitants are in its type-metadata word (opaque) or object word
(class-bound). Guest code that is not specialized hands such values to those
witnesses: generic code, and outlined copies and destroys through metadata
from a mangled name, such as a class's ivar destroyer destroying a lazy
property that was never read. The x86_64 witness takes the outer nil for a
live payload and copies or destroys garbage. For `(any P)??` that is a
destroy of an existential whose type is 1, a fault at 0xfffffffffffffff9.

hao now recognizes a guest call of these native witnesses:
- the Optional value witnesses, which every Optional instantiation shares
  (`$sSqwCP`, `wxx`, `wcp`, `wca`, `wtk`, `wta`, `wet`, `wst`, `wug`, `wup`,
  `wui`);
- the tuple witnesses, a few `tuple_*` sets chosen by layout;
- the runtime's array operations (`swift_arrayDestroy`,
  `swift_arrayInitWithCopy`, `...WithTake*`, `swift_arrayAssign*`), which
  inlined Array code calls.

The witness addresses are read at image registration from Optional and tuple
instantiations of the converted shapes, and the array operations by name. A
call counts only when its metadata argument's own table holds the called
witness in that slot. For a type that can hold such an outer nil (cached per
metadata), the witness gets each outer nil in its own encoding:
`receiver_empty_bits` asks the type's witnesses, so a guest payload type's
arm64 inhabitants are kept. A borrowed source is converted in a scratch copy
and an owned one in place. A value the witness produces gets back its
source's bits at those offsets, so a value keeps the encoding it came with:
a native-made value passing through guest generic code stays x86_64. A nil
made from nothing (storing or injecting a tag) gets the guest's encoding
where guest code can read it inline: every struct on the path to its word
is defined by a guest image (or is `Array` or `String`). Inside a struct of
a native module, which may be resilient (libswiftos `OSSignposter`), the
nil keeps the native encoding. Only native witnesses read it there,
including those native code reaches while handling an enclosing guest
struct, and no conversion sees those. Converting it anyway once made a
guest `Optional` of a struct whose first field is `OSSignposter?` read as
present when nil: the native tag read took the guest's 1 for a payload,
copies duplicated garbage, and a force unwrap in the guest trapped.
Only the outer-nil words change, never a live payload. A value the walk
cannot prove is handed over unchanged, as before. The walk follows
existential payloads (opaque and class-bound; not the Error box or other
special protocols), Swift class references, and Arrays of any element, but
only for these calls. Published values (below) keep their narrower rules,
because there native code takes the value.

`tests/guest/optwitness.sh` has unspecialized generic guest code copy,
destroy, assign and test values through these witnesses: nested Optionals
of an existential, a class-bound existential, a Swift class, `AnyObject` and
`[Int]`, in each of nil, some(nil) and some(some) form, plus a class whose
lazy `(any P)?` property is destroyed unread, read as nil, and read as a
value. Before the change it crashed under JIT, `--interp` and AOT at the
first copy; after it, all 94 lines equal native arm64. It now also copies,
assigns, tests and drops nil, full and empty values of `Optional` of such a
guest struct, and makes a nil one through generic code. Before the narrower
nil rule that case stopped with "Unexpectedly found nil" in all three modes;
now all 101 lines equal native arm64.

Remaining limitations:
- Native code that reads such a value itself, rather than a witness the guest
  calls, still sees the guest's encoding. Examples: native generic code (an
  Array element read through the non-inlined `Array` subscript, which copies
  it with the witness natively) and `swift_dynamicCast` out of `Any`. Memory
  that guest inline code and native code both read can hold only one
  encoding. That is the open part of `guest/xinhab`, which stays `xfail`.
- A native struct counts as guest-readable only when `src/frozenstruct_gen.inc`
  lists it: a table generated from the SDK interfaces' `@frozen` and
  `@_fixed_layout` declarations, joined against the 26B131 x86_64 cache and
  matched at run time by qualified nominal name, since freezing is
  permanent ABI. A frozen struct the table misses (for example in a private
  module without an interface) is treated as resilient and keeps the native
  encoding. Before the table, every native struct was: a nil made through the
  shared Optional witnesses inside `CollectionOfOne<AnyObject>??` read as
  present in guest code.
- Witnesses of native generic structs and enums other than Optional (each
  type has its own) are not recognized: a nil a generic struct's own native
  witness makes (`CollectionOfOne<AnyObject?>?` from unspecialized generic
  code) keeps the native encoding, whatever the frozen table says. Neither are value boxes (an `Any`
  holding such a value is destroyed by the box's witness).
- Two consumed inhabitants are the bound, as for published values: a third
  nesting level is handed over unchanged.

## `Optional<AnyObject>` in a published Swift value

A guest SwiftUI view value that held an `NSNotificationCenter.Publisher`
was refused at publication, which ended the process: `Swift value boundary
refused: result publication has unresolved active representation (...
type=Swift.Optional<Swift.AnyObject>)`. A view value returned by guest code
(`View.body`, an associated result) that holds an enum whose empty cases sit
in a pointer's extra inhabitants is walked whole, and the walk reached the
publisher's `object`, which is `Optional<AnyObject>`. `common_leaf_value`
admitted an `Optional` of a Swift class, of an imported ObjC class, or nil of
a one-witness class existential, but not `AnyObject`.

Native Swift programs on arm64 and x86_64 give the same `AnyObject`:
existential kind 0x303, flags 0, no protocols, the `$sBOWV`
(Builtin.UnknownObject) witnesses (size 8, flags 0x10007, xi 0x7fffffff).
Its first `Optional` has size 8, flags 0x210007 and nil 0 on both. hao now
admits exactly that first `Optional`, with the value rule of imported ObjC
references: nil, or a word the witnesses' own tag function calls a live
value, tagged pointers included. `Optional<Optional<AnyObject>>` stays
refused, because its second nil is an extra inhabitant that is 1 on arm64
and 2 on x86_64.

`tests/host/swiftvalue.sh` walks native x86_64 values with an `NSObject??`
field (walked, with extra inhabitants to patch) next to an
`Optional<AnyObject>` holding nil, an NSObject, a Swift object or a tagged
NSNumber. Before the change every case was refused at
`Optional<AnyObject>`, the publication refusal above. After it they are
walked with the patches of an `Optional<NSObject>` control.

Two more first `Optional`s were refused in values crossing the boundary:

- The Optional of an enum with no payload. It is one byte: a case is 0..n-1
  and nil is n, measured identical on both architectures. The byte rule
  needed 9 bytes or more. Now the first Optional of a payload-free enum of up
  to 255 cases is admitted when its byte is at most n.
- `Optional<Foundation.URL>`. URL is a struct whose only
  field is `any _URLProtocol`, a class-bound existential with one witness
  table: 16 bytes, measured identical on both architectures, nil is a zero
  object word. Only nil of a bare such existential had been admitted, and
  only with flags exactly 0x210007, while URL's Optional also has
  0x2000000 (addressable for dependencies), which changes no layout. Now
  that wrapper is admitted with a live value too. The object word must pass
  the existential's own tag function and the witness table must lie in no
  guest image, since a guest conformance's table is arm64 code. A bare
  existential stays nil-only, because a weak field reports the same type and
  its nonzero storage is not a strong pointer.

`tests/host/swiftvalue.sh` covers both (nil and live values beside the
`NSObject??` field): refused before, walked after. Its URLs all have native
witness tables, so the refusal of a guest witness table is not exercised
there.

## `%a` slots of variadics whose format family is assumed

A guest call of CoreUtils' `LogPrintF` with a `%.6a` conversion (an
Ethernet address: `LogPrintF(cat, func, level, "%.6a\n", bytes)`) crashed
with SIGSEGV in CoreUtils' `PrintFCoreVAList`. `LogPrintF` has no SDK format
attribute, so `src/varsniff.c` typed its slots by assuming the last fixed
argument is a printf format. printf's `%a` takes a double, so the pointer
went to xmm0. CoreUtils reads `%a` as a pointer to an address and took r8,
which held 0.

Native arm64 CoreUtils on the development host (27.2, 26B123), read through
`VSNPrintF` with marker slots, gives these rules. `%a` takes one pointer with
any precision or flag: `.4` is IPv4, `.6` Ethernet, `.8` 8 bytes, `.16`
IPv6, `##` a sockaddr, and a bare `%a` prints an error. `%A`, `%D`, `%O` and
the `L` and `q` modifiers are unknown and take no argument. `v` is a
NumVersion conversion, not a modifier. `%m` takes one integer, `%H` takes a
pointer and two integers, and `%N` takes nothing. `%e %f %g %E %F %G` take
doubles.

A `%a`/`%A` slot of a function without an SDK format attribute is typed by
that function's formatter family, from `src/fmtfamily_gen.inc` (generated
from the x86_64 code: which formatter the function's arguments reach, to a
fixpoint; checked against every SDK-attributed function, none wrong):
CoreUtils' PrintF takes a pointer, printf and CFString formats a double. A
function the table does not list is assumed to be printf. An earlier rule
typed such a slot by its value (a user pointer reads as zero or a positive
subnormal double); no rule of that kind can tell a NULL pointer from 0.0 or
an address from a subnormal, and it misrouted both a bare `%a` of NULL to
CoreUtils and a subnormal to printf. Formats with an SDK attribute are typed
as before. `tests/guest/varsniff.sh` checks printf `%a` (zero, precision,
`#`, `%A`, the smallest subnormal and the minimum normal) and CoreUtils
`SNPrintF` (`%.6a`, `%.4a`, `%##a`, a NULL `%.6a` and a bare `%a` of NULL,
each followed by integer, string and double slots) against native arm64.

Remaining limitations:

- A CoreUtils-family function the table misses (not exported, a family
  reached only through a format string such as CoreUtils' `%V`, a va_list
  passed on the stack) is typed as printf: its `%a` pointer goes to an XMM
  register and later slots read one register off.
- CoreUtils conversions outside printf's set (`%m`, `%H`, `%N`, `%{...}`,
  `%v`) still make the typer give up, and every slot then goes to a GPR. A
  real `%f` in such a format prints a wrong value but cannot crash. CoreUtils'
  `%D`, `%O` and `%q` take no argument but are typed as one slot.

## Signal callback reentry during Objective-C enum initialization

The intermittent `tests/guest/uresume.sh` `alarmnx` SIGILL was a runtime
signal-reentrancy defect. Every untyped native-to-guest callback calls
`hao_objc_enum_args`, which lazily registers selectors with `pthread_once`.
A SIGALRM arriving during that first initialization ran a guest handler
that made another native-to-guest callback. That callback entered the same
unfinished `pthread_once` on the same thread.

A JIT diagnostic run with `HAO_SIGRETURN_KERNEL=1` on the Apple Silicon
development host captured RIP `0x7ff80af03d4b`, resolving to
`libsystem_platform.dylib`'s `_os_once_gate_recursive_abort+0x17`, with
runtime depth 2, signal depth 2, signal nesting 1 and critical depth 0.
The stack contained both `hao_objc_enum_args` calls and the interrupted
outer `enum_init`. This was not the `ud2` after a failed `__sigreturn`.
The guest PC was the callback target (file VM address `0x1000015e0` in
that build), not an instruction fetched from an incorrectly located JIT
block. Temporary diagnostic instrumentation was removed after capture.

`hao_objc_enum_args` now surrounds the complete `pthread_once` call with
hao's existing critical-section protocol. An arriving asynchronous guest
signal is deferred until `pthread_once` has published completion, then
redelivered by `hao_crit_leave`. Guarding only the initializer body would
leave a reentry window before `pthread_once` publishes completion. The
change applies to every caller of the enum bridge, independent of the
program, execution engine and signal-return route.

On 10 November 2026, unmodified `1d230dc` with `TEST_SWITCHES=1` failed
51 of 1,200 fresh processes, in two batches of 100 per route. Every captured
failure had the recursive-once callback stack, including the interpreter
failure. With the fix, all six routes passed 500/500, or 0 failures in
3,000 fresh processes. The native ARM64 reference passed 100/100 fresh
processes.

| Engine | User resume, before | Kernel resume, before | User resume, after | Kernel resume, after |
|---|---:|---:|---:|---:|
| JIT | 20/200 | 19/200 | 0/500 | 0/500 |
| Interpreter | 0/200 | 1/200 | 0/500 | 0/500 |
| AOT | 4/200 | 7/200 | 0/500 | 0/500 |

Additional unmodified-runtime controls used JIT with the same fixture:

| Control | User resume failures | Kernel resume failures |
|---|---:|---:|
| `HAO_NO_CHAIN=1` | 1/100 | 1/100 |
| `HAO_JIT_HELPERS_ONLY=1` | 0/100 | 0/100 |
| Both `39d42d1` and `09a3420` reverted in a scratch build | 0/100 | 0/100 |

The no-chain failures had the same recursive-once stack. Zero failures in
these smaller controls are timing observations, not evidence that the
controls fix the defect: the second unmodified JIT batch also passed
100/100 on each route after the first batch failed 39/200. The captured
once recursion and the interpreter failure identify a cause independent
of either optimization.

Each AOT build uses its own newly generated cache; the fixed build's
`--stats` check reported `aot_blocks=256 aot_rejected=0`. JIT and
interpreter runs use `HAO_NO_AOT=1`. User and kernel routes set
`HAO_SIGRETURN_USER=1` and `HAO_SIGRETURN_KERNEL=1`, respectively.
Each guest process has a 60-second kill timeout; stdout and exit status
are compared with the native ARM64 `alarmnx` reference. The tests retain
their 100-microsecond timer and require at least 5,000 checked callbacks,
50 alarms and zero callback errors. No retries or relaxed expectations
are used.

`make check` passed: `make test`, `oracle-asan`, and the `TEST_SWITCHES`
guest harness (64 passed, 0 failed, 1 known failure in `guest/xinhab`,
8 expected skips, 73 scripts total). The complete `guest/uresume` script
passed, including its state, mask, nesting, exception, thread and
alternate-stack cases and signal-return counters. The gate's host fork
stress also passed 300/300 without hangs or errors; it does not resolve
the historical fork issue below.

The host was macOS 27.2 (26B123) on ARM64, running x86_64 hao through
Rosetta. The captured recursive-once ordering is a runtime bug that also
applies on Intel; execution on an
Intel kernel remains necessary to confirm the fix there. Rosetta's
separate restriction on abandoning nested signal frames still applies,
as described in `src/signals.c`. This result does not establish that all
signal-handler paths are reentrant or change the concurrent fallback in
`hao_jit_locate`. Application and release-image acceptance remain separate.

## Intermittent host fork stress failure

`tests/guest/fork.sh`'s host `test_fork all` has historical SIGSEGV/SIGBUS
failures. Reports from November 2026 include JIT emission
(`x_mov_ld`/`exit_indirect`) and bridge binding (`type_at`) stacks.
`test_fork` does not install hao's guest signal handlers, and those stacks
do not show the recursive enum initialization above. One more SIGSEGV
came from bridge binding (`labels_at` under `type_at`, a `w_bridge`
thread) in a `make test` run on 11 November; six reruns right after passed.
No shared cause has been established; the fork failure remains open.

## Repeated per-call work on guest -> native calls

Every guest branch to a native address went through the same
classification again: `run_loop` asked whether the address was a trap,
guest code (`hao_image_for_code`: the last-image hint, then the range
snapshot), a native entry of a guest function (three thunk tables) or a
shared-cache stub or island (`hao_dsc_redirect`, which takes `dsc_lock` for
an address in the cache), and `native_step` looked the plan up again
(`hao_bridge_plan_for_call`). None of that depends on the call, only on the
address and on state that changes rarely. After the native function
returned, `hao_jit_run_block` probed the whole translation table for the
return address, which no guest `ret` puts in the thread's jump cache.

Now each thread keeps a direct-mapped table in its `hao_thread`
(`src/runtime.c`, native-target cache; 256 entries, 10 KB of the thread's
state mapping): branch address -> native target, the `x1` a redirected
`objc_msgSend` stub sets, and the plan. An entry is
used only while the process-wide generation `hao_ncache_gen` (`hao/macho.h`)
is the one its classification started in; image registration and release
and every plan-table insert or replacement (`tab_put`) increment it after the
change is visible. Traps, guest entries, cache addresses the redirect could
not resolve, hao's own code and plans whose lookup still has side effects
(an ObjC probe plan with probes left, no plan) are never stored. Everything
after the plan still runs per call (selector recheck, interposer redirect,
REFUSED, counting with `HAO_BRIDGE_STATS`/`HAO_BRIDGE_CHECK`, Swift value
and witness hooks, the argument scan), so routes, diagnostics and counters
are unchanged. Also: the dispatcher reads and fills the thread's jump cache
for the block it enters; the argument scan answers "in no guest image" for
a register value with one range search instead of two lookups;
`hao_native_call` copies the usual 16 stack words with vector moves; the
ObjC enum-argument check skips its `pthread_once` critical section once
initialised.

Measured on the development host, where hao itself runs under Rosetta
(ns per call, minimum of 7 interleaved runs of 10^6 calls each, before ->
after, JIT / AOT): EXACT unit-typed function 36.9 -> 26.2 / 36.6 -> 24.0;
TYPED narrow result 39.7 -> 26.8 / 39.1 -> 26.1; untyped (`strlen`)
65.0 -> 49.8 / 60.0 -> 49.2; `memcmp` 92.3 -> 73.1 / 90.1 -> 74.3;
`CFStringGetLength` 79.9 -> 69.9 / 79.5 -> 68.2; a function pointer from
`dlsym` 72.2 -> 62.0 / 71.1 -> 57.9; `objc_msgSend` to a native method
394 -> 378 / 405 -> 372 (native arm64: 1-3 ns). In `sample` profiles of
the `strlen` loop, `hao_image_for_code` fell from 8.9% to 1.9% of samples,
and `hao_bridge_plan_for_call` (2.2%), `hao_dsc_redirect` (0.8%), the
thunk-table lookups (2.3%) and `hao_is_guest_function` (6.4% with the
lookups it made) no longer appear. `tests/guest/nccache.sh` checks that a plan typed after five
threads cached the untyped one reaches all of them (it fails, 1000 of 1000
wrong results per thread, with the generation increment left out of
`tab_put`).

Remaining limitations:
- What is left per call is the JIT exit and re-entry, the exception guard,
  `hao_native_call`, and for every untyped or masked slot the argument scan,
  which reads memory the arguments point to and cannot be remembered by
  target. A native call stays about 20-25 times its native cost under
  Rosetta.
- `objc_msgSend` to a native method calls `class_getInstanceMethod` for the
  encoding on every message (about two thirds of its 380 ns here); its
  result is not cached, since a method list can change without a hook hao
  sees.
- With `HAO_BRIDGE_STATS` or `HAO_BRIDGE_CHECK` each call still increments its
  plan's shared counter atomically, which threads calling one function
  contend on.
- Not measured on Intel hardware or with the target's interface units; the
  share each change has there is not established.

The inline FPCR write also mirrors `x87_rc_set`'s unchanged-control-word
check. It skips FLDCW when rounding is already correct: FLDCW is a waiting
x87 instruction and can deliver a pending unmasked native x87 exception,
where FNSTCW cannot. Nine focused state cases retain the pending invalid
flag and cover all four unchanged rounding modes with live/dead NZCV.
This Rosetta host preserves the flag but does not deliver the exception
even for FWAIT; actual Intel exception delivery remains untested.
