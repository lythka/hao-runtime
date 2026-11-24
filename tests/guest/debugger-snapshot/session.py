"""Read registers from one authored, cooperating child over inherited pipes."""
import ctypes
import argparse
import sys
from pathlib import Path
import socket
import struct
import subprocess
import threading

HEADER = struct.Struct("<IIIIQQQ")
ARM64 = 0x0100000C
NAMES = [f"x{i}" for i in range(29)] + ["fp", "lr", "sp", "pc", "cpsr"]
TRIPLE = b"arm64-apple-macosx".hex()


def exact(stream, size):
    block = stream.read(size)
    if len(block) != size:
        raise EOFError("target ended during a stop")
    return block


def marker_offset(image):
    data = Path(image).read_bytes()
    magic, arch, _, _, commands, _, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xfeedfacf:
        raise ValueError("expected a thin Mach-O executable")
    if arch != ARM64:
        return None, None
    uuid = None
    pos = 32
    text = symbols = None
    for _ in range(commands):
        kind, size = struct.unpack_from("<II", data, pos)
        if kind == 0x19 and data[pos+8:pos+24].rstrip(b"\0") == b"__TEXT":
            text = struct.unpack_from("<QQ", data, pos+24)
        if kind == 0x1b:
            uuid = data[pos+8:pos+24]
        if kind == 2:
            symbols = struct.unpack_from("<4I", data, pos+8)
        pos += size
    if text and symbols:
        offset, count, strings, length = symbols
        for i in range(count):
            name, _, _, _, value = struct.unpack_from("<IBBHQ", data, offset+i*16)
            if name < length and data[strings+name:strings+name+14] == b"_guest_marker\0":
                if text[0] <= value < text[0] + text[1]:
                    return value - text[0], uuid
    raise ValueError("missing guest marker inside __TEXT")


class Stop:
    def __init__(self, target, tid, generation, registers, pc):
        self.target = target
        self.tid = tid
        self.generation = generation
        self.registers = registers
        self.pc = pc
        self.active = True

    def available(self):
        return self.active and self.target.poll() is None


def region(target, address):
    # Darwin PROC_PIDREGIONINFO returns metadata for the first region at or
    # above the address. Accept only actual containment in this launched child.
    layout = struct.Struct("<4IQ14I2Q")
    buffer = ctypes.create_string_buffer(layout.size)
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
    count = libproc.proc_pidinfo(target.pid, 7, ctypes.c_uint64(address), buffer, layout.size)
    values = layout.unpack(buffer.raw)
    base, size = values[-2:]
    if count != layout.size or not base <= address < base + size:
        raise ValueError("address is outside a mapped target region")
    return base, values[0]


def read_stop(target, generation, offset, image_base, tid=None):
    version, cpu, pid, words, thread, sequence, base = HEADER.unpack(exact(target.stdout, HEADER.size))
    if version != 1:
        raise ValueError("unsupported snapshot version")
    if pid != target.pid or not thread or (tid is not None and thread != tid):
        raise ValueError("invalid target lifetime")
    if cpu != ARM64 or words != 68 or offset is None:
        raise ValueError("target has no ARM64 general-register snapshot")
    if sequence != generation:
        raise ValueError("stale stop generation")
    raw = exact(target.stdout, words * 4)
    pc = struct.unpack_from("<Q", raw, 32 * 8)[0]
    sp = struct.unpack_from("<Q", raw, 31 * 8)[0]
    if not base or base != image_base or base % 4096 or pc != base + offset:
        raise ValueError("unexpected guest stop location")
    region_base, protection = region(target, pc)
    if region_base != base or not (protection & 1) or protection & 2:
        raise ValueError("unexpected guest stop location")
    marker = 0x730 + generation
    if (struct.unpack_from("<3Q", raw, 9 * 8) != (marker, marker+1, marker+2) or
        struct.unpack_from("<I", raw, 264)[0] >> 28 != 6 or not sp or sp % 16 or
        region(target, sp)[1] & 3 != 3):
        raise ValueError("unexpected guest registers or stack")
    return Stop(target, thread, generation, raw[:268], pc)


