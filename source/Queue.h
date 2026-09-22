#pragma once

#include <vector>

#include "Shapes.h"

/**
    Where every shape in the train is, at a given phase.

    ## The one idea

    **A shape's place is a pure function of (slot, phase).**

    There is no simulation here: no queue object that shapes are pushed onto and
    popped off, no "has the one in front left yet?" test, no previous frame. Ask
    for the picture at phase 91.7 and you get it without having played the
    preceding 91.7 — which is what lets the offline harness measure a single
    frame, and what stops the procession drifting when Resolume's frame rate
    sags under a heavy show.

    ## The mechanism

    An accumulating conveyor, which is what a shunting yard is. Shapes are
    released from one edge at a steady cadence, run in at a steady speed, and
    stand at a **slot**: the head slot sits `travel` into the frame, and slot k
    sits one `gap` further back. A shape stands for its `dwell` and then carries
    on the way it was going, off the far edge. The one behind is still arriving
    while the one in front is leaving, and how much those two overlap is the
    whole look.

    Four quantities set the geometry, and everything else falls out of them:

    - `travel` — how far in the head of the queue goes, as a fraction of the
      frame's span along the direction of travel.
    - `gap` — centre-to-centre spacing between standing shapes, in the same
      units. `GapSpan` resolves it from either a multiple of the shape's own
      thickness or an absolute pixel count.
    - `dwell` — the fraction of a shape's own cycle it spends standing.
    - `count` — how many shapes are in the train.

    ## Why every shape's run is exactly one cycle long

    A shape's run is: enter from off-stage, stand, leave off the far side. Its
    entry run is short if it stands near the back of the queue and long if it
    stands at the head — but its exit run is longer by exactly as much, because
    the two together always cross the whole frame plus the off-stage margin at
    both ends. So `(entry + exit) / speed` is the same number for every slot,
    whatever `travel` and `gap` say.

    Pick the slide speed as

        speed = ( 1 + 2 * margin ) / ( 1 - dwell )

    and that number is `1 - dwell` cycles, so every shape's run is

        ( 1 - dwell ) + dwell  ==  exactly one cycle

    for every slot, at every setting. With `count` shapes released one per
    `1/count` of a cycle, the runs tile the cycle exactly: there are always
    exactly `count` shapes in play, never a hole in the procession and never two
    shapes wanting one slot. That invariant is what `shtest --tile` checks, and
    it is the reason there is no state to keep.

    ## Dwell is the one dial, and it is continuous

    It is tempting to offer "stack up, then release" and "keep moving" as two
    modes. They are the same mechanism at two ends of one slider:

    - **Short dwell** — the slide speed is low, so a shape takes most of its
      cycle to cross. The head is still leaving when the fourth shape arrives;
      the queue empties from the front while it is still filling from the back.
    - **Long dwell** — the slide speed is high, so the whole train is standing
      before the head moves at all, and then it peels off one at a time.

    Everything in between is reachable, and nothing in the code branches on it.
    `shtest --dwell` measures both ends off rendered frames.

    ## Two coordinate conventions, on purpose

    - **The train runs in frame space** — 0..1 across the raster on each axis,
      y down — so `travel` at 1.0 reaches the far edge of a 16:9 frame rather
      than stopping short, which is what you want when the frame *is* the LED
      rig.
    - **Shapes are sized in short-edge fractions**, so a circle is round.

    Those are the same number only on a square render, and mixing them up is
    invisible at 1:1. `shtest --round` exists for exactly that.
*/
namespace shunt
{
/// Which edge the train comes in from. It travels away from that edge.
enum class Side
{
	Left = 0,
	Right,
	Top,
	Bottom,

	Count
};

const char* SideName( Side side );

/// What the Gap slider is counting.
///
/// Both, because both are the right answer to different jobs and neither
/// substitutes for the other: a multiple of the shape's own thickness keeps the
/// train looking the same when Size changes, and an absolute pixel count is
/// what you need when the output is a pixel map and the spacing has to land on
/// the fixture pitch.
enum class GapUnits
{
	Thickness = 0,  ///< Multiples of the shape's extent along the direction of travel.
	Pixels,         ///< Pixels, measured along the direction of travel.

	Count
};

const char* GapUnitsName( GapUnits units );

enum class ColourMode
{
	White = 0,
	Solid,
	HueSpread,   ///< A hue per slot, spread along the train.
	HueCycle,    ///< The same, turning with the phase.

	Count
};

const char* ColourModeName( ColourMode mode );

/**
    The most shapes that can be in the train at once.

    Bounded because the whole set is uploaded as two `vec4` uniform arrays, and
    a uniform array has to be declared at a fixed size in GLSL. 64 shapes is 128
    vec4s; the GL 4.1 floor for vertex uniform components is 1024 (256 vec4s),
    so this fits with room to spare on the least capable machine that can load
    the plugin at all.
*/
constexpr int kMaxShapes = 64;

/// How far past the frame edge a shape sits when it is off-stage, on top of its
/// own half-thickness, in frame-span units. Its only job is to stop a feathered
/// edge showing at the instant a shape is supposed to be out of sight.
constexpr float kClearance = 0.02f;

/// One shape in the train, placed.
///
/// Called a wagon because that is what it behaves like: it rolls in, buffers up
/// against the one in front, stands, and is drawn away again.
struct Wagon
{
	float x = 0.5f;         ///< Centre in frame space, 0..1, y down.
	float y = 0.5f;
	float scale    = 0.08f; ///< Radius as a fraction of the SHORT edge.
	float rotation = 0.0f;  ///< Radians, clockwise on screen.

	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;

