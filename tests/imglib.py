#!/usr/bin/env python3
#===================================================================================================
#  tiny dependency free image reader : raw PNM (P5/P6) and PNG (8 bit, non interlaced)
#  shared by imgcmp.py and tiles_check.py
#===================================================================================================
import sys, zlib, struct

def read_png (path):
    d = open (path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    i, idat, ihdr = 8, [], None
    while i < len(d):
        ln, typ = struct.unpack ('>I4s', d[i:i+8])
        body = d[i+8:i+8+ln]
        if   typ == b'IHDR': ihdr = struct.unpack ('>IIBBBBB', body)
        elif typ == b'IDAT': idat.append (body)
        elif typ == b'IEND': break
        i += 12 + ln
    w, h, depth, ctype, comp, filt, inter = ihdr
    assert depth == 8 and inter == 0, 'unsupported PNG (depth=%d interlace=%d)' % (depth, inter)
    nch = {0:1, 2:3, 4:2, 6:4}[ctype]
    raw  = zlib.decompress (b''.join(idat))
    stride = w * nch
    out, prev, p = bytearray(), bytearray(stride), 0
    for y in range(h):
        ft = raw[p]; p += 1
        line = bytearray (raw[p:p+stride]); p += stride
        if   ft == 1:
            for x in range(nch, stride): line[x] = (line[x] + line[x-nch]) & 255
        elif ft == 2:
            for x in range(stride):      line[x] = (line[x] + prev[x]) & 255
        elif ft == 3:
            for x in range(stride):
                a = line[x-nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif ft == 4:
            for x in range(stride):
                a = line[x-nch] if x >= nch else 0
                b = prev[x]
                c = prev[x-nch] if x >= nch else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out += line; prev = line
    if nch == 4:  out = bytes(b for k, b in enumerate(out) if k & 3 != 3)
    elif nch == 2:out = bytes(b for k, b in enumerate(out) if k & 1 == 0)
    return [str(w).encode(), str(h).encode(), b'255'], bytes(out)

def read_pnm (path):
    d = open (path, 'rb').read()
    magic = d[0:2].decode('latin1')
    if magic not in ('P4','P5','P6','P7'):
        raise SystemExit ('%s : not a raw PNM file (magic=%r)' % (path, magic))
    i = d.index (b'\n', 0) + 1
    fields = []
    while len (fields) < (4 if magic == 'P7' else 3):
        while i < len(d) and d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while i < len(d) and d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while j < len(d) and not d[j:j+1].isspace(): j += 1
        fields.append (d[i:j]); i = j
    while i < len(d) and d[i:i+1].isspace(): i += 1
    return magic, fields, d[i:]

def read_image (path):
    if open(path,'rb').read(4) == b'\x89PNG':
        f, p = read_png (path)
        return 'png', f, p
    return read_pnm (path)

def channels (magic, fields, data):
    if magic == 'png':                 return len(data) // (int(fields[0]) * int(fields[1]))
    if magic in ('P4', 'P5'):          return 1
    if magic == 'P6':                  return 3
    return int(fields[3])                      # PAM / P7 : TuplType

def rows_crop (magic, fields, data, r0, r1):
    """return rows [r0,r1) of the image"""
    stride = int(fields[0]) * channels (magic, fields, data)
    return data[r0*stride : r1*stride]
