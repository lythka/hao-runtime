"""Build (without launching) a pair of ordinary Intel target activation controls."""
import argparse
from pathlib import Path
import plistlib
import subprocess

here = Path(__file__).resolve().parent
repo = here.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("install_root", type=Path, help="canonical absolute test directory on the target")
parser.add_argument("--build", type=Path, default=repo / "build")
args = parser.parse_args()
if not args.install_root.is_absolute() or ".." in args.install_root.parts:
    parser.error("install_root must be an absolute canonical path")
output = args.output.resolve()
output.mkdir(parents=True)
arches = ["-arch", "arm64", "-arch", "x86_64"]


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def info(bundle, name, ident, kind, **extra):
    file = bundle / "Contents/Info.plist"
    file.write_bytes(plistlib.dumps(dict(CFBundleIdentifier=ident, CFBundleExecutable=name,
        CFBundleName=name, CFBundleVersion="1", CFBundlePackageType=kind, **extra)))


def sign(path):
    run("/usr/bin/codesign", "-fs", "-", path)


for app_name in ["SelectedLaunch", "ActivationClient"]:
    app = output / (app_name + ".app")
    contents = app / "Contents"
    libs = contents / "Frameworks"
    bins = contents / "MacOS"
    libs.mkdir(parents=True)
    bins.mkdir()
    leaf = libs / "Leaf.dylib"
    branch = libs / "Branch.dylib"
    for file, name, deps in [(leaf, "@loader_path/Leaf.dylib", []),
                             (branch, "@rpath/Branch.dylib", [leaf])]:
        role = "-DLEAF" if file == leaf else "-DBRANCH"
        run("clang", *arches, role, "-dynamiclib", here / "selection.c", *deps,
            "-install_name", name, "-o", file)
        sign(file)
    info(app, app_name, "org.hao.fixture.armselection." + app_name.lower(), "APPL", LSBackgroundOnly=True)
    if app_name == "SelectedLaunch":
        selected = bins / app_name
        run("clang", *arches, "-DLS_MAIN", here / "activation.c", branch,
            "-Wl,-rpath,@executable_path/../Frameworks", "-o", selected)
        sign(app)
    else:
        ident = "org.hao.fixture.armselection.user"
        service = contents / "XPCServices" / (ident + ".xpc")
        selected = service / "Contents/MacOS/UserService"
        selected.parent.mkdir(parents=True)
        info(service, selected.name, ident, "XPC!", XPCService={"ServiceType": "User"})
        run("clang", *arches, "-DSERVICE", "-fblocks", here / "activation.c", branch,
            "-Wl,-rpath,@executable_path/../../../../Frameworks", "-o", selected)
        sign(service)
        run("clang", "-arch", "x86_64", "-fblocks", here / "activation.c", "-o", bins / app_name)
    run(repo / "tools/hao-embed", "--stub", args.build / "hao-stub", "--arm-app-member",
        args.install_root / selected.relative_to(output), "--in-place", selected)
    if app_name == "ActivationClient":
        sign(app)
    run("codesign", "--verify", "--all-architectures", "--strict", "--deep", app)
