#!/usr/bin/env python3
"""Validate and materialize versioned hao assemblies; never execute package scripts."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hao_fat
import hao_ifunit

ROLES = ('runtime', 'compatibility', 'image', 'aot', 'record')
RUNTIME_FILES = {'root/usr/libexec/hao', 'root/usr/lib/hao/libhao.dylib',
                 'root/usr/lib/hao/libhao_identity.dylib'}
ASYNC_LIBRARY = 'root/usr/lib/hao/libhao_async_contract.dylib'
ASYNC_DESCRIPTOR = 'root/usr/lib/hao/async-contract.json'
HELPERS = {'hao-install.py', 'hao_fat.py', 'hao_codesig.py', 'stamp_identity.py'}
HEX = re.compile(r'[0-9a-f]{64}')
UUID = re.compile(r'[0-9a-f]{32}')
AOT = struct.Struct('<8sII16s16sQQIIQQQII32s')   # include/hao/aot.h hao_aot_hdr, format v6 (136 bytes)
AOT_ID_CDHASH = 1
FEATURES = {'sse2', 'cx16', 'avx', 'sse3', 'ssse3', 'sse4.1', 'sse4.2', 'avx2'}


class Invalid(ValueError): pass


def require(condition, why):
    if not condition: raise Invalid(why)


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode()


def sha(path):
    with open(path, 'rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, 'duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    require(path.stat().st_size <= 32 * 1024 * 1024, 'manifest too large')
    return json.loads(path.read_bytes(), object_pairs_hook=pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(Invalid('nonfinite JSON')))


def relative(name):
    require(isinstance(name, str) and bool(name), 'empty/non-string path')
    p = PurePosixPath(name)
    require(not p.is_absolute() and '..' not in p.parts and str(p) == name
            and '\\' not in name and '\0' not in name and name != '.', 'unsafe path: ' + name)
    return p


def safe(root, name, missing=False):
    relative(name)
    root = Path(root).resolve()
    p = root
    for part in PurePosixPath(name).parts:
        p = p / part
        require(not p.is_symlink(), 'symlink in path: ' + name)
    require(p.resolve().is_relative_to(root), 'path escapes root: ' + name)
    if not missing: require(p.is_file(), 'missing regular file: ' + name)
    return p


def role_for(name):
    relative(name)
    if name in RUNTIME_FILES: return 'runtime'
    if name.startswith('root/usr/lib/hao/if/') or name in {'root/usr/lib/hao/trusted-cdhashes', ASYNC_LIBRARY, ASYNC_DESCRIPTOR}: return 'compatibility'
    if name.startswith('root/usr/lib/hao/aot/') and name.endswith('.aot'): return 'aot'
    if (name in HELPERS or name.startswith(('HaoExec.kext/', 'embed/stubs/', 'replace/'))
            or name == 'embed/manifest.json'): return 'image'
    if name in {'build.json', 'aot.json', 'trust.log', 'embed/embed.log', 'embed/identity.log'} or name.startswith(('embed/identity/', 'census/')): return 'record'
    raise Invalid('unrecognized payload file: ' + name)


def delivery_for(name):
    """Apple-derived code is always a local recipe output, never a shipped object."""
    if name.startswith(('root/usr/lib/hao/aot/', 'embed/stubs/')) or name.startswith('replace/') and name != 'replace/manifest.json':
        return 'generate-locally'
    return 'ship'


def component_target(role, target):
    fields = {'runtime': ('host_arch', 'cpu_profile'),
              'compatibility': ('os_build', 'guest_arch', 'host_arch', 'native_cache_uuid'),
              'aot': ('guest_arch', 'host_arch', 'cpu_profile')}.get(role, tuple(target))
    return {k: target[k] for k in fields}


def macho(data, cpu=hao_fat.CPU_X86_64):
    slices = hao_fat.slices(data) or []
    choices = [s for s in slices if s[0] == cpu]
    require(bool(choices), 'missing required Mach-O architecture')
    _, sub, data = next((s for s in choices if s[1] & 0xffffff == 2), choices[0])
    require(len(data) >= 32, 'truncated Mach-O')
    count, size = struct.unpack_from('<II', data, 16)
    require(32 + size <= len(data), 'truncated load commands')
    uuid, off, signature = None, 32, None
    for _ in range(count):
        require(off + 8 <= 32 + size, 'truncated load command')
        cmd, length = struct.unpack_from('<II', data, off)
        require(length >= 8 and off + length <= 32 + size, 'bad load command length')
        if cmd == 0x1b:
            require(length >= 24 and uuid is None, 'invalid/duplicate LC_UUID')
            uuid = data[off + 8:off + 24].hex()
        if cmd == 0x1d:
            require(length >= 16 and signature is None, 'invalid/duplicate code signature command')
            start, size_cs = struct.unpack_from('<II', data, off + 8)
            require(start >= 32 + size and start + size_cs <= len(data), 'invalid code signature range')
            signature = (start, size_cs)
        off += length
    require(uuid is not None, 'missing LC_UUID')
    unsigned = data if signature is None else data[:signature[0]] + data[sum(signature):]
    return dict(uuid=uuid, sha256=hashlib.sha256(data).hexdigest(), unsigned_sha256=hashlib.sha256(unsigned).hexdigest(),
                guest_id=hao_ifunit.hash64(data[:32 + size], len(data)))


def aot_identity(path):
    with open(path, 'rb') as f: header = f.read(AOT.size)
    require(len(header) == AOT.size, 'truncated AOT header')
    magic, version, hs, uuid, gen, vm, gid, nb, nl, cs, fs, ph, kind, hash_type, code_id = AOT.unpack(header)
    require(magic == b'HAOAOT2\0' and version == 6 and hs == AOT.size, 'unsupported AOT format')
    require(fs == path.stat().st_size and fs == hs + nb * 8 + nl * 8 + cs and cs >= 16, 'AOT size mismatch')
    # The code identity the runtime checks: for a signed guest (kind 1) the 20-byte cdhash of the slice.
    require(kind != AOT_ID_CDHASH or not any(code_id[20:]), 'AOT cdhash padding is not zero')
    return dict(uuid=uuid.hex(), runtime_uuid=gen.hex(), guest_id=gid, blocks=nb,
                id_kind=kind, id_hash=hash_type, code_id=(code_id[:20] if kind == AOT_ID_CDHASH else code_id).hex())


def async_identity(descriptor, library, target):
    """Validate own-code provider format/identity without executing package code.

    Origin and behavioral admission are release-review responsibilities. A
    content hash or a caller-supplied descriptor cannot establish either.
    """
    require(descriptor.get('schema') == 1 and descriptor.get('kind') == 'hao-async-contract', 'unsupported async descriptor')
    require(type(descriptor.get('abi')) is int and descriptor['abi'] == 1, 'unsupported async provider ABI')
    for key in ('identifier', 'version'):
        require(isinstance(descriptor.get(key), str) and 0 < len(descriptor[key]) <= 160
                and all(32 <= ord(c) < 127 for c in descriptor[key]), 'invalid async ' + key)
    require(all(descriptor.get(k) == target[k] for k in ('os_build', 'guest_arch', 'host_arch')), 'async provider target mismatch')
    require(descriptor.get('library_sha256') == sha(library), 'async provider content mismatch')
    data = library.read_bytes()
    slices = hao_fat.slices(data) or []
    choices = [s[2] for s in slices if s[0] == hao_fat.CPU_X86_64]
    require(len(choices) == 1 and len(choices[0]) >= 32 and struct.unpack_from('<I', choices[0], 12)[0] == 6,
            'async provider must be an x86_64 dylib')
    identity = macho(data)
    require(descriptor.get('library_uuid') == identity['uuid'], 'async provider UUID mismatch')
    return dict(identifier=descriptor['identifier'], version=descriptor['version'], abi=descriptor['abi'],
                library_uuid=identity['uuid'], library_sha256=descriptor['library_sha256'])


def stage_async_input(source, destination, os_build):
    """Stage an already exported own-code pair into a new payload's hao directory."""
    source, destination = Path(source), Path(destination)
    descriptor_path = safe(source, 'async-contract.json')
    library = safe(source, 'libhao_async_contract.dylib')
    descriptor = read_json(descriptor_path)
    identity = async_identity(descriptor, library, dict(os_build=os_build, guest_arch='arm64e', host_arch='x86_64'))
    names = [('libhao_async_contract.dylib', library, 0o755), ('async-contract.json', descriptor_path, 0o644)]
    require(not destination.is_symlink(), 'provider destination is a symlink')
    require(all(not (destination / name).exists() and not (destination / name).is_symlink() for name, _, _ in names),
            'provider destination already exists')
    destination.mkdir(parents=True, exist_ok=True)
    for name, original, mode in names:
        shutil.copyfile(original, destination / name); (destination / name).chmod(mode)
    require(sha(destination / 'libhao_async_contract.dylib') == identity['library_sha256'], 'provider changed during staging')
    return identity


