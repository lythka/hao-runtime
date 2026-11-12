# hao — arm64e -> x86_64 translator ("reverse Rosetta").
# Host artifacts are x86_64; oracle tools are native arm64.
# MACOS_MIN is the deployment target of every x86_64 artifact (hao, libhao,
# hao-stub, front/stub.o; tools/hao-embed relinks stubs with the minos it reads
# from front/stub.o). Build another target into its own directory, e.g.
#   make B=build/macos-26.0                    (macOS 26 Tahoe userspace)
# A build directory named macos-VER implies MACOS_MIN=VER (default 27.0), and
# a MACOS_MIN that contradicts that name is an error.
CC        := clang
B         := build
B_MIN     := $(patsubst macos-%,%,$(filter macos-%,$(notdir $(B:/=))))
MACOS_MIN := $(or $(B_MIN),27.0)
ifneq ($(B_MIN),)
ifneq ($(B_MIN),$(MACOS_MIN))
$(error B=$(B) is the macOS $(B_MIN) build directory, not MACOS_MIN=$(MACOS_MIN))
endif
endif
# The CPU profile hao-x86_64-avx-cx16-v1 (docs/releases.md): Intel Macs with AVX, so also SSSE3, SSE4.1,
# SSE4.2, POPCNT and CMPXCHG16B. Compiled code and the translator use at most x86-64-v2 (no VEX encodings:
# Rosetta, where the tests run, has no AVX); src/main.c refuses a CPU without the profile at startup.
ARCH      := -arch x86_64 -mmacosx-version-min=$(MACOS_MIN) -march=x86-64-v2
# Debug info names sources and objects relative to the checkout, and the debug
# map and libhaocore.a carry no file times (ZERO_AR_DATE), so the same commit
# gives the same bytes in any checkout, worktree or rebuild.
export ZERO_AR_DATE := 1
REPRO     := -ffile-prefix-map=$(CURDIR)=.
# TEST_SWITCHES=1: a build that reads the test-only switches (HAO_LIBVAL_USER,
# HAO_SIGRETURN_USER, ...; src/aot_policy.c). Never the build that ships.
ifdef TEST_SWITCHES
REPRO     += -DHAO_TEST_SWITCHES
endif
WARN      := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS    := $(ARCH) -std=gnu11 -O2 -g $(WARN) -Iinclude -Isrc -mcx16 $(REPRO)
OSO       := -Wl,-oso_prefix,$(CURDIR)/
# Changing MACOS_MIN in one B rebuilds everything there.
MIN_STAMP := $(B)/.macos-min-$(MACOS_MIN)

CORE := src/a64_interp.c src/a64_simd.c src/a64_fp.c src/sysreg.c src/x86_emit.c \
        src/a64_imm.c src/a64_cond.c src/aot_policy.c src/opts.c src/ifunit.c src/gmap.c
CORE_O := $(patsubst src/%.c,$(B)/%.o,$(CORE))

RT := src/bridge.c src/macho.c src/code.c src/libval.c src/dsc.c src/commpage.c src/runtime.c src/interpose.c src/varsniff.c src/vafmt.c src/syscall.c src/signals.c src/objc.c src/regvar.c src/abi.c src/blocks.c src/fnslots.c src/swiftmeta.c src/swiftobjects.c src/callbacks.c src/gss.c src/cxxstr.c src/thunks.c src/libm.c src/swift.c src/swiftcc.c src/jit.c src/trust.c src/idlist.c src/guest_sig.c src/main.c src/unwind.c
RT += src/swiftresult.c src/swiftvalue.c src/swiftschema.c
RT += src/async_contract.c
RT += src/swiftvalue/Recipe.c src/swiftvalue/Enum.c src/swiftvalue/Common.c src/swiftvalue/Aggregate.c
RT += src/swiftvalue/NativeLeaf.c src/swiftvalue/Tables.c
RT_O := $(patsubst src/%.c,$(B)/%.o,$(RT)) $(B)/bridge_x86_64.o

