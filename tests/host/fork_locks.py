"""Every hao mutex is re-made in the fork child.

A fork copies a mutex another thread holds as held, and that thread does not
exist in the child: its first lock waits forever. So each `static
pthread_mutex_t NAME` in src/ must be named in a function of the same file
whose name ends in `fork_child`, or be listed in ALLOW with the reason it
needs no reset. Each such function must be called (from runtime.c's
fork_child or another hook) or registered with pthread_atfork.

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


def main():
    files = sorted(p for p in (ROOT / "src").rglob("*") if p.suffix in (".c", ".m"))
    texts = {p.relative_to(ROOT).as_posix(): strip(p.read_text(errors="replace")) for p in files}
    bad = []
    hooks = {}
    for rel, src in texts.items():
        hb = bodies(src)
        hooks.update({h: rel for h in hb})
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
        calls = len(re.findall(r"\b%s\s*\(" % re.escape(h), everything)) - 1
        atfork = re.search(r"pthread_atfork\s*\([^;]*\b%s\b" % re.escape(h), everything)
        if calls < 1 and not atfork:
            bad.append(f"{rel}: {h} is never called or registered")
    for b in bad:
        print("fork_locks:", b, file=sys.stderr)
    n = sum(len(MUTEX.findall(s)) for s in texts.values())
    print(f"fork_locks: {n} mutexes, {len(hooks)} fork-child hooks, {len(ALLOW)} allowed, {len(bad)} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
