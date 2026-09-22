# shunt — orientation for another LLM (or a newcomer)

**What it is:** shapes that slide in from an edge, bank up a set distance apart,
stand, and are drawn away — as **two** FFGL 2.1 plugins for Resolume
Arena/Avenue. `SW Shunt` is a source that draws the train over its own
background; `SW Shunt Mask` is an effect that draws it over, or cuts it into,
the incoming clip. C++17 + GLSL 4.1, CMake, universal macOS `.bundle` and a
Windows `.dll`. Public, MIT, `github.com/stoatworks-labs/shunt`.

It exists because a Resolume operator asked for it, and the request is worth
keeping in view because every decision below either came straight out of it or
was one it left open:

- pick a shape, pick an edge; it slides in that far and stops for a moment;
- the next one comes in behind it and stops a set distance back;
- they bank up, overlapping, until the first one's wait is over and it carries
  on off the far side — then the next, and the next.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the queue, the coordinate conventions, the draw
order, or the blend state.

---

## The one idea

**A shape's place is a pure function of (slot, phase).**

There is no simulation state anywhere: no queue object that shapes are pushed
onto and popped off, no "has the one in front left yet?" test, no previous
position, no feedback texture. Ask for the picture at phase 91.7 and you get it
without having played the preceding 91.7.

Four things follow, and all four are the reason it is written this way:

- **It cannot drift.** Integrate a velocity a frame at a time and the speed of
  the procession is whatever the host's frame rate happened to be — and
  Resolume's frame rate drops when the show gets heavy. For a generator whose
  output is driving a lighting rig, a chase that slows down when the projection
  load goes up is not a cosmetic problem, it is the lights coming apart from the
  music.
- **Beat sync is free rather than bolted on.** Phase is just a number.
- **Any frame renders on its own.** `shtest` renders phase 3.137 cold. Nearly
  every check depends on it.
- **Resolution independence is free rather than fought for.**

### The trick that makes it possible

A queue is the *obvious* place to need state, and it is not obvious that this
can be a closed form at all. It can, because of one choice:

**The slide speed is derived, not a control.**

    speed = ( 1 + 2 * margin ) / ( 1 - dwell )      // frame-spans per cycle

A shape's run is enter, stand, leave. Its entry run is short if it stands near
the back of the queue and long if it stands at the head — but its exit run is
longer by *exactly* as much, because the two together always cross the whole
frame plus the off-stage margin at each end. So `(entry + exit) / speed` is the
same number for every slot whatever `Travel` and `Gap` say, and with the speed
above that number is `1 - dwell`. Every shape's run is therefore

    ( 1 - dwell ) + dwell  ==  exactly one cycle

and with `Count` shapes released one per `1/Count` of a cycle, the runs **tile
the cycle exactly**. There are always exactly `Count` shapes in play, never a
hole in the procession, and never two shapes wanting one slot — with nothing
keeping track of any of it.

`shtest --tile` is that invariant, stated as a test: 92 slot runs across nine
configurations, including a queue of 40 that overruns the frame and a gap of
zero. It needs no GL. **If you find yourself wanting to make the slide speed a
control, this is what you are trading away**, and the honest replacement is a
real queue with real state, which is a different plugin.

### Dwell is one dial, and it was nearly two modes

The behaviour that needed a decision is what happens when the front of the queue
is leaving while the back is still arriving. Two modes — *stack up, then
release* and *keep moving* — would have been the easy shape, and wrong: they are
the same mechanism at the two ends of one slider.

- **Low dwell** → low slide speed → a shape spends most of its cycle crossing,
  so the head is still leaving when the fourth one arrives.
- **High dwell** → high slide speed → the whole train is standing before the
  head moves at all.

Nothing in the code branches on it. `shtest --dwell` measures both ends off
rendered frames and also asserts the behavioural claim directly: at 10% there is
no phase at which the whole train stands, and there is always one shape leaving
while another arrives; at 90% it is the other way round.

---

## The traps

Ordered by how much time they will cost you.

**Two coordinate conventions, and mixing them up is invisible at 1:1.** The
train runs in **frame space** — 0..1 across the raster on each axis, y down — so
`Travel` at 1.0 reaches the far edge of a 16:9 frame instead of stopping short,
which is what you want when the frame *is* the LED rig. Shapes are sized in
**short-edge fractions**, so a circle is round. Those are the same number only
on a square render. Get it wrong and a square test looks perfect while every
real output draws ellipses. `shtest --round` exists solely for this, and it
checks both the roundness *and* the size — because sizing off the wrong edge
gives a perfectly round circle of entirely the wrong diameter, which the
roundness test alone would pass.

