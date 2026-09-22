# shunt — working notes

Decisions already made, traps that have actually bitten, and what is honestly
still unknown. `AGENTS.md` is the orientation; this is the log.

## Status, 2026-09-22

v0.1.0. `tools/verify.sh` green on an Apple M4 Max, macOS 26.4.1: universal
build, both bundles export `plugMain` and ad-hoc sign, 92 slot runs tile the
cycle, 27 shapes within 1.5 px of prediction, the gap and the travel measured
off rendered profiles to within a pixel, the dwell measured in frames, the draw
order measured by colour, 33 controls all live, and `oxbow` loads both bundles
and renders 120 frames with `gl error 0x0`.

**In Resolume Arena 7.27.1 on 2026-09-22**, the shipped Windows DLLs, via the
fleet's Arena gate on win-lab: 15 passed, 0 failed, 0 skipped. Registration,
all 38 controls per plugin against the declarations, the four 16-character names
complete, a rendered frame from each, a clean log. 13 and 10 controls proven live
in-host, the rest inconclusive against the animation's own motion — see
`AGENTS.md`. On llvmpipe, so nothing about a GPU.

Still unverified: a GPU, macOS Arena, a show, a person reading the inspector,
Bar sync against a real transport, and there is no OpenFX build. All of that is
in the README's Status rather than buried here.

## The first Arena run, 2026-09-22

- The expectation was seeded from the running Arena and then reviewed against
  `shtest --list` — name, order, type, default, range, option element — with a
  script rather than by eye: zero discrepancies on either plugin. That review is
  what makes a seeded file a gate rather than a snapshot of whatever shipped.
- The mask's `Opacity` comes first in Arena. Host behaviour, not drift: Orrery
  Mask's expectation records the same.
- The gate takes most of an hour, nearly all of it in the per-control pass —
  each control is mounted, moved, captured and compared over REST, and the
  capture is a software render.

## The project video, 2026-09-22

