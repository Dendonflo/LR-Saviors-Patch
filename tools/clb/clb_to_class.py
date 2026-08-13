"""
CLB (decrypted) -> real Java .class file translator.

Pure-Python port of Nova Chrysalia v2.0.9's CLB.ClbToJavaClass
(Libraries/CLB.dll, decompiled with ilspycmd). Original tool credit:
"CLB2JC - CLB Script to Java Class and Vice Versa Converter (c)2015 d0ming0_se7en".

Input: the DECRYPTED byte stream produced by clb_crypto.decrypt() (i.e. the
original 8-byte seed header still attached at the front, exactly as the C#
tool expects — it reads its own header fields starting at offset 4).

The decrypted CLB internal layout is a flat, fixed-record, pointer/address
based structure (all fields little-endian, i.e. native x86 — NOT the
big-endian convention seen in this engine's other binary formats like SDRB/
GTEX). This translator resolves those addresses into real 1-based JVM
constant-pool indices and emits a byte-for-byte standard big-endian Java
classfile. Method bytecode requires NO transformation — it's copied
verbatim from the source, meaning script logic is already real JVM
bytecode inside the CLB, just wrapped in this custom addressing scheme.

See ASSET_REVERSE_ENGINEERING.md for the header field map this relies on.
"""
import struct
import sys


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def convert(dec: bytes) -> bytes:
    out = bytearray()
    out += bytes([0xCA, 0xFE, 0xBA, 0xBE])  # magic

    minor = u16(dec, 4)
    major = u16(dec, 6)
    cp_count = u16(dec, 8)
    out += struct.pack(">H", minor)
    out += struct.pack(">H", major)
    out += struct.pack(">H", cp_count)

    # --- Pass 1: scan constant pool for Utf8 entries (address -> (index, str)) + max len
    cp_base = u32(dec, 24)
    utf8_by_addr = {}   # address -> (index, string)
    max_utf8_len = 0
    addr = cp_base
    idx = 0
    while True:
        tag = u32(dec, addr)
        if tag > 18:
            break
        if tag == 1:
            length = u32(dec, addr + 8)
            str_addr = u32(dec, addr + 12)
            s = dec[str_addr:str_addr + length].decode("utf-8", "replace")
            utf8_by_addr[addr] = (idx, s)
            max_utf8_len = max(max_utf8_len, length)
        addr += 16
        idx += 1

    utf8_by_string = {}
    for a, (i, s) in utf8_by_addr.items():
        utf8_by_string[s] = i

    # --- Pass 2: emit constant pool, building addr_to_index (list3 equivalent)
    cp_out = bytearray()
    addr_to_index = {}
    addr = cp_base
    idx = 0
    while True:
        tag = u32(dec, addr)
        if tag > 18:
            break
        addr_to_index[addr] = idx

        if tag == 1:  # Utf8
            length = u32(dec, addr + 8)
            str_addr = u32(dec, addr + 12)
            raw = dec[str_addr:str_addr + length]
            cp_out += bytes([1]) + struct.pack(">H", length) + raw
        elif tag == 3:  # Integer
            cp_out += bytes([3]) + dec[addr + 8:addr + 12][::-1]
        elif tag == 4:  # Float
            cp_out += bytes([4]) + dec[addr + 8:addr + 12][::-1]
        elif tag == 5:  # Long
            cp_out += bytes([5]) + dec[addr + 8:addr + 16][::-1]
        elif tag == 6:  # Double
            cp_out += bytes([6]) + dec[addr + 8:addr + 16][::-1]
        elif tag == 7:  # Class
            name_addr = u32(dec, addr + 8)
            raw = dec[name_addr:name_addr + max_utf8_len + 5]
            name = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
            name_index = utf8_by_string.get(name, 0)
            cp_out += bytes([7]) + struct.pack(">H", name_index)
        elif tag == 8:  # String
            v = u16(dec, addr + 8)
            cp_out += bytes([8]) + struct.pack(">H", v)
        elif tag == 9:  # Fieldref
            c = u16(dec, addr + 8)
            nt = u16(dec, addr + 10)
            cp_out += bytes([9]) + struct.pack(">HH", c, nt)
        elif tag == 10:  # Methodref
            c = u16(dec, addr + 8)
            nt = u16(dec, addr + 10)
            cp_out += bytes([10]) + struct.pack(">HH", c, nt)
        elif tag == 11:  # InterfaceMethodref
            c = u16(dec, addr + 8)
            nt = u16(dec, addr + 10)
            cp_out += bytes([11]) + struct.pack(">HH", c, nt)
        elif tag == 12:  # NameAndType
            n = u16(dec, addr + 8)
            d = u16(dec, addr + 10)
            cp_out += bytes([12]) + struct.pack(">HH", n, d)
        elif tag == 15:  # MethodHandle
            raw = dec[addr + 8:addr + 11][::-1]
            cp_out += bytes([15]) + raw
        elif tag == 16:  # MethodType
            v = u16(dec, addr + 8)
            cp_out += bytes([16]) + struct.pack(">H", v)
        elif tag == 18:  # InvokeDynamic
            raw = dec[addr + 8:addr + 12][::-1]
            cp_out += bytes([18]) + raw
        # tag 0 (phantom Long/Double slot) and 2/13/14/17 (invalid/unused): write nothing

        addr += 16
        idx += 1

    out += cp_out

    # --- access_flags (hardcoded ACC_PUBLIC, matching the original tool)
    out += struct.pack(">H", 1)

    # --- this_class / super_class: header stores an ADDRESS into the Class-tag
    #     entries; resolve via addr_to_index, same map used for attributes.
    def resolve_class_ref(header_off):
        target_addr = u32(dec, header_off)
        return addr_to_index.get(target_addr, 0)

    out += struct.pack(">H", resolve_class_ref(28))  # this_class
    out += struct.pack(">H", resolve_class_ref(36))  # super_class

    # --- interfaces_count (always 0 — unsupported by the source tool)
    out += struct.pack(">H", 0)

    # --- fields
    fields_addr = u32(dec, 44)
    if fields_addr != 0:
        field_count = u16(dec, 14)
        out += struct.pack(">H", field_count)
        for i in range(field_count):
            rec = fields_addr + 24 * i
            access = u16(dec, rec)
            name_index = u16(dec, rec + 2)
            desc_index = u16(dec, rec + 4)
            attr_count = u16(dec, rec + 6)
            out += struct.pack(">HHHH", access, name_index, desc_index, attr_count)
            if attr_count > 0:
                attrs_addr = u32(dec, rec + 8)
                for j in range(attr_count):
                    arec = attrs_addr + 22 * j
                    name_addr = u32(dec, arec)
                    name_idx = addr_to_index.get(name_addr, 0)
                    cv_addr = u32(dec, arec + 4)
                    cv_idx = addr_to_index.get(cv_addr, 0)
                    out += struct.pack(">HIH", name_idx, 2, cv_idx)
    else:
        out += struct.pack(">H", 0)

    # --- methods
    methods_addr = u32(dec, 48)
    if methods_addr != 0:
        method_count = u16(dec, 16)
        out += struct.pack(">H", method_count)
        for i in range(method_count):
            rec = methods_addr + 24 * i
            access = u16(dec, rec)
            name_index = u16(dec, rec + 2)
            desc_index = u16(dec, rec + 4)
            attr_count = u16(dec, rec + 6)
            out += struct.pack(">HHHH", access, name_index, desc_index, attr_count)
            if attr_count > 0:
                attrs_addr = u32(dec, rec + 20)
                for j in range(attr_count):
                    arec = attrs_addr + 22 * j
                    attr_name_index = u16(dec, arec)      # raw index, no lookup (matches original)
                    max_stack = u16(dec, arec + 2)
                    max_locals = u16(dec, arec + 4)
                    code_len = u32(dec, arec + 8)
                    code_addr = u32(dec, arec + 12)
                    code = dec[code_addr:code_addr + code_len]
                    attr_len = code_len + 12
                    out += struct.pack(">HI", attr_name_index, attr_len)
                    out += struct.pack(">HH", max_stack, max_locals)
                    out += struct.pack(">I", code_len)
                    out += code
                    out += struct.pack(">H", 0)  # exception_table_length
                    out += struct.pack(">H", 0)  # attributes_count
    else:
        out += struct.pack(">H", 0)

    # --- class attributes_count (always 0 — unsupported by the source tool)
    out += struct.pack(">H", 0)

    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print("usage: clb_to_class.py <decrypted.dec> <out.class>")
        sys.exit(1)
    with open(sys.argv[1], "rb") as f:
        dec = f.read()
    cls = convert(dec)
    with open(sys.argv[2], "wb") as f:
        f.write(cls)
    print(f"Wrote {len(cls)}-byte class file -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
