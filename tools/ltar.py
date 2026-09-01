"""Reader for LithTech Jupiter EX 2 'LTAR' archives (F.E.A.R. 2 *.Arch01).

Layout (version 3), all little-endian:

    0x00  magic 'LTAR'
    0x04  u32  version (3)
    0x08  u32  string table size
    0x0C  u32  directory count
    0x10  u32  file count
    0x14  u32 x3  flags
    0x20  16 bytes  GUID
    0x30  string table  (NUL-terminated, 4-byte aligned; offset 0 is the "" root)
          file entries, 32 bytes: u32 nameOff, u64 dataOff, u64 diskSize,
                                  u64 rawSize, u32 flags (0 = stored, 9 = deflate)
          dir entries,  16 bytes: u32 nameOff, i32 firstChild, i32 nextSibling,
                                  i32 fileCount

Directory names are full relative paths, and files are handed to directories
sequentially, so file ownership is just a running cursor over the file table.

Compressed payloads are a chunk stream: u32 diskSize, u32 rawSize, zlib data,
repeated. Chunks are 64 KiB uncompressed and each header is 4-byte aligned, so a
chunk whose data does not end on a boundary is followed by pad bytes.
"""

import struct
import zlib

MAGIC = b"LTAR"


class Archive:
    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        head = self.fh.read(0x30)
        if head[:4] != MAGIC:
            raise ValueError(f"{path}: not an LTAR archive")
        (self.version, strsize, ndirs, nfiles) = struct.unpack_from("<IIII", head, 4)
        if self.version != 3:
            raise ValueError(f"{path}: unsupported LTAR version {self.version}")

        strtab = self.fh.read(strsize)

        def name(off):
            end = strtab.index(b"\0", off)
            return strtab[off:end].decode("latin-1")

        files = []
        for _ in range(nfiles):
            n, off, disk, raw, flags = struct.unpack("<IQQQI", self.fh.read(32))
            files.append((name(n), off, disk, raw, flags))

        # Walk directories in table order, handing each its slice of the file
        # list. Directory names are already full relative paths.
        self.entries = {}
        cursor = 0
        for _ in range(ndirs):
            n, _first, _next, count = struct.unpack("<IiiI", self.fh.read(16))
            d = name(n)
            for fname, off, disk, raw, flags in files[cursor : cursor + count]:
                key = (d + "\\" + fname) if d else fname
                self.entries[key.lower()] = (off, disk, raw, flags)
            cursor += count

    def read(self, path):
        """Return the decompressed bytes of one archive member."""
        rec = self.entries.get(path.lower().replace("/", "\\"))
        if rec is None:
            raise KeyError(path)
        off, disk, raw, flags = rec
        self.fh.seek(off)
        if flags == 0:
            return self.fh.read(raw)
        if flags != 9:
            raise ValueError(f"{path}: unknown compression flag {flags}")

        out = bytearray()
        pos = off
        while len(out) < raw:
            self.fh.seek(pos)
            hdr = self.fh.read(8)
            if len(hdr) < 8:
                break
            cdisk, craw = struct.unpack("<II", hdr)
            if cdisk == 0:
                break
            out += zlib.decompress(self.fh.read(cdisk))
            # The next chunk header starts on a 4-byte boundary, so a chunk
            # whose data does not end aligned is followed by pad bytes.
            pos = (pos + 8 + cdisk + 3) & ~3
        return bytes(out[:raw])

    def close(self):
        self.fh.close()
