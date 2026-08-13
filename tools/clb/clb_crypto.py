"""
CLB script decryption — pure-Python reimplementation.

Ported from Nova Chrysalia v2.0.9's Libraries/CLB.dll (namespace
clbEdit.Core.Handlers.CryptoClasses), decompiled with ilspycmd. Original
author credit embedded in that tool's own output: "CLB2JC - CLB Script to
Java Class and Vice Versa Converter (c)2015 d0ming0_se7en".

Format: 8-byte plaintext "seed" header, followed by the encrypted body in
8-byte blocks (body length must be a multiple of 8; a 4-byte checksum is
appended after the last block on the encrypt side — not verified here).

Cipher: a custom, self-keyed 8-byte-block cipher structurally close to
TEA/XTEA (32-round Feistel-style key schedule building a 264-byte XOR table,
delta-less variant using a x5 multiply instead of a golden-ratio constant),
combined with a simple byte substitution ("integers" table = x -> (x+120) mod 256)
applied per output byte. The key schedule is seeded ONLY from the file's own
first 8 bytes — there is no external secret. See ASSET_REVERSE_ENGINEERING.md
for the full writeup.
"""
import sys

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

# IntegersArray.Integers: Integers[x] = (x + 120) mod 256
def integers(x: int) -> int:
    return (x + 120) & 0xFF

# Inverse of the above (used on the encrypt side via Array.IndexOf — not
# needed for decryption, kept for completeness / future encrypt support).
def integers_inv(x: int) -> int:
    return (x - 120) & 0xFF


def rotl32(x: int, n: int) -> int:
    x &= MASK32
    return ((x << n) | (x >> (32 - n))) & MASK32


def rotr32(x: int, n: int) -> int:
    x &= MASK32
    return ((x >> n) | (x << (32 - n))) & MASK32


def generate_xor_table(seed: bytes) -> bytes:
    """Generator.GenerateXORtable — builds a 264-byte (33 x 8) table from
    the file's own first 8 bytes."""
    assert len(seed) == 8
    seed = bytes(reversed(seed))
    num = int.from_bytes(seed[0:4], "little")
    num2 = int.from_bytes(seed[4:8], "little")
    num = rotl32(num, 8)
    num2 = rotr32(num2, 16)  # rotate-16 is symmetric but keep semantics explicit
    block = list(num2.to_bytes(4, "little") + num.to_bytes(4, "little"))

    block[0] = (block[0] + 69) & 0xFF
    for i in range(1, 8):
        v = (block[i] + 212 + block[i - 1]) & 0xFFFFFFFF
        v ^= (block[i - 1] << 2)
        v ^= 0x45
        block[i] = v & 0xFF

    table = bytearray(264)
    table[0:8] = block

    num4 = int.from_bytes(bytes(block), "little")
    write_off = 8
    for _ in range(1, 33):
        lo = num4 & MASK32
        hi = (num4 >> 32) & MASK32
        num8 = (5 * num4) & MASK64
        num8 ^= (hi << 32)
        num9 = (lo ^ num8) & MASK64          # zero-extended 32-bit XOR full 64-bit
        num10 = (num8 >> 32) & MASK32
        num8 = (lo & MASK32) | (num8 & 0xFFFFFFFF00000000)
        value = (num8 ^ num9) & MASK32       # truncate to low 32 bits
        num10 ^= hi
        value2 = num10 & MASK32

        block = list(value.to_bytes(4, "little") + value2.to_bytes(4, "little"))
        table[write_off:write_off + 8] = block
        num4 = int.from_bytes(bytes(block), "little")
        write_off += 8

    return bytes(table)


def block_counter_setup(block_counter: int):
    """CryptoBase.BlockCounterSetup — returns (xor_table_offset, eval_, fval_)."""
    num = block_counter & 0xFFFF
    num = (num >> 3) << 3
    xor_table_offset = num & 0xF8

    bc64 = block_counter & MASK64
    num2 = (bc64 << 10) & MASK64
    num3 = (bc64 << 20) & MASK64
    num4 = (bc64 << 30) & MASK64

    num5 = num2 & MASK32
    num6 = (num2 >> 32) & MASK32
    num7 = num3 & MASK32
    num8 = (num3 >> 32) & MASK32
    num7 |= num5
    num8 |= num6

    eval_ = (num4 & MASK32) | (block_counter & MASK32) | num7
    fval_ = ((num4 >> 32) & MASK32) | num8
    return xor_table_offset, eval_ & MASK32, fval_ & MASK32


