#!/usr/bin/env python3
"""Checks a snapshot of the feedback-test effect (see effects/feedback-test).

Usage: convert snapshot.png -depth 8 rgb:- | check-feedback.py WIDTH HEIGHT EXPECTED_LINE_Y

Fails if any pixel is magenta (persistent state came back mirrored, shifted or
clamped) or if the green line isn't at EXPECTED_LINE_Y (0..1, from accumulated
uStepDelta) within 0.02.
"""
import sys

w, h, expected = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
data = sys.stdin.buffer.read()
if len(data) != w * h * 3:
    sys.exit(f"expected {w * h * 3} bytes of RGB, got {len(data)}")
magenta = sum(1 for i in range(0, len(data), 3) if data[i] > 200 and data[i + 1] < 80 and data[i + 2] > 200)
x = w // 2
rows = [y for y in range(h) if data[(y * w + x) * 3 + 1] > 200 and data[(y * w + x) * 3] < 120]
line = sum(rows) / len(rows) / h if rows else -1
print(f"feedback-test: {magenta} magenta pixels, line at {line:.3f} (expected {expected:.3f})")
if magenta or abs(line - expected) > 0.02:
    sys.exit("feedback-test: FAILED")
print("feedback-test: passed")