**`ShapeBound` and `ShapeHalfExtents` are different numbers and they are not
interchangeable.** The bound is padded, because it sizes the quad and erring low
silently cuts a corner off a rotated shape. The half extents are the shape's
*true* axis-aligned extents, because they are what the Gap control means by
"thickness" — and a gap of one thickness has to put two shapes edge to edge.
Use the padded one there and "touching" leaves a visible space while the test
still passes, because the test would be measuring the same wrong number.

**The triangle's half extent is 0.75, not 1.** It reaches 1 above its centre and
0.5 below, so its full height is 1.5. Spacing is about the *full* extent however
lopsided the shape is about its own centre, which is why `ShapeHalfExtents`
returns half the full extent rather than the reach in each direction.

**The draw order rotates, and a fixed one passes a single-phase test.** "Newest
on top" is the whole of "the leading edge of #2 slides over the trailing edge of
#1", and which slot is newest turns over once per release. Sorting the wagons by
descending age is what does it. A version that drew slot 0 last would look
correct at one phase in `Count` and wrong for the rest of every cycle — so
`shtest --order` deliberately sweeps five phases, and checks the rendered
colour in the overlap rather than the order of the array.

**`Blend` Max throws the draw order away, and that is correct.** Channel-wise
maximum is commutative, so under Max there is no "on top" at all. It is the
right trade for a mask, where two overlapping white shapes should stay white,
and the wrong one for a caterpillar. Hence **Over** is the default, and
`--order` sets Blend explicitly rather than relying on it.

**`sdStar5` comes out positive inside.** Every other distance function here is
negative inside, and everything downstream — the outline's `abs()`, the
feather's `smoothstep`, the coverage — assumes it. Left alone, the star renders
as a solid quad with a star-shaped hole in it: a striking picture, and
completely wrong. It is negated at the end of the function. Inherited from
orrery, where it was caught by *looking at* a contact sheet rather than by any
assertion — which is why `shtest --shapes` is worth regenerating whenever a
shape changes.

**A gap of zero makes Phase periodic with period 1/Count.** With every shape
standing at the same depth, advancing the phase by 1/Count maps each shape onto
where its neighbour was and the frame comes back **pixel-identical** — so a
sweep would report a perfectly working Phase slider as dead. `tools/sweep.py`
pins a non-zero Gap in its base settings and says why. (Phase is also periodic
with period 1 by construction, so 0 and 1 are always the same frame.)

**A centroid cannot measure a shape that overlaps another one**, and in this
plugin they overlap on purpose. `--place` skips shapes that are within three
radii of another or straddling a frame edge, and *prints the count it skipped*;
it fails outright if fewer than twenty survive across the whole run. The
overlapping cases are covered by `--gap` (a profile along the travel axis, which
does not care) and `--order`. Measuring the head's dwell in a full train read
92 frames of an expected 120 for exactly this reason, before that check was cut
back to a single shape — and the per-slot equality it gave up is `--tile`'s job
instead.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. There is no `SetParamDefault`. So every host
parameter here is 0..1 and the conversions live in `Controls.cpp`. A default gap
of 128 px would silently become 1.

**Option parameters do NOT hold 0..1.** They hold the element value the operator
chose — 0, 1, 2… — so they are read through `Option()`, which rounds and clamps.
A stale composition naming an element that no longer exists is the reason for
the clamp.

**`PT_COUNT` is the enum terminal, so the shape count is `PT_WAGONS`.** Naming
the "how many shapes" parameter `PT_COUNT` compiles into a redefinition error
forty lines away and a `static_assert` about the About block. It cost a build.

**There is no FBO anywhere, on purpose.** Every mode — including the effect's
Reveal and Hide, which look like they need a mask buffer — is reachable with a
background pass and a blend function. Reveal samples the clip *inside the shape
fragment*; Hide draws the clip and punches the shapes out with
`glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_ALPHA )`. Reaching for an FBO is the
obvious move and walks straight into two SDK bugs: `FFGLFBO::Release` leaks its
colour texture, and `FFGLFBO::Initialise` allocates under a
`ScopedTextureBinding` whose destructor **clears the binding to 0 rather than
restoring it**, so allocating a buffer silently unbinds the input texture for
exactly the frames on which it was allocated. Shunt never allocates one.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** Which is why the render path uses plain `glUseProgram` and
`glBindTexture` and puts the state back by hand at the end.

