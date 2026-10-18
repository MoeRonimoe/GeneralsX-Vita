"""Name the code addresses found on the stack of a parsed PS Vita core dump.

    python parse_core.py <dump> <elf> --stack 200 > parsed.txt
    arm-vita-eabi-nm -C --defined-only <elf> > symbols.txt
    python resolve_stack.py parsed.txt symbols.txt

Every stack word that points into the code segment is printed with the function it
falls into. These are candidates, not a real backtrace: stale return addresses of
earlier calls appear between the live ones. Read from the top (the innermost frame).

Looking the addresses up in the symbol table takes a moment; asking addr2line about
each one takes minutes on an ELF with full debug information.
"""

import bisect
import re
import sys

CODE_BASE = 0x81000000  # link address of the code segment ("g_generals@1")


def load_symbols(path):
    symbols = []
    with open(path, errors='replace') as f:
        for line in f:
            parts = line.rstrip('\n').split(' ', 2)
            if len(parts) == 3 and parts[1] in 'TtWw':
                symbols.append((int(parts[0], 16) & ~1, parts[2]))
    symbols.sort()
    return symbols


def main():
    parsed_path, symbols_path = sys.argv[1:3]
    symbols = load_symbols(symbols_path)
    addresses = [address for address, name in symbols]

    in_stack = False
    pattern = re.compile(r'(SP => )?\s*0x([0-9a-f]+): 0x[0-9a-f]+ \(\S+@1 \+ 0x([0-9a-f]+)\)')
    with open(parsed_path, errors='replace') as f:
        for line in f:
            line = re.sub(r'\x1b\[[0-9;]*m', '', line)
            if line.startswith('STACK CONTENTS'):
                in_stack = True
                continue
            if not in_stack:
                continue
            match = pattern.search(line)
            if not match:
                continue
            address = CODE_BASE + (int(match.group(3), 16) & ~1)
            index = bisect.bisect_right(addresses, address) - 1
            if index < 0:
                continue
            start, name = symbols[index]
            marker = 'SP' if match.group(1) else '  '
            print('%s 0x%s  %s + 0x%x' % (marker, match.group(2), name[:110], address - start))


if __name__ == '__main__':
    main()