# libhao.dylib holds the runtime; hao (CLI, kext launcher) and hao-stub (the
# x86_64 slice tools/hao-embed adds to arm64e-only programs) are front ends.
HAO_LIBDIR ?= /usr/lib/hao
FRONT_LD := -Wl,-rpath,@executable_path -Wl,-rpath,$(HAO_LIBDIR)
# Front ends get CFLAGS' warnings but no -g: they are linked straight from
# source (a debug map would name temporary objects), and front/stub.o is
# linked into every embedded program by tools/hao-embed.
FRONT_CFLAGS := $(ARCH) -O2 $(WARN)

.PHONY: all test check test-guest oracle unit aot forktest jitlocktest bridgetest async-contract-test fuzz fuzz-smoke
all: $(B)/libhaocore.a $(B)/libhao.dylib $(B)/hao $(B)/hao-stub $(B)/front/stub.o $(B)/libhao_codeid.a

$(MIN_STAMP):
	@mkdir -p $(B)
	@rm -f $(B)/.macos-min-*
	@touch $@

$(B)/bridge_x86_64.o: src/bridge_x86_64.S $(MIN_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

# Linked by hao and hao-stub (not inserted): its __interpose tuples apply
# process-wide and the main executable stays dyld image 0 (src/process_identity.c).
# src/oslog.c: guest os_log/os_signpost records composed in process (it says why).
$(B)/libhao_identity.dylib: src/process_identity.c src/oslog.c src/native_bundle_identity.c include/hao/native_bundle.h include/hao/identity.h $(MIN_STAMP)
	$(CC) $(FRONT_CFLAGS) -Iinclude -dynamiclib -install_name @rpath/libhao_identity.dylib -o $@ src/process_identity.c src/oslog.c src/native_bundle_identity.c -lobjc
	codesign -s - -f $@

# hao code runs in signal handlers on native threads that may hold the malloc
# locks, and dyld allocates a thread's __thread block on its first access:
# libhao must have no thread-local variables (pthread keys instead).
NO_TLV = @if otool -l $@ | grep -q __thread_vars; then \
	echo "$@: has thread-local variables (__thread_vars); use a pthread key (src/macho.c, last_code_img)" >&2; \
	rm -f $@; exit 1; fi

$(B)/libhao.dylib: $(RT_O) $(B)/libhaocore.a
	$(CC) $(ARCH) -dynamiclib -install_name @rpath/libhao.dylib $(OSO) -o $@ $(RT_O) $(B)/libhaocore.a -lobjc
	$(NO_TLV)
	codesign -s - -f $@

$(B)/hao: src/front/cli.c $(B)/libhao.dylib $(B)/libhao_identity.dylib
	$(CC) $(FRONT_CFLAGS) -o $@ $< -L$(B) -lhao -lhao_identity $(FRONT_LD)
	codesign -s - -f --entitlements tools/hao.entitlements $@

# Object form for per-program stubs that carry the program's own data sections.
$(B)/front/stub.o: src/front/stub.c $(MIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FRONT_CFLAGS) -c $< -o $@

# The code identity Security.framework gives listed stubs and re-signed native programs
# (include/hao/codeid.h): no libhao, for the image's x86_64 Security.framework to link.
CODEID_O := $(B)/codeid/code_identity.o $(B)/codeid/idlist.o
$(B)/codeid/%.o: src/%.c include/hao/codeid.h include/hao/idlist.h $(MIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FRONT_CFLAGS) $(REPRO) -c $< -o $@
$(B)/libhao_codeid.a: $(CODEID_O)
	rm -f $@ && libtool -static -o $@ $(CODEID_O)

# Unsigned: tools/hao-embed signs each copy with its program's identity.
$(B)/hao-stub: src/front/stub.c $(B)/libhao.dylib $(B)/libhao_identity.dylib $(MIN_STAMP)
	$(CC) $(FRONT_CFLAGS) -o $@ $< -L$(B) -lhao -lhao_identity -Wl,-rpath,$(HAO_LIBDIR) -Wl,-no_adhoc_codesign

$(B)/%.o: src/%.c $(wildcard include/hao/*.h src/*.inc) $(MIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# A native callee that throws unwinds through hao_cxxstr_call: its cleanup destroys the native strings.
$(B)/cxxstr.o: src/cxxstr.c $(wildcard include/hao/*.h src/*.inc) $(MIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fexceptions -c $< -o $@

$(B)/libhaocore.a: $(CORE_O)
	ar rcs $@ $^

$(B)/oracle-native: tests/oracle/native_arm64.c tests/oracle/record.h tests/oracle/fma_cases.h
	@mkdir -p $(B)
	$(CC) -arch arm64 -O1 -Wall -o $@ tests/oracle/native_arm64.c && codesign -s - -f $@

$(B)/oracle-check: tests/oracle/check.c tests/oracle/record.h tests/oracle/known.h $(B)/libhaocore.a $(B)/jit.o $(B)/stubs_oracle.o
	$(CC) $(CFLAGS) -o $@ $< $(B)/jit.o $(B)/stubs_oracle.o $(B)/libhaocore.a && codesign -s - -f $@

# Concurrency and memory-permission checks the record format cannot express:
# 16-byte LDP/STP/LDXP atomicity across threads, exclusives on read-only memory.
$(B)/oracle-atomic: tests/oracle/atomic16.c $(B)/libhaocore.a $(B)/jit.o $(B)/stubs_oracle.o
	$(CC) $(CFLAGS) -o $@ $< $(B)/jit.o $(B)/stubs_oracle.o $(B)/libhaocore.a -lpthread && codesign -s - -f $@

# Every mode runs; the target fails if any of them reports a failure.
ORACLE_N ?= 200000
ORACLE_MODES ?= dp simd mem block fma
oracle: $(B)/oracle-native $(B)/oracle-check $(B)/oracle-atomic
	@fail=0; for m in $(ORACLE_MODES); do \
	  $(B)/oracle-native $(B)/oracle-$$m.bin $(ORACLE_N) 0x$$(date +%s)$$RANDOM $$m || fail=1; \
	  echo "== $$m"; $(B)/oracle-check $(B)/oracle-$$m.bin --show 5 --bad $(B)/oracle_bad_$$m.txt || fail=1; \
	done; \
	echo "== atomic"; $(B)/oracle-atomic || fail=1; \
	exit $$fail

# The same oracle checks with the translator itself (interpreter, JIT, x86
# encoder: CORE, src/jit.c) built under ASan and UBSan, objects in $(B)/asan.
# Any sanitizer report fails the target; reports are left in
# $(B)/asan/oracle-*.log. ORACLE_ASAN_N records per mode (default 20000).
ASAN_CFLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_CORE_O := $(patsubst src/%.c,$(B)/asan/%.o,$(CORE) src/jit.c src/stubs_oracle.c)
ASAN_ENV    := ASAN_OPTIONS=symbolize=0:handle_segv=0:handle_sigbus=0:handle_sigill=0:handle_sigfpe=0:detect_leaks=0 \
               UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0
ORACLE_ASAN_N ?= 20000
$(B)/asan/%.o: src/%.c $(wildcard include/hao/*.h src/*.inc) $(MIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(ASAN_CFLAGS) -c $< -o $@
$(B)/asan/oracle-check: tests/oracle/check.c tests/oracle/record.h tests/oracle/known.h $(ASAN_CORE_O)
	$(CC) $(ASAN_CFLAGS) -o $@ $< $(ASAN_CORE_O) && codesign -s - -f $@
$(B)/asan/oracle-atomic: tests/oracle/atomic16.c $(ASAN_CORE_O)
	$(CC) $(ASAN_CFLAGS) -o $@ $< $(ASAN_CORE_O) -lpthread && codesign -s - -f $@
.PHONY: oracle-asan
oracle-asan: $(B)/oracle-native $(B)/asan/oracle-check $(B)/asan/oracle-atomic
	@fail=0; seed=0x$$(date +%s)$$RANDOM; echo "seed $$seed"; for m in $(ORACLE_MODES); do \
	  $(B)/oracle-native $(B)/asan/oracle-$$m.bin $(ORACLE_ASAN_N) $$seed $$m || fail=1; \
	  echo "== $$m (asan)"; $(ASAN_ENV) timeout -s KILL 900 $(B)/asan/oracle-check $(B)/asan/oracle-$$m.bin --show 5 \
	    --bad $(B)/asan/oracle_bad_$$m.txt >$(B)/asan/oracle-$$m.log 2>&1 || fail=1; tail -3 $(B)/asan/oracle-$$m.log; \
	done; \
	echo "== atomic (asan)"; $(ASAN_ENV) timeout -s KILL 300 $(B)/asan/oracle-atomic >$(B)/asan/oracle-atomic.log 2>&1 || fail=1; \
	tail -3 $(B)/asan/oracle-atomic.log; \
	if grep -lE 'runtime error:|ERROR: AddressSanitizer' $(B)/asan/oracle-*.log; then echo "sanitizer reports in the logs above"; fail=1; fi; \
	exit $$fail

# Unit tests of the A64 immediate decoder and condition mapping.
$(B)/test_imm_cond: tests/a64/test_imm_cond.c src/a64_imm.c src/a64_cond.c include/hao/a64_imm.h include/hao/a64_cond.h include/hao/cpu.h include/hao/x86.h $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/a64/test_imm_cond.c src/a64_imm.c src/a64_cond.c && codesign -s - -f $@

# Unit test of the switch snapshot parser (src/opts.c) with the tier rules.
$(B)/opts_test: tests/aot/opts_test.c $(B)/libhaocore.a
	$(CC) $(CFLAGS) -o $@ $< $(B)/libhaocore.a && codesign -s - -f $@

# Fork while other threads hold the JIT, thunk, signature and block locks.
$(B)/test_fork: tests/host/fork.c $(B)/libhao.dylib
	$(CC) $(ARCH) -O1 -Iinclude -o $@ $< -L$(B) -lhao -Wl,-rpath,$(abspath $(B)) && codesign -s - -f $@

forktest: $(B)/test_fork
	timeout -s KILL 300 $(B)/test_fork 300 all

# The bridge plan table: capacity, lock-free reads while it grows, upgrades
# (tests/host/bridge.c).
$(B)/test_bridge: tests/host/bridge.c $(B)/libhao.dylib
	$(CC) $(ARCH) -O1 -Iinclude -o $@ $< -L$(B) -lhao -Wl,-rpath,$(abspath $(B)) && codesign -s - -f $@

bridgetest: $(B)/test_bridge
	timeout -s KILL 120 $(B)/test_bridge

# Code cache and thunk areas: AOT load vs translation, signature thunks,
# cache exhaustion (tests/host/jitlock.c).
jitlocktest: $(B)/libhao.dylib $(B)/hao
	OUT=$(abspath $(B))/jitlock LIB=$(B) tests/host/jitlock.sh

# hao's growable maps (src/gmap.c): capacity, lock-free reads while they grow, fork.
$(B)/test_gmap: tests/host/gmap.c src/gmap.c include/hao/gmap.h $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/host/gmap.c src/gmap.c -lpthread && codesign -s - -f $@

# Unit test of the type-encoding layout and classification in src/abi.c.
$(B)/test_abi_layout: tests/abi/test_layout.m src/abi.c $(wildcard include/hao/*.h) $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -Wno-unused-function -framework Foundation -o $@ tests/abi/test_layout.m && codesign -s - -f $@


# Unit test of the validating Mach-O loader (src/macho.c), under ASan, on
# corrupted copies of the fuzz seeds.
$(B)/test_macho: tests/loader/test_macho.c src/macho.c src/code.c src/libval.c include/hao/macho.h include/hao/code.h $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(ARCH) -std=gnu11 -O1 -g -Iinclude -fsanitize=address -fno-omit-frame-pointer $(REPRO) -o $@ tests/loader/test_macho.c src/macho.c src/code.c src/libval.c && codesign -s - -f $@

# hao_images lookups on reader threads while loads, unloads and failed loads
# change the list (ASan).
$(B)/test_images: tests/loader/test_images.c src/macho.c src/code.c src/libval.c include/hao/macho.h include/hao/code.h $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(ARCH) -std=gnu11 -O1 -g -Iinclude -fsanitize=address -fno-omit-frame-pointer $(REPRO) -o $@ tests/loader/test_images.c src/macho.c src/code.c src/libval.c && codesign -s - -f $@

# Unit test of the interface-unit parser (src/ifunit.c), under ASan/UBSan: one
# corruption per rule and every single-bit edit of the fuzz seeds.
$(B)/test_ifunit: tests/loader/test_ifunit.c src/ifunit.c src/aot_policy.c include/hao/bridge.h include/hao/aot.h $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(ARCH) -std=gnu11 -O1 -g -Iinclude -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(REPRO) -o $@ tests/loader/test_ifunit.c src/ifunit.c src/aot_policy.c && codesign -s - -f $@

$(B)/test_helper_descriptor: tests/aot/helper_descriptor.c $(B)/jit.o $(B)/stubs_oracle.o $(B)/libhaocore.a
	$(CC) $(CFLAGS) -o $@ $< $(B)/jit.o $(B)/stubs_oracle.o $(B)/libhaocore.a
	codesign -s - -f $@

# Only the unit build counts descriptor helper routing, including declines
# whose result bits happen to equal the inadmissible inline attempt.
$(B)/jit-vector.o: src/jit.c $(wildcard include/hao/*.h) $(MIN_STAMP)
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -Da64_exec_helper=jit_vector_helper -c $< -o $@

# Translated vector FP, shifts and structure loads/stores against the interpreter.
$(B)/test_jit_vector: tests/aot/jit_vector.c tests/oracle/fma_cases.h tests/oracle/fma16_cases.h $(B)/jit-vector.o $(B)/stubs_oracle.o $(B)/libhaocore.a
	$(CC) $(CFLAGS) -o $@ $< $(B)/jit-vector.o $(B)/stubs_oracle.o $(B)/libhaocore.a
	codesign -s - -f $@

$(B)/test_round16: tests/aot/round16.c $(B)/libhaocore.a
	$(CC) $(CFLAGS) -o $@ $< $(B)/libhaocore.a && codesign -s - -f $@

unit: $(B)/test_round16 $(B)/test_helper_descriptor $(B)/test_jit_vector $(B)/test_imm_cond $(B)/opts_test $(B)/test_abi_layout $(B)/test_macho $(B)/test_images $(B)/test_ifunit $(B)/test_gmap
	timeout -s KILL 120 $(B)/test_round16
	timeout -s KILL 90 $(B)/test_helper_descriptor
	timeout -s KILL 120 $(B)/test_jit_vector
	$(B)/test_imm_cond
	timeout -s KILL 120 $(B)/test_gmap
	$(B)/test_abi_layout
	$(B)/opts_test
	@mkdir -p $(B)/loader-test
	$(B)/test_macho tests/fuzz/corpus/macho $(B)/loader-test
	$(B)/test_images tests/fuzz/corpus/macho $(B)/loader-test
	$(B)/test_ifunit tests/fuzz/corpus/ifunit

# AOT cache trust and format checks, plus the policy unit test
# (tests/aot/run.sh; scratch in AOT_OUT, which it empties first).
AOT_OUT ?= $(B)/aot-test
aot: all
	OUT=$(abspath $(AOT_OUT)) HAO=$(abspath $(B))/hao B=$(abspath $(B)) tests/aot/run.sh

async-contract-test: all
	python3.14 tests/host/async_package.py --build $(B)

test: unit oracle aot bridgetest async-contract-test legacy-dyld-test indexed-mulhigh-test forktest jitlocktest
	tools/check_x86.sh

# Every other test script that runs unattended on a development host
# (tests/check.list, tests/README.md), in parallel (CHECK_J jobs), each with a
# timeout and a private output directory; fails on any failure or on a skip
# tests/expected-skips.txt does not list. `make check` is the full gate.
CHECK_J ?= 8
test-guest: all $(FUZZERS)
	B=$(abspath $(B)) J=$(CHECK_J) tests/check.sh
# The guest scripts exercise test-only switches, so the gate runs them on a
# TEST_SWITCHES build in its own directory; the default build stays as shipped.
# oracle-asan (the oracle checks under ASan and UBSan) is part of the gate and
# not of `make test`: it builds the translator a second time.
check: test
	$(MAKE) oracle-asan
	$(MAKE) B=$(B)-ts TEST_SWITCHES=1 test-guest

# Fuzzing (tests/fuzz/README.md): ASan + trace-pc-guard targets driven by
# tests/fuzz/driver.c (the toolchain has no libFuzzer runtime). `make fuzz`
# builds them; tests/fuzz/run.sh runs them; `make fuzz-smoke` replays the seed
# corpus and runs each target briefly (FUZZ_TIME seconds each, default 20).
FUZZ_CFLAGS := $(ARCH) -std=gnu11 -O1 -g -Iinclude -Isrc -fsanitize=address -fno-omit-frame-pointer $(REPRO)
FUZZ_COV    := -fsanitize-coverage=trace-pc-guard
FUZZERS     := $(B)/fuzz/fuzz_macho $(B)/fuzz/fuzz_typeenc $(B)/fuzz/fuzz_codesig $(B)/fuzz/fuzz_ifunit
FUZZ_TIME   ?= 20
$(B)/fuzz/driver.o: tests/fuzz/driver.c $(MIN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FUZZ_CFLAGS) -c $< -o $@
$(B)/fuzz/fuzz_macho: tests/fuzz/fuzz_macho.c src/macho.c src/code.c src/libval.c include/hao/macho.h include/hao/code.h $(B)/fuzz/driver.o
	$(CC) $(FUZZ_CFLAGS) $(FUZZ_COV) -o $@ tests/fuzz/fuzz_macho.c src/macho.c src/code.c src/libval.c $(B)/fuzz/driver.o && codesign -s - -f $@
$(B)/fuzz/fuzz_typeenc: tests/fuzz/fuzz_typeenc.c src/abi.c $(wildcard include/hao/*.h) $(B)/fuzz/driver.o
	$(CC) $(FUZZ_CFLAGS) $(FUZZ_COV) -fsanitize=undefined -fno-sanitize-recover=all -Wno-unused-function -o $@ tests/fuzz/fuzz_typeenc.c $(B)/fuzz/driver.o && codesign -s - -f $@
$(B)/fuzz/fuzz_codesig: tests/fuzz/fuzz_codesig.c src/libval.c $(wildcard include/hao/*.h) $(B)/fuzz/driver.o
	$(CC) $(FUZZ_CFLAGS) $(FUZZ_COV) -fsanitize=undefined -fno-sanitize-recover=all -Wno-unused-function -o $@ tests/fuzz/fuzz_codesig.c $(B)/fuzz/driver.o && codesign -s - -f $@
$(B)/fuzz/fuzz_ifunit: tests/fuzz/fuzz_ifunit.c src/ifunit.c src/aot_policy.c include/hao/bridge.h include/hao/aot.h $(B)/fuzz/driver.o
	$(CC) $(FUZZ_CFLAGS) $(FUZZ_COV) -fsanitize=undefined -fno-sanitize-recover=all -o $@ tests/fuzz/fuzz_ifunit.c src/ifunit.c src/aot_policy.c $(B)/fuzz/driver.o && codesign -s - -f $@
fuzz: $(FUZZERS)
fuzz-smoke: $(FUZZERS)
	B=$(B) FUZZ_TIME=$(FUZZ_TIME) tests/fuzz/run.sh

# Host testing: a stub that finds libhao in build/ (not for images).
$(B)/hao-stub-dev: src/front/stub.c $(B)/libhao.dylib $(MIN_STAMP)
	$(CC) $(FRONT_CFLAGS) -o $@ $< -L$(B) -lhao -Wl,-rpath,$(abspath $(B)) -Wl,-no_adhoc_codesign

# Swift callback islands share the bounded VM-hole allocator.
$(B)/swiftmeta.o: src/near_map.h

$(B)/swiftvalue/%.o: CFLAGS += -DHAO_SWIFT_VALUE_RUNTIME

# Optional local extensions: a makefile fragment named by HAO_EXTRA_MK (unset by default).
ifdef HAO_EXTRA_MK
include $(HAO_EXTRA_MK)
endif

# Own native ARM/x86/JIT/interpreter/AOT comparisons of legacy dyld pointer streams.
.PHONY: legacy-dyld-test
legacy-dyld-test: all
	HAO=$(abspath $(B))/hao OUT=$(abspath $(B))/legacy-fixups python3.14 -I tests/guest/legacy-fixups/run.py

# Signed indexed saturating multiply-high values, aliases, upper clearing and QC.
.PHONY: indexed-mulhigh-test
indexed-mulhigh-test: all
	HAO=$(abspath $(B))/hao OUT=$(abspath $(B))/indexed-mulhigh python3.14 -I tests/guest/indexed-mulhigh/run.py