**The quad is the only thing being rasterised, so `ShapeBound` is load-bearing.**
A shape that reaches past its own quad is not clipped in a way that looks like
clipping — it loses a corner, and a lost corner on a rotated square reads as the
shape *wobbling*.

**The antialiasing width comes from the distance *before* the outline's
`abs()`.** `abs()` creases the field at d = 0, which is the centre line of the
stroke, and `fwidth()` of a crease spikes — putting a dark seam straight down
the middle of every outline.

**`FFGLShader::Set` has no integer-vector overload and reaching for the float one
is silent.** `Set( name, someInt, someInt )` against an `ivec2` resolves to the
`(float,float)` overload and issues a `glUniform2f` against an integer uniform:
a `GL_INVALID_OPERATION` that leaves the uniform at zero with nothing anywhere
the plugin can see. Every uniform here is `float`, `vec2`, `vec3`, `vec4` or a
single `int`. The shape arrays go through a raw `glUniform4fv` on a
`FindUniform` location.

**The GLSL declares `Xform[64]` as a literal**, because the shader is a plain
string. `Shunt.cpp` carries a `static_assert` that `kMaxShapes` is still 64, so
raising one without the other is a build error rather than a uniform-array
overrun.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit — giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `shunt_core` is an **OBJECT** library, and
`SourcePlugin.cpp` / `EffectPlugin.cpp` are listed **directly** in their own
`MODULE` targets. Putting either in the shared library would register both
plugins into both bundles.

**Beat sync recovers a bar count without keeping one.** The host gives a tempo
and a position *within* the current bar, and never says which bar. A counter
would be state. So: the clock estimates how many bars have passed, `barPhase`
gives the exact position inside one, and `round( estimate − barPhase )` is the
integer that reconciles them. Continuous across the bar line, and exact even if
the clock estimate is off by up to half a bar.

**The host's time unit is measured, not guessed.** Resolume sends milliseconds;
the offline harness sends seconds; FFGL never says which. Guessing from the
magnitude of one frame delta has three holes — a delta between 0.5 and 2.0
decides nothing, a burst of sub-millisecond frames at load locks it wrongly for
the session, and while undecided it has to assume something. So `steady_clock`
says how much real time passed, the host says how much host time passed, and the
ratio names the unit. Carried in from orrery, where the guess was a bug reported
from the field.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that will not compile surfaces only at runtime, as
"the plugin does nothing". That is what `Diag` is for.

---

## Checking your work

`tools/verify.sh` runs the lot. The ones that matter check different things:

- **`--tile`** is the invariant, with no GL involved. Everything else in the
  repo assumes it.
- **`--place`** renders real frames and measures where every measurable shape
  actually landed against what `Queue.cpp` said, to within 1.5 px, across four
  entry sides and five aspect ratios. It exercises the solver, the uniform
  upload, the vertex transform, the aspect correction, the distance function and
  the blend **at once**, and it catches things a mirror test structurally cannot,
  such as the shape array being uploaded off by one. It measures inside a window
  centred on the *prediction*, so a shape that is somewhere else registers as an
  empty window and fails — deliberately, because a nearest-blob search would
  assume the very thing under test.
- **`--gap`** is the request's own units, made measurable. It collapses the
  frame onto the travel axis and reads the lit runs off the profile, because
  "these two are touching" has no centroid answer at all and two shapes a third
  of a thickness apart have overlapping centroid windows. Bars turned across the
  track, because a rung's profile is flat and a threshold crossing is therefore
  its actual edge.
- **`--order`** is the one claim that is about compositing rather than geometry.
- **`--round`** is the two-coordinate-conventions trap, above.
- **`--mask`** checks each of the four effect modes on the picture, inside a
  shape and outside it. Its reference clip is *captured* by rendering at zero
  opacity rather than predicted, because predicting it would mean
  reimplementing the UV flip in the test, and a test that reimplements what it
  tests agrees with its own mistakes. Colourise is swept against a **red** shape
  colour, because against white it is arithmetically identical to Reveal and
  would pass whether or not the multiply happened.
