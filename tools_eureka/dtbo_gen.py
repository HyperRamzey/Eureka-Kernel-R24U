#!/usr/bin/env python3
"""Generate Eureka-style DM-verity-patched DTBO images from stock dtbo.img.

Proven recipe (validated against original R24U a30s release dtbo):
  For each entry blob:
    - decompile FDT -> dts
    - remove the nested `firmware` node (the DM-verity patch: drops the
      `parts = "vbmeta,boot,..."` AVB list and the DT fstab)
    - recompile with dtc (matches vendor rebuild: garbage-collected strings)
  Rebuild container:
    - customs := (dtbo-hw_rev, dtbo-hw_rev_end, 0, 0) from each blob's props
    - contiguous entry packing (same as original)
    - page_size preserved from source

Usage:
  dtbo_gen.py <stock.img> <out.img> [--verify <reference.img>]
"""
import struct, sys, os, subprocess, hashlib, re, tempfile

FDT_MAGIC = b'\xd0\x0d\xfe\xed'

def parse_dtbo(path):
    d = open(path, 'rb').read()
    magic, total, hdr, esz, ec, eoff, page, ver = struct.unpack('>8I', d[:32])
    assert magic == 0xD7B7AB1E, f'not a DTBO container: {magic:#x}'
    ents = []
    for i in range(ec):
        o = eoff + i * esz
        sz, off, eid, rev = struct.unpack('>4I', d[o:o+16])
        ents.append({'size': sz, 'offset': off})
    return d, ents, page

def dtc(args, tmp):
    """Run dtc with cwd=tmp. Files are referenced by bare names to avoid path mixups."""
    subprocess.run(['dtc'] + args, check=True, cwd=tmp)

def dtc_blob_to_dts(blob, tmp):
    with open(f'{tmp}/in.dtb', 'wb') as f:
        f.write(blob)
    dtc(['-I', 'dtb', '-O', 'dts', '-q', '-o', 's.dts', 'in.dtb'], tmp)
    return open(f'{tmp}/s.dts').read()

def dtc_dts_to_blob(dts_text, tmp):
    open(f'{tmp}/s2.dts', 'w').write(dts_text)
    dtc(['-I', 'dts', '-O', 'dtb', '-q', '-o', 'out.dtb', 's2.dts'], tmp)
    return open(f'{tmp}/out.dtb', 'rb').read()

def blob_hw_revs(dts_text):
    lo = re.search(r'dtbo-hw_rev\s*=\s*<0x([0-9a-fA-F]+)>', dts_text)
    hi = re.search(r'dtbo-hw_rev_end\s*=\s*<0x([0-9a-fA-F]+)>', dts_text)
    return (int(lo.group(1), 16) if lo else 0,
            int(hi.group(1), 16) if hi else 0xff)

def strip_firmware(dts_text):
    lines = dts_text.split('\n')
    out, skip, depth = [], False, 0
    for ln in lines:
        if not skip and re.match(r'\s*firmware\s*\{', ln):
            skip, depth = True, ln.count('{') - ln.count('}')
            continue
        if skip:
            depth += ln.count('{') - ln.count('}')
            if depth <= 0:
                skip = False
                # also swallow the blank line that follows, like the vendor build
                continue
            continue
        out.append(ln)
    # vendor rebuild removed exactly the node; their dts has no blank line there.
    # collapse the resulting double blank line
    res = []
    for i, ln in enumerate(out):
        if ln.strip() == '' and i + 1 < len(out) and out[i+1].strip() == '' and i > 0:
            continue
        res.append(ln)
    return '\n'.join(res)

def build_dtbo(pairs, page):
    n = len(pairs)
    table = b''
    body = b''
    cur = 32 + n * 32
    for custom, blob in pairs:
        pad = (-cur) % 4
        body += b'\0' * pad
        cur += pad
        table += struct.pack('>8I', len(blob), cur, 0, 0, *custom)
        body += blob
        cur += len(blob)
    total = cur
    hdr = struct.pack('>8I', 0xD7B7AB1E, total, 32, 32, n, 32, page, 0)
    return hdr + table + body

def main():
    stock_path, out_path = sys.argv[1], sys.argv[2]
    verify = None
    if '--verify' in sys.argv:
        verify = sys.argv[sys.argv.index('--verify') + 1]

    stock, ents, page = parse_dtbo(stock_path)
    print(f"stock {stock_path}: {len(ents)} entries, page={page}")

    tmp = tempfile.mkdtemp(prefix='dtbo_gen_')
    pairs = []
    for i, e in enumerate(ents):
        blob = stock[e['offset']:e['offset'] + e['size']]
        assert blob[:4] == FDT_MAGIC
        src = dtc_blob_to_dts(blob, tmp)
        assert 'firmware' in src or True
        assert re.search(r'\bfirmware\s*\{', src), f'entry {i}: no firmware node!'
        stripped = strip_firmware(src)
        nb = dtc_dts_to_blob(stripped, tmp)
        # vendor rounds FDT totalsize up to 4-byte alignment, pad included
        pad = (-len(nb)) % 4
        if pad:
            nb = nb + b'\0' * pad
            nb = nb[:4] + struct.pack('>I', len(nb)) + nb[8:]
        lo, hi = blob_hw_revs(stripped)
        pairs.append(((lo, hi, 0, 0), nb))
        print(f"  [{i}] {e['size']} -> {len(nb)} bytes, customs=({lo},{hi},0,0)")
    out = build_dtbo(pairs, page)
    open(out_path, 'wb').write(out)
    print(f"wrote {out_path}: {len(out)} bytes")

    if verify:
        ref = open(verify, 'rb').read()
        if out == ref:
            print("*** BYTE-IDENTICAL to reference — recipe proven ***")
        else:
            print(f"reference: {len(ref)} bytes")
            rd, rents, rpage = parse_dtbo(verify)
            for i, re_ in enumerate(rents):
                rb = rd[re_['offset']:re_['offset'] + re_['size']]
                ours = pairs[i][1] if i < len(pairs) else b''
                if rb == ours:
                    print(f"  blob[{i}]: IDENTICAL")
                else:
                    print(f"  blob[{i}]: differs (ours {len(ours)} vs ref {len(rb)})")
                    # find first differing dts line
                    dtc(ours if ours else rb, ['-I', 'dtb', '-O', 'dts', '-q', '-o', f'{tmp}/o.dts', f'{tmp}/in.dtb']) if ours else None
                    dtc(rb, ['-I', 'dtb', '-O', 'dts', '-q', '-o', f'{tmp}/r.dts', f'{tmp}/in.dtb'])
                    if ours:
                        r1 = subprocess.run(['diff', f'{tmp}/o.dts', f'{tmp}/r.dts'], capture_output=True, text=True)
                        print(r1.stdout[:1500])

main()
