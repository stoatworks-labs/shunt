#!/usr/bin/env python3
"""Move every parameter and fail if any of them made no difference to the frame.

**This is the only thing in the repo that catches a dead control**, and it is
not a theoretical risk. A GLSL uniform whose name does not match the C++ is
silently ignored -- `glGetUniformLocation` returns -1 and `glUniform` on -1 is a
documented no-op -- so a slider can be stone dead while everything compiles,
links, loads and renders.

None of `shtest`'s measurements will catch it. `--place`, `--gap` and `--travel`
check where shapes landed, and a uniform that is never set still lands them
somewhere consistent; `--mask` only ever exercises one setting of everything
else.

## Why there is a context table

Most of these parameters are *supposed* to do nothing in the default
configuration, and a naive sweep would report a dozen false failures:

- `Angle` and `Roundness` are invisible on a **circle** -- a rotated circle is
  the same circle, and a circle has no corners to round.
- `Colour` is ignored unless `Colour Mode` is off White, and White and Solid are
  identical while the swatch is still white.
- `Hue Spread` needs a hue mode.
- `Light` has nothing to light with `Shade` at zero.
- `Blend` needs shapes that **overlap** at partial opacity: over an opaque black
  background, separated white shapes are white in all three modes.
- `Speed` and `Sync` need the real clock, which is what `--time` is for.
- A shadow is black, so it needs a background that is not.
- Every Image control needs an image loaded, which this script writes itself.

So each entry below says what else has to be true for the parameter to be
capable of doing anything. Getting one of these wrong shows up as a failure, not
as a silent pass.

## The trap this script used to have

Up to 0.1.0 `Mix` and `Mask Mode` were listed as EFFECT_ONLY: the source
declared them and ignored them, and the sweep was told not to look. It was
right that they were dead and wrong to excuse it -- an operator found them
within an hour and reported them as broken. Nothing is excused any more: a
parameter either moves the picture in both plugins or it is not declared.

## Two traps in the sweep VALUES

**Phase is periodic with period 1 by construction**, so 0 and 1 are the same
frame. Three of the four values below differ from each other, so a working Phase
still reports live -- but a two-value sweep of 0 and 1 would call it dead.

**A gap of zero makes Phase periodic with period 1/count as well.** With every
shape standing at the same depth, advancing the phase by 1/count maps each shape
onto where its neighbour was and the frame comes back pixel-identical. That is
why `BASE` pins a non-zero Gap rather than leaving it at the default: it would
otherwise be one edit away from a very convincing false alarm.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose]
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Applied to every render: a full train, large enough to see, banked up far
# enough into the frame that there is something on both sides of the queue.
BASE = ["Count=0.45", "Size=0.5", "Gap=0.2", "Travel=0.8", "Dwell=0.5"]

# Parameters that mean something different in the two bundles, so each is swept
# against both: Mask Mode masks the clip in the effect and draws a matte in the
# source, and Mix fades against the clip in one and to transparent in the other.
BOTH = {"Mix", "Mask Mode"}

# Parameters the effect ignores, so they are only ever swept against the source.
SOURCE_ONLY = {
    "Background",
    "Background_Green",
    "Background_Blue",
    "Background Alpha",
}

# What else has to be true for a parameter to be able to do anything.
CONTEXT = {
    # Invisible on a circle.
    "Angle":        ["Shape=7"],                   # Bar: it has an orientation
    "Roundness":    ["Shape=1"],                   # Square: it has corners

    # Colour is ignored on White, and White and Solid are identical while the
    # swatch is still white.
    "Colour Mode":  ["Colour_Green=0.0", "Colour_Blue=0.0"],
    "Colour":       ["Colour Mode=1"],
    "Colour_Green": ["Colour Mode=1"],
    "Colour_Blue":  ["Colour Mode=1"],
    "Hue Spread":   ["Colour Mode=2"],             # Hue Spread

    # The light has nothing to light with the shading turned off.
    "Light":        ["Shade=1.0"],

    # Over, Add and Max are the same picture unless shapes overlap at less than
    # full opacity. Half a thickness of gap guarantees the overlap.
    "Blend":        ["Gap=0.125", "Opacity=0.5"],

    # Thickness and Pixels only differ if there is a gap to measure, and the
    # shape has to be big enough that one thickness is not one pixel.
    "Gap Units":    ["Gap=0.25", "Size=0.7"],

    # Lane Step only acts when Lanes is Step.
    "Lane Step":    ["Lanes=1"],

    # A black shadow on the default black background is invisible.
    "Shadow":          ["Background=0.5"],
    "Shadow Distance": ["Shadow=1.0", "Background=0.5"],
    "Shadow Blur":     ["Shadow=1.0", "Background=0.5"],

    # The grid and the pick only mean something to a sheet.
    "Columns":      ["Image From=2", "Rows=2"],
    "Rows":         ["Image From=2", "Columns=4"],
    "Pick":         ["Image From=2", "Columns=4", "Rows=2"],
    "Sprite":       ["Image From=2", "Columns=4", "Rows=2"],
}

# Parameters that need an image loaded. The sheet is written into a folder of
# other images, so Image From can tell Single, Folder and Sprite Sheet apart.
NEEDS_IMAGE = {"Image From", "Columns", "Rows", "Pick", "Sprite", "Image Mix"}

# Integer parameters hold real values, not 0..1.
INTEGER_VALUES = {
    "Columns": [1.0, 2.0, 4.0],
    "Rows": [1.0, 2.0],
    "Sprite": [0.0, 1.0, 2.0, 5.0],
}

# Parameters that need the host clock running rather than a pinned phase.
NEEDS_CLOCK = {"Speed", "Sync"}

# The values every non-option parameter is swept across. The awkward numbers are
# deliberate -- see the note at the top about Phase.
SWEEP_VALUES = [0.0, 0.137, 0.611, 1.0]

# Option parameters are swept across their elements instead.
OPTION_RANGE = {
    "Shape": 8,
    "From": 4,
    "Gap Units": 2,
    "Sync": 4,
    "Colour Mode": 4,
    "Blend": 3,
    "Mask Mode": 4,
    "Preset": 8,
    "Lanes": 3,
    "Image From": 3,
    "Pick": 3,
}


def write_png(path, width, height, pixel):
    """A PNG from a function of (x, y) -> (r, g, b). Enough of the format for
    stb_image to read: 8-bit RGB, one IDAT, filter 0 on every row."""
    rows = b"".join(
        b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width))
        for y in range(height))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows))
                     + chunk(b"IEND", b""))


def make_images(tmp):
    """A 4x2 sheet of eight flat colours, in a folder beside two other images."""
    folder = tmp / "images"
    folder.mkdir()
    palette = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
               (255, 0, 255), (0, 255, 255), (255, 128, 0), (128, 0, 255)]
    sheet = folder / "b_sheet.png"
    write_png(sheet, 64, 32, lambda x, y: palette[(y // 16) * 4 + x // 16])
    write_png(folder / "a_red.png", 8, 8, lambda x, y: (200, 30, 30))
    write_png(folder / "c_blue.png", 8, 8, lambda x, y: (30, 30, 200))
    return sheet


def read_png(path):
    """Decode a PNG to raw bytes. Enough of the format for our own writer's
    output -- 8-bit RGBA, one IDAT, filter 0 on every row."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IDAT":
            idat += body
        pos += 12 + length

    return zlib.decompress(idat)


