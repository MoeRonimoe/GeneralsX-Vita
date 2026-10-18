"""Turns the samples of compat/VitaSampler.cpp into a list of functions by time.

    samples.py <vita_samples.bin> <symbols> <elf> [--all] [--after NAME] [--frames FIRST-LAST] [--still | --moving]
               [--top N] [--callers NAME] [--callees NAME] [--frame-ms MS]

<symbols> is the output of "arm-vita-eabi-nm -C --defined-only" for the program that ran,
<elf> the program itself (the file the .self was made from).

A sample is the function the game's thread had entered last at one moment, and the code
addresses on its stack. "self" is the share of the samples that name a function, "with what
it calls" the share taken while the function was on the stack. Two things to keep in mind
when reading the list:

 - The last function entered is not always the one running: when a function returns, its
   caller goes on without leaving a mark. A function that calls small ones in a loop gives
   its own time to them.
 - What is not compiled with the marks (the C library, vitaGL, the system, waiting) counts
   for the function that called it.

The stack is read without knowing where its frames are, so a word that points into the code
may be a return address or what an earlier call left behind. A word only counts if it follows
a call instruction, and if that call goes to an address, then only if it goes to the function
found before it. Calls through a pointer (virtual functions) cannot be checked that way; they
are taken on trust, and dropped again when a call that can be checked contradicts them. What
is left is right most of the time, not always. A function that ends by jumping to another
(a tail call) breaks the chain there, which makes the callers' shares too small rather than
too large.

Only samples taken during a mission are used unless --all is given. Loading the mission is
part of that; "--after GameLogic::tryStartNewGame" leaves out everything up to the last
sample with that function on the stack, which is the loading. Every sample says which step
of the mission's logic it was taken in: --frames takes the samples of some steps, --still
those of steps that lasted a second or more, which is the game standing still for a message
or a pause and doing nothing but draw the same picture, --moving the others. --callers lists who called
a function when it was the one named, --callees what was named while the function was on the
stack. --frame-ms turns the shares into milliseconds of a frame of that length.
"""
import bisect
import struct
import sys


def read_samples(path):
    data = open(path, 'rb').read()
    magic, version, words, count, text_start, text_end, period, unmarked = struct.unpack_from('<4s7I', data, 0)
    if magic != b'VSMP' or version != 1:
        raise SystemExit('%s is not a file of samples' % path)
    samples = []
    offset = 32
    for _ in range(count):
        if offset + words * 4 > len(data):
            break
        samples.append(struct.unpack_from('<%dI' % words, data, offset))
        offset += words * 4
    return samples, text_start, text_end, period, unmarked


def read_symbols(path):
    symbols = {}
    linked_start = None
    for line in open(path, encoding='utf-8', errors='replace'):
        parts = line.rstrip('\n').split(' ', 2)
        # Labels without a name and the markers of ARM and Thumb code are not functions.
        if len(parts) < 3 or parts[1] not in 'tTwW' or not parts[2].strip() or parts[2].startswith('$'):
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        name = parts[2].strip()
        # The system does not load the program where it was linked. The samples say where
        # its code started when it ran, this symbol where the linker put that.
        if name == '__executable_start':
            linked_start = address
        # Thumb functions have their lowest bit set in some listings.
        address &= ~1
        # One name per address, and not the compiler's own label if there is another.
        known = symbols.get(address)
        if known is None or (known.startswith('.L') and not name.startswith('.L')):
            symbols[address] = name
    if linked_start is None:
        raise SystemExit('%s has no __executable_start' % path)
    addresses = sorted(symbols)
    return addresses, [symbols[a] for a in addresses], linked_start


class Program:
    """The loaded parts of the ELF file, to look at the instruction before an address."""

    def __init__(self, path):
        self.segments = []
        data = open(path, 'rb').read()
        if data[:4] != b'\x7fELF':
            raise SystemExit('%s is not an ELF file' % path)
        phoff, = struct.unpack_from('<I', data, 28)
        phentsize, phnum = struct.unpack_from('<HH', data, 42)
        for i in range(phnum):
            p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<5I', data, phoff + i * phentsize)
            if p_type == 1 and p_filesz > 0:
                self.segments.append((p_vaddr, data[p_offset:p_offset + p_filesz]))

    def read(self, address, size):
        for start, content in self.segments:
            if start <= address and address + size <= start + len(content):
                return content[address - start:address - start + size]
        return None

    def call_before(self, address):
        """What the instruction before this return address is: None if it is no call, 0 if it
        calls through a register, else the address it calls."""
        if address & 1:
            where = address & ~1
            before = self.read(where - 4, 4)
            if before is None:
                return None
            first, second = struct.unpack('<HH', before)
            if (first & 0xF800) == 0xF000 and (second & 0xD000) in (0xD000, 0xC000):
                sign = (first >> 10) & 1
                j1 = (second >> 13) & 1
                j2 = (second >> 11) & 1
                offset = (sign << 24) | ((~(j1 ^ sign) & 1) << 23) | ((~(j2 ^ sign) & 1) << 22) \
                    | ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
                if sign:
                    offset -= 1 << 25
                if second & 0x1000:
                    return (where + offset) & ~1            # bl, to Thumb code
                return ((where & ~3) + offset) & ~3         # blx, to ARM code
            if (second & 0xFF87) == 0x4780:
                return 0                                    # blx to a register
            return None
        before = self.read(address - 4, 4)
        if before is None:
            return None
        instruction, = struct.unpack('<I', before)
        if (instruction & 0x0F000000) == 0x0B000000:
            offset = instruction & 0x00FFFFFF
            if offset & 0x00800000:
                offset -= 1 << 24
            return address + 4 + offset * 4
        if (instruction & 0x0FFFFFF0) == 0x012FFF30:
            return 0
        return None


