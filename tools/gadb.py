"""Reader for the LithTech 'Loki' game database (F.E.A.R. 2 *.gamedb, 'GADB' v6).

    0x00  magic 'GADB'
    0x04  u32 version (6)
    0x08  u32 string table size
    0x0C  u32 category count
    0x10  u32 record count
    0x14  u32 (unused here)
    0x18  u32 attribute-descriptor count
    0x1C  string table (NUL-terminated)
          attribute descriptors, 8 bytes: u32 nameHash, u8 type, u8 count, u16 slot
          categories, each { u32 nameOff, u32 recordCount, u32 hash } followed
          inline by its records, each
              { u32 nameOff, u32 attrCount, u32 descCount, u32 descIdx, u32 hash,
                u32 values[attrCount] }

A value of attribute type 9 is a cross-reference to another record, encoded as
((categoryIndex + 1) << 16) | recordIndex, with 0xFFFFFFFF meaning null. Type 4
is a string-table offset; types 1 and 3 are bool and int32.
"""

import struct

MAGIC = b"GADB"
NULL_REF = 0xFFFFFFFF


class Record:
    __slots__ = ("name", "values", "desc_idx", "desc_count")

    def __init__(self, name, values, desc_idx, desc_count):
        self.name = name
        self.values = values
        self.desc_idx = desc_idx
        self.desc_count = desc_count


class Category:
    __slots__ = ("name", "records", "by_name")

    def __init__(self, name, records):
        self.name = name
        self.records = records
        self.by_name = {r.name: i for i, r in enumerate(records)}


class Database:
    def __init__(self, data):
        if data[:4] != MAGIC:
            raise ValueError("not a GADB file")
        (self.version, strsize, ncat, nrec, _u, ndesc) = struct.unpack_from("<IIIIII", data, 4)
        if self.version != 6:
            raise ValueError(f"unsupported GADB version {self.version}")

        self.data = data
        self.strtab = data[0x1C : 0x1C + strsize]

        # The declared descriptor count is a couple of entries short of the real
        # table, so probe forward and accept the first offset from which the
        # whole category table parses cleanly and accounts for every record the
        # header promises. Guessing from one header is not enough - an empty
        # first category and mid-string offsets both look "sane" in isolation.
        base = 0x1C + strsize
        self.categories = None
        for extra in range(0, 9):
            cats = self._try_parse(base + (ndesc + extra) * 8, ncat, nrec)
            if cats is not None:
                self.categories = cats
                break
        if self.categories is None:
            raise ValueError("could not locate the category table")

        self.by_name = {c.name: i for i, c in enumerate(self.categories)}

    def _try_parse(self, off, ncat, nrec):
        """Parse the category table at `off`; return None if it does not fit."""
        data = self.data
        cats = []
        total = 0
        try:
            for _ in range(ncat - 1):  # index 0 is an implicit null root
                nameOff, count, _h = struct.unpack_from("<III", data, off)
                off += 12
                if nameOff >= len(self.strtab) or count > 100000:
                    return None
                records = []
                for _r in range(count):
                    rname, nattr, ndc, didx, _rh = struct.unpack_from("<IIIII", data, off)
                    off += 20
                    if rname >= len(self.strtab) or nattr > 512:
                        return None
                    vals = struct.unpack_from(f"<{nattr}I", data, off) if nattr else ()
                    off += nattr * 4
                    records.append(Record(self.string(rname), vals, didx, ndc))
                total += count
                cats.append(Category(self.string(nameOff), records))
        except (struct.error, ValueError):
            return None
        # The walk must consume the file and produce exactly the promised rows.
        if total != nrec or not (0 <= len(data) - off < 64):
            return None
        return cats

    def string(self, off):
        if off >= len(self.strtab):
            return ""
        end = self.strtab.index(b"\0", off)
        return self.strtab[off:end].decode("latin-1")

    def category(self, name):
        return self.categories[self.by_name[name]]

    def deref(self, value):
        """Resolve a type-9 cross-reference to (Category, Record), or None."""
        if value == NULL_REF:
            return None
        cat_idx = (value >> 16) - 1
        rec_idx = value & 0xFFFF
        if not (0 <= cat_idx < len(self.categories)):
            return None
        cat = self.categories[cat_idx]
        if not (0 <= rec_idx < len(cat.records)):
            return None
        return cat, cat.records[rec_idx]
