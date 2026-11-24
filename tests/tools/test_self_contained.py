#!/usr/bin/env python3
import pathlib,subprocess,shutil,json,struct
import argparse
parser = argparse.ArgumentParser(description="Verify prepared main execution retains sandbox read restrictions and ARM integrity")
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path("build"))
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[2]
build = args.build.resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = out / "read-probe"
results=[]
def run(args,expected=0):
 p=subprocess.run(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=90)
 results.append({'argv':list(map(str,args)),'exit':p.returncode,'stdout':p.stdout.decode(errors='replace'),'stderr':p.stderr.decode(errors='replace')})
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
 assert p.returncode==expected,results[-1]
 return p
(out/'read-probe.c').write_text('#include <errno.h>\n#include <fcntl.h>\n#include <stdio.h>\n#include <unistd.h>\nint main(int argc, char **argv) {\n    if (argc != 3) return 2;\n    for (int i = 1; i < 3; i++) {\n        int fd = open(argv[i], O_RDONLY);\n        if (fd >= 0) { close(fd); return 3; }\n        if (errno != EPERM && errno != EACCES) return 4;\n    }\n    puts("both file reads denied");\n    return 0;\n}\n')
run(['xcrun','clang','-arch','arm64','-o',source,out/'read-probe.c'])
run(['codesign','-f','-s','-',source])
for mode in ['old','mapped']:
 target=out/mode;shutil.copy2(source,target)
 args=['python3',root/'tools/hao-embed','--host-test','--stub',build/'hao-stub','--in-place']
 if mode=='mapped':args+=['--self-contained']
 run(args+[target])
 # Thin explicitly to force guest execution on the Apple-silicon host.
 run(['lipo',target,'-thin','x86_64','-output',out/(mode+'.thin')])
 run(['codesign','-f','-s','-',out/(mode+'.thin')])
 target=out/(mode+'.thin'); forbidden=out/'unrelated';forbidden.write_text('unrelated test data')
 profile='(version 1)(allow default)(deny file-read-data (literal '+json.dumps(str(target))+') (literal '+json.dumps(str(forbidden))+'))'
 p=run(['sandbox-exec','-p',profile,'/usr/bin/arch','-x86_64',target,target,forbidden],126 if mode=='old' else 0)
 if mode=='mapped':assert p.stdout.strip()==b'both file reads denied'
# Corruption of mapped ARM code must still fail even after re-signing outer executable.
data=bytearray((out/'mapped.thin').read_bytes()); ncmd=struct.unpack_from('<I',data,16)[0];pos=32;found=False
for _ in range(ncmd):
 cmd,size=struct.unpack_from('<II',data,pos)
 if cmd==0x19 and data[pos+8:pos+24].rstrip(b'\0')==b'__HAO_MAIN':
  sec=pos+72;offset=struct.unpack_from('<I',data,sec+48)[0];data[offset+4096]^=1;found=True;break
 pos+=size
assert found
bad=out/'damaged';bad.write_bytes(data);bad.chmod(0o755);run(['codesign','-f','-s','-',bad]);p=run(['/usr/bin/arch','-x86_64',bad],126);assert b'does not match its hash' in p.stderr
print('PASS: original denied; mapped executes with both reads denied; altered ARM page refused')
