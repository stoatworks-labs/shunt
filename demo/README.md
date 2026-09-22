# demo/ — the browser demo

Live at **https://shunt-demo.stoatworks-labs.com**, linked from the
[project page](https://stoatworks-labs.com/software/shunt/) and from the
[video plugins page](https://stoatworks-labs.com/video-plugins/).

**This is not the plugin.** It is the GLSL from [`source/Shaders.cpp`](../source/Shaders.cpp),
copied across unedited — `#ifdef SHUNT_EFFECT` branches included — and run in
WebGL2 with the parameters the plugin's constructor declares. Both bundles are
here: the picker in the transport bar compiles the shaders twice, once each way,
exactly as the two plugins do.

## What is copied and what is ported

- **The four shaders are copied, byte for byte.** `demo/tools/sync_shaders.py`
  writes them into `plugin.js` from `source/Shaders.cpp`;
  `demo/tools/check_shaders.py` proves the two copies still agree and runs in
  `tools/verify.sh`. Edit the C++ and re-run the sync — never the other way
  round.
- **`Queue.cpp` is ported by hand**, with `Shapes.cpp`'s extents and bounds and
  `Controls.cpp`'s 0..1 conversions. It runs on the CPU in the plugin and it
  runs on the CPU here, so it survives the trip intact: **a shape's place is a
  pure function of (slot, phase)**, which is why Step on this page is exact
  rather than approximate, and why dragging the transport backwards shows the
  same picture it showed on the way past.

  **Nothing checks that port.** A change to `Queue.cpp` has to be made here by
  hand, and the only thing that will tell you it was not is the page behaving
  unlike the plugin.

## What this page cannot reproduce

Listed on the page itself, from the plugin's own list rather than a template, so
it cannot quietly go out of date:

- the clip picker does nothing in the source variant, which has no input at all
  — except as the stand-in for the **Image**, which is a file parameter in the
  plugin and a path a page cannot read. **Folder** has no stand-in and behaves
  as Single;
- Columns, Rows and Sprite are integer parameters in the plugin; here they are
  sliders that show the integer they land on;
- Beat and Bar lock to a 120 BPM transport this page generates, which is the
  tempo the plugin falls back to when a host reports none;
- Preset is an option parameter in the plugin, with Custom as element 0 and a
  slider edit dropping back to it; here the same seven presets are in the panel
  header;
- a pixel Gap counts pixels of *this canvas*, so the resolution picker changes
  how much of the frame a given number of pixels is. That is the point of the
  unit and exactly what it does against a real pixel map.

## Deploying it

A static-assets Cloudflare Worker. `.github/workflows/deploy.yml` deploys it on
every push to main that touches it, using the repo's `CLOUDFLARE_API_TOKEN`
secret, and then proves the live page changed. `gen-downloads.py` does not know
it exists. By hand, from the repo root:

```bash
cf-run npx wrangler deploy
```

There is no build step; what is committed is what is served. Check `git status`
first — a parallel session sharing the checkout may have staged its own work
into `demo/`.

Verify by **content**, never by status code, because a stale page returns a
cheerful 200:

```bash
curl -s 'https://shunt-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

## vendor/

The shared kit — chrome, clock, inspector, generated clips, GL helpers — is
vendored from `stoatworks-backend/resolume-demo` by its own `sync.sh`. Edit it
there, not here.
