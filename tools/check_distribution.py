#!/usr/bin/env python3
"""Reject unapproved paths in the runtime tree, optionally across reachable history."""
import argparse
from pathlib import Path
import subprocess


def check(repo, ref='HEAD', history=False):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args])
    allowed = set(git('show', ref + ':distribution/runtime-files.txt').decode().splitlines())
    refs = git('rev-list', ref).decode().splitlines() if history else [ref]
    failures = []
    for commit in refs:
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
