#!/usr/bin/env python3
"""Reject unapproved paths in the runtime tree, optionally across reachable history."""
import argparse
from pathlib import Path
import subprocess


ALLOWLIST = 'releases/runtime-files.txt'
LEGACY_ALLOWLIST = 'distribution/runtime-files.txt'   # before the layout move


def check(repo, ref='HEAD', history=False):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args])

    def allowlist(commit):
        for path in (ALLOWLIST, LEGACY_ALLOWLIST):
            listed = git('ls-tree', '--name-only', commit, '--', path).decode().strip()
            if listed:
                return set(git('show', commit + ':' + path).decode().splitlines())
        raise ValueError('no allowlist in ' + commit)

    # With --history each commit is held to the allowlist it carried.
    refs = git('rev-list', ref).decode().splitlines() if history else [ref]
    failures = []
    for commit in refs:
        allowed = allowlist(commit)
        entries = git('ls-tree', '-rz', commit).split(b'\0')
        for entry in filter(None, entries):
            meta, raw_path = entry.split(b'\t', 1)
            path = raw_path.decode()
            mode, kind, oid = meta.decode().split()
            if path not in allowed or kind != 'blob' or mode not in ('100644', '100755'):
                failures.append((commit, path))
    if failures:
        raise ValueError('unapproved distribution paths: ' + repr(failures[:10]))
    return len(refs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--ref', default='HEAD')
    parser.add_argument('--history', action='store_true')
    args = parser.parse_args()
    print('distribution check: %d commit(s) passed' % check(args.repo, args.ref, args.history))