def xor_block_setup(xor_table: bytes, table_offset: int):
    lo = int.from_bytes(xor_table[table_offset:table_offset + 4], "little")
    hi = int.from_bytes(xor_table[table_offset + 4:table_offset + 8], "little")
    return lo, hi


def special_key_setup(eval_: int, fval_: int):
    carry = 1 if eval_ > 1587207352 else 0
    special1 = eval_ + 2707759943
    special2 = fval_ + carry
    return special1, special2


def loop_a_byte(decrypted_byte: int, xor_table: bytes, table_offset: int) -> int:
    v = decrypted_byte
    for i in range(8):
        a = integers(v)
        b = xor_table[table_offset + i]
        v = (a - b) & 0xFF
    return v


def decrypt(data: bytes) -> bytes:
    """Full CryptoCLB.ProcessClb('d', ...) equivalent. Returns the decrypted
    file (8-byte plaintext header preserved, followed by the decrypted body).
    Ignores/does not verify the trailing 4-byte checksum."""
    assert len(data) >= 8
    seed = data[0:8]
    body = data[8:]
    body_len = len(body)
    if body_len % 8 != 0:
        # Real files have a 4-byte checksum tacked on after the last block;
        # only whole 8-byte blocks are decrypted, matching the C# tool
        # (num // 8 blocks, remainder — the checksum — left alone).
        pass
    block_count = body_len // 8

    xor_table = generate_xor_table(seed)
    out = bytearray(8 + block_count * 8)
    out[0:8] = seed

    counter = 0
    for i in range(block_count):
        chunk = body[i * 8:i * 8 + 8]
        xor_table_offset, eval_, fval_ = block_counter_setup(counter)

        d1 = ((i ^ 0x45) & 0xFF) ^ chunk[0]
        d1 = loop_a_byte(d1, xor_table, xor_table_offset)
        d2 = loop_a_byte(chunk[0] ^ chunk[1], xor_table, xor_table_offset)
        d3 = loop_a_byte(chunk[1] ^ chunk[2], xor_table, xor_table_offset)
        d4 = loop_a_byte(chunk[2] ^ chunk[3], xor_table, xor_table_offset)
        d5 = loop_a_byte(chunk[3] ^ chunk[4], xor_table, xor_table_offset)
        d6 = loop_a_byte(chunk[4] ^ chunk[5], xor_table, xor_table_offset)
        d7 = loop_a_byte(chunk[5] ^ chunk[6], xor_table, xor_table_offset)
        d8 = loop_a_byte(chunk[6] ^ chunk[7], xor_table, xor_table_offset)

        value = bytes([d5, d6, d7, d8, d1, d2, d3, d4])
        num3 = int.from_bytes(value[0:4], "little")
        num4 = int.from_bytes(value[4:8], "little")

        xor_lo, xor_hi = xor_block_setup(xor_table, xor_table_offset)
        special1, special2 = special_key_setup(eval_, fval_)

        num5 = num4
        num6 = num3
        carry = 1 if num5 < xor_lo else 0
        num5 = (num5 - xor_lo) & MASK64
        num6 = (num6 - xor_hi - carry) & MASK64
        num5 ^= special1
        num6 ^= special2
        num5 ^= xor_lo
        num6 ^= xor_hi

        bytes5 = (num5 & MASK32).to_bytes(4, "little")
        bytes6 = (num6 & MASK32).to_bytes(4, "little")

        out[8 + i * 8: 8 + i * 8 + 4] = bytes6
        out[8 + i * 8 + 4: 8 + i * 8 + 8] = bytes5

        counter += 8

    return bytes(out)


def main():
    if len(sys.argv) < 2:
        print("usage: clb_crypto.py <file.clb> [out.dec]")
        sys.exit(1)
    in_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else in_path + ".dec"
    with open(in_path, "rb") as f:
        data = f.read()
    dec = decrypt(data)
    with open(out_path, "wb") as f:
        f.write(dec)
    print(f"Decrypted {len(data)} bytes -> {out_path}")

    # Sanity check, mirroring what CLB.ClbToJavaClass reads first:
    if len(dec) >= 10:
        minor = int.from_bytes(dec[4:6], "little")
        major = int.from_bytes(dec[6:8], "little")
        cp_count = int.from_bytes(dec[8:10], "little")
        print(f"minor version field @4  = {minor}")
        print(f"major version field @6  = {major}")
        print(f"constant pool count @8  = {cp_count}")


if __name__ == "__main__":
    main()
