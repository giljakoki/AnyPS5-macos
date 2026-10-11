#!/usr/bin/env python3
"""Resolves the frames of a test in an abort_backtrace report to file and line with atos.

Usage: resolve_backtrace.py <test program> <test output>

The program carries line tables (-gline-tables-only) and a debug map to its object files.
"""
import os
import re
import subprocess
import sys

program, log = sys.argv[1], sys.argv[2]
name = os.path.basename(program)
load = None
addresses = []
with open(log, encoding="utf-8", errors="replace") as lines:
    for line in lines:
        header = re.search(r"abort_backtrace: signal \d+, load address (0x[0-9a-f]+)", line)
        if header:
            load = header.group(1)
            continue
        fields = line.split()
        if load and len(fields) >= 3 and fields[1] == name and fields[2].startswith("0x"):
            addresses.append(fields[2])
if load and addresses:
    command = ["atos", "-arch", "x86_64", "-o", program, "-l", load] + addresses
    try:
        resolved = subprocess.run(command, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired) as error:
        resolved = f"atos failed: {error}\n"
    print(f"frames of {name}:")
    print(resolved, end="")
