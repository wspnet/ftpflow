#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate 10k mixed-type files for compressibility-decision accuracy stats.

Categories (ground truth = python zstd level-1 on the whole file):
  text      .txt/.log -- highly compressible        -> expect COMPRESS
  code      .c/.cpp/.py/.json/.xml -- compressible  -> expect COMPRESS
  csv       .csv -- numeric logs, compressible      -> expect COMPRESS
  zip       .zip -- already compressed              -> expect STORE
  png       .png -- already compressed              -> expect STORE
  jpgish    .jpg -- random body + real JFIF header  -> expect STORE
  mp4       .mp4 -- random body + ftyp box          -> expect STORE
  random    .bin -- pure random                     -> expect STORE
  mixed     .dat -- 50% text + 50% random (10MB+)   -> small file: STORE
                                                    # (below minSaving or
                                                    #  gain threshold at
                                                    #  the aggregation
                                                    #  boundary)
  tinytext  .log -- <64KiB text                     -> STORE (aggregation)

Writes files under <root>/ and a manifest.csv with: relpath, category,
size, truth_ratio (zstd -1), expected verdict.
"""
import os
import random
import struct
import sys
import zlib

try:
    import zstandard as zstd
    HAVE_ZSTD = True
except ImportError:
    HAVE_ZSTD = False
    print("NOTE: pip install zstandard for ground-truth ratios")

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["TEMP"], "ftz_stats")
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
rng = random.Random(20261115)

WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
         "request response handler timeout retry upstream downstream queue "
         "error warning info debug trace audit transaction commit rollback").split()


def gen_text(n: int) -> bytes:
    out = []
    total = 0
    while total < n:
        line = " ".join(rng.choice(WORDS) for _ in range(rng.randint(4, 12)))
        line += "\n"
        out.append(line.encode())
        total += len(line)
    return b"".join(out)[:n]


def gen_code(n: int) -> bytes:
    decl = (b"int fn_%d(int a, int b) { return a * %d + b - (a << %d); }\n"
            b"static const char* kName%d = \"module_%d\";\n")
    out = bytearray()
    i = 0
    while len(out) < n:
        out += decl % (i, rng.randint(1, 99), rng.randint(1, 15), i, i)
        i += 1
    return bytes(out[:n])


def gen_csv(n: int) -> bytes:
    out = bytearray(b"ts,sensor,value,state\n")
    while len(out) < n:
        out += (b"%d,%d,%.3f,%s\n" % (rng.randint(1600000000, 1730000000),
                                       rng.randint(0, 4096),
                                       rng.random() * 100.0,
                                       b"OK" if rng.random() < 0.9 else b"ERR"))
    return bytes(out[:n])


def gen_random(n: int) -> bytes:
    return rng.randbytes(n)


def gen_zip(n: int) -> bytes:
    # Real stored-mode zip: local header + random payload + central dir.
    payload = rng.randbytes(max(64, n - 200))
    name = b"data.bin"
    lh = (b"PK\x03\x04" + struct.pack("<HHHHHIIIHH", 20, 0, 0, 0, 0, 0,
                                      0, len(payload), 0, len(name)))
    cd = (b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII", 20, 20, 0, 0, 0, 0,
                                      0, 0, len(payload), 0, len(name), 0, 0,
                                      0, 0, 30 + len(payload)))
    eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH", 0, 0, 1, 1, len(cd), 30 + len(payload) + len(cd), 0)
    return lh + name + payload + cd + name + eocd


def gen_png(n: int) -> bytes:
    # Valid PNG signature + IHDR + one huge random IDAT (deflate-stored-ish).
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    body = rng.randbytes(max(64, n - 64))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(body, 0)[:8] + body) + chunk(b"IEND", b""))


def gen_jpg(n: int) -> bytes:
    body = rng.randbytes(max(64, n - 4))
    return b"\xff\xd8\xff\xe0" + body + b"\xff\xd9"


def gen_mp4(n: int) -> bytes:
    body = rng.randbytes(max(64, n - 24))
    return b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom" + body


def gen_mixed(n: int) -> bytes:
    half = n // 2
    return gen_text(half) + gen_random(n - half)


CATEGORIES = [
    # (name, extension, generator, size range, expected)
    ("text",   "log", gen_text,  (256 * 1024, 4 * 1024 * 1024), "COMPRESS"),
    ("code",   "c",   gen_code,  (256 * 1024, 4 * 1024 * 1024), "COMPRESS"),
    ("csv",    "csv", gen_csv,   (256 * 1024, 4 * 1024 * 1024), "COMPRESS"),
    ("zip",    "zip", gen_zip,   (256 * 1024, 2 * 1024 * 1024), "STORE"),
    ("png",    "png", gen_png,   (256 * 1024, 2 * 1024 * 1024), "STORE"),
    ("jpgish", "jpg", gen_jpg,   (256 * 1024, 2 * 1024 * 1024), "STORE"),
    ("mp4",    "mp4", gen_mp4,   (256 * 1024, 2 * 1024 * 1024), "STORE"),
    ("random", "bin", gen_random, (256 * 1024, 2 * 1024 * 1024), "STORE"),
    ("mixed",  "dat", gen_mixed, (256 * 1024, 2 * 1024 * 1024), "STORE"),
    ("tiny",   "log", gen_text,  (1 * 1024, 48 * 1024),         "STORE"),
]


def main() -> None:
    os.makedirs(ROOT, exist_ok=True)
    per_cat = COUNT // len(CATEGORIES)
    manifest = ["relpath,category,size,truth_ratio,expected"]
    idx = 0
    cctx = zstd.ZstdCompressor(level=1) if HAVE_ZSTD else None
    for cat, ext, gen, (lo, hi), expected in CATEGORIES:
        d = os.path.join(ROOT, cat)
        os.makedirs(d, exist_ok=True)
        for k in range(per_cat):
            n = rng.randint(lo, hi)
            data = gen(n)
            name = "%s_%05d.%s" % (cat, k, ext)
            path = os.path.join(d, name)
            with open(path, "wb") as f:
                f.write(data)
            ratio = (len(cctx.compress(data)) / len(data)) if cctx else 0.0
            manifest.append("%s,%s,%d,%.4f,%s" % (
                os.path.join(cat, name).replace("\\", "/"), cat, len(data),
                ratio, expected))
            idx += 1
    with open(os.path.join(ROOT, "manifest.csv"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    print("generated %d files under %s (zstd ground truth: %s)" %
          (idx, ROOT, "yes" if HAVE_ZSTD else "NO -- pip install zstandard"))


if __name__ == "__main__":
    main()
