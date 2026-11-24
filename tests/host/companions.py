#!/usr/bin/env python3
"""Exercise actual signed native launchers and ARM libraries without starting any service."""
import hashlib
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import hao_codesig


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def signed(path):
    run("codesign", "--force", "--sign", "-", path, stderr=subprocess.PIPE)
    return path


def signature(path):
    sig = hao_codesig.check(path.read_bytes())
    assert sig["verdict"] == "valid", path
    return bytes([sig["hash_type"]]) + sig["cdhash"]


def invoke(binary, guest, required, succeeds):
    proc = subprocess.run([binary, guest], text=True, capture_output=True, timeout=45,
                          env={**os.environ, "HAO_VERBOSE": "1"})
    output = proc.stderr + proc.stdout
    assert (proc.returncode == 0) == succeeds and all(word in output for word in required), (proc.returncode, output)
    if not succeeds:
        assert "guest result 74" not in output, output
    return output


def test(tmp, lib):
    original = tmp / "Original.dylib"
    adapter = tmp / "Host.dylib"
    library_id = "@rpath/Original.dylib"
    arm = tmp / "original.c"
    arm.write_text("int fixture_value(void) { return 74; }\n")
    run("clang", "-arch", "arm64", "-dynamiclib", arm, "-install_name", library_id, "-o", original)
    signed(original)
    arm_sig = signature(original)
    marker = tmp / "marker"
    marker.write_bytes(b"haoslice" + bytes([1, arm_sig[0], 0, 0]) + arm_sig[1:])
    empty = tmp / "empty.c"
    empty.touch()
    run("clang", "-arch", "x86_64", "-dynamiclib", empty, "-install_name", adapter, "-Xlinker", "-sectcreate",
        "-Xlinker", "__TEXT", "-Xlinker", "__hao_adapter", "-Xlinker", marker, "-o", adapter)
    signed(adapter)
    native_sig = signature(adapter)
    sha = hashlib.sha256(original.read_bytes()).digest()
    record = arm_sig + native_sig + sha + b"\0".join(os.fsencode(x) for x in (original, library_id, adapter)) + b"\0"
    guest_source = tmp / "guest.c"
    guest_source.write_text('#include <stdio.h>\nextern int fixture_value(void);\n'
                            'int main(void) { int n=fixture_value(); printf("guest result %d\\n",n); return n!=74; }\n')
    guest = tmp / "guest"
    run("clang", "-arch", "arm64", guest_source, original, "-headerpad_max_install_names", "-Wl,-rpath,@executable_path", "-o", guest)
    signed(guest)

    def launcher(name, records, declare=True, source=None, identity=True):
        binary = tmp / name
        args = []
        if records is not None:
            section = tmp / (name + ".record")
            section.write_bytes(records)
            args.extend(["-Xlinker", "-sectcreate", "-Xlinker", "__TEXT", "-Xlinker", "__hao_companions", "-Xlinker", section])
        if declare:
            args.extend(["-Xlinker", "-needed_library", "-Xlinker", adapter])
        run("clang", "-arch", "x86_64", source or ROOT / "src/front/cli.c", *args, "-I", ROOT / "include", "-L", lib,
            "-lhao", *(["-lhao_identity"] if identity else []), "-Xlinker", "-rpath", "-Xlinker", lib, "-o", binary)
        return signed(binary)
    prefix = b"haocomp1"
    positive = launcher("declared", prefix + record)
    invoke(positive, guest, ["verified mapped native framework companion", "verified original ARM framework", "guest result 74"], True)
    symlink = tmp / "AliasOfOriginal.dylib"
    symlink.symlink_to(original)
    for index, path in enumerate((original, symlink)):
        direct = tmp / f"absolute-guest-{index}"
        shutil.copy2(guest, direct)
        run("install_name_tool", "-change", library_id, path, direct, stderr=subprocess.PIPE)
        signed(direct)
        invoke(positive, direct, ["verified original ARM framework", "guest result 74"], True)
    hidden = adapter.with_suffix(".hidden")
    adapter.rename(hidden)
    try:
        invoke(positive, guest, ["Library not loaded"], False)
    finally:
        hidden.rename(adapter)
    invoke(launcher("unselected", None, False), guest, ["guest result 74"], True)
    invoke(launcher("missing-hook", prefix + record, identity=False), guest,
           ["require the pre-open identity hook"], False)
    invoke(launcher("missing-edge", prefix + record, False), guest, ["invalid or ambiguous direct framework"], False)
    invoke(launcher("duplicate", prefix + record + record), guest, ["invalid or ambiguous direct framework"], False)
    bad = bytearray(record)
    bad[22] ^= 1
    invoke(launcher("wrong-adapter", prefix + bad), guest, ["mapped native framework companion failed verification"], False)
    bad = bytearray(record)
    bad[42] ^= 1
    invoke(launcher("wrong-original", prefix + bad), guest, ["original framework SHA-256 differs"], False)
    # Preload a different physical guest dylib with the same install name
    # before starting the selected guest. Its export must never satisfy the import.
    alias = tmp / "alias.dylib"
    arm.write_text("int fixture_value(void) { return 99; }\n")
    run("clang", "-arch", "arm64", "-dynamiclib", arm, "-install_name", library_id, "-o", alias)
    signed(alias)
    preload = tmp / "preload.c"
    preload.write_text((ROOT / "src/front/cli.c").read_text().replace(
        "return hao_cli_main(argc, argv, envp);",
        'extern void *hao_load_guest_library(const char *,char *,unsigned long); char why[512]; '
        f'if (!hao_load_guest_library("{alias}",why,sizeof why)) return 3; '
        'return hao_cli_main(argc, argv, envp);'))
    invoke(launcher("alias-collision", prefix + record, source=preload), guest,
           ["resident install-name alias differs"], False)
    preload.write_text(preload.read_text().replace(str(alias), str(original)))
    invoke(launcher("unverified-resident", prefix + record, source=preload), guest,
           ["resident install-name alias differs"], False)

    competing = tmp / "earlier-runpath"
    competing.mkdir()
    shutil.copy2(alias, competing / original.name)
    dynamic_source = tmp / "dynamic.c"
    dynamic_source.write_text('#include <dlfcn.h>\n#include <stdio.h>\n'
                              'int main(void) { void *h=dlopen("@rpath/Original.dylib", RTLD_NOW);'
                              'if (!h) { puts(dlerror()); return 2; } int (*f)(void)=dlsym(h,"fixture_value");'
                              'if (!f) return 3; printf("dynamic result %d\\n", f()); return 0; }\n')
    dynamic = tmp / "dynamic-guest"
    run("clang", "-arch", "arm64", dynamic_source, "-Wl,-rpath,@executable_path/earlier-runpath",
        "-Wl,-rpath,@executable_path", "-o", dynamic)
    signed(dynamic)
    invoke(tmp / "unselected", dynamic, ["dynamic result 99"], True)
    invoke(positive, dynamic, ["framework lookup differs from declared original"], False)
    (competing / original.name).unlink()
    invoke(positive, dynamic, ["dynamic result 74"], True)
    # Select the declared name, then move the identical signed file and put
    # a symlink at its former canonical path. Use the runtime's loader entry
    # on that saved selection. It must refuse the path change even though the
    # file bytes, signature, and inode behind the symlink are unchanged.
    selected_test = tmp / "selected-test.c"
    selected_test.write_text('#include <stdbool.h>\n#include <stdio.h>\n#include <unistd.h>\n'
                             'extern bool hao_companions_init(char *,size_t);\n'
                             'extern const char *hao_companion_original(const char *);\n'
                             'extern void *hao_load_guest_library(const char *,char *,size_t);\n'
                             'int main(void) { char why[512]; if(!hao_companions_init(why,sizeof why)) return 1;'
                             f'const char *p=hao_companion_original("{library_id}");'
                             f'const char *moved="{tmp / "moved.dylib"}";'
                             'if(!p || rename(p,moved) || symlink(moved,p)) return 2;'
                             'void *im=hao_load_guest_library(p,why,sizeof why);'
                             'if(unlink(p) || rename(moved,p)) return 3;'
                             'if(im) return 4; puts(why); return 0; }\n')
    invoke(launcher("post-selection", prefix + record, source=selected_test), guest,
           ["cannot identify declared original framework"], True)
    # A strong dependency spelled through a symlink can fail after startup.
    # Its refusal must survive the ordinal lookup and fail dlopen, including
    # when no symbol references the required library. The native test launcher
    # exports the same symbol to make accidental flat fallback observable.
    native_export = tmp / "native-export.c"
    native_export.write_text((ROOT / "src/front/cli.c").read_text() +
                             '\n__attribute__((visibility("default"))) int fixture_value(void) {return 99;}\n')
    late_launcher = launcher("late-launcher", prefix + record, source=native_export)
    late_guest_c = tmp / "late-guest.c"
    saved = tmp / "saved-original"
    late_guest_c.write_text('#include <dlfcn.h>\n#include <stdio.h>\n#include <string.h>\n'
                            'int main(int argc,char **argv) { char why[512]; '
                            f'if(rename("{original}","{saved}") || rename("{alias}","{original}")) return 1;'
                            'void *h=dlopen(argv[1],RTLD_NOW); const char *error=h ? "" : dlerror();'
                            'snprintf(why,sizeof why,"%s",error ? error : "");'
                            f'if(rename("{original}","{alias}") || rename("{saved}","{original}")) return 2;'
                            'printf("late framework: %s\\n",why); return h || !strstr(why,"declared framework dependency"); }\n')
    late_guest = tmp / "late-guest"
    run("clang", "-arch", "arm64", late_guest_c, "-o", late_guest)
    signed(late_guest)
    for mode in ("imported", "required-only"):
        late_c = tmp / "late.c"
        late_c.write_text("extern int fixture_value(void); int late(void) { return fixture_value(); }\n"
                          if mode == "imported" else "int late(void) { return 17; }\n")
        late = tmp / (mode + ".dylib")
        run("clang", "-arch", "arm64", "-dynamiclib", "-headerpad_max_install_names", late_c,
            "-Xlinker", "-needed_library", "-Xlinker", original, "-o", late)
        run("install_name_tool", "-change", library_id, symlink, late, stderr=subprocess.PIPE)
        signed(late)
        try:
            proc = subprocess.run([late_launcher, late_guest, late], text=True, capture_output=True, timeout=45)
            assert proc.returncode == 0 and "declared framework dependency" in proc.stdout, (proc.returncode, proc.stderr, proc.stdout)
        finally:
            if saved.exists():
                if original.exists():
                    os.replace(original, alias)
                os.replace(saved, original)
    # A publishes its export while binding B, B binds back to A, and A's
    # required-only companion subsequently fails. Check residency without
    # calling provisional addresses, then restore and verify a normal retry.
    a, b = tmp / "CycleA.dylib", tmp / "CycleB.dylib"
    cycle_source = tmp / "cycle.c"

    def cycle_library(path, text, *deps):
        cycle_source.write_text(text)
        run("clang", "-arch", "arm64", "-dynamiclib", cycle_source, "-headerpad_max_install_names",
            "-install_name", path, *deps, "-o", path)
        signed(path)
    steady = tmp / "Steady.dylib"
    cycle_library(steady, "int steady(void){return 73;}\n")
    cycle_library(a, "int a_value(void){return 7;}\n")
    cycle_library(b, "extern int a_value(void); int b_value(void){return a_value()+1;}\n", a)
    cycle_library(a, "extern int b_value(void); int a_value(void){return 7;} int call_b(void){return b_value();}\n",
                  b, "-Xlinker", "-needed_library", "-Xlinker", original)
    run("install_name_tool", "-change", library_id, symlink, a, stderr=subprocess.PIPE)
    signed(a)
    cycle_guest_c = tmp / "cycle-guest.c"
    cycle_guest_c.write_text('#include <dlfcn.h>\n#include <stdio.h>\n'
                             f'int main(void) {{ if(!dlopen("{steady}",RTLD_NOW)) return 6;'
                             f'if(rename("{original}","{saved}") || rename("{alias}","{original}")) return 1;'
                             f'void *bad=dlopen("{a}",RTLD_NOW);'
                             f'void *resident=dlopen("{b}",RTLD_NOLOAD|RTLD_NOW);'
                             f'if(rename("{original}","{alias}") || rename("{saved}","{original}")) return 2;'
                             'if(bad || resident) return 3;'
                             f'if(!dlopen("{steady}",RTLD_NOLOAD|RTLD_NOW) || !dlopen("{a}",RTLD_NOW)) return 7;'
                             f'void *h=dlopen("{b}",RTLD_NOW); if(!h) {{puts(dlerror()); return 4;}}'
                             'int (*value)(void)=dlsym(h,"b_value"); if(!value || value()!=8) return 5;'
                             'puts("cycle rejected, detached, restored and retried"); return 0; }\n')
    cycle_guest = tmp / "cycle-guest"
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    try:
        invoke(positive, cycle_guest, ["cycle rejected, detached, restored and retried"], True)
        cycle_library(b, "extern int a_value(void); int b_value(void){return a_value()+1;}\n",
                      "-undefined", "dynamic_lookup")
        invoke(positive, cycle_guest, ["cycle rejected, detached, restored and retried"], True)
        # A native dependency initializer completes B, then reenters the
        # normal native dlopen path for an independent ARM library. Its own
        # initializer must run immediately, while B's remains deferred.
        inner = tmp / "Inner.dylib"
        inner_mark, b_mark, observation = (tmp / x for x in ("inner-init", "b-init", "native-observation"))
        def marker_initializer(path):
            return ('#include <stdio.h>\n__attribute__((constructor)) static void record(void){'
                    f'FILE *f=fopen("{path}","w"); if(f) fclose(f);' + '}\n')
        cycle_library(inner, marker_initializer(inner_mark) + "int independent(void){return 42;}\n")
        cycle_library(b, marker_initializer(b_mark) + "extern int a_value(void); int b_value(void){return a_value()+1;}\n", a)
        lazy = tmp / "LazyConsumer.dylib"
        cycle_library(lazy, "extern int b_value(void); int lazy_value(void){return b_value();}\n",
                      "-mmacosx-version-min=27.0", f"-Wl,-lazy_library,{b}")
        native_lib = tmp / "Reenter.dylib"
        call_during_reentry = tmp / "call-during-reentry"
        native_source = tmp / "reenter.c"
        native_source.write_text('#include <stdio.h>\n#include <dlfcn.h>\n#include <unistd.h>\n'
                                 '#include "hao/runtime.h"\n#include <string.h>\n'
                                 'static int lazy_deferred(void){ char why[512];'
                                 f'hao_image *im=hao_load_guest_library("{lazy}",why,sizeof why);'
                                 'if(!im || im->nlazy!=1 || im->lazy_info_size[0]<24) return 0;'
                                 f'if(!access("{call_during_reentry}",F_OK)) {{ uint64_t fn=0;'
                                 'if(!hao_image_export(im,"_lazy_value",&fn)) return 0;'
                                 'uint64_t result=hao_call_guest(hao_current(),fn,NULL,0);'
                                 'if(result==8) puts("constructor lazy ARM result 8"); return result==8;}'
                                 'uint32_t rec[6]; memcpy(rec,im->lazy_info[0],sizeof rec);'
                                 'if(rec[4]!=1) return 0;'
                                 'uint32_t *flag=(void *)(im->base+rec[1]); uint64_t *slot=(void *)(im->base+rec[3]);'
                                 'uint64_t saved=*slot; return !*flag && hao_try_lazy_load((uintptr_t)flag,im->base)'
                                 '==HAO_LAZY_DEFERRED && !*flag && *slot==saved;}'
                                 '__attribute__((constructor)) static void reenter(void) { char why[512];'
                                 f'void *loaded=hao_load_guest_library("{b}",why,sizeof why);'
                                 f'void *h=dlopen("{inner}",RTLD_NOW);'
                                 f'FILE *f=fopen("{observation}","w"); if(f) {{'
                                 f'if(loaded && h && lazy_deferred() && !access("{inner_mark}",F_OK) && access("{b_mark}",F_OK)) '
                                 'fputs("independent initialized; cycle deferred; lazy untouched",f); else fputs("wrong order",f); fclose(f); }}'
                                 'int native_trigger(void) {return 1;}\n')
        run("clang", "-arch", "x86_64", "-dynamiclib", native_source, "-I", ROOT / "include", "-install_name", native_lib,
            "-L", lib, "-lhao", "-Xlinker", "-rpath", "-Xlinker", lib, "-o", native_lib)
        signed(native_lib)
        interface = tmp / "arm-interface.dylib"
        cycle_source.write_text("int native_trigger(void){return 1;}\n")
        run("clang", "-arch", "arm64", "-dynamiclib", cycle_source, "-install_name", native_lib, "-o", interface)
        cycle_library(a, "extern int native_trigger(void),b_value(void); int a_value(void){return 7;} "
                      "int call_b(void){return native_trigger()+b_value();}\n",
                      interface, b, "-Xlinker", "-needed_library", "-Xlinker", original)
        run("install_name_tool", "-change", library_id, symlink, a, stderr=subprocess.PIPE)
        signed(a)
        retry = (f'void *lh=dlopen("{lazy}",RTLD_NOW); if(!lh) return 8;'
                 'int (*lv)(void)=dlsym(lh,"lazy_value"); if(!lv || lv()!=8) return 9;')
        cycle_guest_c.write_text(cycle_guest_c.read_text().replace('puts("cycle rejected', retry + 'puts("cycle rejected'))
        run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
        signed(cycle_guest)
        invoke(positive, cycle_guest, ["cycle rejected, detached, restored and retried"], True)
        assert observation.read_text() == "independent initialized; cycle deferred; lazy untouched" and b_mark.exists()
        # The ABI supplies no failure return. A recursive synchronous call
        # into an unsettled closure cannot wait on its own native constructor.
        call_during_reentry.touch()
        invoke(positive, cycle_guest, ["provider is still binding in a reentrant load"], False)
    finally:
        if saved.exists():
            if original.exists():
                os.replace(original, alias)
            os.replace(saved, original)
    # The same recursive call can complete when its entire closure validates.
    cycle_guest_c.write_text('#include <dlfcn.h>\n#include <stdio.h>\n'
                             f'int main(void) {{void *h=dlopen("{a}",RTLD_NOW); if(!h) return 1;'
                             'int (*f)(void)=dlsym(h,"call_b"); if(!f || f()!=9)return 2;'
                             'puts("outer ARM result 9"); return 0;}')
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    for exe in (positive, launcher("ordinary-cycle", None, False)):
        invoke(exe, cycle_guest, ["constructor lazy ARM result 8", "outer ARM result 9"], True)

    original_a = tmp / "saved-chained-a"
    shutil.copy2(a, original_a)
    cycle_guest_c.write_text('#include <dlfcn.h>\n#include <stdio.h>\n#include <pthread.h>\n'
                             '#include <stdatomic.h>\n#include <sched.h>\nstatic atomic_int ready,go;'
                             'static void *worker(void *unused){atomic_fetch_add(&ready,1);'
                             'while(!atomic_load(&go))sched_yield();'
                             f'void *h=dlopen("{a}",RTLD_NOW);int (*f)(void)=h?dlsym(h,"call_b"):NULL;'
                             'return (void *)(long)(f?f():0);}'
                             'int main(void){pthread_t t[2]; void *v;'
                             'for(int k=0;k<2;k++)if(pthread_create(&t[k],0,worker,0))return 1;'
                             'while(atomic_load(&ready)!=2)sched_yield();atomic_store(&go,1);'
                             'for(int k=0;k<2;k++)if(pthread_join(t[k],&v) || (long)v!=9)return 2;'
                             'puts("two concurrent opens return 9");return 0;}')
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    for fixups in ("chained", "legacy"):
        if fixups == "legacy":
            cycle_library(a, "extern int native_trigger(void),b_value(void);int a_value(void){return 7;}"
                          "int call_b(void){return native_trigger()+b_value();}\n",
                          "-mmacosx-version-min=11.0", "-Wl,-no_fixup_chains", interface, b,
                          "-Xlinker", "-needed_library", "-Xlinker", original)
            run("install_name_tool", "-change", library_id, symlink, a, stderr=subprocess.PIPE)
            signed(a)
        text = invoke(positive, cycle_guest, ["constructor lazy ARM result 8", "two concurrent opens return 9"], True)
        assert text.count("constructor lazy ARM result 8") == 1, text
    os.replace(original_a, a)
    # Interpose only this authored launcher's identity-hook installer. The
    # ordinary loader still admits and initializes its real dependency.
    # Vary its outer result *after* the verified recursive call returned.
    reconcile_source = tmp / "reconcile-launcher.c"
    cycle_guest_c.write_text('#include <dlfcn.h>\n#include <stdio.h>\n'
                             f'int main(void) {{if(dlopen("{a}",RTLD_NOW)) return 1;'
                             f'void *h=dlopen("{a}",RTLD_NOLOAD|RTLD_NOW); if(!h) return 2;'
                             'int (*f)(void)=dlsym(h,"call_b"); if(!f || f()!=9)return 3;'
                             'puts("outer refused, committed ARM result 9"); return 0;}')
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    for tag, replacement in (("error", "NULL"), ("different", 'real("/usr/lib/libSystem.B.dylib",RTLD_NOW)')):
        reconcile_source.write_text((ROOT / "src/front/cli.c").read_text() +
                                    '#include "hao/native_bundle.h"\n#include <dlfcn.h>\n#include <string.h>\n'
                                    'static hao_real_dlopen_fn real;'
                                    'static void *outer(const char *p,int f) {void *h=real(p,f);'
                                    f'if(h && !(f&RTLD_NOLOAD) && p && !strcmp(p,"{native_lib}")) {{'
                                    f'dlclose(h); return {replacement};}} return h;}}'
                                    'hao_real_dlopen_fn hao_identity_bundle_hooks2(hao_bundle_open2_fn o,'
                                    'hao_bundle_sym2_fn s,hao_bundle_close_fn c,hao_bundle_class_fn k,hao_bundle_expand_fn e)'
                                    '{hao_bundle_install2_fn install=dlsym(RTLD_NEXT,"hao_identity_bundle_hooks2");'
                                    'real=install(o,s,c,k,e); return outer;}')
        exe = launcher("reconcile-" + tag, prefix + record, source=reconcile_source)
        invoke(exe, cycle_guest, ["constructor lazy ARM result 8", "outer refused, committed ARM result 9"], True)

    # The same native open may occur after mapping, during discovery of a
    # required edge with no import. The outer dlopen still owns that result.
    cycle_library(a, "extern int b_value(void);int a_value(void){return 7;}int call_b(void){return 1+b_value();}\n",
                  "-Xlinker", "-needed_library", "-Xlinker", interface, b,
                  "-Xlinker", "-needed_library", "-Xlinker", original)
    run("install_name_tool", "-change", library_id, symlink, a, stderr=subprocess.PIPE)
    signed(a)
    for tag in ("error", "different"):
        invoke(tmp / ("reconcile-" + tag), cycle_guest,
               ["constructor lazy ARM result 8", "outer refused, committed ARM result 9"], True)

    # The cyclic consumers have an outgoing edge to a *different* pending
    # image. Its required-only companion is refused during constructor reentry.
    provider, proxy = tmp / "Provider.dylib", tmp / "Reexport.dylib"
    consumer_a, consumer_b = tmp / "ConsumerA.dylib", tmp / "ConsumerB.dylib"
    event = tmp / "consumer-init"
    cycle_library(provider, "int external_value(void){return 31;}\n")
    cycle_library(proxy, "int proxy_marker(void){return 1;}\n", f"-Wl,-reexport_library,{provider}")
    cycle_library(consumer_a, "int own_value(void){return 7;}\n")
    ext_hook = tmp / "OpenConsumers.dylib"
    ext_source = tmp / "open-consumers.c"
    ext_source.write_text('#include "hao/runtime.h"\n#include <dlfcn.h>\n'
                          '__attribute__((constructor)) static void open_consumers(void){char why[512];'
                          f'hao_load_guest_library("{consumer_a}",why,sizeof why);'
                          f'dlopen("{inner}",RTLD_NOW);}} int native_edge(void){{return 1;}}')
    run("clang", "-arch", "x86_64", "-dynamiclib", ext_source, "-I", ROOT / "include", "-install_name", ext_hook,
        "-L", lib, "-lhao", "-Xlinker", "-rpath", "-Xlinker", lib, "-o", ext_hook)
    signed(ext_hook)
    cycle_source.write_text("int native_edge(void){return 1;}\n")
    run("clang", "-arch", "arm64", "-dynamiclib", cycle_source, "-install_name", ext_hook, "-o", interface)
    cycle_library(provider, 'extern int native_edge(void); int external_value(void){return 31;}'
                  ' int call_native(void){return native_edge();}\n', interface, "-Xlinker", "-needed_library",
                  "-Xlinker", original)
    run("install_name_tool", "-change", library_id, symlink, provider, stderr=subprocess.PIPE)
    signed(provider)
    cycle_guest_c.write_text('#include <stdio.h>\n#include <unistd.h>\n#include <dlfcn.h>\n'
                             f'int main(void) {{if(rename("{original}","{saved}") || rename("{alias}","{original}")) return 1;'
                             f'if(dlopen("{provider}",RTLD_NOW) || dlopen("{consumer_a}",RTLD_NOLOAD|RTLD_NOW) ||'
                             f'dlopen("{consumer_b}",RTLD_NOLOAD|RTLD_NOW) || !access("{event}",F_OK) ||'
                             f'!dlopen("{inner}",RTLD_NOLOAD|RTLD_NOW)) return 2;'
                             f'if(rename("{original}","{alias}") || rename("{saved}","{original}")) return 3;'
                             f'if(!dlopen("{provider}",RTLD_NOW))return 4;void *h=dlopen("{consumer_a}",RTLD_NOW);'
                             'int (*v)(void)=h?dlsym(h,"both_values"):NULL; if(!v || v()!=38)return 5;'
                             'puts("external provider refused, independent image retained, retry 38");return 0;}')
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    try:
        for providers in (("-undefined", "dynamic_lookup"), (proxy,)):
            event.unlink(missing_ok=True)
            cycle_library(consumer_b, marker_initializer(event) + "extern int own_value(void),external_value(void);"
                          "int consumer_value(void){return own_value()+external_value();}\n", consumer_a, *providers)
            cycle_library(consumer_a, "extern int consumer_value(void); int own_value(void){return 7;}"
                          "int both_values(void){return consumer_value();}\n", consumer_b)
            invoke(positive, cycle_guest, ["external provider refused, independent image retained, retry 38"], True)
            assert event.exists()
    finally:
        if saved.exists():
            if original.exists():
                os.replace(original, alias)
            os.replace(saved, original)
    # Neither edge supplies an import. Discovery of the first native
    # dependency reenters while the second, ordinary strong edge is missing.
    missing, missing_saved = tmp / "RequiredOnly.dylib", tmp / "saved-required.dylib"
    owner_event = tmp / "owner-init"
    cycle_library(missing, "int ordinary_required(void){return 1;}\n")
    cycle_library(provider, marker_initializer(owner_event) + "int external_value(void){return 31;}\n",
                  "-Xlinker", "-needed_library", "-Xlinker", interface,
                  "-Xlinker", "-needed_library", "-Xlinker", missing)
    cycle_guest_c.write_text('#include <stdio.h>\n#include <unistd.h>\n#include <dlfcn.h>\n'
                             f'int main(void) {{if(rename("{missing}","{missing_saved}"))return 1;'
                             f'if(dlopen("{provider}",RTLD_NOW) || !access("{owner_event}",F_OK) ||'
                             f'!access("{event}",F_OK) || !dlopen("{inner}",RTLD_NOLOAD|RTLD_NOW))return 2;'
                             f'if(rename("{missing_saved}","{missing}") || !dlopen("{provider}",RTLD_NOW))return 3;'
                             f'if(access("{owner_event}",F_OK))return 4;puts("required-only refusal and retry");return 0;}}')
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    try:
        for exe in (positive, launcher("ordinary-required", None, False)):
            owner_event.unlink(missing_ok=True)
            event.unlink(missing_ok=True)
            invoke(exe, cycle_guest, ["required-only refusal and retry"], True)
    finally:
        if missing_saved.exists():
            os.replace(missing_saved, missing)

    # No native constructor and no imports reference the missing dependency.
    cycle_library(provider, marker_initializer(owner_event) + "int external_value(void){return 31;}\n",
                  "-Xlinker", "-needed_library", "-Xlinker", missing)
    cycle_guest_c.write_text('#include <stdio.h>\n#include <unistd.h>\n#include <dlfcn.h>\n'
                             f'int main(void) {{if(rename("{missing}","{missing_saved}"))return 1;'
                             f'if(dlopen("{provider}",RTLD_NOW) || !access("{owner_event}",F_OK))return 2;'
                             f'if(rename("{missing_saved}","{missing}") || !dlopen("{provider}",RTLD_NOW))return 3;'
                             f'if(access("{owner_event}",F_OK))return 4;puts("ordinary required-only refusal and retry");return 0;}}')
    run("clang", "-arch", "arm64", cycle_guest_c, "-o", cycle_guest)
    signed(cycle_guest)
    try:
        for exe in (positive, launcher("ordinary-missing", None, False)):
            owner_event.unlink(missing_ok=True)
            invoke(exe, cycle_guest, ["ordinary required-only refusal and retry"], True)
    finally:
        if missing_saved.exists():
            os.replace(missing_saved, missing)
    # A new validly signed file at the identical canonical path must still fail.
    arm.write_text("int fixture_value(void) { return 75; }\n")
    replacement = tmp / "replacement"
    run("clang", "-arch", "arm64", "-dynamiclib", arm, "-install_name", library_id, "-o", replacement)
    os.replace(signed(replacement), original)
    invoke(positive, guest, ["original framework signature or install name differs"], False)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        test(Path(directory).resolve(), Path(sys.argv[1]).resolve())
    print("framework companions: signed declarations, real ARM export, independent launcher and rejection controls PASS")
