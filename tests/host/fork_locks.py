"""Every hao mutex is re-made in the fork child.

A fork copies a mutex another thread holds as held, and that thread does not
exist in the child: its first lock waits forever. So each `static
pthread_mutex_t NAME` in src/ must be named in a function of the same file
whose name ends in `fork_child`, or be listed in ALLOW with the reason it
needs no reset. Each such function must be called from the body of another
(runtime.c's fork_child, say) or registered with pthread_atfork. The script
first checks itself: each of MUTATIONS, applied to the tree, must be reported.

  python3.14 -I -B tests/host/fork_locks.py      exit 0: every mutex covered
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (file, mutex): why the fork child needs no reset
ALLOW = {
    ("src/runtime.c", "load_lock"): "recursive; loads_fork_child re-makes it through load_lock_init",
    ("src/jit.c", "scratch_lock"): "hao_jit_exec_code (oracle and unit tests) only; never taken by a guest",
}

MUTEX = re.compile(r"\bstatic\s+pthread_mutex_t\s+(\w+)")
HOOK = re.compile(r"^[\w \t\*]*?\b(\w*fork_child)\s*\(\s*void\s*\)\s*\{", re.M)


def strip(src):
    """Comments and string literals blanked, line structure kept."""
    out = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group()), src, flags=re.S)
    out = re.sub(r"//[^\n]*", "", out)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', out)


def bodies(src):
    """{hook name: body text} for every *fork_child definition."""
    found = {}
    for m in HOOK.finditer(src):
        depth, k = 0, m.end() - 1
        while k < len(src):
            if src[k] == "{":
                depth += 1
            elif src[k] == "}":
                depth -= 1
                if not depth:
                    break
            k += 1
        found[m.group(1)] = src[m.end():k]
    return found


def check(texts):
    """Problems in {path: stripped source}."""
    bad = []
    hooks, calls = {}, ""
    for rel, src in texts.items():
        hb = bodies(src)
        hooks.update({h: rel for h in hb})
        calls += "\n".join(hb.values()) + "\n"
        for name in MUTEX.findall(src):
            if (rel, name) in ALLOW:
                continue
            if not any(re.search(r"\b%s\b" % re.escape(name), b) for b in hb.values()):
                bad.append(f"{rel}: mutex {name} is in no *fork_child function of its file and not in ALLOW")
    for rel, name in ALLOW:
        if rel not in texts or not re.search(r"\bstatic\s+pthread_mutex_t\s+%s\b" % re.escape(name), texts[rel]):
            bad.append(f"ALLOW lists {rel} {name}, which is not a static mutex there any more")
    everything = "\n".join(texts.values())
    for h, rel in hooks.items():
        # a call inside another hook's body (prototypes and definitions are outside every body)
        called = re.search(r"\b%s\s*\(" % re.escape(h), calls)
        atfork = re.search(r"pthread_atfork\s*\([^;]*\b%s\b" % re.escape(h), everything)
        if not called and not atfork:
            bad.append(f"{rel}: {h} is called from no fork-child hook and not registered with pthread_atfork")
    return bad, hooks


# Each mutation of the tree must be reported: (file, text removed, what it shows)
MUTATIONS = [
    ("src/objc.c", "typestr_fork_child();", "a hook whose only call is gone (a prototype remains)"),
    ("src/runtime.c", "traps_fork_child();", "a static hook whose call is gone"),
    ("src/signals.c", "disp_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;", "a mutex no hook resets"),
    ("src/runtime.c", "hao_signals_fork_child();", "an exported hook runtime.c no longer calls"),
]


def main():
    files = sorted(p for p in (ROOT / "src").rglob("*") if p.suffix in (".c", ".m"))
    texts = {p.relative_to(ROOT).as_posix(): strip(p.read_text(errors="replace")) for p in files}
    bad, hooks = check(texts)
    caught = 0
    for rel, cut, what in MUTATIONS:
        if texts.get(rel, "").count(cut) != 1:
            bad.append(f"mutation check: {rel} no longer has exactly one {cut!r}")
            continue
        if check({**texts, rel: texts[rel].replace(cut, "")})[0]:
            caught += 1
        else:
            bad.append(f"mutation check: removing {cut!r} from {rel} ({what}) is not reported")
    for b in bad:
        print("fork_locks:", b, file=sys.stderr)
    n = sum(len(MUTEX.findall(s)) for s in texts.values())
    print(f"fork_locks: {n} mutexes, {len(hooks)} fork-child hooks, {len(ALLOW)} allowed, "
          f"{caught} of {len(MUTATIONS)} mutations caught, {len(bad)} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
