# shunt

Shapes that slide in from an edge, bank up a set distance apart, stand, and are
drawn away — as **two** FFGL plugins for Resolume Arena/Avenue: a source
(`SW Shunt`) and an effect that masks the clip (`SW Shunt Mask`). For animated
masks, and for chroma animations driving a pixel map. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the queue, the coordinate conventions, the
draw order or the blend state.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build`
- Render a frame offline: `./build/shtest --out /tmp/frame.png --phase 0.62`
- The effect over a test clip: `./build/shtest --effect --out /tmp/mask.png`
- Drive the real clock instead of pinning: `--time 2.0`
- List parameters: `./build/shtest --list`
- Contact sheets: `./build/shtest --shapes /tmp/shapes.png --sides /tmp/sides.png`
- Set anything by name: `--set "Shape=7" --set "Dwell=0.9"`
- A factory preset by number: `--set "Preset=6"` (0 is Custom)
- Film it: `--pipe --size 1920x1080 --fps 30 [--effect] [--script cues.txt]` —
  raw RGBA frames on stdin, raw RGBA on stdout, the fleet's cue-sheet format
  (`frame  Parameter Name  value`). The source reads a frame per frame out as
  its clock. The project video is rendered with it:
  `stoatworks-backend/video/projects/shunt/render.py`.

## Verify
- Everything: `tools/verify.sh`
- The invariant the design rests on: `./build/shtest --tile`
- Where every shape landed, against `Queue.cpp`: `./build/shtest --place`
- The gap, in both units, off the picture: `./build/shtest --gap`
- Where the head stops: `./build/shtest --travel`
- How long a shape stands, in frames: `./build/shtest --dwell`
- Newest on top: `./build/shtest --order`
- Circles stay round off 1:1: `./build/shtest --round`
- The four effect mask modes: `./build/shtest --mask`
- The host clock unit, and a Speed change: `--clock`, `--speed`
- Factory presets against three host behaviours: `--presets`
- ms/frame at 720p, 1080p and 4K: `--cost`
- No dead controls: `python3 tools/sweep.py`
- **Before a release**, the shipped Windows artefact in a real Arena:
  `../plugin-bench/arena/gate.sh shunt` — loads the actual `.dll` on the win-lab
  VM and checks registration, the control surface the host sees, FFGL's 16-char
  name truncation, and that every control still moves the picture. Takes
  minutes, needs the VM, and is deliberately NOT in `tools/verify.sh`. Last run
  2026-09-22 against v0.1.0: 15 passed, 0 failed — see AGENTS.md for why most
  controls come back inconclusive rather than live.

## Browser demo
- `demo/` is served at `shunt-demo.stoatworks-labs.com` by this repo's own
  Worker. `.github/workflows/deploy.yml` deploys it on every push to main that
  touches it, and proves the page changed; by hand it is
  `cf-run npx wrangler deploy` from the repo root. Either way, verify by
  CONTENT, because a stale page answers 200.
- `demo/plugin.js` carries the shader text a second time and ports `Queue.cpp`.
  After editing `source/Shaders.cpp`: `python3 demo/tools/sync_shaders.py`.
  `demo/tools/check_shaders.py` proves the two copies agree and runs in
  `tools/verify.sh`; `sync_shaders.py --check` is the same question without
  writing.
- The ported queue in `demo/plugin.js` is NOT checked by anything. A change to
  `Queue.cpp` has to be made there by hand.

## Notes
- **A shape's place is a pure function of (slot, phase).** No queue object, no
  "has the one in front left yet?", no feedback buffer. That is what makes it
  frame-rate independent, beat-syncable for free, resolution independent and
  testable a frame at a time.
- **The slide speed is derived, not a control**: `(1 + 2m) / (1 - dwell)`. That
  is the whole trick — it makes every shape's run exactly one cycle long
  whatever its slot, so `count` shapes tile the cycle exactly. `--tile` is the
  check; do not "simplify" it away.
- **The queue is C++ only — there is no GLSL mirror.** It runs once per shape,
  not per pixel, so there is one copy of the maths and the harness tests the
  real one. (The browser demo is a second copy, and it is a page rather than the
  plugin.)
- **Two coordinate conventions.** The train runs in frame space (0..1 per axis,
  y down); shapes are sized in short-edge fractions. Same number only at 1:1 —
  `--round` is the test for exactly this.
- **Angle is measured from the direction of travel**, not the frame. Changing
  `From` must not make an operator re-dial it.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
  **Option parameters are the exception** — they hold the element value.
- **No FBO anywhere**, including for the effect's Reveal and Hide. Sidesteps two
  SDK bugs; see `AGENTS.md`.
- `shunt_core` is an **OBJECT** library, and each plugin's registration is
  listed directly in its own target — see `AGENTS.md`.
- The GLSL declares `Xform[64]` as a literal; a `static_assert` keeps
  `kMaxShapes` in step.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that all look identical
from outside ("it does nothing"): a shader that will not compile, and shape
uniform arrays that did not resolve.

    ~/Library/Logs/shunt/shunt.YYYY-MM-DD.log
