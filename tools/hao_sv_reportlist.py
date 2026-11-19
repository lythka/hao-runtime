#!/usr/bin/env python3
"""Write src/swiftvalue_report.inc, the Swift value types whose uncertified values hao reports and passes as
found instead of refusing (src/swiftvalue.c report_listed), from the hao-swift-audit records of a boot.

    python3 tools/hao_sv_reportlist.py [--merge] [-o src/swiftvalue_report.inc] LOG...

LOG: files holding hao's diagnostics (console.log under /private/var/log/hao and its user directories); every
line with a "hao-swift-audit {...}" record counts. Run the boot that collects them with the boot argument
hao_swift_value_report=1, so that no unlisted type stops a process before it is recorded. Only records of an
uncertified reason (no row or rule proves the value either way) are taken; a known-wrong value is refused
whether listed or not. --merge keeps the entries the output file already lists. KEY lines name a type by the
walk's table key (src/swiftvalue/Facts.h walk_fact_key, from the records); NOMINAL lines a generic nominal type
outside the standard library by the runtime's name with its generic arguments removed (from the "Swift value
survey:" lines, which name the type the walk declined), admitting each of its instantiations. Each kind is
sorted bytewise for the runtime's binary search.
"""
import argparse
import json
import os
import re
import sys

# src/swiftvalue.c uncertified(): the reasons that mean "not certified", not "known wrong".
EXACT = {
    "type graph past the walk's bounds",
    "guest-defined fields past the walk's bound",
    "box of a type that is not settled cannot be certified at its destruction",
    "array storage of a type that is not settled cannot be certified at its destruction",
    "child task result is copied by native code before it can be certified",
}
KEY = re.compile(r'^[\x21-\x7e]{1,2047}$')
HEAD = """/* Swift value types hao reports and passes as found when no row or rule
 * certifies a value holding them (src/swiftvalue.c report_listed): the types
 * a boot logged as uncertified, until rows certify them. Any other
 * uncertified type is refused. Written by tools/hao_sv_reportlist.py from
 * a boot's diagnostics; edit by rerunning it, not by hand. Each kind sorted
 * bytewise (strcmp). */
"""
SURVEY = (re.compile(r"not certified, passed as found \(report-only\): .* xi=0x[0-9a-f]+ type=(.*)\) root="),
          re.compile(r"not certified and not listed for report-only: .* \(type=(.*)\)$"))


def uncertified(reason):
    return reason in EXACT or "unresolved" in reason


def nominal(name):
    """NAME's generic arguments removed, for a generic nominal type outside the standard library; else None."""
    if "<" not in name or name.startswith(("Swift.", "any ")) or "(unknown context" in name or "<<<" in name:
        return None
    out, depth = [], 0
    for c in name:
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth < 0:
                return None
        elif not depth:
            out.append(c)
    n = "".join(out)
    return n if not depth and KEY.match(n.replace(" ", "_")) and '"' not in n and "\\" not in n else None


def read_entries(paths):
    keys, nominals = set(), set()
    for p in paths:
        with open(p, "rb") as f:
            for raw in f:
                line = raw.decode("utf-8", "replace").rstrip("\n")
                for pat in SURVEY:
                    m = pat.search(line)
                    n = nominal(m.group(1)) if m else None
                    if n:
                        nominals.add(n)
                at = line.find("hao-swift-audit {")
                if at < 0:
                    continue
                try:
                    rec = json.loads(line[at + len("hao-swift-audit "):])
                except ValueError:
                    continue
                key = rec.get("type", "")
                for parent in reversed(rec.get("chain", [])):   # a builtin has no key: its nearest named parent
                    if key:
                        break
                    key = parent if isinstance(parent, str) else ""
                if rec.get("v") == 1 and uncertified(rec.get("reason", "")) and KEY.match(key) and '"' not in key \
                        and "\\" not in key:
                    keys.add(key)
    return keys, nominals


def read_inc(path):
    if not os.path.exists(path):
        return set(), set()
    with open(path) as f:
        text = f.read()
    return ({m.group(1) for m in re.finditer(r'^KEY\("([^"\\]*)"\)$', text, re.M)},
            {m.group(1) for m in re.finditer(r'^NOMINAL\("([^"\\]*)"\)$', text, re.M)})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", default=os.path.join(os.path.dirname(__file__), "..", "src", "swiftvalue_report.inc"))
    ap.add_argument("--merge", action="store_true", help="keep the keys the output already lists")
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    keys, nominals = read_entries(a.logs)
    if a.merge:
        k, n = read_inc(a.output)
        keys |= k
        nominals |= n
    order = lambda k: k.encode()
    out = HEAD + "".join('KEY("%s")\n' % k for k in sorted(keys, key=order)) + \
        "".join('NOMINAL("%s")\n' % n for n in sorted(nominals, key=order))
    tmp = a.output + ".tmp"
    with open(tmp, "w") as f:
        f.write(out)
    os.replace(tmp, a.output)
    print("%s: %d types, %d generic types" % (a.output, len(keys), len(nominals)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
