#!/usr/bin/env python3
"""List the menu movies in the game's .bndl bundles, and disassemble their ActionScript.

The menus are Scaleform movies (AS2) packed into bundles inside the .Arch01
archives. A bundle (BNDL v3), all little-endian:

    0x00  magic 'BNDL'
    0x04  u32  version (3)
    0x08  u32  name table size
    0x0C  u32 x2  (0)
    0x14  u32  member count
    0x18  names, NUL-terminated, each padded to 4 bytes
          then each member in name order: u32 (unknown), u32 size, data

A .gfx member is an uncompressed SWF ("GFX", version 8). Its ActionScript is in
DoAction (12) and DoInitAction (59) tags, the classes in DoInitAction tags right
after the ExportAssets naming them. The listing resolves constant-pool strings
and nests function bodies, which is enough to follow what calls into the game:
those calls are flash.external.ExternalInterface.call("Menu.SetReturnMenu", ...)
and the like, in the Monolith.* wrapper classes.

An archive's member is taken from the archive listed last in default.archcfg
that has it, as the game does.

Usage:  python3 tools/menu_as.py --game-dir <FEAR2 dir> --list
        python3 tools/menu_as.py --game-dir <FEAR2 dir> <bundle> <movie>  > listing.txt
  e.g.  python3 tools/menu_as.py --game-dir "$GAME_DIR" \\
            'interface\\_postrelease\\pc\\mainmenu_v102.bndl' 'interface\\menu\\main.gfx'
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ltar  # noqa: E402


def archives(game_dir):
    """The archives in default.archcfg order (a later one wins)."""
    with open(os.path.join(game_dir, "default.archcfg"), "rb") as fh:
        lines = fh.read().decode("latin-1").splitlines()
    return [l.strip().strip("\0") for l in lines if l.strip().strip("\0").lower().endswith(".arch01")]


def read_member(game_dir, path):
    found = None
    for name in archives(game_dir):
        a = ltar.Archive(os.path.join(game_dir, name))
        if path.lower() in a.entries:
            found = name
        a.close()
    if not found:
        raise KeyError(path)
    a = ltar.Archive(os.path.join(game_dir, found))
    try:
        return a.read(path), found
    finally:
        a.close()


def bundle_members(b):
    """[(name, data)] of a BNDL v3 bundle."""
    magic, version, names_size, _a, _b, count = struct.unpack_from("<4sIIIII", b, 0)
    if magic != b"BNDL" or version != 3:
        raise ValueError(f"not a BNDL v3 bundle ({magic!r}, {version})")
    names, o, end = [], 0x18, 0x18 + names_size
    while o < end and len(names) < count:
        e = b.index(b"\0", o)
        names.append(b[o:e].decode("latin-1"))
        o = (e + 1 + 3) & ~3
    members, o = [], end
    for n in names:
        _flag, size = struct.unpack_from("<II", b, o)
        members.append((n, b[o + 8:o + 8 + size]))
        o += 8 + size
    return members


# --- AS2 -----------------------------------------------------------------------
ACTIONS = {
    0x04: "NextFrame", 0x05: "PrevFrame", 0x06: "Play", 0x07: "Stop", 0x0A: "Add", 0x0B: "Subtract",
    0x0C: "Multiply", 0x0D: "Divide", 0x0E: "Equals", 0x0F: "Less", 0x10: "And", 0x11: "Or", 0x12: "Not",
    0x13: "StringEquals", 0x14: "StringLength", 0x15: "StringExtract", 0x17: "Pop", 0x18: "ToInteger",
    0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2", 0x21: "StringAdd", 0x22: "GetProperty",
    0x23: "SetProperty", 0x24: "CloneSprite", 0x25: "RemoveSprite", 0x26: "Trace", 0x27: "StartDrag",
    0x28: "EndDrag", 0x29: "StringLess", 0x2A: "Throw", 0x2B: "CastOp", 0x2C: "ImplementsOp",
    0x30: "RandomNumber", 0x31: "MBStringLength", 0x32: "CharToAscii", 0x33: "AsciiToChar", 0x34: "GetTime",
    0x35: "MBStringExtract", 0x36: "MBCharToAscii", 0x37: "MBAsciiToChar", 0x3A: "Delete", 0x3B: "Delete2",
    0x3C: "DefineLocal", 0x3D: "CallFunction", 0x3E: "Return", 0x3F: "Modulo", 0x40: "NewObject",
    0x41: "DefineLocal2", 0x42: "InitArray", 0x43: "InitObject", 0x44: "TypeOf", 0x45: "TargetPath",
    0x46: "Enumerate", 0x47: "Add2", 0x48: "Less2", 0x49: "Equals2", 0x4A: "ToNumber", 0x4B: "ToString",
    0x4C: "PushDuplicate", 0x4D: "StackSwap", 0x4E: "GetMember", 0x4F: "SetMember", 0x50: "Increment",
    0x51: "Decrement", 0x52: "CallMethod", 0x53: "NewMethod", 0x54: "InstanceOf", 0x55: "Enumerate2",
    0x60: "BitAnd", 0x61: "BitOr", 0x62: "BitXor", 0x63: "BitLShift", 0x64: "BitRShift", 0x65: "BitURShift",
    0x66: "StrictEquals", 0x67: "Greater", 0x68: "StringGreater", 0x69: "Extends", 0x81: "GotoFrame",
    0x83: "GetURL", 0x87: "StoreRegister", 0x88: "ConstantPool", 0x8A: "WaitForFrame", 0x8B: "SetTarget",
    0x8C: "GoToLabel", 0x8D: "WaitForFrame2", 0x8E: "DefineFunction2", 0x8F: "Try", 0x94: "With",
    0x96: "Push", 0x99: "Jump", 0x9A: "GetURL2", 0x9B: "DefineFunction", 0x9D: "If", 0x9E: "Call",
    0x9F: "GotoFrame2",
}


def cstr(b, o):
    e = b.index(b"\0", o)
    return b[o:e].decode("latin-1"), e + 1


def push_values(data, pool):
    vals, p = [], 0
    while p < len(data):
        t = data[p]
        p += 1
        if t == 0:
            s, p = cstr(data, p)
            vals.append(repr(s))
        elif t == 1:
            vals.append(repr(struct.unpack_from("<f", data, p)[0]))
            p += 4
        elif t in (2, 3):
            vals.append("null" if t == 2 else "undefined")
        elif t == 4:
            vals.append(f"r{data[p]}")
            p += 1
        elif t == 5:
            vals.append("true" if data[p] else "false")
            p += 1
        elif t == 6:  # the two halves of the double are swapped
            hi, lo = struct.unpack_from("<II", data, p)
            vals.append(repr(struct.unpack("<d", struct.pack("<II", lo, hi))[0]))
            p += 8
        elif t == 7:
            vals.append(str(struct.unpack_from("<i", data, p)[0]))
            p += 4
        elif t in (8, 9):
            i = data[p] if t == 8 else struct.unpack_from("<H", data, p)[0]
            p += 1 if t == 8 else 2
            vals.append(repr(pool[i]) if i < len(pool) else f"c{i}")
        else:
            vals.append(f"?type{t}")
            break
    return vals


def disasm(code, indent, out, pool):
    """One action block; `pool` is a one-element list holding the constant pool in force."""
    o = 0
    while o < len(code):
        at, op = o, code[o]
        o += 1
        if op == 0:
            break
        data = b""
        if op >= 0x80:
            ln = struct.unpack_from("<H", code, o)[0]
            data = code[o + 2:o + 2 + ln]
            o += 2 + ln
        name = ACTIONS.get(op, f"op{op:02X}")
        if op == 0x88:
            n, p, strs = struct.unpack_from("<H", data, 0)[0], 2, []
            for _ in range(n):
                s, p = cstr(data, p)
                strs.append(s)
            pool[0] = strs
            text = f"ConstantPool [{n} strings]"
        elif op == 0x96:
            text = "Push " + ", ".join(push_values(data, pool[0]))
        elif op in (0x99, 0x9D):
            text = f"{name} -> {o + struct.unpack_from('<h', data, 0)[0]:05X}"
        elif op == 0x87:
            text = f"StoreRegister r{data[0]}"
        elif op in (0x9B, 0x8E, 0x94):
            if op == 0x94:
                size, head = struct.unpack_from("<H", data, 0)[0], "with"
            else:
                fname, p = cstr(data, 0)
                n = struct.unpack_from("<H", data, p)[0]
                p += 2
                if op == 0x8E:
                    p += 3  # register count, flags
                params = []
                for _ in range(n):
                    reg = 0
                    if op == 0x8E:
                        reg, p = data[p], p + 1
                    s, p = cstr(data, p)
                    params.append(s + (f"@r{reg}" if reg else ""))
                size = struct.unpack_from("<H", data, p)[0]
                head = f"function{'2' if op == 0x8E else ''} {fname or '<anon>'}({', '.join(params)})"
            out.append(f"{indent}{at:05X}: {head} {{")
            disasm(code[o:o + size], indent + "    ", out, pool)
            out.append(f"{indent}}}")
            o += size
            continue
        else:
            text = name
        out.append(f"{indent}{at:05X}: {text}")


def tags(b, o, end, out, depth, where):
    while o < end:
        h = struct.unpack_from("<H", b, o)[0]
        o += 2
        code, ln = h >> 6, h & 0x3F
        if ln == 0x3F:
            ln = struct.unpack_from("<I", b, o)[0]
            o += 4
        body = b[o:o + ln]
        pad = "  " * depth
        if code == 0:
            break
        if code == 12:
            out.append(f"{pad}== DoAction ({where})")
            disasm(body, pad + "  ", out, [[]])
        elif code == 59:
            out.append(f"{pad}== DoInitAction sprite {struct.unpack_from('<H', body, 0)[0]} ({where})")
            disasm(body[2:], pad + "  ", out, [[]])
        elif code == 39:
            tags(body, 4, len(body), out, depth + 1, f"sprite {struct.unpack_from('<H', body, 0)[0]}")
        elif code == 56:
            n, p, names = struct.unpack_from("<H", body, 0)[0], 2, []
            for _ in range(n):
                cid = struct.unpack_from("<H", body, p)[0]
                s, p = cstr(body, p + 2)
                names.append(f"{cid}={s}")
            out.append(f"{pad}== ExportAssets {', '.join(names)}")
        o += ln


def listing(movie):
    if movie[:3] not in (b"GFX", b"FWS"):
        raise ValueError(f"not an uncompressed SWF/GFX movie ({movie[:3]!r})")
    nbits = movie[8] >> 3
    out = []
    tags(movie, 8 + (5 + 4 * nbits + 7) // 8 + 4, len(movie), out, 0, "root")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-dir", required=True)
    ap.add_argument("--list", action="store_true", help="list the bundles and the .gfx movies in them")
    ap.add_argument("bundle", nargs="?")
    ap.add_argument("movie", nargs="?")
    args = ap.parse_args()

    if args.list:
        seen = {}
        for name in archives(args.game_dir):
            a = ltar.Archive(os.path.join(args.game_dir, name))
            for k in a.entries:
                if k.endswith(".bndl"):
                    seen[k] = name
            a.close()
        for k, arch in sorted(seen.items()):
            try:
                data, _ = read_member(args.game_dir, k)
                movies = [n for n, _ in bundle_members(data) if n.lower().endswith(".gfx")]
            except Exception as e:  # a member ltar.py cannot unpack, or not a BNDL v3
                print(f"{k}  ({arch}): not read - {e}")
                continue
            if movies:
                print(f"{k}  ({arch})")
                for n in movies:
                    print(f"    {n}")
        return

    if not (args.bundle and args.movie):
        ap.error("give a bundle and a movie, or --list")
    data, arch = read_member(args.game_dir, args.bundle)
    for n, movie in bundle_members(data):
        if n.lower() == args.movie.lower():
            print(f"// {args.movie} from {args.bundle} ({arch})")
            print("\n".join(listing(movie)))
            return
    sys.exit(f"{args.movie} is not in {args.bundle}")


if __name__ == "__main__":
    main()
