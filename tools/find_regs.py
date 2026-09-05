#!/usr/bin/env python3
"""Static probe for F.E.A.R. 3's script-function registrations and RTTI vtables.

Pure stdlib. Works on the on-disk exe (the image is not packed, so the on-disk
bytes are what the mod sees in memory, minus relocation - which the exe does
not have at its preferred base 0x400000).

    python3 tools/find_regs.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --list            # every registration
    python3 tools/find_regs.py --exe ... --name AwardChallengeRequirement --name IsPauseMenuShowing
    python3 tools/find_regs.py --exe ... --vtable FearScoreMgr --slots 17,21,37,39,40
    python3 tools/find_regs.py --exe ... --xref "Invalid challenge index:%d"      # code refs to a string

Two registration shapes exist (see src/game.cpp for the byte patterns):
  A: B8 <impl> 89 47 20 33 C0 C7 47 10 <name> ...   (Despair::Reflection method proxies;
     the owner is the static object loaded with `mov ebx, imm32` in the registering
     function's prologue 83 EC ?? 53 55 56 33 F6 57 BB <static>)
  B: B8 <fn> 50 68 <name> E8 .. 83 C4 0C 50 B9 <static> E8   (GameScriptGame)
"""
import argparse, re, struct, subprocess, sys

OBJDUMP = "i686-w64-mingw32-objdump"


class Image:
    def __init__(self, path):
        self.path = path
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<I", d, opt + 28)[0]
        opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + opt_sz + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
            self.secs.append((name, va, vsz, rptr, rsz))

    def off2va(self, off):
        for name, va, vsz, rptr, rsz in self.secs:
            if rptr <= off < rptr + rsz:
                return self.base + va + (off - rptr), name
        return None, None

    def va2off(self, va):
        rva = va - self.base
        for name, v, vsz, rptr, rsz in self.secs:
            if v <= rva < v + max(vsz, rsz):
                return rptr + (rva - v), name
        return None, None

    def rd32(self, va):
        off, _ = self.va2off(va)
        return struct.unpack_from("<I", self.d, off)[0] if off is not None else None

    def cstr(self, va, n=300):
        off, _ = self.va2off(va)
        if off is None:
            return None
        e = self.d.find(b"\0", off, off + n)
        return self.d[off:e].decode(errors="replace") if e >= 0 else None

    def find_all(self, needle):
        out, i = [], 0
        while True:
            i = self.d.find(needle, i)
            if i < 0:
                return out
            out.append(i)
            i += 1

    def text(self):
        for s in self.secs:
            if s[0] == ".text":
                return s[3], s[3] + s[4]
        raise SystemExit("no .text section")

    def string_va(self, s):
        for h in self.find_all(s.encode() + b"\0"):
            if h == 0 or self.d[h - 1] == 0:
                return self.off2va(h)[0]
        return None

    def rtti_name(self, vtable_va):
        col = self.rd32(vtable_va - 4)
        td = self.rd32(col + 0xC) if col else None
        return self.cstr(td + 8, 400) if td else "?"

    def disasm(self, start, stop):
        r = subprocess.run([OBJDUMP, "-d", "-M", "intel", f"--start-address=0x{start:x}",
                            f"--stop-address=0x{stop:x}", self.path], capture_output=True, text=True)
        return [l for l in r.stdout.splitlines() if l.startswith(" ") and "\t" in l]


