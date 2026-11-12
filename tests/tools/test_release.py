"""Assembly validation, dependency rejection and read-only image preflight."""
import copy
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import hao_release as r


def executable(cpu, uuid, body=b''):
    return struct.pack('<IIIIIIII', 0xfeedfacf, cpu, 0, 2, 1, 24, 0, 0) + struct.pack('<II', 0x1b, 24) + bytes.fromhex(uuid) + body


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name); self.bundle = self.base / 'bundle'
        self.root = self.base / 'system'; self.root.mkdir()
        self.generated = self.base / 'generated'; self.generated.mkdir()
        (self.bundle / 'packages').mkdir(parents=True); (self.bundle / 'objects').mkdir()
        self.kernel = self.base / 'kernel'; self.kernel.write_bytes(b'kernel input')
        self.cache = self.base / 'cache'; self.cache.write_bytes(b'dyld' + bytes(0x58 - 4) + bytes.fromhex('3' * 32))
        self.target = dict(os_build='fixture-build', guest_arch='arm64e', host_arch='x86_64', native_cache_uuid='3' * 32,
                           kernel_input=dict(stage='source-input', sha256=r.sha(self.kernel)),
                           cpu_profile=dict(id='hao-x86_64-avx-cx16-v1', required_features=['sse2', 'cx16', 'avx'], tuning='generic'))
        self.deployment = {k: v for k, v in self.target.items() if k != 'cpu_profile'}
        self.deployment['cpu_features'] = ['sse2', 'cx16', 'avx']
        self.packages = {role: dict(schema=1, kind=role, revision='fixture', target=r.component_target(role, copy.deepcopy(self.target)), files={}, requires={}) for role in r.ROLES}
        self.runtime = executable(r.hao_fat.CPU_X86_64, '1' * 32)
        for name in r.RUNTIME_FILES: self.add(name, self.runtime)
        self.packages['runtime']['runtime_uuid'] = '1' * 32
        unit = r.hao_ifunit.build('4' * 32, {'_fixture': (1, 0, 0, 0, 0, '', 'fixture', '')})
        self.add('root/usr/lib/hao/if/' + '4' * 32, unit)
        self.packages['compatibility'].update(interface_version=2, counts=dict(units=1, records=1))
        self.guest = executable(r.hao_fat.CPU_ARM64, '2' * 32, b'guest')
        self.stub = executable(r.hao_fat.CPU_X86_64, '5' * 32, b'stub')
        self.result = r.hao_fat.combine(self.guest, self.stub)
        self.add('embed/stubs/fixture.stub', self.stub)
        self.embed = dict(required=['usr/bin/fixture'], files=[dict(path='usr/bin/fixture', stub='stubs/fixture.stub',
                         stub_sha256=hashlib.sha256(self.stub).hexdigest(), source_sha256=hashlib.sha256(self.guest).hexdigest(),
                         result_sha256=hashlib.sha256(self.result).hexdigest(), bound=[])])
        self.add('embed/manifest.json', r.canonical(self.embed))
        self.packages['image']['plan'] = r.image_plan(self.embed, {})
        identity = r.macho(self.guest, r.hao_fat.CPU_ARM64)
        self.aot_name = 'root/usr/lib/hao/aot/' + '2' * 32 + '.aot'
        code = bytes(16)
        self.add(self.aot_name, self.aot_file(identity, code))
        self.packages['aot'].update(format_version=7, runtime_uuid='1' * 32, guests={self.aot_name: dict(
            uuid=identity['uuid'], guest_id=identity['guest_id'], unsigned_sha256=identity['unsigned_sha256'],
            header=r.aot_identity(self.generated / self.aot_name),
            paths={'usr/bin/fixture': identity['sha256']})})
        (self.root / 'usr/bin').mkdir(parents=True); (self.root / 'usr/bin/fixture').write_bytes(self.guest)
        ver = self.root / 'System/Library/CoreServices/SystemVersion.plist'; ver.parent.mkdir(parents=True)
        ver.write_bytes(plistlib.dumps({'ProductBuildVersion': 'fixture-build'}))
        self.save()

    def aot_file(self, identity, code, version=7, kind=1, hash_type=2, code_id=bytes.fromhex('c0' * 20) + bytes(12)):
        """An AOT cache of the fixture guest: format v7, bound to a (made up) SHA-256 cdhash."""
        return r.AOT.pack(b'HAOAOT2\0', version, r.AOT.size, bytes.fromhex('2' * 32), bytes.fromhex('1' * 32), 4096,
                          identity['guest_id'], 0, 0, len(code), r.AOT.size + len(code), r.hao_ifunit.hash64(code),
                          kind, hash_type, code_id) + code

    def add(self, name, data):
        digest = hashlib.sha256(data).hexdigest()
        delivery = r.delivery_for(name)
        destination = self.bundle / 'objects' / digest if delivery == 'ship' else self.generated / name
        destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(data)
        self.packages[r.role_for(name)]['files'][name] = dict(sha256=digest, size=len(data), mode=0o644, delivery=delivery)
        referenced = {f['sha256'] for p in self.packages.values() for f in p['files'].values() if f['delivery'] == 'ship'}
        for obj in (self.bundle / 'objects').iterdir():
            if obj.name not in referenced: obj.unlink()

    def save(self, dependencies=True):
        refs = {}
        for role in r.ROLES:
            if dependencies:
                deps = ['runtime'] if role == 'compatibility' else ['runtime', 'compatibility'] if role in ('image', 'aot') else []
                self.packages[role]['requires'] = {d: refs[d]['sha256'] for d in deps}
            path = self.bundle / 'packages' / (role + '.json'); path.write_bytes(r.canonical(self.packages[role]))
            refs[role] = dict(path='packages/' + role + '.json', sha256=r.sha(path))
        (self.bundle / 'assembly.json').write_bytes(r.canonical(dict(schema=1, kind='hao-assembly', revision='fixture',
                                                         target=self.target, components=refs, distribution='hao-code-contracts-and-recipes-only')))

    def preflight(self): return r.preflight(self.bundle, self.root, self.deployment, self.cache, self.kernel, self.generated)

    def test_round_trip_and_read_only_preflight(self):
        summary = self.preflight()
        self.assertEqual((summary['contracts'], summary['aot_caches'], summary['image_changes']), (1, 1, 1))
        self.assertFalse(summary['deployment_accepted'])
        self.assertEqual((self.root / 'usr/bin/fixture').read_bytes(), self.guest)
        self.assertFalse((self.root / 'usr/lib').exists())
        out = self.base / 'payload'; r.materialize(self.bundle, out, self.generated)
        for p in self.packages.values():
            for name, record in p['files'].items(): self.assertEqual(r.sha(out / name), record['sha256'])

    def test_already_embedded_is_idempotent(self):
        (self.root / 'usr/bin/fixture').write_bytes(self.result)
        self.assertEqual(self.preflight()['already_applied'], 1)

    def provider(self):
        data=bytearray(executable(r.hao_fat.CPU_X86_64, '6'*32, b'own-code fixture'))
        struct.pack_into('<I',data,12,6)
        descriptor=dict(schema=1,kind='hao-async-contract',abi=1,identifier='fixture@1',version='1',
                        os_build=self.target['os_build'],guest_arch='arm64e',host_arch='x86_64',
                        library_sha256=hashlib.sha256(data).hexdigest(),library_uuid='6'*32)
        self.add(r.ASYNC_LIBRARY,data);self.add(r.ASYNC_DESCRIPTOR,r.canonical(descriptor))
        self.packages['runtime']['async_contract_package_abis']=[1]
        self.packages['compatibility']['async_provider']=r.async_identity(descriptor,self.bundle/'objects'/descriptor['library_sha256'],self.target)
        self.save();return descriptor

    def test_async_provider_is_versioned_compatibility_input(self):
        self.provider();summary=r.validate(self.bundle)[3]
        self.assertEqual(summary['async_provider']['identifier'],'fixture@1')
        self.assertEqual(summary['interface_records'],1)
        out=self.base/'with-provider';r.materialize(self.bundle,out,self.generated)
        self.assertEqual(r.sha(out/r.ASYNC_LIBRARY),summary['async_provider']['library_sha256'])

    def test_async_provider_requires_runtime_support(self):
        self.provider();del self.packages['runtime']['async_contract_package_abis'];self.save()
        with self.assertRaisesRegex(r.Invalid,'runtime does not declare'):r.validate(self.bundle)

    def test_async_provider_identity_and_target_refuse(self):
        descriptor=self.provider()
        for field,value in [('abi',2),('os_build','other-build'),('library_sha256','0'*64),('library_uuid','0'*32)]:
            altered=dict(descriptor,**{field:value})
            self.add(r.ASYNC_DESCRIPTOR,r.canonical(altered));self.save()
            with self.subTest(field=field),self.assertRaises(r.Invalid):r.validate(self.bundle)

    def test_async_provider_executable_is_not_a_dylib(self):
        descriptor=self.provider();data=executable(r.hao_fat.CPU_X86_64,'6'*32)
        self.add(r.ASYNC_LIBRARY,data);descriptor['library_sha256']=hashlib.sha256(data).hexdigest()
        self.add(r.ASYNC_DESCRIPTOR,r.canonical(descriptor));self.save()
        with self.assertRaisesRegex(r.Invalid,'must be an x86_64 dylib'):r.validate(self.bundle)

    def test_provider_staging_checks_target_before_writing(self):
        descriptor=self.provider();source=self.base/'export';source.mkdir()
        (source/'async-contract.json').write_bytes(r.canonical(descriptor))
        shutil.copyfile(self.bundle/'objects'/descriptor['library_sha256'],source/'libhao_async_contract.dylib')
        destination=self.base/'staged'
        with self.assertRaisesRegex(r.Invalid,'target mismatch'):r.stage_async_input(source,destination,'other-build')
        self.assertFalse(destination.exists())
        identity=r.stage_async_input(source,destination,self.target['os_build'])
        self.assertEqual(r.sha(destination/'libhao_async_contract.dylib'),identity['library_sha256'])
        self.assertEqual((destination/'libhao_async_contract.dylib').stat().st_mode & 0o777,0o755)
        with self.assertRaisesRegex(r.Invalid,'already exists'):r.stage_async_input(source,destination,self.target['os_build'])

    def test_corrupt_artifact_leaves_no_output(self):
        record = self.packages['aot']['files'][self.aot_name]
        (self.generated / self.aot_name).write_bytes(b'bad')
        out = self.base / 'payload'
        with self.assertRaisesRegex(r.Invalid, 'local generated artifact mismatch'): r.materialize(self.bundle, out, self.generated)
        self.assertFalse(out.exists())

    def test_wrong_runtime_dependency(self):
        self.packages['aot']['requires']['runtime'] = '0' * 64; self.save(False)
        with self.assertRaisesRegex(r.Invalid, 'dependency mismatch'): r.validate(self.bundle)

    def test_rehashed_aot_wrong_runtime_rejected(self):
        record = self.packages['aot']['files'][self.aot_name]
        data = bytearray((self.generated / self.aot_name).read_bytes()); data[32:48] = bytes.fromhex('8' * 32)
        self.add(self.aot_name, data)
        self.packages['aot']['guests'][self.aot_name]['header'] = r.aot_identity(self.generated / self.aot_name)
        self.save()
        with self.assertRaisesRegex(r.Invalid, 'guest/runtime identity mismatch'): r.validate(self.bundle)

    def test_aot_cpu_generator_profiles(self):
        for byte, flip in ((15, 0), (15, 1), (15, 2), (15, 128), (0, 1)):
            data = bytearray(self.aot_file(r.macho(self.guest, r.hao_fat.CPU_ARM64), bytes(16)))
            data[32 + byte] ^= flip
            self.add(self.aot_name, data)
            self.packages['aot']['guests'][self.aot_name]['header'] = r.aot_identity(self.generated / self.aot_name)
            self.save()
            if byte == 15 and flip in (0, 1):
                self.assertEqual(self.preflight()['aot_caches'], 1)
            else:
                with self.assertRaisesRegex(r.Invalid, 'guest/runtime identity mismatch'):
                    self.preflight()

    def test_aot_profile_does_not_change_runtime_dependency(self):
        aes_uuid = bytearray.fromhex(self.packages['runtime']['runtime_uuid']); aes_uuid[15] ^= 1
        self.packages['aot']['runtime_uuid'] = aes_uuid.hex()
        self.save()
        with self.assertRaisesRegex(r.Invalid, 'AOT runtime dependency mismatch'):
            self.preflight()

    def rebind_aot(self, **changes):
        self.add(self.aot_name, self.aot_file(r.macho(self.guest, r.hao_fat.CPU_ARM64), bytes(16), **changes))
        self.packages['aot']['guests'][self.aot_name]['header'] = r.aot_identity(self.generated / self.aot_name)
        self.save()

    def test_aot_header_binds_the_guest_code(self):
        header = self.packages['aot']['guests'][self.aot_name]['header']
        self.assertEqual((r.AOT.size, header['id_kind'], header['id_hash'], header['code_id']), (136, 1, 2, 'c0' * 20))
        # a cache translated from other code under the same UUID, load commands and runtime: not the recorded one
        self.add(self.aot_name, self.aot_file(r.macho(self.guest, r.hao_fat.CPU_ARM64), bytes(16), code_id=bytes.fromhex('c1' * 20) + bytes(12)))
        self.save()
        with self.assertRaisesRegex(r.Invalid, 'generated AOT header mismatch'): self.preflight()

    def test_aot_for_unsigned_code_rejected(self):
        self.rebind_aot(kind=2, hash_type=0, code_id=bytes.fromhex('ab' * 32))
        with self.assertRaisesRegex(r.Invalid, 'not bound to a signed guest'): r.validate(self.bundle)

    def test_old_aot_versions_rejected(self):
        for version in (5, 6):
            self.packages['aot']['format_version'] = 7
            self.add(self.aot_name, self.aot_file(r.macho(self.guest, r.hao_fat.CPU_ARM64), bytes(16), version=version)); self.save()
            with self.assertRaisesRegex(r.Invalid, 'unsupported AOT format'): self.preflight()
            self.packages['aot']['format_version'] = version; self.save()
            with self.assertRaisesRegex(r.Invalid, 'unsupported execution format'): r.validate(self.bundle)

    def test_rehashed_unit_wrong_library_rejected(self):
        name = 'root/usr/lib/hao/if/' + '4' * 32
        self.add(name, r.hao_ifunit.build('8' * 32, {})); self.save()
        with self.assertRaisesRegex(r.hao_ifunit.Bad, 'UUID differs'): r.validate(self.bundle)

    def test_wrong_os_cache_and_cpu_rejected(self):
        for key, value in [('os_build', 'next-build'), ('native_cache_uuid', '9' * 32), ('host_arch', 'arm64'), ('cpu_features', ['sse2'])]:
            target = dict(self.deployment, **{key: value})
            with self.subTest(key=key), self.assertRaises(r.Invalid): r.validate(self.bundle, target)

    def test_actual_kernel_cache_and_os_checked(self):
        self.kernel.write_bytes(b'other kernel')
        with self.assertRaisesRegex(r.Invalid, 'kernel input hash'): self.preflight()
        self.kernel.write_bytes(b'kernel input'); self.cache.write_bytes(b'dyld' + bytes(100))
        with self.assertRaisesRegex(r.Invalid, 'cache identity'): self.preflight()

    def test_changed_source_rejected_before_mutation(self):
        p = self.root / 'usr/bin/fixture'; p.write_bytes(self.guest + b'changed')
        with self.assertRaisesRegex(r.Invalid, 'image source mismatch'): self.preflight()
        self.assertEqual(p.read_bytes(), self.guest + b'changed')
        self.assertFalse((self.root / 'usr/lib').exists())

    def test_plan_cannot_inject_extra_operations(self):
        self.packages['image']['plan']['operations'].append(dict(op='shell', command='false')); self.save()
        with self.assertRaisesRegex(r.Invalid, 'image plan differs'): r.validate(self.bundle)

    def test_paths_and_symlinks_rejected(self):
        for name in ('../escape', '/absolute', 'a/../b', './x', 'a//b'):
            with self.subTest(name=name), self.assertRaises(r.Invalid): r.relative(name)
        record = next(iter(self.packages['compatibility']['files'].values())); obj = self.bundle / 'objects' / record['sha256']
        elsewhere = self.base / 'elsewhere'; obj.rename(elsewhere); obj.symlink_to(elsewhere)
        with self.assertRaisesRegex(r.Invalid, 'symlink'): r.validate(self.bundle)

    def test_image_symlink_rejected(self):
        p = self.root / 'usr/bin/fixture'; p.unlink(); p.symlink_to(self.kernel)
        with self.assertRaisesRegex(r.Invalid, 'symlink'): self.preflight()

    def test_unknown_schema_and_duplicate_json_rejected(self):
        p = self.bundle / 'assembly.json'; d = r.read_json(p); d['schema'] = 2; p.write_bytes(r.canonical(d))
        with self.assertRaisesRegex(r.Invalid, 'unsupported assembly'): r.validate(self.bundle)
        p.write_text('{"schema":1,"schema":2}')
        with self.assertRaisesRegex(r.Invalid, 'duplicate JSON'): r.validate(self.bundle)

    def test_signature_only_variants_have_distinct_exact_hashes(self):
        header = struct.pack('<IIIIIIII', 0xfeedfacf, r.hao_fat.CPU_ARM64, 0, 2, 2, 40, 0, 0)
        commands = struct.pack('<II', 0x1b, 24) + bytes.fromhex('2' * 32) + struct.pack('<IIII', 0x1d, 16, 76, 4)
        a = r.macho(header + commands + b'code' + b'sig1', r.hao_fat.CPU_ARM64)
        b = r.macho(header + commands + b'code' + b'sig2', r.hao_fat.CPU_ARM64)
        c = r.macho(header + commands + b'bad!' + b'sig2', r.hao_fat.CPU_ARM64)
        self.assertNotEqual(a['sha256'], b['sha256'])
        self.assertEqual(a['unsigned_sha256'], b['unsigned_sha256'])
        self.assertNotEqual(a['unsigned_sha256'], c['unsigned_sha256'])

    def test_apple_derived_code_cannot_be_marked_for_shipping(self):
        self.packages['aot']['files'][self.aot_name]['delivery'] = 'ship'; self.save()
        with self.assertRaisesRegex(r.Invalid, 'distribution policy violation'): r.validate(self.bundle)

    def test_extra_binary_cannot_hide_in_bundle(self):
        (self.bundle / 'objects' / ('0' * 64)).write_bytes(self.guest)
        with self.assertRaisesRegex(r.Invalid, 'forbidden object'): r.validate(self.bundle)
        (self.bundle / 'objects' / ('0' * 64)).unlink()
        (self.bundle / 'apple.bin').write_bytes(self.guest)
        with self.assertRaisesRegex(r.Invalid, 'unreferenced file'): r.validate(self.bundle)

    def test_missing_local_generation_is_explicit(self):
        with self.assertRaisesRegex(r.Invalid, 'local generation required'):
            r.materialize(self.bundle, self.base / 'unbuilt')
        self.assertFalse((self.base / 'unbuilt').exists())

    def test_kernel_change_does_not_reversion_userspace_contracts(self):
        changed = copy.deepcopy(self.target); changed['kernel_input']['sha256'] = 'a' * 64
        for role in ('runtime', 'compatibility', 'aot'):
            self.assertEqual(r.component_target(role, self.target), r.component_target(role, changed))
        self.assertNotEqual(r.component_target('image', self.target), r.component_target('image', changed))

    def test_local_payload_cannot_be_created_inside_distribution(self):
        with self.assertRaisesRegex(r.Invalid, 'inside distributable bundle'):
            r.materialize(self.bundle, self.bundle / 'local-payload', self.generated)
        self.assertFalse((self.bundle / 'local-payload').exists())

    def test_case_collisions_rejected(self):
        self.add('embed/stubs/Fixture.stub', self.stub); self.save()
        with self.assertRaisesRegex(r.Invalid, 'path collision'): r.validate(self.bundle)


if __name__ == '__main__': unittest.main()
