#!/usr/bin/env python3
"""Build the binary RomFS image tables of an extracted RomFS directory, so the host can serve it as an IStorage to the SDK's own RomFS parser.
Usage: mkromfs.py <romfs dir> <out dir>   -> <out>/romfs.tables (header + directory / file hash and meta tables) and <out>/romfs_files.tsv (data offset, size, host path).
The file data region is not copied: the host reads a file's bytes from its extracted copy when the storage is read at that offset."""
import os, struct, sys

def hash_(parent, name, nb):
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xffffffff
        h ^= c
    return h % nb

def buckets(n):
    if n < 3: return 3
    if n < 19: return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)): n += 1
    return n

def build(root, out):
    dirs = []; files = []                     # (parent index, name bytes, ...)
    def walk(path, parent):
        ents = sorted(os.listdir(path), key=lambda s: s.encode())
        mydirs = [e for e in ents if os.path.isdir(os.path.join(path, e))]
        myfiles = [e for e in ents if not os.path.isdir(os.path.join(path, e)) and not e.startswith('.')]
        d_idx = []
        for e in mydirs:
            idx = len(dirs); dirs.append(dict(parent=parent, name=e.encode(), path=os.path.join(path, e), first_child=0xffffffff, first_file=0xffffffff, next=0xffffffff, hnext=0xffffffff))
            d_idx.append(idx)
        for a, b in zip(d_idx, d_idx[1:]): dirs[a]['next'] = b
        f_idx = []
        for e in myfiles:
            p = os.path.join(path, e)
            idx = len(files); files.append(dict(parent=parent, name=e.encode(), path=p, size=os.path.getsize(p), next=0xffffffff, hnext=0xffffffff, off=0)); f_idx.append(idx)
        for a, b in zip(f_idx, f_idx[1:]): files[a]['next'] = b
        if parent is not None and parent >= 0:
            if d_idx: dirs[parent]['first_child'] = d_idx[0]
            if f_idx: dirs[parent]['first_file'] = f_idx[0]
        return d_idx, f_idx
    # directory 0 is the root
    dirs.append(dict(parent=0, name=b'', path=root, first_child=0xffffffff, first_file=0xffffffff, next=0xffffffff, hnext=0xffffffff))
    queue = [0]
    while queue:
        di = queue.pop(0)
        path = dirs[di]['path']
        ents = sorted(os.listdir(path), key=lambda s: s.encode())
        mydirs = [e for e in ents if os.path.isdir(os.path.join(path, e))]
        myfiles = [e for e in ents if not os.path.isdir(os.path.join(path, e)) and not e.startswith('.')]
        prev = None
        for e in mydirs:
            idx = len(dirs); dirs.append(dict(parent=di, name=e.encode(), path=os.path.join(path, e), first_child=0xffffffff, first_file=0xffffffff, next=0xffffffff, hnext=0xffffffff))
            if prev is None: dirs[di]['first_child'] = idx
            else: dirs[prev]['next'] = idx
            prev = idx; queue.append(idx)
        prev = None
        for e in myfiles:
            p = os.path.join(path, e)
            idx = len(files); files.append(dict(parent=di, name=e.encode(), path=p, size=os.path.getsize(p), next=0xffffffff, hnext=0xffffffff, off=0))
            if prev is None: dirs[di]['first_file'] = idx
            else: files[prev]['next'] = idx
            prev = idx
    # meta tables: offsets of each entry within its table
    def align4(n): return (n + 3) & ~3
    doff = []; o = 0
    for d in dirs: doff.append(o); o += 24 + align4(len(d['name']))
    dmeta_size = o
    foff = []; o = 0
    for f in files: foff.append(o); o += 32 + align4(len(f['name']))
    fmeta_size = o
    # file data offsets: 16-byte aligned, in order of the file table
    pos = 0
    for f in files: f['off'] = pos; pos = (pos + f['size'] + 15) & ~15
    data_size = pos
    nbd, nbf = buckets(len(dirs)), buckets(len(files))
    dhash = [0xffffffff] * nbd; fhash = [0xffffffff] * nbf
    for i, d in enumerate(dirs):
        h = hash_(doff[d['parent']] if i else 0, d['name'], nbd)      # the hash key is the parent's offset in the directory table
        d['hnext'] = dhash[h]; dhash[h] = doff[i]
    for i, f in enumerate(files):
        h = hash_(doff[f['parent']], f['name'], nbf)
        f['hnext'] = fhash[h]; fhash[h] = foff[i]
    dmeta = bytearray()
    for i, d in enumerate(dirs):
        par = doff[d['parent']] if i else 0
        nxt = doff[d['next']] if d['next'] != 0xffffffff else 0xffffffff
        fc = doff[d['first_child']] if d['first_child'] != 0xffffffff else 0xffffffff
        ff = foff[d['first_file']] if d['first_file'] != 0xffffffff else 0xffffffff
        dmeta += struct.pack('<6I', par, nxt, fc, ff, d['hnext'], len(d['name'])) + d['name'] + b'\0' * (align4(len(d['name'])) - len(d['name']))
    fmeta = bytearray()
    for i, f in enumerate(files):
        nxt = foff[f['next']] if f['next'] != 0xffffffff else 0xffffffff
        fmeta += struct.pack('<IIQQII', doff[f['parent']], nxt, f['off'], f['size'], f['hnext'], len(f['name'])) + f['name'] + b'\0' * (align4(len(f['name'])) - len(f['name']))
    assert len(dmeta) == dmeta_size and len(fmeta) == fmeta_size
    def al(n, a=0x200): return (n + a - 1) & ~(a - 1)
    dh_off = 0x200; dh_size = 4 * nbd
    dm_off = al(dh_off + dh_size, 4); fh_off = al(dm_off + dmeta_size, 4); fh_size = 4 * nbf
    fm_off = al(fh_off + fh_size, 4); data_off = al(fm_off + fmeta_size, 0x200)
    hdr = struct.pack('<10Q', 0x50, dh_off, dh_size, dm_off, dmeta_size, fh_off, fh_size, fm_off, fmeta_size, data_off)
    img = bytearray(data_off)
    img[0:len(hdr)] = hdr
    img[dh_off:dh_off + dh_size] = struct.pack('<%dI' % nbd, *dhash)
    img[dm_off:dm_off + dmeta_size] = dmeta
    img[fh_off:fh_off + fh_size] = struct.pack('<%dI' % nbf, *fhash)
    img[fm_off:fm_off + fmeta_size] = fmeta
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'romfs.tables'), 'wb').write(img)
    with open(os.path.join(out, 'romfs_files.tsv'), 'w') as f:
        f.write('%d\t%d\n' % (data_off, data_size))
        for fl in files: f.write('%d\t%d\t%s\n' % (data_off + fl['off'], fl['size'], os.path.relpath(fl['path'], root)))
    print('%d directories, %d files, tables %d bytes, data %d bytes' % (len(dirs), len(files), len(img), data_size))

if __name__ == '__main__': build(sys.argv[1], sys.argv[2])