def image_plan(embed, replacements):
    operations = []
    for e in embed['files']:
        operations.append(dict(op='embed-v1', path=e['path'], artifact='embed/' + e['stub'],
                               source_sha256=e['source_sha256'], result_sha256=e['result_sha256'],
                               artifact_sha256=e['stub_sha256'], drop_x86=e.get('mode') == 'translate',
                               bound=e.get('bound', [])))
    for e in replacements.get('files', []):
        operations.append(dict(op='replace-v1', path=e['path'], artifact='replace/' + e['file'],
                               source_sha256=e['source_sha256'], result_sha256=e['result_sha256'],
                               artifact_sha256=e['result_sha256']))
    return dict(schema=1, install_prefix='root/', required=embed.get('required', []),
                directories=[dict(path='usr/lib/hao/aot', uid=0, gid=0, mode=0o755),
                             dict(path='private/var/db/hao', uid=0, gid=0, mode=0o755, if_parent_exists=True)],
                operations=operations,
                external_steps=['stamp-final-kernel-identity-v2', 'image-sealing-and-boot-integration'],
                scope='hao payload only; wider OS/driver image composition is external')


def validate(bundle, target=None):
    bundle = Path(bundle).resolve()
    assembly = read_json(safe(bundle, 'assembly.json'))
    require(assembly.get('schema') == 1 and assembly.get('kind') == 'hao-assembly', 'unsupported assembly schema')
    require(set(assembly['components']) == set(ROLES), 'missing/unknown component')
    require(assembly.get('distribution') == 'hao-code-contracts-and-recipes-only', 'unsupported distribution policy')
    packages, files, shipped = {}, {}, set()
    for role in ROLES:
        ref = assembly['components'][role]
        require(ref['path'] == 'packages/' + role + '.json', 'unexpected component path')
        path = safe(bundle, ref['path'])
        require(HEX.fullmatch(ref['sha256']) and sha(path) == ref['sha256'], 'component hash mismatch: ' + role)
        p = read_json(path)
        require(p.get('schema') == 1 and p.get('kind') == role and isinstance(p.get('revision'), str)
                and p['revision'], 'invalid component version: ' + role)
        require(p['target'] == component_target(role, assembly['target']), 'component target mismatch: ' + role)
        for name, record in p['files'].items():
            require(role_for(name) == role and name not in files, 'wrong/duplicate file ownership: ' + name)
            require(HEX.fullmatch(record['sha256']) and type(record['size']) is int and record['size'] >= 0
                    and type(record['mode']) is int and record['mode'] in (0o644, 0o755), 'invalid file record: ' + name)
            require(record.get('delivery') == delivery_for(name), 'distribution policy violation: ' + name)
            obj = None
            if record['delivery'] == 'ship':
                shipped.add(record['sha256'])
                obj = safe(bundle, 'objects/' + record['sha256'])
                require(obj.stat().st_size == record['size'] and sha(obj) == record['sha256'], 'artifact hash mismatch: ' + name)
            files[name] = dict(record, object=obj)
        packages[role] = p
    require({p.name for p in (bundle / 'objects').iterdir()} == shipped, 'unreferenced or forbidden object in distribution')
    allowed = {'assembly.json', 'target.json'} | {'packages/' + role + '.json' for role in ROLES} | {'objects/' + digest for digest in shipped}
    for path in bundle.rglob('*'):
        require(not path.is_symlink(), 'symlink in distribution')
        if not path.is_dir(): require(str(path.relative_to(bundle)) in allowed, 'unreferenced file in distribution')
    for role, p in packages.items():
        expected = {'runtime'} if role == 'compatibility' else {'runtime', 'compatibility'} if role in ('image', 'aot') else set()
        require(set(p['requires']) == expected, 'wrong dependency set: ' + role)
        for dep, digest in p['requires'].items():
            require(digest == assembly['components'][dep]['sha256'], 'dependency mismatch: ' + role + '/' + dep)
    t = assembly['target']
    require(t['guest_arch'] == 'arm64e' and t['host_arch'] == 'x86_64', 'unsupported architecture pair')
    require(isinstance(t['os_build'], str) and bool(t['os_build']) and UUID.fullmatch(t['native_cache_uuid']), 'invalid OS/cache identity')
    require(t['kernel_input']['stage'] == 'source-input' and HEX.fullmatch(t['kernel_input']['sha256']), 'invalid kernel input identity')
    cpu = t['cpu_profile']
    require(cpu['id'] == 'hao-x86_64-avx-cx16-v1' and set(cpu['required_features']) == {'sse2', 'cx16', 'avx'}
            and cpu['tuning'] == 'generic', 'unsupported CPU profile')
    if target is not None:
        require(all(target[k] == t[k] for k in ('os_build', 'guest_arch', 'host_arch', 'native_cache_uuid', 'kernel_input')), 'deployment target mismatch')
        require(set(cpu['required_features']) <= set(target['cpu_features']), 'target CPU features missing')
    require(set(packages['runtime']['files']) == RUNTIME_FILES, 'runtime file set mismatch')
    runtime_uuid = macho(files['root/usr/lib/hao/libhao.dylib']['object'].read_bytes())['uuid']
    require(packages['runtime']['runtime_uuid'] == runtime_uuid, 'runtime UUID mismatch')
    require(packages['compatibility']['interface_version'] == 2 and packages['aot']['format_version'] == 6, 'unsupported execution format')
    units = records = 0
    for name in packages['compatibility']['files']:
        leaf = PurePosixPath(name).name
        if name.startswith('root/usr/lib/hao/if/'):
            require(name == 'root/usr/lib/hao/if/' + leaf, 'nested contract path')
            if UUID.fullmatch(leaf):
                _, rows = hao_ifunit.read(files[name]['object'].read_bytes(), leaf)
                units += 1; records += len(rows)
            else: require(leaf.endswith('.thunks'), 'invalid contract filename')
    require(packages['compatibility']['counts'] == dict(units=units, records=records), 'contract accounting mismatch')
    async_provider = None
    present = {ASYNC_LIBRARY, ASYNC_DESCRIPTOR} & files.keys()
    if present:
        require(present == {ASYNC_LIBRARY, ASYNC_DESCRIPTOR}, 'async provider needs library and descriptor')
        require(packages['runtime'].get('async_contract_package_abis') == [1], 'runtime does not declare packaged async ABI support')
        async_provider = async_identity(read_json(files[ASYNC_DESCRIPTOR]['object']), files[ASYNC_LIBRARY]['object'], t)
    require(packages['compatibility'].get('async_provider') == async_provider, 'async provider component identity mismatch')
    require(packages['aot']['runtime_uuid'] == runtime_uuid, 'AOT runtime dependency mismatch')
    bindings = packages['aot']['guests']
    require(set(bindings) == set(packages['aot']['files']), 'AOT guest inventory mismatch')
    for name, binding in bindings.items():
        identity = binding['header']
        require(PurePosixPath(name).name == identity['uuid'] + '.aot', 'AOT filename/UUID mismatch')
        require(identity['runtime_uuid'] == runtime_uuid and identity['uuid'] == binding['uuid']
                and identity['guest_id'] == binding['guest_id'], 'AOT guest/runtime identity mismatch')
        # usr/lib/hao/aot is a system cache directory: the runtime serves only caches bound to a signed guest's cdhash from it
        require(identity.get('id_kind') == AOT_ID_CDHASH and isinstance(identity.get('code_id'), str)
                and re.fullmatch(r'[0-9a-f]{40}', identity['code_id']) and identity.get('id_hash') in (1, 2, 3, 4),
                'AOT cache is not bound to a signed guest (cdhash)')
        require(isinstance(binding['paths'], dict) and bool(binding['paths']) and HEX.fullmatch(binding['unsigned_sha256']), 'invalid guest binding')
        for name, digest in binding['paths'].items():
            relative(name); require(HEX.fullmatch(digest), 'invalid guest content hash')
    def artifact_json(name): return read_json(files[name]['object'])
    embed = artifact_json('embed/manifest.json')
    replace = artifact_json('replace/manifest.json') if 'replace/manifest.json' in files else {'files': []}
    plan = packages['image']['plan']
    require(plan == image_plan(embed, replace), 'image plan differs from validated legacy operations')
    paths = set()
    for op in plan['operations']:
        relative(op['path']); relative(op['artifact'])
        require(op['path'] not in paths, 'duplicate image destination')
        paths.add(op['path'])
        require(op['artifact'] in files and files[op['artifact']]['sha256'] == op['artifact_sha256'], 'image artifact mismatch')
        require(HEX.fullmatch(op['source_sha256']) and HEX.fullmatch(op['result_sha256']), 'invalid image identity')
        for bound in op.get('bound', []):
            if 'file' in bound: relative(bound['file']); require(HEX.fullmatch(bound['sha256']), 'invalid bound identity')
    for path in plan['required']: require(path in paths, 'required image operation missing')
    require(not any('root/' + p in files for p in paths), 'install and modification paths overlap')
    # A file cannot be the parent directory of another artifact or destination.
    for names in (set(files), paths | {n[5:] for n in files if n.startswith('root/')}):
        folded = [unicodedata.normalize('NFD', name).casefold() for name in names]
        require(len(folded) == len(set(folded)), 'case/Unicode path collision')
        for name in names:
            require(not any(str(p) in names for p in PurePosixPath(name).parents if str(p) != '.'), 'file/directory path collision')
    summary = dict(schema=1, assembly_sha256=sha(bundle / 'assembly.json'), files=len(files),
                   contracts=units, interface_records=records, aot_caches=len(bindings),
                   image_operations=len(plan['operations']), runtime_uuid=runtime_uuid,
                   signature_variant_groups=sum(len(set(g['paths'].values())) > 1 for g in bindings.values()),
                   shipped_objects=len(shipped), local_generation_files=sum(f['delivery'] == 'generate-locally' for f in files.values()),
                   validation='content, format identities and dependency bindings; not behavioral acceptance',
                   pending_builder_steps=plan['external_steps'])
    summary['async_provider'] = async_provider
    return assembly, packages, files, summary


