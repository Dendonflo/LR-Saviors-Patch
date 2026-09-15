#!/usr/bin/env python3
"""Disassemble D3D9 shader bytecode dumps (shaders\ps_XXXXXXXX.bin) with
D3DDisassemble from d3dcompiler_47.dll.   python tools/disasm_ps.py file.bin ..."""
import ctypes, sys, os
d3d = ctypes.WinDLL("d3dcompiler_47.dll")
class ID3DBlob(ctypes.Structure): pass
def disasm(data):
    blob = ctypes.c_void_p()
    hr = d3d.D3DDisassemble(data, len(data), 0, None, ctypes.byref(blob))
    if hr != 0: return f"D3DDisassemble failed 0x{hr & 0xffffffff:08x}"
    vt = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    GetBufferPointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    GetBufferSize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    p = GetBufferPointer(blob); n = GetBufferSize(blob)
    return ctypes.string_at(p, n).decode(errors="replace")
for f in sys.argv[1:]:
    data = open(f, "rb").read()
    print(f"==== {os.path.basename(f)}  ({len(data)} bytes)")
    print(disasm(data))
