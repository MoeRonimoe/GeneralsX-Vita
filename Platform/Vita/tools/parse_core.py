"""Symbolize a PS Vita core dump (.psp2dmp) of the game.

When the game crashes, the console writes ux0:data/psp2core-<...>-eboot.bin.psp2dmp.
This script runs xyzz's vita-parse-core on it under Python 3 and prints the crashed
thread, its registers, the disassembly around the fault and the stack.

    python parse_core.py <core dump> <elf> [--stack WORDS]
                         [--parse-core DIR] [--pyelftools DIR]

<elf> has to be the unstripped executable the installed eboot.bin was made from
(build/vita/Generals/Code/Main/g_generals of that exact build), so keep a copy of it
for every build that goes onto a console.

Needs two checkouts, by default looked up in references/ at the repository root:

    git clone https://github.com/xyzz/vita-parse-core references/vita-parse-core
    git clone --branch v0.24 https://github.com/eliben/pyelftools references/pyelftools

and arm-vita-eabi-addr2line / arm-vita-eabi-objdump from $VITASDK/bin.

Addresses in the output that are not resolved (the stack words) are runtime addresses.
"g_generals@1 + 0x1234" is offset 0x1234 into the code segment, which is address
0x81000000 + 0x1234 in the ELF and can be given to arm-vita-eabi-addr2line.
"""

import argparse
import builtins
import os
import sys

REPOSITORY = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('core')
    parser.add_argument('elf')
    parser.add_argument('--stack', default='48', help='stack words to print (default 48)')
    parser.add_argument('--parse-core', default=os.path.join(REPOSITORY, 'references', 'vita-parse-core'))
    parser.add_argument('--pyelftools', default=os.path.join(REPOSITORY, 'references', 'pyelftools'))
    args = parser.parse_args()

    for path in (args.parse_core, args.pyelftools):
        if not os.path.isdir(path):
            sys.exit('missing checkout: %s (see the comment at the top of this script)' % path)

    vitasdk = os.environ.get('VITASDK')
    if vitasdk:
        os.environ['PATH'] = os.path.join(vitasdk, 'bin') + os.pathsep + os.environ.get('PATH', '')

    # vita-parse-core predates Python 3.
    builtins.xrange = range
    sys.path.insert(0, args.pyelftools)
    sys.path.insert(0, args.parse_core)
    sys.argv = ['vita-parse-core', '-s', args.stack, args.core, args.elf]

    import main as vita_parse_core
    vita_parse_core.main()


if __name__ == '__main__':
    main()
