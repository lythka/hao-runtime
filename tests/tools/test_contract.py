"""hao-runtime keeps its contract (contract.json, checked by the vendored hao-assets checker: generated.lock
against src/*_gen.inc and include/hao/gen_stamp.h), and the vendored copy is exactly the pinned one.
    python3.14 -I -B tests/tools/test_contract.py"""
import hashlib
from pathlib import Path
import runpy
import unittest

TOP = Path(__file__).resolve().parents[2]
VENDORED = TOP / 'tools/hao-contract/hao_contract.py'


class Contract(unittest.TestCase):
    def test_vendored_checker_is_the_pinned_one(self):
        _, commit, sha = (TOP / 'tools/hao-contract/PIN').read_text().split()
        body = VENDORED.read_text().split('\n', 2)[2]            # after the two-line vendoring header
        self.assertEqual(hashlib.sha256(body.encode()).hexdigest(), sha, 'vendored checker differs from %s' % commit)

    def test_repository_keeps_its_contract(self):
        self.assertEqual(runpy.run_path(str(VENDORED))['check'](TOP), [])


if __name__ == '__main__':
    unittest.main()
