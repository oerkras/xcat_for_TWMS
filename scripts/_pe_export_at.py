"""把模块 RVA 反查成「最近的导出函数 + 偏移」。

用途：卡死取证里只有 `Foo.dll+0xRVA`，没有符号。对带导出表的模块（游戏自带的
CrashReporter.dll 之类），最近的导出名往往就足以认出调用的是哪个 API。

用法： python scripts/_pe_export_at.py <dll路径> <rva十六进制> [更多 rva...]
"""
import bisect
import struct
import sys


def load_exports(path):
    data = open(path, 'rb').read()
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b'PE\0\0':
        raise SystemExit('not a PE: ' + path)
    coff = e_lfanew + 4
    n_sections = struct.unpack_from('<H', data, coff + 2)[0]
    opt_size = struct.unpack_from('<H', data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, exp_size = struct.unpack_from('<II', data, dd)
    if not exp_rva:
        raise SystemExit('no export directory')

    sec = opt + opt_size
    sections = []
    for i in range(n_sections):
        off = sec + i * 40
        va = struct.unpack_from('<I', data, off + 12)[0]
        raw_size = struct.unpack_from('<I', data, off + 16)[0]
        raw_ptr = struct.unpack_from('<I', data, off + 20)[0]
        sections.append((va, raw_size, raw_ptr))

    def to_off(rva):
        for va, raw_size, raw_ptr in sections:
            if va <= rva < va + raw_size:
                return raw_ptr + (rva - va)
        return None

    eo = to_off(exp_rva)
    n_funcs, n_names = struct.unpack_from('<II', data, eo + 20)
    addr_rva, names_rva, ords_rva = struct.unpack_from('<III', data, eo + 28)
    addr_off, names_off, ords_off = to_off(addr_rva), to_off(names_rva), to_off(ords_rva)

    by_ord = {}
    for i in range(n_names):
        name_rva = struct.unpack_from('<I', data, names_off + i * 4)[0]
        o = to_off(name_rva)
        end = data.index(b'\0', o)
        ordinal = struct.unpack_from('<H', data, ords_off + i * 2)[0]
        by_ord[ordinal] = data[o:end].decode('ascii', 'ignore')

    out = []
    for i in range(n_funcs):
        f_rva = struct.unpack_from('<I', data, addr_off + i * 4)[0]
        if f_rva:
            out.append((f_rva, by_ord.get(i, '<ordinal %d>' % i)))
    out.sort()
    return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    exports = load_exports(sys.argv[1])
    keys = [e[0] for e in exports]
    print('%s — %d exports' % (sys.argv[1], len(exports)))
    for arg in sys.argv[2:]:
        rva = int(arg, 16)
        i = bisect.bisect_right(keys, rva) - 1
        if i < 0:
            print('  +0x%-8x  <before first export>' % rva)
            continue
        base, name = exports[i]
        print('  +0x%-8x  %s+0x%x' % (rva, name, rva - base))


if __name__ == '__main__':
    main()