Rendered, not filmed — `stoatworks-backend/video/projects/shunt/`, through the
new `shtest --pipe` (galvo's pipe format and cue sheets, unchanged). Two passes,
because it is two plugins: the source for the whole piece over a black stream
that is only its clock, the mask for its one beat over Resolume's
IntoTheGlow_02, cut together without compositing. The cue sheet is GENERATED
from a table of what each beat IS, with a hold key at every beat's end for every
parameter and a check that reads the sheet back through the harness's own
interpolation rule before a frame is rendered — the two hand-written-cue traps
that cost galvo and vertigo a re-render cannot happen. The ladder beat's claim is
measured off the take itself: 72.00 px, worst error 0.00.

Found while working out that beat's geometry: the `Presets.h` comment said
`Count` 0.408 is twelve shapes. It is eleven — `1 + round(63 × 0.408²)` — and the
rounding step is narrow there (0.415 is twelve). The comment is corrected; the
preset values are unchanged, because changing one would change what an existing
composition renders.

## Where it came from

A feature request from a Resolume operator, through the site's contact form on
2026-09-09, answered on 2026-09-22. The description is in `AGENTS.md`,
paraphrased: **the request itself is private correspondence and is not quoted
publicly anywhere in this repo, and the person who sent it is not named.** If
they would like the credit, that is theirs to give and the README can carry it
afterwards.

Two things were settled in the reply and are not open questions:

- **Newest draws on top** — "the leading edge of #2 slides over the trailing
  edge of #1" only reads one way.
- **The departure is one continuous dial, not two modes.** See `AGENTS.md`.

Two were asked for in both units and got both: the **gap** is offerable as a
multiple of the shape's own thickness *and* as absolute pixels.

## Decisions

### Dwell is a fraction of the cycle, not a time in milliseconds

The request named a time — "a selectable delay e.g. 500ms". It is a fraction
here, and the trade is worth writing down because it is the one place the
plugin does not do literally what was asked.

A dwell in seconds has to be converted into a fraction of the cycle before it
can be used, because the slide speed is derived from that fraction. That
conversion needs a cycles-per-second, which exists under **Free** sync, can be
recovered under **Beat** and **Bar** from the host's tempo — and does not exist
at all under **Manual**, where the operator is driving Phase by hand from a
keyframe or a fader. It would also have to be clamped whenever
`dwell × rate ≥ 1`, which means the plugin would silently hold for less time
than the number on the slider said.

A fraction is exact, means the same thing in all four sync modes, never clamps
and never lies. The millisecond figure is a division:

    dwell in seconds = Dwell ÷ Speed

so Dwell 50% at Speed 1.0 cycles/s is 500 ms, and the user guide says so with a
worked table rather than leaving it as arithmetic.

If this turns out to be the wrong call in practice, the honest fix is a second
option parameter — `Dwell Units: Cycle / Seconds` — matching how `Gap Units`
already works, and not a silent clamp.

### Count is a control, not derived

The number of slots that *fit* between the entry edge and the head is
`floor(Travel / Gap) + 1`, and deriving `Count` from it would be very much the
house style — the artefact falling out of the model. It is a control anyway,
because the operator needs to be able to say how dense the procession is
independently of how far it runs, and because a derived count would jump as Gap
crosses a boundary.

The consequence is that a queue can be longer than the run it has to stand in.
It degrades rather than breaking: `SlotDepth` clamps at the off-stage margin, so
the surplus shapes wait off-frame and then cross the whole width, and the
one-cycle invariant still holds for them because the entry they lose is exactly
the exit they gain. `--tile`'s "queue overruns" case is 40 shapes in a run that
holds about 9.

### No audio, no Size Variation

Both would modulate the shape's size, and a Gap in thicknesses is a function of
that size — so the headline claim, that standing shapes sit exactly one Gap
apart, would quietly stop being true. The train is uniform by design. See
`AGENTS.md`, "Things deliberately not done".

## Traps that bit during the build

- **`PT_COUNT` was the enum terminal and the shape-count parameter at once.**
  The error lands forty lines away, as a redefinition plus a `static_assert`
  about the About block. The count is `PT_WAGONS` now.
- **The first default looked like a picket fence.** Bar at a quarter turn is a
  *rung*, and a rung's thickness along the direction of travel is 0.15 of its
  radius — so a gap of 0.68 thicknesses put eight shapes inside 5% of the frame
  and the train read as one fat blob. The maths was right and the defaults were
  wrong; they are discs now, and the `Caterpillar` preset is those same defaults.
- **Measuring the dwell in a full train measured the wrong thing.** The head's
  centroid window is crossed by every other shape in turn, so "did it move?" was
  answering about the crossings: 92 frames of an expected 120. Cut back to one
  shape, it reads 119 of 120. That the dwell is the same for every slot is
  `--tile`'s job, without a renderer in the way.
- **Bucketing ages to prove the tiling measured float rounding instead.** The
  ages sit exactly *on* the boundaries of any `Count`-wide bucketing whenever
  `phase × Count` is near a whole number, so at Count 40 two shapes landed in
  one bucket. Sorted ages and the gaps between them say the same thing and
  cannot straddle anything.
- **The end-of-run depth check needed the slide speed in it.** Probing at
  `age = 1 − 1e-4` is a hair before fully off-stage, and at Dwell 95% the slide
  speed is high enough that the hair is worth several thousandths of the frame.
  Predict it (`1 + m − v·ε`) rather than loosening the tolerance to hide it.

## Things worth checking the first time it is in Arena

1. How the seven groups — Shape, Queue, Timing, Colour, Shading, Output, Preset
   — land in the inspector. Seven is more than any sibling.
2. Whether `Bar` sync locks against a real transport.
3. Whether **Gap Units** reads sensibly as a dropdown sitting immediately above
   the slider whose meaning it changes, or whether the two want swapping.
4. Whether `Travel` at 100% with a long queue is a useful place to be or just
   confusing.
