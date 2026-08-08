"""从 cdb 抓栈文件里提取每个线程的栈顶 RIP，做等待状态分布。

栈走不出来时（读内存被挡）仍然可用：线程寄存器上下文是能读到的，
所以每个线程至少有一行栈顶地址。把它们按地址分桶，就能看出
「多少线程卡在同一个等待原语上」——锁挤压一眼就能认出来。
"""
import collections
import glob
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob('Dumps/runtime/hang/hang_live_*.txt'))[-1]
lines = open(path, 'r', encoding='utf-8', errors='replace').read().splitlines()

marks = [i for i, l in enumerate(lines) if l.strip() == '===ALLTHREADS===']
start = marks[-1] if marks else 0

re_head = re.compile(r'^[.\s*#]*(\d+)\s+Id:\s+\w+\.(\w+)')
re_name = re.compile(r'"(.*?)"')
re_frame = re.compile(r'^[0-9a-f]{8}`[0-9a-f]{8}\s+[0-9a-f]{8}`[0-9a-f]{8}\s+0x([0-9a-f]{8})`([0-9a-f]{8})')

cur = None
rips = {}
names = {}
for line in lines[start:]:
    m = re_head.match(line)
    if m:
        cur = m.group(1)
        q = re_name.search(line)
        if q:
            names[cur] = q.group(1)
        continue
    m2 = re_frame.match(line)
    if m2 and cur is not None and cur not in rips:
        rips[cur] = int(m2.group(1) + m2.group(2), 16)

print('file            :', path)
print('threads with RIP:', len(rips))
if '0' in rips:
    print('main thread 0   : 0x%016x' % rips['0'])
print()

counter = collections.Counter(rips.values())
print('%-20s %6s  %s' % ('RIP', 'count', 'thread names / tids'))
for rip, n in counter.most_common(12):
    tids = [t for t in rips if rips[t] == rip]
    nm = sorted({names[t] for t in tids if t in names})[:4]
    label = ', '.join(nm) if nm else '(unnamed) ' + ','.join(tids[:8])
    print('0x%016x %6d  %s' % (rip, n, label))

with open('Dumps/runtime/hang/_rips.txt', 'w') as fh:
    for rip in counter:
        fh.write('0x%016x\n' % rip)
print()
print('distinct RIPs written to Dumps/runtime/hang/_rips.txt')
