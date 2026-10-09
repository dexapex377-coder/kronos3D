#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what Blender actually drew on an Android device.

Crash-only tests pass a completely black frame, which is how a shader
compilation failure once shipped in a release build. This grabs the framebuffer
and checks regions of it, either against a stored reference or for being blank.

    # record references once, from a build known to be good
    uicheck.py --serial R52... --save viewport --save preview

    # verify a later build against them
    uicheck.py --serial R52... --check viewport --check preview

    # no reference needed: fail if a region is flat (black or empty)
    uicheck.py --serial R52... --not-blank preview

Regions are fractions of the screen, so the same names work on a phone and a
tablet. Reads the raw framebuffer rather than a PNG, so there is nothing to
install.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

# Fractions of the screen: (x0, y0, x1, y1). Blender on Android is landscape.
REGIONS = {
    # 3D viewport, avoiding the toolbars on every edge.
    "viewport": (0.10, 0.15, 0.60, 0.85),
    # The material preview thumbnail itself. Kept tight: the surrounding panel is
    # full of text and widgets, whose variance masks a black thumbnail.
    "preview": (0.78, 0.50, 0.96, 0.68),
    # Tight on the centre of the viewport, where the default cube sits. A whole
    # viewport diff is weak because the grid dominates it: an object failing to
    # shade at all moves the overall mean by only a few percent.
    "subject": (0.34, 0.35, 0.50, 0.60),
    # Whole screen, for a coarse "did anything draw at all".
    "screen": (0.02, 0.02, 0.98, 0.98),
}

REFERENCE_DIR = Path(__file__).resolve().parent / "uicheck_references"


