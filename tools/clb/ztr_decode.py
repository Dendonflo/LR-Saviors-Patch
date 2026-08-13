"""
ZTR (Zone Text Resource) decoder for LR:FFXIII.

Big-endian. Text is byte-pair-encoded: a "page" is a rule index -> (first,
second), where each item is either another page index (expand recursively,
always pointing backward) or a literal byte.

Layout:
    0x00  u64  magic (1)
    0x08  u32  line count
    0x0C  u32  decompressed line-IDs size
    0x10  u32  number of dictionary chunk offsets
    0x14  u32[n] chunk offsets (relative to the start of the chunk area)
    ...   lineInfo[lineCount]: u8 chunk, u8 startPageIdx, u16 relOffset
    ...   chunks: u32 dictSize, dict (3-byte page entries), compressed strings

Chunk 0 holds the line IDs as one blob; the rest hold the text.
Written against the format description on the LR Research Team wiki, then
checked against known-good output (IDs must come out as printable ASCII keys).
"""
import struct
import sys


def parse_pages(dict_bytes):
    """3-byte entries: page index, first item, second item."""
    pages = {}
    for i in range(0, len(dict_bytes) - 2, 3):
        idx = dict_bytes[i]
        pages[idx] = (dict_bytes[i + 1], dict_bytes[i + 2])
    return pages


def expand(value, pages, depth=0):
    if depth > 64:
        return b""
    if value in pages:
        a, b = pages[value]
        return expand(a, pages, depth + 1) + expand(b, pages, depth + 1)
    return bytes([value])


def decode_stream(data, pages):
    out = bytearray()
    for b in data:
        out += expand(b, pages)
    return bytes(out)


def load(path):
    d = open(path, "rb").read()
    magic = struct.unpack_from(">Q", d, 0)[0]
    line_count = struct.unpack_from(">I", d, 8)[0]
    ids_size = struct.unpack_from(">I", d, 12)[0]
    n_chunks = struct.unpack_from(">I", d, 16)[0]
    offs = [struct.unpack_from(">I", d, 20 + 4 * i)[0] for i in range(n_chunks)]

    li_start = 20 + 4 * n_chunks
    line_info = []
    for i in range(line_count):
        c, sp = d[li_start + 4 * i], d[li_start + 4 * i + 1]
        rel = struct.unpack_from(">H", d, li_start + 4 * i + 2)[0]
        line_info.append((c, sp, rel))

    chunk_area = li_start + 4 * line_count
    chunks = []
    for i, off in enumerate(offs):
        start = chunk_area + off
        end = chunk_area + offs[i + 1] if i + 1 < len(offs) else len(d)
        blob = d[start:end]
        dsize = struct.unpack_from(">I", blob, 0)[0]
        dict_bytes = blob[4:4 + dsize]
        comp = blob[4 + dsize:]
        chunks.append({"pages": parse_pages(dict_bytes), "comp": comp,
                       "dsize": dsize, "raw": blob})
    return {"magic": magic, "line_count": line_count, "ids_size": ids_size,
            "offs": offs, "line_info": line_info, "chunks": chunks,
            "chunk_area": chunk_area, "data": d}


def decode_ids(z):
    """Chunk 0 decompresses to one NUL-separated blob of line IDs."""
    c = z["chunks"][0]
    blob = decode_stream(c["comp"], c["pages"])
    parts = blob.split(b"\x00")
    return [p.decode("ascii", "replace") for p in parts if p]


def main():
    path = sys.argv[1]
    z = load(path)
    print(f"lines={z['line_count']} chunks={len(z['chunks'])} ids_size={z['ids_size']}")
    for i, c in enumerate(z["chunks"]):
        print(f"  chunk {i}: dsize={c['dsize']} pages={len(c['pages'])} comp={len(c['comp'])}")
    ids = decode_ids(z)
    print(f"\ndecoded {len(ids)} ids (expected {z['line_count']})")
    for s in ids[:15]:
        print("   ", repr(s))


if __name__ == "__main__":
    main()
