"""Name the code offsets in the "[throw]" lines of vita_log.txt.

    arm-vita-eabi-nm -C --defined-only <elf> > symbols.txt
    python resolve_log.py vita_log.txt symbols.txt

The game writes every C++ exception to its log with the call chain that led to it, as
offsets into the code segment (see compat/VitaDiagnostics.cpp). This prints each of those
lines followed by the functions the offsets fall into, innermost first. The symbols have
to come from the unstripped ELF of exactly the build that wrote the log.

A line labelled "stack scan" is not a call chain: it lists everything on the stack that
looks like a return address, including leftovers of calls that had already returned.
"""

import bisect
import os
import re
import sys

# Found next to this script, also when Python runs isolated (-I) and leaves its directory
# out of the module search path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolve_stack import CODE_BASE, load_symbols


def main():
    log_path, symbols_path = sys.argv[1:3]
    symbols = load_symbols(symbols_path)
    addresses = [address for address, name in symbols]

    with open(log_path, errors='replace') as f:
        for line in f:
            if not line.startswith('[throw]'):
                continue
            print(line.rstrip('\n'))
            for offset in re.findall(r'\+0x([0-9a-f]+)', line):
                address = CODE_BASE + int(offset, 16)
                index = bisect.bisect_right(addresses, address) - 1
                if index < 0:
                    continue
                start, name = symbols[index]
                print('    +0x%-7s %s + 0x%x' % (offset, name[:120], address - start))


if __name__ == '__main__':
    main()
