"""Build a fresh native and guest XPC discovery fixture pair in OUTPUT."""
import argparse
import plistlib
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--service-location", choices=("framework", "app"), default="framework")
parser.add_argument("--adapter-case", choices=("nested", "advertised"),
                    help="load the fixture's original ARM framework from a distinct host adapter")
parser.add_argument("--declared-runtime", type=Path,
                    help="build an Intel fixture launcher declaring the adapter and installed hao runtime")
parser.add_argument("--callback-capability", action="store_true")
args = parser.parse_args()
if args.declared_runtime and not args.adapter_case:
    parser.error("--declared-runtime requires --adapter-case")
args.output.mkdir(parents=True, exist_ok=False)


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def info(path, identifier, executable, kind, **extra):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(dict(CFBundleIdentifier=identifier,
        CFBundleExecutable=executable, CFBundleName=executable,
        CFBundlePackageType=kind, CFBundleVersion="1", **extra)))


def sign(bundle):
    run("codesign", "--force", "--sign", "-", bundle)
    run("codesign", "--verify", "--strict", bundle)


for variant, arch in (("native", "x86_64"), ("translated", "arm64")):
    prefix = "org.hao.fixture.frameworkxpc.bulk." + variant
    if args.service_location == "app":
        prefix += ".appplacement"
    if args.callback_capability:
        prefix += ".capability"
    if args.adapter_case:
        prefix += ".adapter." + args.adapter_case
    if args.declared_runtime:
        prefix += ".declared"
    app = args.output / variant / "Probe.app"
    contents = app / "Contents"
    frameworks = contents / "Frameworks"
    framework = frameworks / "ProbeKit.framework"
    version = framework / "Versions/A"
    version.mkdir(parents=True)
    service_owner = version if args.service_location == "framework" else contents
    service = service_owner / "XPCServices" / (prefix + ".service.xpc")
    service_name = prefix + ".service"
    service_bin = service / "Contents/MacOS/ProbeService"
    service_bin.parent.mkdir(parents=True)
    cc = ["xcrun", "--sdk", "macosx", "clang", "-Wall", "-Wextra", "-Werror",
          "-fblocks", "-mmacosx-version-min=13.0"]
    define = '-DSERVICE_NAME="' + service_name + '"'
    run(*cc, "-arch", "x86_64", define, source / "service.c", "-o", service_bin)
    info(service / "Contents/Info.plist", service_name, "ProbeService", "XPC!",
         XPCService={"ServiceType": "User"},
         HaoAdvertisedAdapterIdentifiers=["org.hao.adapter.haoowneddiscovery"])
    sign(service)
    run(*cc, "-arch", arch, "-dynamiclib", define, source / "framework.c",
        "-install_name", "@rpath/ProbeKit.framework/Versions/A/ProbeKit", "-o", version / "ProbeKit")
    info(version / "Resources/Info.plist", prefix + ".kit", "ProbeKit", "FMWK")
    (framework / "Versions/Current").symlink_to("A")
    links = ["ProbeKit", "Resources"]
    if args.service_location == "framework":
        links.append("XPCServices")
    for name in links:
        (framework / name).symlink_to("Versions/Current/" + name)
    sign(framework)
    rpath = "@executable_path/../Frameworks"
    observer_flags = []
    if args.adapter_case and arch == "arm64":
        adapter = frameworks / "HaoOwnedDiscovery.framework"
        association = ["--associate-original", (framework / "ProbeKit").resolve()] if args.declared_runtime else []
        advertised = ["--advertise-service", service.name] if args.adapter_case == "advertised" else []
        run(source.parents[2] / "tools/hao-prepare-framework", "--separate-adapter",
            *association, *advertised, framework, adapter)
        if args.declared_runtime:
            observer_flags.append("-DDECLARED_FIXTURE_ADAPTER")
        else:
            rpath += "/HaoOwnedDiscovery.framework/Versions/A/Frameworks"
            observer_flags.append("-DOPEN_FIXTURE_ADAPTER")
    observer = frameworks / "ProbeObserver.dylib"
    observer_sources = [source / "observer.c"]
    if args.callback_capability:
        observer_sources.append(source / "capability.c")
    run(*cc, "-arch", "x86_64", "-dynamiclib", *observer_flags, *observer_sources,
        "-install_name", "@rpath/ProbeObserver.dylib", "-o", observer)
    sign(observer)
    client = contents / "MacOS/Probe"
    client.parent.mkdir()
    run(*cc, "-arch", arch, source / "client.c", "-F", frameworks, "-framework", "ProbeKit",
        f"-Wl,-rpath,{rpath}", "-o", client)
    main = "Probe"
    if args.declared_runtime and arch == "arm64":
        main = "DeclaredProbe"
        record = args.output / "launcher-record"
        run(source.parents[2] / "tools/hao-companion-record", "-o", record, adapter)
        run(*cc, "-arch", "x86_64", source / "declared_launcher.c",
            "-Xlinker", "-sectcreate", "-Xlinker", "__TEXT", "-Xlinker", "__hao_companions",
            "-Xlinker", record, "-headerpad_max_install_names", "-L", args.declared_runtime,
            "-lhao", "-lhao_identity", "-Wl,-rpath,/usr/lib/hao", "-F", frameworks,
            "-Wl,-needed_framework,HaoOwnedDiscovery", "-Wl,-rpath,@executable_path/../Frameworks",
            "-o", contents / "MacOS" / main)
        adapter_binary = (adapter / "HaoOwnedDiscovery").resolve(strict=True)
        run("/usr/bin/install_name_tool", "-change",
            "@rpath/HaoOwnedDiscovery.framework/Versions/A/HaoOwnedDiscovery", adapter_binary,
            contents / "MacOS" / main)
    info(contents / "Info.plist", prefix + ".app", main, "APPL")
    sign(app)
    run("codesign", "--verify", "--deep", "--strict", app)
