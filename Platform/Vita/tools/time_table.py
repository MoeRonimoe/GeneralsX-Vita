"""Prints the "[time]" lines of a log as a table: one row per line, one column per timer named
on the command line (milliseconds per frame; with ":n" the calls per frame, with ":d" the draw
calls), with the "[bench]" lines in between.

    time_table.py <log> <timer>[:n|:d]...
"""
import re
import sys

ENTRY = re.compile(r'(~?)([A-Za-z_0-9]+) ~?([0-9.]+) ms \(([0-9.]+)x(?:, ([0-9]+) draws)?\)')

columns = sys.argv[2:]
print('line  ' + ' '.join('%9s' % c[-9:] for c in columns))
for number, line in enumerate(open(sys.argv[1], encoding='latin-1'), 1):
    if line.startswith('[bench]'):
        print(line.rstrip()[:110])
        continue
    if not line.startswith('[time] per frame:'):
        continue
    entries = {m.group(2): (float(m.group(3)), float(m.group(4)), int(m.group(5) or 0)) for m in ENTRY.finditer(line)}
    cells = []
    for column in columns:
        name, _, what = column.partition(':')
        entry = entries.get(name)
        if entry is None:
            cells.append('%9s' % '-')
        elif what == 'n':
            cells.append('%9.1f' % entry[1])
        elif what == 'd':
            cells.append('%9d' % entry[2])
        else:
            cells.append('%9.1f' % entry[0])
    print('%5d ' % number + ' '.join(cells))
