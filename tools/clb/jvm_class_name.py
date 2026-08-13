"""Read the fully-qualified internal class name (e.g. "cmn/common") out of a
standard Java classfile, by parsing the constant pool and following
this_class -> Class entry -> name_index -> Utf8 entry. Needed because the
CLB source file path doesn't match the package the compiled class actually
declares (e.g. sys/script/cmn/common.clb compiles to package "cmn", not
"sys.script.cmn") — used to lay out script_research/classes/ so CFR's
cross-reference resolution and --outputdir package layout both work."""
import struct


def get_class_name(data: bytes) -> str:
    pos = 8  # skip magic, minor, major
    cp_count = struct.unpack_from(">H", data, pos)[0]
    pos += 2
    cp = {}  # index -> (tag, payload_start)
    i = 1
    while i < cp_count:
        tag = data[pos]
        entry_start = pos
        pos += 1
        if tag == 1:  # Utf8
            length = struct.unpack_from(">H", data, pos)[0]
            pos += 2 + length
        elif tag in (3, 4, 9, 10, 11, 12, 18):
            pos += 4
        elif tag in (5, 6):
            pos += 8
        elif tag == 7:
            pos += 2
        elif tag == 8:
            pos += 2
        elif tag == 15:
            pos += 3
        elif tag == 16:
            pos += 2
        else:
            raise ValueError(f"unknown cp tag {tag} at index {i}")
        cp[i] = (tag, entry_start)
        if tag in (5, 6):  # Long/Double take two slots
            i += 2
        else:
            i += 1

    access_flags_pos = pos
    this_class_index = struct.unpack_from(">H", data, access_flags_pos + 2)[0]
    class_tag, class_entry_start = cp[this_class_index]
    assert class_tag == 7
    name_index = struct.unpack_from(">H", data, class_entry_start + 1)[0]
    utf8_tag, utf8_entry_start = cp[name_index]
    assert utf8_tag == 1
    length = struct.unpack_from(">H", data, utf8_entry_start + 1)[0]
    name = data[utf8_entry_start + 3:utf8_entry_start + 3 + length].decode("utf-8", "replace")
    return name