def registrations(img):
    """All Pattern A and Pattern B registrations: (kind, name, fn, owner, site)."""
    t0, t1 = img.text()
    rows = []
    pa = re.compile(rb"\xb8(....)\x89\x47\x20\x33\xc0\xc7\x47\x10(....)\x89\x5f\x14\x89\x77\x18\xc7\x07(....)", re.S)
    prologue = re.compile(rb"\x83\xec.\x53\x55\x56\x33\xf6\x57\xbb(....)", re.S)
    for m in pa.finditer(img.d, t0, t1):
        impl, name_va, vt = (struct.unpack("<I", m.group(i))[0] for i in (1, 2, 3))
        name = img.cstr(name_va)
        if not name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.:]*", name):
            continue
        owner = None
        pm = None
        for cand in prologue.finditer(img.d, max(t0, m.start() - 0x4000), m.start()):
            pm = cand  # the last prologue before the site
        if pm:
            owner = struct.unpack("<I", pm.group(1))[0]
        rows.append(("A", name, impl, owner, img.off2va(m.start())[0], img.rtti_name(vt)))
    pb = re.compile(rb"\xb8(....)\x50\x68(....)\xe8....\x83\xc4\x0c\x50\xb9(....)\xe8", re.S)
    for m in pb.finditer(img.d, t0, t1):
        fn, name_va, owner = (struct.unpack("<I", m.group(i))[0] for i in (1, 2, 3))
        name = img.cstr(name_va)
        if not name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.:]*", name):
            continue
        rows.append(("B", name, fn, owner, img.off2va(m.start())[0], ""))
    return rows


def vtables_for(img, cls):
    mangled = f".?AV{cls}@Despair@@"
    out = []
    for h in img.find_all(mangled.encode() + b"\0"):
        if img.d[h - 1] != 0:
            continue
        td_va = img.off2va(h - 8)[0]
        for r in img.find_all(struct.pack("<I", td_va)):
            col_off = r - 0xC
            if col_off < 0:
                continue
            sig, off, cd = struct.unpack_from("<III", img.d, col_off)
            if sig != 0 or off > 0x10000:
                continue
            col_va = img.off2va(col_off)[0]
            for v in img.find_all(struct.pack("<I", col_va)):
                out.append((img.off2va(v)[0] + 4, off))
    return sorted(set(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--list", action="store_true", help="print every registration")
    ap.add_argument("--name", action="append", default=[], help="resolve one registered name")
    ap.add_argument("--vtable", help="class name (Despair namespace) to dump vtables for")
    ap.add_argument("--slots", help="comma list of slots to print for --vtable (default all)")
    ap.add_argument("--xref", action="append", default=[], help="show code references to a string")
    a = ap.parse_args()
    img = Image(a.exe)
    print(f"image base 0x{img.base:x}, {len(img.secs)} sections")

    if a.list or a.name:
        rows = registrations(img)
        for kind, name, fn, owner, site, rtti in rows:
            if a.list or name in a.name:
                own = f"exe+0x{owner - img.base:06x}" if owner else "?"
                print(f"{kind} {name:44s} fn=exe+0x{fn - img.base:06x} owner={own} site=exe+0x{site - img.base:06x} {rtti[:90]}")
    if a.vtable:
        want = set(int(x) for x in a.slots.split(",")) if a.slots else None
        t0, t1 = img.text()
        for vt, off in vtables_for(img, a.vtable):
            print(f"vtable for {a.vtable} (subobject offset 0x{off:x}) @ exe+0x{vt - img.base:x}")
            for slot in range(128):
                fn = img.rd32(vt + slot * 4)
                if fn is None:
                    break
                o, sec = img.va2off(fn)
                if sec != ".text":
                    break
                if want is None or slot in want:
                    print(f"  slot {slot:3d} -> exe+0x{fn - img.base:06x}")
    for s in a.xref:
        va = img.string_va(s)
        if va is None:
            print(f"'{s}': not found")
            continue
        refs = [r for r in img.find_all(struct.pack("<I", va)) if img.off2va(r)[1] == ".text"]
        print(f"'{s}' @ exe+0x{va - img.base:x}: {len(refs)} code ref(s)")
        for r in refs[:8]:
            rva = img.off2va(r)[0]
            print(f"  ref @ exe+0x{rva - img.base:x}")
            for l in img.disasm(rva - 0x20, rva + 0x14)[-8:]:
                print("     " + l.split("\t", 2)[-1].strip())


if __name__ == "__main__":
    main()
