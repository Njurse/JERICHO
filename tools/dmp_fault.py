"""Print the faulting exception, address and owning module of a Windows minidump.

Usage: python3 tools/dmp_fault.py <file.dmp> [more.dmp ...]

Deliberately minimal (no symbols): enough to say "AV at 0x... inside X.exe at
rva 0x..." and to list the modules, so a crash can be attributed without a
debugger installed.
"""
import os
import struct
import sys


def parse(path):
    data = open(path, "rb").read()
    if data[:4] != b"MDMP":
        return None
    _, _, nstreams, dirrva = struct.unpack_from("<IIII", data, 0)

    streams = {}
    for i in range(nstreams):
        stype, ssize, srva = struct.unpack_from("<III", data, dirrva + 12 * i)
        streams[stype] = (ssize, srva)

    mods = []
    if 4 in streams:  # ModuleListStream
        _, rva = streams[4]
        count, = struct.unpack_from("<I", data, rva)
        for i in range(count):
            off = rva + 4 + 108 * i
            base, imgsize = struct.unpack_from("<QI", data, off)
            namerva, = struct.unpack_from("<I", data, off + 20)
            if namerva:
                nlen, = struct.unpack_from("<I", data, namerva)
                name = data[namerva + 4:namerva + 4 + nlen].decode("utf-16-le", "replace")
                name = os.path.basename(name)
            else:
                name = "?"
            mods.append((base, imgsize, name))

    out = {"file": os.path.basename(path), "modules": mods}
    if 6 in streams:  # ExceptionStream
        _, rva = streams[6]
        # MINIDUMP_EXCEPTION_STREAM: ThreadId(4) alignment(4) then
        # MINIDUMP_EXCEPTION: Code(0) Flags(4) ExceptionRecord(8) Address(16)
        tid, _ = struct.unpack_from("<II", data, rva)
        code, flags, _excptr, addr = struct.unpack_from("<IIQQ", data, rva + 8)
        out.update(thread=tid, code=code, flags=flags, address=addr)
        for base, sz, name in mods:
            if base <= addr < base + sz:
                out["module"] = name
                out["rva"] = addr - base
                break
    return out


NAMES = {
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC0000096: "PRIV_INSTRUCTION",
    0xC00000FD: "STACK_OVERFLOW",
    0x80000003: "BREAKPOINT",
    0xE06D7363: "C++ exception",
}


def main(paths):
    for p in paths:
        info = parse(p)
        if info is None:
            print("%s: not a minidump" % p)
            continue
        print("== %s" % info["file"])
        if "code" in info:
            code = info["code"]
            print("   exception %s (0x%08X) thread %d" % (NAMES.get(code, "unknown"), code, info["thread"]))
            print("   address   0x%X" % info["address"])
            if "module" in info:
                print("   in module %s at rva 0x%X" % (info["module"], info["rva"]))
            else:
                print("   NOT inside any listed module")
        print("   modules:" if info["modules"] else "   (no module list)")
        for base, sz, name in sorted(info["modules"]):
            print("      0x%012X %9d  %s" % (base, sz, name))


if __name__ == "__main__":
    main(sys.argv[1:])