- **`sweep.py`** is the only thing that catches a dead control.
- **`oxbow`** is the only thing that runs the BUNDLE rather than the class.

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from a
session is unreliable. **Nothing in this repo has been loaded into Resolume**,
and the two things most worth checking there are how the seven parameter groups
land in the inspector and whether Bar sync actually locks against a real
transport.

---

## Things deliberately not done

- **No OpenFX build.** The queue is plain C++ and would port; the per-pixel work
  is eight distance functions and the shading, which would have to be mirrored
  on the CPU the way orrery mirrors them. Not done, so there is nothing here for
  Resolve, Nuke or Vegas.
- **No audio.** Orrery gives every shape its own slice of the spectrum, and the
  same code would drop in. It is left out because the headline claim here is
  that standing shapes sit exactly one Gap apart — and a Gap measured in
  thicknesses is a function of the shape's size, so modulating size with audio
  would quietly stop that being true. `Size Variation` is absent for the same
  reason: the train is uniform by design.
- **No lanes.** Several parallel trains would be genuinely useful for a pixel
  map, and it would mean splitting `Count` between lanes — which breaks the
  tiling the whole design rests on, because each lane would need its own
  release cadence. It is a second instance on a second layer today.
- **No per-shape shape.** One primitive per plugin instance, as in orrery.
- **No continuous entry angle.** Four sides, because the request said "the side
  you choose" and because `Travel` as a percentage and `Gap` in pixels are both
  unambiguous along an axis and would need defining along a diagonal.
- **Motion blur and trails** need history, and history is the one thing this
  design does not have.

Related: [orrery](https://github.com/stoatworks-labs/orrery) — the closest
relative, and where the eight primitives, their distance functions, the shading,
the About block, `Diag` and the CMake shape all came from. Also downpour,
old-cathode, porthole.

## Factory presets

### The host owns the parameters, and a preset has to know that

Reported against **vertigo** as its issue #2 and fixed across the fleet on
2026-08-22: choosing a factory preset in Resolume did nothing and the dropdown
snapped straight back to `Custom`. This repo carries that fix from the start
rather than rediscovering it.

The naive pattern is copy-based — `applyPreset` writes the values into `params[]`
and raises `FF_EVENT_FLAG_VALUE` so the host re-reads its sliders — and it rests
on an assumption FFGL never makes. **The host owns parameter state.** It pushes
its own values back down whenever it likes, and nothing obliges it to act on a
value event. Resolume does not: it carries on restating the values it still
believes in, which are the ones from before the preset. Those restatements
arrive as `SetFloatParameter` calls carrying a changed value, so the rule "a
covered parameter changed, therefore the operator has taken over" fires on the
host's own echo, instantly, every time.

Three things arrive through that one call while a preset is active, and only the
third is a person:

| What arrives | How it is recognised | What happens |
|---|---|---|
| the preset's own values, from a host that honoured the events | matches the preset | ignored — nothing to write |
| the values from *before* the preset, from a host that did not | matches `hostValues[]`, the host's own last word | ignored — writing it would undo the preset |
| a new value from neither | matches neither | written, and the preset falls back to Custom |

`hostValues[]` is the record of what the **host** last sent, which is not what
the plugin is rendering with, and `seedHostValues()` fills it from the defaults
on the first parameter traffic — **before `applyPreset` can run**. Seeding it
afterwards records the preset's own values as the host's opening position, so
the host's very next restatement looks like an edit.

Two tolerances matter and they are not the same number. `kSame` is **1e-3**, a
host-quantisation allowance rather than a float epsilon — a host that keeps its
parameters shorter than a float hands back a number *near* ours. The "did a
covered parameter move?" test works to 1e-4, which is why a value matching the
preset is **ignored rather than written**: letting a rounded copy of our own
value into `params[]` would trip that tighter test.

`shtest --presets` drives all three hosts across every preset, with no GL
involved, and runs in `tools/verify.sh`.

`source/Presets.h` is one table of named looks in the host-facing 0..1
parameter space. Element 0 of the dropdown is always **Custom**, which is not in
the table: it means "the sliders are the truth". **Caterpillar is the plugin's
own defaults, named** — keep the two in step, or picking it will change the
picture on a fresh instance.

A preset covers the shape, the queue geometry and the colour. It leaves alone
Sync and Phase (the operator's driver, often keyed), Across (framing), **Gap
Units** (an operator who has switched to pixels to match a pixel map does not
want a preset switching them back, and the same number means wildly different
things in the two units), Mask Mode and Mix.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
