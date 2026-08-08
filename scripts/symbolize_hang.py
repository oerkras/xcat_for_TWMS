"""给 hang_autopsy 取证文件里的 xcat.dll 帧补上函数名与源码行。

取证文件落盘时只有 `xcat.dll+0xRVA`（进程内没有符号）。这里用 dbghelp 加载
bin/XCat_data/xcat.pdb 离线解析。

用法：
    python scripts/symbolize_hang.py bin/XCat_data/logs/hang/hang_xxx.txt
    python scripts/symbolize_hang.py <文件> --rva 6c1a 715f      # 只查几个偏移

**RVA 与 PDB 必须同源**：取证是哪一次构建产出的 DLL 记下来的，就得用那一次的
PDB。DLL 重新链接过之后，旧取证里的 RVA 就对不上了——脚本无从校验这一点，
名字看着不合理时先怀疑这里。
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

MAX_SYM_NAME = 2000
SYMOPT_UNDNAME = 0x00000002
SYMOPT_DEFERRED_LOADS = 0x00000004
SYMOPT_LOAD_LINES = 0x00000010
FAKE_BASE = 0x10000000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ('SizeOfStruct', wt.ULONG),
        ('TypeIndex', wt.ULONG),
        ('Reserved', ctypes.c_ulonglong * 2),
        ('Index', wt.ULONG),
        ('Size', wt.ULONG),
        ('ModBase', ctypes.c_ulonglong),
        ('Flags', wt.ULONG),
        ('Value', ctypes.c_ulonglong),
        ('Address', ctypes.c_ulonglong),
        ('Register', wt.ULONG),
        ('Scope', wt.ULONG),
        ('Tag', wt.ULONG),
        ('NameLen', wt.ULONG),
        ('MaxNameLen', wt.ULONG),
        ('Name', ctypes.c_char * (MAX_SYM_NAME + 1)),
    ]


# dbghelp 校验的是「不含 Name 尾巴」的 SYMBOL_INFO 大小（x64 下 88 字节），
# 不是我们这个把 Name 撑成 2001 字节数组后的 sizeof——填错会被直接拒掉、无任何提示。
SIZEOF_SYMBOL_INFO = 88


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ('SizeOfStruct', wt.DWORD),
        ('Key', ctypes.c_void_p),
        ('LineNumber', wt.DWORD),
        ('FileName', ctypes.c_char_p),
        ('Address', ctypes.c_ulonglong),
    ]


class Symbolizer:
    def __init__(self, dll_path, pdb_dir=None):
        self.dbghelp = ctypes.WinDLL('dbghelp.dll')
        self.h = ctypes.c_void_p(-1)  # 伪句柄：不附着任何进程，纯离线解析
        # 不能加 SYMOPT_DEFERRED_LOADS：配合伪进程句柄时符号会一直停在延迟态不加载，
        # SymFromAddr 全部静默返回「查不到」。
        self.dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES)
        search = pdb_dir or os.path.dirname(os.path.abspath(dll_path))
        if not self.dbghelp.SymInitialize(self.h, search.encode('mbcs'), False):
            raise OSError('SymInitialize failed: %d' % ctypes.get_last_error())
        self.dbghelp.SymLoadModuleExW.restype = ctypes.c_ulonglong
        self.dbghelp.SymLoadModuleExW.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p,
            ctypes.c_ulonglong, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong]
        self.dbghelp.SymFromAddr.argtypes = [
            ctypes.c_void_p, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
            ctypes.c_void_p]
        self.dbghelp.SymGetLineFromAddr64.argtypes = [
            ctypes.c_void_p, ctypes.c_ulonglong, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
        base = self.dbghelp.SymLoadModuleExW(self.h, None, os.path.abspath(dll_path), None,
                                             FAKE_BASE, 0, None, 0)
        if not base:
            raise OSError('SymLoadModuleEx failed: %d' % ctypes.get_last_error())
        self.base = base

    def resolve(self, rva):
        # SYMBOL_INFO 的 Name 是变长尾巴，用裸缓冲区手填偏移最稳：
        # SizeOfStruct@0、MaxNameLen@80、Name@84。
        buf = ctypes.create_string_buffer(SIZEOF_SYMBOL_INFO + MAX_SYM_NAME)
        ctypes.memset(buf, 0, len(buf))
        ctypes.cast(buf, ctypes.POINTER(wt.ULONG))[0] = SIZEOF_SYMBOL_INFO
        ctypes.cast(ctypes.byref(buf, 80), ctypes.POINTER(wt.ULONG))[0] = MAX_SYM_NAME
        disp = ctypes.c_ulonglong(0)
        addr = ctypes.c_ulonglong(self.base + rva)
        name = None
        if self.dbghelp.SymFromAddr(self.h, addr, ctypes.byref(disp), buf):
            name = '%s+0x%x' % (ctypes.string_at(ctypes.byref(buf, 84)).decode('mbcs', 'replace'),
                                disp.value)
        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        ldisp = wt.DWORD(0)
        where = None
        if self.dbghelp.SymGetLineFromAddr64(self.h, addr, ctypes.byref(ldisp),
                                             ctypes.byref(line)):  # noqa: E501
            fn = line.FileName.decode('mbcs', 'replace') if line.FileName else '?'
            where = '%s:%d' % (os.path.basename(fn), line.LineNumber)
        return name, where


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    rvas = None
    if '--rva' in args:
        i = args.index('--rva')
        rvas = [int(x, 16) for x in args[i + 1:]]

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    data = os.path.join(repo, 'bin', 'XCat_data')
    dll = os.path.join(data, 'xcat.dll')
    # 老构建会把 PDB 落在 <config> 子目录里（多配置生成器的默认行为），一并找。
    cands = [os.path.join(data, 'xcat.pdb')] + [
        os.path.join(data, c, 'xcat.pdb') for c in ('Release', 'RelWithDebInfo', 'Debug')
    ]
    pdb = next((p for p in cands if os.path.exists(p)), None)
    if not pdb:
        raise SystemExit('找不到 xcat.pdb（找过 %s）—— 需要开启 /DEBUG 的构建' % ', '.join(cands))
    print('dll: %s' % dll)
    print('pdb: %s (mtime %s)' % (pdb, __import__('datetime').datetime.fromtimestamp(
        os.path.getmtime(pdb)).strftime('%Y-%m-%d %H:%M:%S')))
    print()

    sym = Symbolizer(dll, os.path.dirname(pdb))

    if rvas is not None:
        for rva in rvas:
            name, where = sym.resolve(rva)
            print('  xcat.dll+0x%-8x %-60s %s' % (rva, name or '<no symbol>', where or ''))
        return

    text = open(path, 'r', encoding='utf-8', errors='replace').read()
    pat = re.compile(r'^(\s+[0-9a-f]{16}\s+)xcat\.dll\+0x([0-9a-f]+)\s*$', re.M)

    def repl(m):
        name, where = sym.resolve(int(m.group(2), 16))
        tail = '  %s' % where if where else ''
        return '%sxcat.dll+0x%s  %s%s' % (m.group(1), m.group(2), name or '<no symbol>', tail)

    out, n = pat.subn(repl, text)
    dest = path + '.sym.txt'
    open(dest, 'w', encoding='utf-8').write(out)
    print('已解析 %d 个 xcat.dll 帧 -> %s' % (n, dest))


if __name__ == '__main__':
    main()
