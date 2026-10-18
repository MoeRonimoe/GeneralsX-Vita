"""Lists the audio files in the game's archives the way VitaAudioManager hands them to miniaudio
(>archive|offset|size|name), one per line, for stresstest.cpp.

    make_list.py <directory with the .big archives> <list to write>

Also prints how many files of which encoding each archive holds.
"""
import collections
import os
import struct
import sys

SEPARATOR = chr(92)  # the path separator inside the archives
FORMATS = {1: 'PCM', 2: 'MS ADPCM', 3: 'float', 0x11: 'IMA ADPCM', 0x55: 'MP3 in WAV'}


def entries(path):
    """(name, offset, size) of every file in a .big archive."""
    with open(path, 'rb') as archive:
        head = archive.read(16)
        if head[:4] not in (b'BIGF', b'BIG4'):
            return []
        count, first = struct.unpack('>II', head[8:16])
        table = archive.read(first - 16)

    found, position = [], 0
    for _ in range(count):
        offset, size = struct.unpack('>II', table[position:position + 8])
        end = table.index(b'\0', position + 8)
        found.append((table[position + 8:end].decode('latin-1'), offset, size))
        position = end + 1
    return found


def wav_format(archive, offset, size):
    archive.seek(offset)
    head = archive.read(min(size, 256))
    if head[:4] != b'RIFF' or head[8:12] != b'WAVE':
        return 'not a WAV file'

    position = 12
    while position + 24 <= len(head):
        tag, length = head[position:position + 4], struct.unpack('<I', head[position + 4:position + 8])[0]
        if tag == b'fmt ':
            encoding, channels, rate = struct.unpack('<HHI', head[position + 8:position + 16])
            return '%s, %d channel(s), %d Hz' % (FORMATS.get(encoding, hex(encoding)), channels, rate)
        position += 8 + length + (length & 1)
    return 'no format chunk'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    directory, list_path = sys.argv[1].replace(SEPARATOR, '/').rstrip('/'), sys.argv[2]
    kinds = collections.Counter()
    lines = []
    for name in sorted(os.listdir(directory)):
        if not name.lower().endswith('.big'):
            continue

        path = directory + '/' + name
        audio = [entry for entry in entries(path) if entry[0].lower().endswith(('.wav', '.mp3'))]
        with open(path, 'rb') as archive:
            for file_name, offset, size in audio:
                kind = 'MP3' if file_name.lower().endswith('.mp3') else wav_format(archive, offset, size)
                kinds[(name, kind)] += 1
                lines.append('>%s|%d|%d|%s' % (path, offset, size, file_name))

    with open(list_path, 'w', encoding='latin-1', newline='\n') as output:
        output.write('\n'.join(lines) + '\n')

    for (name, kind), count in sorted(kinds.items()):
        print('%-22s %-36s %5d' % (name, kind, count))
    print('%d files' % len(lines))


main()