def frame(body):
    data = body.encode("ascii")
    return b"$" + data + b"#" + f"{sum(data) & 255:02x}".encode()


def serve(connection, stop_record, trace=False, advance=None):
    target = stop_record.target
    no_ack = False
    previous = b""
    with connection, connection.makefile("rb") as stream:
        while True:
            first = stream.read(1)
            if not first:
                return
            if first == b"-" and previous:
                connection.sendall(previous)
            if first != b"$":
                continue
            body = bytearray()
            while True:
                byte = stream.read(1)
                if byte == b"#":
                    break
                if not byte or len(body) >= 8192:
                    raise ValueError("invalid debugger packet")
                body += byte
            if int(exact(stream, 2), 16) != sum(body) & 255:
                if not no_ack:
                    connection.sendall(b"-")
                continue
            if not no_ack:
                connection.sendall(b"+")
            command = body.decode("ascii")
            if trace:
                print("debugger packet", command[:150], file=sys.stderr, flush=True)
            if not stop_record.available():
                connection.sendall(frame("E01"))
                return
            tid, registers = stop_record.tid, stop_record.registers
            result = ""
            stop = f"T05thread:{tid:x};"
            if command.startswith("qSupported"):
                result = "PacketSize=2000;QStartNoAckMode+"
            elif command == "vCont?":
                result = "vCont;c" if advance else ""
            elif command in ("c", "vCont;c", f"vCont;c:{tid:x}") and advance:
                next_stop = advance(stop_record)
                if next_stop is None:
                    connection.sendall(frame("W00"))
                    return
                stop_record = next_stop
                result = f"T05thread:{next_stop.tid:x};"
            elif command == "QStartNoAckMode":
                no_ack = True
                result = "OK"
            elif command in ("qHostInfo", "qProcessInfo"):
                result = f"cputype:{ARM64:x};cpusubtype:0;ptrsize:8;endian:little;ostype:macosx;triple:{TRIPLE};"
                if command == "qProcessInfo":
                    result += f"pid:{target.pid:x};"
            elif command == "qC":
                result = f"QC{tid:x}"
            elif command == "qfThreadInfo":
                result = f"m{tid:x}"
            elif command == "qsThreadInfo":
                result = "l"
            elif command == "?" or command == f"qThreadStopInfo{tid:x}":
                result = stop
            elif command.startswith("qRegisterInfo"):
                index = int(command[len("qRegisterInfo"):], 16)
                result = "E45"
                if index < len(NAMES):
                    name = NAMES[index]
                    result = f"name:{name};bitsize:{32 if name == 'cpsr' else 64};offset:{index * 8};encoding:uint;format:hex;set:General Purpose Registers;"
                    generic = {"pc":"pc", "sp":"sp", "fp":"fp", "lr":"ra", "cpsr":"flags"}.get(name)
                    if generic:
                        result += f"generic:{generic};"
            elif command.startswith("Hg") or command.startswith("Hc"):
                result = "OK" if command[2:] in ("0", "-1", f"{tid:x}") else "E01"
            elif command == "g":
                result = registers.hex()
            elif command.startswith("p") and command[1:]:
                index = int(command[1:], 16)
                result = registers[index*8:(index+1)*8].hex() if index < len(NAMES) else "E45"
            elif command.startswith("D"):
                connection.sendall(frame("OK"))
                return
            previous = frame(result)
            connection.sendall(previous)


def selftest(stop):
    tid, registers = stop.tid, stop.registers
    server, client = socket.socketpair()
    thread = threading.Thread(target=serve, args=(server, stop,))
    thread.start()
    with client, client.makefile("rb") as stream:
        for packet, expected in [("?", f"T05thread:{tid:x};"), ("p9", registers[72:80].hex()),
                                 ("p20", registers[256:264].hex()), ("g", registers.hex()),
                                 ("pffff", "E45"), ("m0,4", ""), ("P9=00000000",""), ("D", "OK")]:
            client.sendall(frame(packet))
            assert exact(stream,1) == b"+"
            assert exact(stream,len(frame(expected))) == frame(expected)
    thread.join()