def main():
    arguments = sys.argv[1:]
    options = {'--top': 40, '--callers': None, '--callees': None, '--frame-ms': None, '--after': None, '--frames': None}
    use_all = '--all' in arguments
    if use_all:
        arguments.remove('--all')
    still = '--still' in arguments
    if still:
        arguments.remove('--still')
    moving = '--moving' in arguments
    if moving:
        arguments.remove('--moving')
    for name in list(options):
        if name in arguments:
            index = arguments.index(name)
            options[name] = arguments[index + 1]
            del arguments[index:index + 2]
    if len(arguments) < 3:
        raise SystemExit(__doc__)

    samples, text_start, text_end, period, unmarked = read_samples(arguments[0])
    addresses, names, linked_start = read_symbols(arguments[1])
    program = Program(arguments[2])
    top = int(options['--top'])
    frame_ms = float(options['--frame-ms']) if options['--frame-ms'] else None

    # From here on every address is where the linker put it.
    moved = text_start - linked_start
    text_start -= moved
    text_end -= moved

    def index_of(address):
        address &= ~1
        if address < text_start or address >= text_end:
            return -1
        return bisect.bisect_right(addresses, address) - 1

    calls = {}

    def call_before(address):
        if address not in calls:
            calls[address] = program.call_before(address)
        return calls[address]

    def chain_of(sample):
        """The functions on the stack, innermost first, as indices into the symbols."""
        running = index_of(sample[0] - moved)
        # Each entry: the function, and whether the call that led out of it could be checked.
        chain = [(running, True)]
        for word in sample[3:]:
            if word == 0:
                continue
            address = word - moved
            target = call_before(address)
            if target is None:
                continue
            caller = index_of(address)
            if caller < 0:
                continue
            if target == 0:
                # Through a pointer: nothing to check it against.
                if caller != chain[-1][0]:
                    chain.append((caller, False))
                continue
            callee = index_of(target)
            if callee == chain[-1][0]:
                if caller != callee:
                    chain.append((caller, True))
                continue
            # It does not call the function found last. If it calls one found before that,
            # what was taken on trust in between was left over from earlier calls.
            for depth in range(len(chain) - 2, -1, -1):
                if not chain[depth + 1][1] and chain[depth][0] == callee:
                    del chain[depth + 1:]
                    if caller != callee:
                        chain.append((caller, True))
                    break
                if chain[depth + 1][1]:
                    break
        return [entry[0] for entry in chain]

    def name_of(index):
        return names[index] if index >= 0 else '(outside the program)'

    used = [s for s in samples if use_all or s[2] != 0]
    print('%d samples, one every %.1f ms; %d of them %s; %d looks found no mark' % (
        len(samples), period / 1000.0, len(used), 'in all' if use_all else 'during a mission', unmarked))

    if options['--frames']:
        first, _, last = options['--frames'].partition('-')
        used = [s for s in used if int(first) <= s[2] <= int(last)]
        print('%d of them in steps %s of the logic' % (len(used), options['--frames']))

    if still or moving:
        # A step that lasts a second is not a step of a running game.
        long_enough = max(1, int(1000000 / period))
        per_step = {}
        for sample in used:
            per_step[sample[2]] = per_step.get(sample[2], 0) + 1
        used = [s for s in used if (per_step[s[2]] >= long_enough) == still]
        print('%d of them while the game %s' % (len(used), 'stood still' if still else 'was running'))

    if options['--after']:
        last = -1
        for number, sample in enumerate(used):
            if any(options['--after'] in name_of(index) for index in chain_of(sample)):
                last = number
        used = used[last + 1:]
        print('%d of them after the last one with "%s" on the stack' % (len(used), options['--after']))

    if not used:
        return

    own = {}
    inclusive = {}
    callers_of = {}
    callees_of = {}
    for sample in used:
        chain = chain_of(sample)
        running = chain[0]
        own[running] = own.get(running, 0) + 1
        for index in set(chain):
            inclusive[index] = inclusive.get(index, 0) + 1

        if options['--callers'] and options['--callers'] in name_of(running):
            caller = chain[1] if len(chain) > 1 else -2
            callers_of[caller] = callers_of.get(caller, 0) + 1
        if options['--callees'] and any(options['--callees'] in name_of(index) for index in chain[1:]):
            callees_of[running] = callees_of.get(running, 0) + 1

    def share(count):
        text = '%5.1f %%' % (100.0 * count / len(used))
        if frame_ms is not None:
            text += ' %5.1f ms' % (frame_ms * count / len(used))
        return text

    print()
    print('by the time spent in the function itself:')
    for index, count in sorted(own.items(), key=lambda item: -item[1])[:top]:
        print('  %s  (with what it calls %s)  %s' % (share(count), share(inclusive.get(index, count)), name_of(index)[:150]))

    print()
    print('by the time spent in the function and what it calls:')
    for index, count in sorted(inclusive.items(), key=lambda item: -item[1])[:top * 2]:
        print('  %s  (itself %s)  %s' % (share(count), share(own.get(index, 0)), name_of(index)[:150]))

    for option, table, title in (('--callers', callers_of, 'who called "%s" when it was the function named:'),
                                 ('--callees', callees_of, 'what was named while "%s" was on the stack:')):
        if options[option]:
            print()
            print(title % options[option])
            total = sum(table.values())
            for index, count in sorted(table.items(), key=lambda item: -item[1])[:top]:
                print('  %5.1f %%  %s  %s' % (100.0 * count / max(total, 1), share(count),
                                             '(nobody found)' if index == -2 else name_of(index)[:150]))


main()