def resolve_generated(files, generated):
    for name, record in files.items():
        if record['delivery'] == 'ship': continue
        require(generated is not None, 'local generation required; supply --generated')
        obj = safe(generated, name)
        require(obj.stat().st_size == record['size'] and sha(obj) == record['sha256'], 'local generated artifact mismatch: ' + name)
        record['object'] = obj


def check_generated_aot(packages, files):
    for name, binding in packages['aot']['guests'].items():
        require(aot_identity(files[name]['object']) == binding['header'], 'generated AOT header mismatch')


def preflight(bundle, system, target, native_cache, kernel_input, generated=None):
    assembly, packages, files, summary = validate(bundle, target)
    resolve_generated(files, generated); check_generated_aot(packages, files)
    root = Path(system).resolve()
    ver = plistlib.loads(safe(root, 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    require(ver['ProductBuildVersion'] == assembly['target']['os_build'], 'base OS build mismatch')
    with open(native_cache, 'rb') as f: cache_header = f.read(0x68)
    require(cache_header[:4] == b'dyld' and cache_header[0x58:0x68].hex() == assembly['target']['native_cache_uuid'], 'native cache identity mismatch')
    require(sha(kernel_input) == assembly['target']['kernel_input']['sha256'], 'kernel input hash mismatch')
    plan = packages['image']['plan']; changes = already = 0
    for op in plan['operations']:
        path = safe(root, op['path'])
        data = path.read_bytes(); digest = hashlib.sha256(data).hexdigest()
        require(digest in (op['source_sha256'], op['result_sha256']), 'image source mismatch: ' + op['path'])
        for bound in op.get('bound', []):
            if 'file' in bound:
                require(sha(safe(root, bound['file'])) == bound['sha256'], 'bundle resource mismatch')
        if digest == op['result_sha256']: already += 1; continue
        if op['op'] == 'embed-v1':
            result = hao_fat.combine(data, files[op['artifact']]['object'].read_bytes(), drop_x86_64=op['drop_x86'])
            require(hashlib.sha256(result).hexdigest() == op['result_sha256'], 'embedded result mismatch')
        changes += 1
    for name in files:
        if name.startswith('root/'):
            dest = safe(root, name[5:], missing=True)
            require(not dest.exists() or dest.is_file(), 'install destination is not a file')
    for d in plan['directories']:
        dest = safe(root, d['path'], missing=True)
        require(not dest.exists() or dest.is_dir(), 'directory destination is not a directory')
    for name, binding in packages['aot']['guests'].items():
        for path, digest in binding['paths'].items():
            guest = macho(safe(root, path).read_bytes(), hao_fat.CPU_ARM64)
            require(guest['uuid'] == binding['uuid'] and guest['guest_id'] == binding['guest_id']
                    and guest['sha256'] == digest and guest['unsigned_sha256'] == binding['unsigned_sha256'], 'AOT guest bytes mismatch: ' + path)
    return dict(summary, image_changes=changes, already_applied=already,
                ready_to_stage=True, deployment_accepted=False)


def materialize(bundle, out, generated=None):
    _, packages, files, summary = validate(bundle)
    resolve_generated(files, generated); check_generated_aot(packages, files)
    out = Path(out).absolute()
    require(not out.resolve().is_relative_to(Path(bundle).resolve()), 'local output cannot be inside distributable bundle')
    require(not out.exists() and not out.is_symlink(), 'output must not exist')
    out.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.' + out.name + '-', dir=out.parent))
    try:
        for name, r in files.items():
            dest = stage / name; dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(r['object'], dest); dest.chmod(r['mode'])
            require(sha(dest) == r['sha256'], 'artifact changed during materialization')
        (stage / '.hao-local-only.json').write_bytes(canonical(dict(distribution='local build output; do not redistribute',
                                                                 assembly_sha256=summary['assembly_sha256'])))
        os.rename(stage, out)
    finally:
        if stage.exists(): shutil.rmtree(stage)
    return summary


def generate(bundle, system, native_cache, build, out, only=()):
    """Run fixed hao-owned generators against builder-supplied Apple inputs."""
    assembly, packages, files, summary = validate(bundle)
    system, build, out = map(lambda p: Path(p).resolve(), (system, build, out))
    require(not out.is_relative_to(Path(bundle).resolve()), 'local output cannot be inside distributable bundle')
    require(not out.exists(), 'local generation output must not exist')
    expected = {n for n, f in files.items() if f['delivery'] == 'generate-locally'}
    selected = set(only) if only else expected
    require(selected <= expected, 'selection contains no generation recipe')
    # Reproduction uses the frozen runtime and its own stub object, not a rebuilt ABI.
    for source, installed in [('hao', 'root/usr/libexec/hao'), ('libhao.dylib', 'root/usr/lib/hao/libhao.dylib'),
                              ('libhao_identity.dylib', 'root/usr/lib/hao/libhao_identity.dylib')]:
        require(sha(safe(build, source)) == files[installed]['sha256'], 'generator runtime differs: ' + source)
    embed = read_json(files['embed/manifest.json']['object'])
    for name, digest in embed['inputs_sha256'].items():
        require(sha(safe(build, name)) == digest, 'stub generation input differs: ' + name)
    require(sha(safe(build, 'hao-stub')) == embed['stub_sha256'], 'stub template differs')
    ver = plistlib.loads(safe(system, 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    require(ver['ProductBuildVersion'] == assembly['target']['os_build'], 'generation OS build mismatch')
    with open(native_cache, 'rb') as f: header = f.read(0x68)
    require(header[:4] == b'dyld' and header[0x58:0x68].hex() == assembly['target']['native_cache_uuid'], 'generation native cache mismatch')
    out.mkdir(parents=True)
    (out / '.hao-local-only.json').write_bytes(canonical(dict(distribution='locally generated Apple-derived outputs; do not redistribute')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('HAO_')}
    toolroot = Path(__file__).resolve().parent
    def run(args, label, extra=None):
        p = subprocess.run(['timeout', '-s', 'KILL', '300', *map(str, args)], env=dict(env, **(extra or {})), capture_output=True)
        (out / (label + '.log')).write_bytes(p.stdout + p.stderr)
        require(p.returncode == 0, 'generation failed; see ' + label + '.log')
    stubs = [op for op in packages['image']['plan']['operations'] if op['op'] == 'embed-v1' and op['artifact'] in selected]
    if stubs:
        for op in stubs: require(sha(safe(system, op['path'])) == op['source_sha256'], 'stub source changed')
        require(not any(op['drop_x86'] for op in stubs), 'translate-mode recipe not supported by generation v1')
        run([sys.executable, toolroot / 'hao-embed', '--stub', build / 'hao-stub', '--root', system,
             '--x86-cache', Path(native_cache).parent, '--stubs', out / 'embed',
             *[system / op['path'] for op in stubs]], 'generate-stubs')
    for op in packages['image']['plan']['operations']:
        if op['op'] != 'replace-v1' or op['artifact'] not in selected: continue
        require(op['path'] == 'usr/lib/dyld' and sha(safe(system, op['path'])) == op['source_sha256'], 'unsupported or changed replacement source')
        dest = out / op['artifact']; dest.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, toolroot / 'dyld-ignition-fix.py', system / op['path'], dest], 'generate-dyld')
    for name, binding in packages['aot']['guests'].items():
        if name not in selected: continue
        path, digest = next(iter(binding['paths'].items()))
        require(macho(safe(system, path).read_bytes(), hao_fat.CPU_ARM64)['sha256'] == digest, 'AOT generation source changed')
        dest = out / name; dest.parent.mkdir(parents=True, exist_ok=True)
        run([build / 'hao', '--aot', system / path], 'aot-' + binding['uuid'], {'HAO_CACHE': str(dest.parent)})
    for name in selected:
        require(sha(safe(out, name)) == files[name]['sha256'], 'generated bytes differ: ' + name)
    receipt = dict(schema=1, assembly_sha256=summary['assembly_sha256'], generated=sorted(selected),
                   tools={n: sha(toolroot / n) for n in ('hao_release.py', 'hao-embed', 'dyld-ignition-fix.py')},
                   distribution='local only; never add these artifacts to a distributable bundle')
    (out / 'generation.json').write_bytes(canonical(receipt))
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    for name in ('check', 'materialize', 'preflight', 'generate'):
        s = sub.add_parser(name); s.add_argument('bundle', type=Path)
        if name == 'materialize': s.add_argument('--out', type=Path, required=True)
        if name in ('materialize', 'preflight'): s.add_argument('--generated', type=Path, required=True)
        if name == 'generate':
            for arg in ('system', 'native-cache', 'build', 'out'): s.add_argument('--' + arg, type=Path, required=True)
            s.add_argument('--only', action='append', default=[])
        if name in ('check', 'preflight'): s.add_argument('--target', type=Path, required=name == 'preflight')
        if name == 'preflight':
            s.add_argument('--system', type=Path, required=True)
            s.add_argument('--native-cache', type=Path, required=True)
            s.add_argument('--kernel-input', type=Path, required=True)
    a = p.parse_args()
    try:
        if a.command == 'materialize': result = materialize(a.bundle, a.out, a.generated)
        elif a.command == 'preflight': result = preflight(a.bundle, a.system, read_json(a.target), a.native_cache, a.kernel_input, a.generated)
        elif a.command == 'generate': result = generate(a.bundle, a.system, a.native_cache, a.build, a.out, a.only)
        else: result = validate(a.bundle, read_json(a.target) if a.target else None)[3]
        print(json.dumps(result, indent=2))
    except (Invalid, ValueError, KeyError, TypeError, OSError, struct.error, hao_ifunit.Bad) as e:
        p.exit(1, 'hao-release: ' + str(e) + '\n')


if __name__ == '__main__': main()