class Frame:
    def __init__(self, width: int, height: int, pixels: bytes):
        self.width = width
        self.height = height
        self.pixels = pixels

    def region_sharpness(self, rect: tuple[float, float, float, float], rows: int = 240) -> float:
        """Mean absolute luminance gradient between horizontally adjacent pixels.

        Higher means more edge energy, i.e. a crisper image. Must read adjacent
        pixels at full resolution: the subsampled grid used for comparisons
        discards precisely the high frequencies this is measuring. Rows are
        sampled to bound the cost, columns never are.
        """
        x0 = int(rect[0] * self.width)
        x1 = int(rect[2] * self.width)
        y0 = int(rect[1] * self.height)
        y1 = int(rect[3] * self.height)
        step_y = max(1, (y1 - y0) // rows)

        total = 0
        count = 0
        for y in range(y0, y1, step_y):
            row = y * self.width * 4
            previous = None
            for x in range(x0, x1):
                i = row + x * 4
                lum = (self.pixels[i] * 299 + self.pixels[i + 1] * 587 +
                       self.pixels[i + 2] * 114) // 1000
                if previous is not None:
                    total += abs(lum - previous)
                    count += 1
                previous = lum
        return total / count if count else 0.0

    def region_samples(self, rect: tuple[float, float, float, float]) -> list[int]:
        """Luminance samples over a region, on a coarse grid.

        A grid rather than every pixel: enough signal to spot a blank or
        changed region, cheap enough to run after each step of a test.
        """
        x0 = int(rect[0] * self.width)
        x1 = int(rect[2] * self.width)
        y0 = int(rect[1] * self.height)
        y1 = int(rect[3] * self.height)
        step_x = max(1, (x1 - x0) // 64)
        step_y = max(1, (y1 - y0) // 64)

        samples = []
        for y in range(y0, y1, step_y):
            row = y * self.width * 4
            for x in range(x0, x1, step_x):
                i = row + x * 4
                r, g, b = self.pixels[i], self.pixels[i + 1], self.pixels[i + 2]
                samples.append((r * 299 + g * 587 + b * 114) // 1000)
        return samples


def capture(serial: str | None) -> Frame:
    cmd = ["adb"] + (["-s", serial] if serial else []) + ["exec-out", "screencap"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    width, height, _fmt = struct.unpack("<III", raw[:12])
    # Android 9+ adds a colorspace field; detect it from the payload size.
    for header in (12, 16):
        if len(raw) - header == width * height * 4:
            return Frame(width, height, raw[header:])
    raise SystemExit(f"unexpected screencap payload: {len(raw)} bytes for {width}x{height}")


def capture_settled(serial: str | None, region: str, timeout: float) -> Frame:
    """Capture once the image stops changing.

    EEVEE keeps refining the viewport, so a frame grabbed mid-refinement differs
    from the reference for reasons that are not bugs.
    """
    import time

    previous = None
    deadline = time.time() + timeout
    while True:
        frame = capture(serial)
        samples = frame.region_samples(REGIONS[region])
        if previous is not None:
            drift = sum(abs(a - b) for a, b in zip(previous, samples)) / len(samples) / 255.0
            if drift < 0.002:
                return frame
        if time.time() > deadline:
            print(f"note: {region} still changing after {timeout:.0f}s, comparing anyway")
            return frame
        previous = samples
        time.sleep(1.0)


def stats(samples: list[int]) -> tuple[float, float]:
    mean = sum(samples) / len(samples)
    variance = sum((s - mean) ** 2 for s in samples) / len(samples)
    return mean, variance ** 0.5


def split_target(target: str) -> tuple[str, str]:
    """"viewport:solid" -> region "viewport", reference "viewport-solid"."""
    region, _, label = target.partition(":")
    if region not in REGIONS:
        raise SystemExit(f"unknown region {region!r}; known: {', '.join(sorted(REGIONS))}")
    return region, f"{region}-{label}" if label else region


def reference_path(name: str) -> Path:
    return REFERENCE_DIR / f"{name}.txt"


def save_reference(name: str, samples: list[int]) -> None:
    REFERENCE_DIR.mkdir(parents=True, exist_ok=True)
    reference_path(name).write_text(" ".join(str(s) for s in samples))
    mean, sd = stats(samples)
    print(f"saved {name}: {len(samples)} samples, mean={mean:.1f} stddev={sd:.1f}")


def check_reference(name: str, samples: list[int], tolerance: float) -> bool:
    path = reference_path(name)
    if not path.exists():
        print(f"FAIL {name}: no reference, record one with --save {name}")
        return False
    reference = [int(v) for v in path.read_text().split()]
    if len(reference) != len(samples):
        print(f"FAIL {name}: reference has {len(reference)} samples, got {len(samples)} "
              "(screen size or orientation differs)")
        return False

    diff = sum(abs(a - b) for a, b in zip(reference, samples)) / len(samples) / 255.0
    verdict = "ok" if diff <= tolerance else "FAIL"
    print(f"{verdict} {name}: mean difference {diff * 100:.2f}% (tolerance {tolerance * 100:.1f}%)")
    return diff <= tolerance


def check_not_blank(name: str, samples: list[int], min_stddev: float, min_mean: float) -> bool:
    """Flat regions and near-black ones both mean nothing was drawn.

    Mean matters on its own: a black render whose region clips a panel edge still
    has plenty of variance, so stddev alone lets it through.
    """
    mean, sd = stats(samples)
    ok = sd >= min_stddev and mean >= min_mean
    reason = "" if ok else (" [too dark]" if mean < min_mean else " [flat]")
    print(f"{'ok' if ok else 'FAIL'} {name}: mean={mean:.1f} stddev={sd:.1f} "
          f"(needs mean >= {min_mean}, stddev >= {min_stddev}){reason}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("-s", "--serial", help="adb device serial")
    parser.add_argument("--save", action="append", default=[], metavar="REGION",
                        help="record this region as the reference")
    parser.add_argument("--check", action="append", default=[], metavar="REGION",
                        help="compare this region against its reference")
    parser.add_argument("--not-blank", action="append", default=[], metavar="REGION",
                        help="fail if this region is flat, no reference needed")
    parser.add_argument("--tolerance", type=float, default=0.02,
                        help="allowed mean difference for --check (default 2%%)")
    parser.add_argument("--min-stddev", type=float, default=4.0,
                        help="lowest stddev counted as drawn (default 4)")
    parser.add_argument("--min-mean", type=float, default=10.0,
                        help="lowest mean luminance counted as drawn (default 10)")
    parser.add_argument("--settle", type=float, default=0.0, metavar="SECONDS",
                        help="wait for the image to stop changing before comparing")
    parser.add_argument("--sharpness", action="append", default=[], metavar="REGION",
                        help="report edge energy for this region; compare drivers with it")
    args = parser.parse_args()

    if not (args.save or args.check or args.not_blank or args.sharpness):
        parser.error("nothing to do: pass --save, --check, --not-blank or --sharpness")

    targets = [split_target(t) for t in
               args.save + args.check + args.not_blank + args.sharpness]
    if args.settle:
        frame = capture_settled(args.serial, targets[0][0], args.settle)
    else:
        frame = capture(args.serial)
    print(f"framebuffer {frame.width}x{frame.height}")

    ok = True
    for target in args.save:
        region, name = split_target(target)
        save_reference(name, frame.region_samples(REGIONS[region]))
    for target in args.check:
        region, name = split_target(target)
        ok &= check_reference(name, frame.region_samples(REGIONS[region]), args.tolerance)
    for target in args.not_blank:
        region, name = split_target(target)
        ok &= check_not_blank(
            name, frame.region_samples(REGIONS[region]), args.min_stddev, args.min_mean)
    for target in args.sharpness:
        region, name = split_target(target)
        value = frame.region_sharpness(REGIONS[region])
        print(f"sharpness {name}: {value:.3f} mean |gradient| per pixel pair")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