def render(shtest, out, settings, effect, clock, verbose, files=()):
    args = [str(shtest), "--out", str(out), "--size", "480x270"]
    for f in files:
        args += ["--file", f]
    if effect:
        args.append("--effect")
    if clock:
        args += ["--time", "2.0"]
    else:
        args += ["--phase", "0.37"]
    for setting in settings:
        args += ["--set", setting]

    if verbose:
        print("   ", " ".join(args))

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"shtest failed: {result.stderr.strip()}")

    return read_png(out)


def parameters(shtest, effect):
    """Name and kind of every parameter, in declaration order."""
    args = [str(shtest), "--list"]
    if effect:
        args.append("--effect")

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"shtest --list failed: {result.stderr.strip()}")

    found = []
    for line in result.stdout.splitlines()[1:]:
        # id, name (may contain spaces), type, default
        parts = line.split()
        if len(parts) < 3:
            continue
        kind = parts[-2]
        name = " ".join(parts[1:-2])
        found.append((name, kind))

    # The About block is a text field and browser buttons, declared last. They
    # never touch a pixel, so sweeping them only buries a real dead control.
    for i, entry in enumerate(found):
        if entry[0] == "About":
            return found[:i]

    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    shtest = REPO / args.build / "shtest"
    if not shtest.exists():
        print(f"no shtest at {shtest} -- build first", file=sys.stderr)
        return 2

    dead = []
    checked = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        sheet = make_images(tmp)

        for effect in (False, True):
            for name, kind in parameters(shtest, effect):
                # The effect shares every control with the source, so it is
                # swept only for the ones that mean something different there.
                if effect and name not in BOTH:
                    continue
                if effect and name in SOURCE_ONLY:
                    continue

                context = CONTEXT.get(name, [])
                clock = name in NEEDS_CLOCK
                base = BASE + context
                files = [f"Image={sheet}"] if name in NEEDS_IMAGE else []

                frames = []
                out = tmp / "sweep.png"

                if kind == "file":
                    # A file parameter is swept as "none" against "one".
                    for chosen in ([], [f"{name}={sheet}"]):
                        frames.append(render(shtest, out, base, effect, clock,
                                             args.verbose, chosen))
                else:
                    if name in OPTION_RANGE:
                        values = [float(i) for i in range(OPTION_RANGE[name])]
                    elif name in INTEGER_VALUES:
                        values = INTEGER_VALUES[name]
                    else:
                        values = SWEEP_VALUES

                    for value in values:
                        frames.append(
                            render(shtest, out, base + [f"{name}={value}"],
                                   effect, clock, args.verbose, files))

                checked += 1
                if all(f == frames[0] for f in frames[1:]):
                    where = "effect" if effect else "source"
                    dead.append(f"{name} ({kind}, {where})")
                    print(f"  DEAD {name}")
                elif args.verbose:
                    print(f"  ok   {name}")

    print()
    if dead:
        print(f"sweep: {checked} parameters, {len(dead)} made no difference:")
        for entry in dead:
            print(f"  - {entry}")
        return 1

    print(f"sweep: {checked} parameters, all live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
