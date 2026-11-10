"""Exercise package selection and reject untrusted code before its constructor runs.

Own-code native probes only. A guarded, root-owned positive control also runs on
the target; these unprivileged host checks must reject guarded package loading.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r'''
#include "hao/runtime.h"
#include <stdio.h>
int main(void) {
    hao_opts_init(NULL);
    struct { void *parent,*resume; } context={0};
    hao_cpu cpu={0}; cpu.x[22]=(uint64_t)(uintptr_t)&context;
    hao_async_contract_prepare(&cpu,0,NULL);
    puts("completed"); return 0;
}
'''
PROVIDER = r'''
#include "hao/async_contract.h"
#include <stdio.h>
#ifndef TEST_ABI
#define TEST_ABI HAO_ASYNC_CONTRACT_ABI
#endif
__attribute__((constructor)) static void start(void) { fputs("provider-constructor\n",stderr); }
static bool match(const hao_async_call *call,hao_async_rule *rule) { return false; }
const hao_async_provider *hao_async_contract_provider_v1(void) {
    static const hao_async_provider p={TEST_ABI,sizeof p,"package-test",match}; return &p;
}
'''


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='hao-package-tests-', dir='/tmp')
        cls.base = Path(cls.work.name)
        (cls.base/'driver.c').write_text(DRIVER)
        (cls.base/'provider.c').write_text(PROVIDER)
        common = ['clang','-arch','x86_64','-mmacosx-version-min=27.0','-I'+str(ROOT/'include')]
        commands = [common+['-o',str(cls.base/'driver'),str(cls.base/'driver.c'),
                            '-L'+str(cls.build),'-lhao','-Wl,-rpath,@executable_path']]
        for name, abi in [('valid',1),('invalid',99)]:
            commands.append(common+['-dynamiclib','-DTEST_ABI='+str(abi),'-o',str(cls.base/(name+'.dylib')),
                                    str(cls.base/'provider.c')])
        for command in commands:
            subprocess.run(command,check=True,capture_output=True)
            binary = command[command.index('-o')+1]
            subprocess.run(['codesign','-s','-','-f',binary],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(dir=self.base))
        shutil.copy2(self.build/'libhao.dylib',self.directory/'libhao.dylib')
        shutil.copy2(self.base/'driver',self.directory/'driver')
        self.package = self.directory/'libhao_async_contract.dylib'

    def provider(self, name='valid'):
        shutil.copy2(self.base/(name+'.dylib'),self.package)

    def run_probe(self, **environment):
        env = {k:v for k,v in os.environ.items() if not k.startswith(('HAO_','DYLD_'))}
        env.update(HAO_VERBOSE='1',**environment)
        return subprocess.run([str(self.directory/'driver')],env=env,capture_output=True,text=True,timeout=15)

    def refused(self, result, message):
        self.assertNotEqual(result.returncode,0,result.stderr)
        self.assertIn(message,result.stderr)
        self.assertNotIn('provider-constructor',result.stderr)

    def test_absent_package_preserves_runtime_without_provider(self):
        result=self.run_probe()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout,'completed\n')
        self.assertNotIn('provider-constructor',result.stderr)

    def test_trusted_user_package_loads(self):
        self.provider();result=self.run_probe()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('package-test (package)',result.stderr)
        self.assertEqual(result.stderr.count('provider-constructor'),1)

    def test_explicit_open_override_remains_available(self):
        self.provider('invalid')
        result=self.run_probe(HAO_ASYNC_CONTRACT_LIBRARY=str(self.base/'valid.dylib'))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('package-test (environment)',result.stderr)

    def test_guarded_and_setugid_do_not_load_user_code(self):
        self.provider()
        for tier in ['1','2']:
            result=self.run_probe(HAO_DISTRUST_ENV=tier,HAO_ASYNC_CONTRACT_LIBRARY=str(self.base/'valid.dylib'))
            self.refused(result,'async contract:')
            self.assertRegex(result.stderr,r'owned by uid|writable by group or others')
            # Setugid also suppresses HAO_VERBOSE; refusal before the provider
            # constructor is the invariant, not availability of diagnostics.
            if tier == '1':
                self.assertIn('HAO_ASYNC_CONTRACT_LIBRARY ignored',result.stderr)

    def test_writable_provider_refused_before_loading(self):
        self.provider();self.package.chmod(0o666)
        self.refused(self.run_probe(),'writable by group or others')

    def test_writable_ancestor_refused_before_loading(self):
        self.provider();self.directory.chmod(0o777)
        self.refused(self.run_probe(),'writable by group or others')

    def test_provider_symlink_refused_before_loading(self):
        self.package.symlink_to(self.base/'valid.dylib')
        self.refused(self.run_probe(),'symlink')

    def test_write_acl_refused_before_loading(self):
        self.provider()
        subprocess.run(['chmod','+a','group:everyone allow write',str(self.package)],check=True)
        self.refused(self.run_probe(),'ACL grants write access')

    def test_unreadable_package_is_not_absent(self):
        self.provider();self.package.chmod(0)
        try:self.refused(self.run_probe(),'Permission denied')
        finally:self.package.chmod(0o755)

    def test_bad_abi_is_not_an_unadapted_fallback(self):
        self.provider('invalid');result=self.run_probe()
        self.assertNotEqual(result.returncode,0,result.stderr)
        self.assertIn('unsupported provider ABI',result.stderr)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--build',type=Path,required=True)
    args,remaining=parser.parse_known_args()
    PackageTests.build=args.build.resolve()
    unittest.main(argv=[__file__,*remaining])
