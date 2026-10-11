#!/usr/bin/env python3
"""Prints the macOS crash reports of one program written after a given file was last modified.

Usage: crash_reports.py <file touched before the program ran> <program path>

A report (.ips, written by ReportCrash) holds the exception and the stack of every thread. Frames in the
program's own build are resolved to file and line with atos, from the line tables the build carries.
"""
import glob
import json
import os
import subprocess
import sys

REPORT_DIRECTORIES = ("~/Library/Logs/DiagnosticReports", "/Library/Logs/DiagnosticReports")
OTHER_THREAD_FRAMES = 25


def load(path):
    with open(path, encoding="utf-8", errors="replace") as source:
        header = source.readline()
        body = source.read()
    try:
        return json.loads(header), json.loads(body)
    except ValueError:
        return None, None


def resolve(images, frames, root):
    """Returns {(image index, offset): "function (file:line)"} for frames in images below root."""
    wanted = {}
    for frame in frames:
        index = frame.get("imageIndex")
        if index is None or index >= len(images):
            continue
        path = images[index].get("path", "")
        if path.startswith(root) and os.path.exists(path):
            wanted.setdefault(index, set()).add(frame.get("imageOffset", 0))
    resolved = {}
    for index, offsets in wanted.items():
        image = images[index]
        offsets = sorted(offsets)
        base = image.get("base", 0)
        command = ["atos", "-arch", image.get("arch", "x86_64"), "-o", image["path"], "-l", hex(base)]
        command += [hex(base + offset) for offset in offsets]
        try:
            output = subprocess.run(command, capture_output=True, text=True, timeout=60).stdout.splitlines()
        except (OSError, subprocess.TimeoutExpired):
            continue
        for offset, line in zip(offsets, output):
            resolved[(index, offset)] = line.strip()
    return resolved


def describe(frame, images, resolved):
    index = frame.get("imageIndex")
    image = images[index].get("name", "?") if index is not None and index < len(images) else "?"
    offset = frame.get("imageOffset", 0)
    if (index, offset) in resolved:
        return f"{image}  {resolved[(index, offset)]}"
    symbol = frame.get("symbol")
    if symbol:
        return f"{image}  {symbol} + {frame.get('symbolLocation', 0)}"
    return f"{image}  +0x{offset:x}"


def report(path, body, root):
    print(f"--- crash report {path}")
    exception = body.get("exception", {})
    print(f"exception: {exception.get('type', '?')} {exception.get('signal', '')} {exception.get('subtype', '')}".rstrip())
    for library, messages in (body.get("asi") or {}).items():
        for message in messages:
            print(f"message ({library}): {message}")
    termination = body.get("termination", {})
    if termination.get("indicator"):
        print(f"termination: {termination['indicator']}")
    images = body.get("usedImages", [])
    threads = body.get("threads", [])
    resolved = resolve(images, [frame for thread in threads for frame in thread.get("frames", [])], root)
    for number, thread in enumerate(threads):
        crashed = thread.get("triggered", False)
        title = thread.get("name") or thread.get("queue") or ""
        print(f"thread {number}{' (crashed)' if crashed else ''} {title}".rstrip() + ":")
        frames = thread.get("frames", [])
        shown = frames if crashed else frames[:OTHER_THREAD_FRAMES]
        for position, frame in enumerate(shown):
            print(f"  {position:2} {describe(frame, images, resolved)}")
        if len(shown) < len(frames):
            print(f"  ... {len(frames) - len(shown)} more frames")


def main():
    since = os.path.getmtime(sys.argv[1])
    program = os.path.realpath(sys.argv[2])
    root = os.path.dirname(os.path.dirname(program))
    found = 0
    for directory in REPORT_DIRECTORIES:
        for path in sorted(glob.glob(os.path.join(os.path.expanduser(directory), "*.ips"))):
            if os.path.getmtime(path) < since:
                continue
            _, body = load(path)
            if not body or os.path.realpath(body.get("procPath", "")) != program:
                continue
            report(path, body, root)
            found += 1
    if not found:
        print("no crash report")


if __name__ == "__main__":
    main()
