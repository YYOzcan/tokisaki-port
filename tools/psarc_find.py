# psarc_find.py <psarc> <size hex> <offset hex> <len hex>: find TOC entries of that
# uncompressed size, decompress the range, report nonzero bytes and dump the head.
import sys, struct, zlib

path, want_size, off, ln = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
f = open(path, 'rb')
hdr = f.read(32)
magic, ver, comp, toc_len, entry_sz, n_entries, block_sz, flags = struct.unpack('>4sI4sIIIII', hdr)
toc = f.read(toc_len - 32)
entries = []
for i in range(n_entries):
    e = toc[i * entry_sz:(i + 1) * entry_sz]
    block = struct.unpack('>I', e[16:20])[0]
    usize = int.from_bytes(e[20:25], 'big')
    offs = int.from_bytes(e[25:30], 'big')
    entries.append((i, block, usize, offs))
# block size table: 2 bytes per block when block_sz <= 0x10000
nbytes = 2 if block_sz <= 0x10000 else (3 if block_sz <= 0x1000000 else 4)
table = toc[n_entries * entry_sz:]
nblocks = len(table) // nbytes
sizes = [int.from_bytes(table[j * nbytes:(j + 1) * nbytes], 'big') for j in range(nblocks)]

def read_entry_range(ent, start, length):
    i, block, usize, offs = ent
    out = bytearray()
    pos = offs
    b = block
    produced = 0
    while produced < start + length and produced < usize:
        csz = sizes[b] or block_sz
        f.seek(pos)
        raw = f.read(csz)
        expect = min(block_sz, usize - produced)
        if csz == expect or raw[:2] not in (b'\x78\xda', b'\x78\x9c', b'\x78\x01', b'\x78\x5e'):
            data = raw[:expect]
        else:
            data = zlib.decompress(raw)
        out += data
        produced += len(data)
        pos += csz
        b += 1
    return bytes(out[start:start + length])

hits = [e for e in entries if e[2] == want_size]
print('entries with size %x: %d' % (want_size, len(hits)))
for e in hits[:5]:
    d = read_entry_range(e, off, ln)
    nz = sum(1 for x in d if x)
    print('entry %d block %d offset 0x%x: nz=%d/%d head=%s' % (e[0], e[1], e[3], nz, len(d), d[:32].hex()))
