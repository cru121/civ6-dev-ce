# SPDX-License-Identifier: AGPL-3.0-only
# Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
# Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
"""python pdb_sym.py DLL HEXOFFSET... : resolve offsets inside a DLL with its PDB (DbgHelp via ctypes)."""
import ctypes, sys
from ctypes import wintypes as w

dll = sys.argv[1]
offs = [int(x, 16) for x in sys.argv[2:]]
dh = ctypes.WinDLL('dbghelp', use_last_error=True)
dh.SymSetOptions.argtypes = [w.DWORD]
dh.SymInitialize.argtypes = [w.HANDLE, w.LPCSTR, w.BOOL]
dh.SymLoadModuleEx.argtypes = [w.HANDLE, w.HANDLE, w.LPCSTR, w.LPCSTR, ctypes.c_uint64, w.DWORD, ctypes.c_void_p, w.DWORD]
dh.SymLoadModuleEx.restype = ctypes.c_uint64
dh.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
dh.SymGetLineFromAddr64.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
h = ctypes.c_void_p(1)
dh.SymSetOptions(0x2 | 0x10)
assert dh.SymInitialize(h, None, False)
base = 0x10000000
b = dh.SymLoadModuleEx(h, None, dll.encode(), None, base, 0, None, 0)
assert b, ctypes.get_last_error()

class SYM(ctypes.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', w.ULONG), ('Size', w.ULONG),
                ('ModBase', ctypes.c_uint64), ('Flags', w.ULONG), ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', w.ULONG),
                ('Scope', w.ULONG), ('Tag', w.ULONG), ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG), ('Name', ctypes.c_char * 512)]

class LINE(ctypes.Structure):
    _fields_ = [('SizeOfStruct', w.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', w.DWORD), ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]

for o in offs:
    s = SYM(); s.SizeOfStruct = 88; s.MaxNameLen = 512
    d = ctypes.c_uint64(0)
    if dh.SymFromAddr(h, base + o, ctypes.byref(d), ctypes.byref(s)):
        ln = LINE(); ln.SizeOfStruct = ctypes.sizeof(LINE); dd = w.DWORD(0)
        where = ''
        if dh.SymGetLineFromAddr64(h, base + o, ctypes.byref(dd), ctypes.byref(ln)):
            where = '  %s:%d' % (ln.FileName.decode(errors='replace'), ln.LineNumber)
        print('0x%x  %s+0x%x%s' % (o, s.Name.decode(errors='replace'), d.value, where))
    else:
        print('0x%x  ?' % o)
