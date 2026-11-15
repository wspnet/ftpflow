#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run the ftpflow decision funnel over the generated corpus and report
per-category accuracy + stage distribution.

Usage: python tools/run_stats.py [corpus_root]
"""
import csv
import os
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["TEMP"], "ftz_stats")
CLI = os.path.join(os.path.dirname(__file__), "..", "build", "release-full", "cli", "ftpcli.exe")

def main() -> None:
    with open(os.path.join(ROOT, "manifest.csv")) as f:
        rows = list(csv.DictReader(f))
    # One ftpcli invocation over all files: the CLI already batches.
    # (chunk to keep the command line sane on Windows)
    stats = defaultdict(Counter)
    mistakes = []
    CH = 200
    for i in range(0, len(rows), CH):
        chunk = rows[i:i + CH]
        cmd = [CLI, "decide"] + [os.path.join(ROOT, r["relpath"]) for r in chunk]
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
        parsed = {}
        for line in out.splitlines():
            # name is padded to 28 chars; verdict is cols 29-38
            try:
                name = line[:28].strip()
                verdict = line[29:39].strip()
                stage = int(line[40:44])
                reason = line[45:]
                parsed[name] = (verdict, stage, reason)
            except (ValueError, IndexError):
                continue
        for r in chunk:
            name = os.path.basename(r["relpath"])
            cat = r["category"]
            expected = r["expected"]
            got = parsed.get(name)
            if not got:
                stats[cat]["MISSING"] += 1
                continue
            verdict, stage, reason = got
            stats[cat]["total"] += 1
            stats[cat]["stage%d" % stage] += 1
            stats[cat][verdict] += 1
            if verdict != expected:
                stats[cat]["WRONG"] += 1
                if len(mistakes) < 15:
                    mistakes.append((r["relpath"], expected, verdict, stage, reason, r["truth_ratio"]))

    print("%-8s %6s %6s %6s  %-24s" % ("CAT", "N", "OK%", "WRONG", "stage distribution"))
    total = wrong = 0
    for cat in sorted(stats):
        s = stats[cat]
        n = s["total"]
        w = s["WRONG"]
        total += n
        wrong += w
        stages = " ".join("%s=%d" % (k, v) for k, v in sorted(s.items())
                          if k.startswith("stage"))
        print("%-8s %6d %5.1f%% %6d  %s" % (cat, n, 100.0 * (n - w) / max(n, 1), w, stages))
    print("\nTOTAL: %d files, %d wrong, accuracy %.2f%%" %
          (total, wrong, 100.0 * (total - wrong) / max(total, 1)))
    if mistakes:
        print("\nfirst mistakes:")
        for m in mistakes:
            print("  %s: expected %s got %s (stage %d, ratio %s) -- %s" % m)

if __name__ == "__main__":
    main()