def check_expired(stop):
    server, client = socket.socketpair()
    with client:
        client.sendall(frame("p9"))
        serve(server, stop)
        with client.makefile("rb") as reply:
            assert exact(reply, 1) == b"+"
            assert exact(reply, len(frame("E01"))) == frame("E01")


def debug(lldb, stop):
    generation, pc = stop.generation, stop.pc
    server, client = socket.socketpair()
    worker = threading.Thread(target=serve, args=(server, stop, True), daemon=True)
    worker.start()
    with client:
        args = [lldb, "-b", "-o", f"process connect -p gdb-remote fd://{client.fileno()}",
                "-o", "register read pc x9 sp", "-o", "process detach"]
        proc = subprocess.run(args, pass_fds=(client.fileno(),), text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=75)
    worker.join(timeout=5)
    print(proc.stdout, end="")
    import re
    values = {key:int(value,16) for key, value in re.findall(r"\b(pc|x9|sp)\s*=\s*(0x[0-9a-fA-F]+)",proc.stdout)}
    if proc.returncode or values.get("pc") != pc or values.get("x9") != 0x730 + generation or values.get("sp") != struct.unpack_from("<Q", stop.registers, 31*8)[0]:
        raise ValueError("debugger did not read the target's stopped ARM registers")


def continuous(target, first, offset, base, lldb):
    observed = [first]
    def advance(stopped):
        if not stopped.available() or stopped is not observed[-1]:
            raise ValueError("resume on expired stop")
        stopped.active = False
        target.stdin.write(struct.pack("<Q", stopped.generation))
        target.stdin.flush()
        if stopped.generation == 2:
            status = target.wait(timeout=10)
            if status != 0:
                raise ValueError(f"target failed after final resume: {status}")
            return None
        next_stop = read_stop(target, stopped.generation+1, offset, base, stopped.tid)
        assert not stopped.available()
        observed.append(next_stop)
        return next_stop

    server, client = socket.socketpair()
    errors = []
    def run_server():
        try:
            serve(server, first, True, advance)
        except Exception as error:
            errors.append(error)

    worker = threading.Thread(target=run_server, daemon=True)
    worker.start()
    try:
        if lldb:
            # An expected SIGTRAP is a crash stop to LLDB's batch driver. Drive
            # the synchronous SBProcess API inside a single command so batch mode
            # does not discard the second register read after a successful stop.
            inspect = ("script d=lldb.debugger; d.SetAsync(False); "
                       "p=d.GetSelectedTarget().GetProcess(); "
                       "show=lambda: print('STOP x9=0x%x pc=0x%x sp=0x%x' % tuple("
                       "p.GetThreadAtIndex(0).GetFrameAtIndex(0).FindRegister(r).GetValueAsUnsigned() "
                       "for r in ('x9','pc','sp'))); "
                       "show(); r=p.Continue(); assert r.Success() and p.GetState()==lldb.eStateStopped; "
                       "show(); r=p.Continue(); assert r.Success() and p.GetState()==lldb.eStateExited "
                       "and p.GetExitStatus()==0; print('CONTINUES_OK')")
            args = [lldb, "-b", "-o", f"process connect -p gdb-remote fd://{client.fileno()}",
                    "-o", inspect]
            with client:
                proc = subprocess.run(args, pass_fds=(client.fileno(),), text=True, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, timeout=75)
            print(proc.stdout, end="")
            import re
            stops = re.findall(r"^STOP x9=(0x[0-9a-fA-F]+) pc=(0x[0-9a-fA-F]+) sp=(0x[0-9a-fA-F]+)$", proc.stdout, re.M)
            actual = [tuple(int(value,16) for value in stop) for stop in stops]
            expected = [(0x730+s.generation, s.pc, struct.unpack_from("<Q",s.registers,248)[0]) for s in observed]
            if proc.returncode or "CONTINUES_OK" not in proc.stdout.splitlines() or len(actual) != 2 or actual != expected:
                raise ValueError("debugger did not read both stop generations")
        else:
            with client, client.makefile("rb") as stream:
                for query, expected in [("p9",struct.pack("<Q",0x731).hex()),
                                        ("vCont;c",f"T05thread:{first.tid:x};"),
                                        ("p9",struct.pack("<Q",0x732).hex()),("c","W00")]:
                    client.sendall(frame(query))
                    assert exact(stream,1) == b"+"
                    assert exact(stream,len(frame(expected))) == frame(expected)
    finally:
        client.close()
        worker.join(timeout=5)
        if worker.is_alive():
            raise ValueError("debugger service did not stop")
        if errors:
            raise errors[0]
    if target.poll() != 0 or len(observed) != 2 or any(s.available() for s in observed):
        raise ValueError("target lifetime did not close cleanly")
    print("one connection observed both stopped generations and clean target exit")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lldb")
    parser.add_argument("--continuous",action="store_true")
    parser.add_argument("--negative", action="store_true")
    parser.add_argument("--invalid-pc", action="store_true")
    parser.add_argument("--invalid-version", action="store_true")
    parser.add_argument("--truncated", action="store_true")
    parser.add_argument("--stale-ack", action="store_true")
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()
    image = Path(args.command[-1])
    offset, uuid = marker_offset(image)
    with subprocess.Popen(args.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as target:
        try:
            base, dev, inode, observed_uuid = struct.unpack("<3Q16s", exact(target.stdout, 40))
            if uuid is not None:
                st = image.stat()
                if observed_uuid != uuid or inode != st.st_ino or dev != st.st_dev or base == 0:
                    raise ValueError("wrong target image identity")
            previous = None
            for generation in (1, 2):
                try:
                    stop = read_stop(target, generation, offset, base, previous.tid if previous else None)
                except EOFError:
                    if args.truncated:
                        print("truncated record refused")
                        return
                    raise
                except ValueError as error:
                    if ((args.negative and str(error) == "target has no ARM64 general-register snapshot") or
                        (args.invalid_pc and str(error) == "unexpected guest stop location") or
                        (args.invalid_version and str(error) == "unsupported snapshot version")):
                        print(error)
                        return
                    raise
                assert not any((args.negative, args.invalid_pc, args.invalid_version, args.truncated))
                if args.continuous:
                    continuous(target, stop, offset, base, args.lldb)
                    return
                if previous is not None:
                    if target.poll() is not None or previous.available():
                        raise ValueError("old stop still readable after resume")
                    check_expired(previous)
                if args.stale_ack:
                    target.stdin.write(struct.pack("<Q", generation - 1))
                    target.stdin.flush()
                    assert target.wait(timeout=10) == 5
                    check_expired(stop)
                    print("stale acknowledgment refused; exited target has no readable registers")
                    return
                if args.lldb:
                    debug(args.lldb, stop)
                else:
                    selftest(stop)
                if target.poll() is not None:
                    raise ValueError("target exited before acknowledgment")
                x9, x10, x11 = struct.unpack_from("<3Q", stop.registers, 72)
                flags = struct.unpack_from("<I", stop.registers, 264)[0] >> 28
                print(f"ARM stop {generation}: image PC +{stop.pc-base:#x}, x9={x9:#x}, "
                      f"x10={x10:#x}, x11={x11:#x}, NZCV={flags:x}, target alive", flush=True)
                stop.active = False
                previous = stop
                target.stdin.write(struct.pack("<Q",generation))
                target.stdin.flush()
            if target.wait(timeout=10) != 0:
                raise ValueError("unexpected target status")
        finally:
            if target.poll() is None:
                target.terminate()

if __name__ == "__main__":
    main()
