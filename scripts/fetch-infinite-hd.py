#!/usr/bin/env python3
"""
Reassemble Infinite Mac's "Infinite HD" disk image.

The live site serves the disk as content-addressed 256 KiB chunks plus a
manifest. This fetches the manifest, downloads each unique non-zero chunk once,
and writes the reconstructed 1.95 GiB image.

Chunks that the manifest leaves empty are zero-filled (the site does not store
all-zero chunks).
"""
import concurrent.futures as cf
import hashlib
import json
import os
import re
import sys
import time
import urllib.request

MANIFEST_JS = (
    "https://infinitemac.org/assets/0/Infinite%20HD.dsk-BRADXI0E.js"
)
CHUNK_BASE = "https://infinitemac.org/Disk"
CHUNK_SIZE = 256 * 1024
OUT = os.path.expanduser("~/ss-run/Infinite HD.dsk")
# Cache chunks so an interrupted run does not re-download everything.
CACHE = os.path.expanduser("~/ss-run/.infinite-hd-chunks")

UA = {"User-Agent": "Mozilla/5.0 (Macintosh; arm64 Mac OS X) SheepShaver-arm64/import"}


def fetch_manifest():
    req = urllib.request.Request(MANIFEST_JS, headers=UA)
    with urllib.request.urlopen(req, timeout=60) as r:
        s = r.read().decode("utf-8", "replace")
    m = re.search(r"var e=`([^`]*)`,t=([0-9.e]+),n=`([^`]*)`", s)
    if not m:
        raise SystemExit("Could not parse manifest")
    name, total, chunkstr = m.group(1), m.group(2), m.group(3)
    chunks = chunkstr.split(".")
    return name, int(float(total)), chunks


def download_chunk(sig):
    path = os.path.join(CACHE, sig + ".chunk")
    if os.path.exists(path) and os.path.getsize(path) == CHUNK_SIZE:
        with open(path, "rb") as f:
            return f.read()
    url = f"{CHUNK_BASE}/{sig}.chunk"
    last = None
    for attempt in range(6):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
            break
        except Exception as e:  # transient 5xx / network
            last = e
            time.sleep(1.0 * (attempt + 1))
    else:
        raise RuntimeError(f"{sig}: giving up after retries: {last}")
    if len(data) != CHUNK_SIZE:
        raise RuntimeError(f"{sig}: got {len(data)} bytes, want {CHUNK_SIZE}")
    # Content-addressed: verify the first half of the signature (blake2b-16).
    got = hashlib.blake2b(data, digest_size=16, salt=b"raw").hexdigest()
    if not sig.startswith(got):
        raise RuntimeError(f"{sig}: hash mismatch (got {got})")
    os.makedirs(CACHE, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return data


def main():
    os.makedirs(CACHE, exist_ok=True)
    name, total, chunks = fetch_manifest()
    nchunks = (total + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"Image: {name!r}  size={total}  ({total/1024/1024/1024:.2f} GiB)")
    print(f"Manifest lists {len(chunks)} chunks; image spans {nchunks}")
    while len(chunks) < nchunks:
        chunks.append("")

    unique = sorted({c for c in chunks if c})
    print(f"Unique non-zero chunks: {len(unique)} ({len(unique)*CHUNK_SIZE/1024/1024:.0f} MiB)")

    done = 0

    def work(sig):
        nonlocal done
        data = download_chunk(sig)
        done += 1
        if done % 200 == 0:
            print(f"  {done}/{len(unique)} chunks", file=sys.stderr)
        return sig, data

    # Download concurrently, hold only the working set.
    chunks_data = {}
    with cf.ThreadPoolExecutor(max_workers=4) as ex:
        for sig, data in ex.map(work, unique):
            chunks_data[sig] = data

    print("Writing image...")
    zero = b"\0" * CHUNK_SIZE
    tmp = OUT + ".partial"
    with open(tmp, "wb") as f:
        written = 0
        for idx, sig in enumerate(chunks):
            if not sig:
                block = zero
            else:
                block = chunks_data.get(sig)
                if block is None:
                    block = download_chunk(sig)
            remain = total - written
            if remain <= 0:
                break
            if remain < len(block):
                block = block[:remain]
            f.write(block)
            written += len(block)
        if written < total:
            f.write(b"\0" * (total - written))
            written = total
    os.replace(tmp, OUT)
    print(f"Wrote {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