	//-----------------------------------------------------------------------
	// Not uploaded. Carried because the harness asks about them by name, and
	// because the draw order is worked out from `age`.
	//-----------------------------------------------------------------------
	int slot      = 0;      ///< 0 is the head of the queue; count-1 is the back.
	float age     = 0.0f;   ///< 0..1, how far through its own run this one is.
	float depth   = 0.0f;   ///< Distance from the entry edge, in frame-span units.
	bool standing = false;  ///< True while it is stopped at its slot.
};

/// The queue's parameters in physical units. `Controls.cpp` turns the host's
/// 0..1 sliders into one of these.
struct QueueParams
{
	Shape shape = Shape::Bar;
	Side side   = Side::Left;
	int count   = 8;

	/// Cycles. Unbounded and monotonic — `Shunt.cpp` builds it from the host
	/// clock or the host's bar position.
	float phase = 0.0f;

	/// Where the head of the queue stops, as a fraction of the frame's span
	/// along the direction of travel. 1.0 puts it at the far edge.
	float travel = 0.7f;

	/// The fraction of a shape's own cycle it spends standing still. Clamped
	/// below 1 by Controls.cpp: at exactly 1 the slide speed is infinite.
	float dwell = 0.45f;

	//-----------------------------------------------------------------------
	// The gap, in the operator's units. Resolved to a frame-span number by
	// GapSpan(), which needs the shape geometry below — so it is kept raw here
	// rather than converted in Controls.cpp, and the harness can measure the
	// conversion instead of taking it on trust.
	//-----------------------------------------------------------------------
	GapUnits gapUnits = GapUnits::Thickness;
	float gapValue    = 1.0f;
	int spanPixels    = 1920;  ///< Pixels along the direction of travel.

	/// Where the run sits on the other axis, in frame space.
	float across = 0.5f;

	/// Shape radius as a fraction of the short edge.
	float size = 0.08f;

	/// Long-axis stretch, applied in the shape's own space before rotation.
	float stretch = 1.0f;

	/// Shape rotation in turns, measured **from the direction of travel**. So a
	/// Bar at 0 is a dash pointing the way it is going, and at 0.25 it is a rung
	/// across the train — whichever edge the train comes in from.
	float angle = 0.0f;

	/// Frame width / height. Needed to turn a short-edge radius into a
	/// frame-space one, which is what makes a circle round off 1:1.
	float aspect = 16.0f / 9.0f;

	ColourMode colourMode = ColourMode::White;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float hueSpread = 1.0f;
	float opacity   = 1.0f;
};

/// True when the train runs along x (Left or Right), false when it runs along y.
bool TravelsHorizontally( Side side );

/// The shape's radius expressed as a fraction of each axis of the frame.
///
/// `scale` is in short-edge fractions so that a circle is round; frame space is
/// 0..1 per axis. Those are the same number only on a square output.
void FrameRadius( float scale, float aspect, float& rx, float& ry );

/// Half the shape's extent along the direction of travel, in frame-span units
/// of that axis.
///
/// This is what "thickness" means for the Gap control, and it is a measured
/// extent rather than the padded quad bound: it takes the shape's own half
/// extents, stretches them, rotates them by `angle`, and projects onto the
/// travel axis.
float HalfThickness( const QueueParams& p );

/// Centre-to-centre spacing between standing shapes, in frame-span units along
/// the direction of travel.
///
/// At `GapUnits::Thickness` and `gapValue` 1.0 this is exactly twice
/// `HalfThickness`, so consecutive shapes stand edge to edge; below 1.0 they
/// overlap, which is the "leading edge of #2 slides over the trailing edge of
/// #1" the effect was asked for.
float GapSpan( const QueueParams& p );

/// How far off-stage a shape sits before it is released, in frame-span units:
/// its own half-thickness plus `kClearance`.
float Margin( const QueueParams& p );

/// The slide speed, in frame-spans per cycle. Derived, not a control — see the
/// header note on why every run is exactly one cycle long.
float SlideSpeed( const QueueParams& p );

/// Where slot `k` stands, as a distance from the entry edge in frame-span
/// units. Clamped so a queue longer than the run backs up off-stage rather than
/// trying to stand behind its own starting point.
float SlotDepth( const QueueParams& p, int slot );

/// How far through its own run slot `k` is, 0..1.
float SlotAge( const QueueParams& p, int slot );

/// Place every shape, in **draw order**: oldest first, newest last, so the
/// newest is drawn on top. Clears and fills `out`.
void Solve( const QueueParams& p, std::vector< Wagon >& out );

/// One shape, by slot rather than by draw order. `slot` must be < p.count.
///
/// The harness predicts with this and the renderer draws with `Solve`, so the
/// two disagreeing is a test failure rather than a shared mistake.
Wagon SolveSlot( const QueueParams& p, int slot );

/// HSV to RGB, all components 0..1, hue wrapping.
void HsvToRgb( float h, float s, float v, float& r, float& g, float& b );

/// RGB to HSV. Used to take the saturation and value off the operator's colour
/// swatch so the hue modes can replace only the hue.
void RgbToHsv( float r, float g, float b, float& h, float& s, float& v );

} // namespace shunt
