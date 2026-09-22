#pragma once

/**
    Factory presets: named trains an operator can reach in one gesture. Each
    entry is a recognisable *use* — the caterpillar the plugin was asked for, a
    conveyor of separated blocks, a stack that fills and empties — not a random
    collection of slider positions.

    The values live in the host-facing 0..1 parameter space, so the table is the
    one place a preset is written down. Plain data only; the application
    machinery lives with the host glue in Shunt.cpp. Both plugins (the source
    and the mask) share the same class, so both get the dropdown from this one
    table.

    Element 0 of the host-facing dropdown is "Custom" and is not in this table:
    it means "the sliders are the truth".

    A preset covers the shape, the queue geometry and the colour. It leaves
    alone: **Sync** and **Phase** (the operator's driver, often keyed or
    beat-locked), **Across** (framing), **Gap Units** (an operator who has
    switched to pixels to match a pixel map does not want a preset switching
    them back, and the same number means wildly different things in the two
    units), **Mask Mode** and **Mix** (what the effect does to the clip is the
    operator's call).
*/

namespace shunt
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. Shunt.cpp binds this order
/// to its ParamIds and static_asserts against kParamCount, so the two lists
/// cannot drift apart silently.
enum Param
{
	kShape,
	kSize,
	kStretch,
	kAngle,
	kRoundness,
	kOutline,
	kSoftness,
	kSide,
	kWagons,
	kTravel,
	kGap,
	kDwell,
	kSpeed,
	kColourMode,
	kShapeR,
	kShapeG,
	kShapeB,
	kHueSpread,
	kOpacity,
	kBackR,
	kBackG,
	kBackB,
	kBackOpacity,
	kBlend,
	kShade,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices: Shape 0 Circle / 1 Square / 4 Star /
// 6 Ring / 7 Bar; Side 0 Left / 1 Right / 2 Top / 3 Bottom; Colour mode
// 0 White / 1 Solid / 2 Hue Spread / 3 Hue Cycle; Blend 0 Over / 1 Add /
// 2 Max. Count is the 1..64 quadratic curve, 1 + round(63 v^2): 0.333 is 8
// shapes, 0.408 is 11 and 0.415 is 12 -- the rounding step is narrow there;
// Stretch sits at unity on 0.5; Gap is in THICKNESSES here — 0.25 of the
// slider is exactly 1 thickness, so anything below that overlaps.
inline constexpr Preset kPresets[] = {
	// The effect as it was described: discs sliding in from the left, each
	// overlapping the one in front by a quarter, standing, then drawn away. The
	// plugin's own defaults, named -- keep the two in step.
	{ "Caterpillar",
	  { /*Shape*/ 0, /*Size*/ 0.62f, /*Stretch*/ 0.5f, /*Angle*/ 0.0f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Side*/ 0, /*Count*/ 0.333f, /*Travel*/ 0.78f,
	    /*Gap*/ 0.1875f, /*Dwell*/ 0.55f, /*Speed*/ 0.574f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 0, /*Shade*/ 0.0f } },

	// Separated blocks on a steady belt: no overlap, short stand, so the queue
	// is always emptying from the front while it fills from the back.
	{ "Conveyor",
	  { /*Shape*/ 1, /*Size*/ 0.46f, /*Stretch*/ 0.5f, /*Angle*/ 0.0f, /*Round*/ 0.25f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Side*/ 0, /*Count*/ 0.29f, /*Travel*/ 0.85f,
	    /*Gap*/ 0.32f, /*Dwell*/ 0.18f, /*Speed*/ 0.6f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 2, /*Shade*/ 0.0f } },

	// The other end of the one dial: a long stand, so the whole train is
	// standing before the head moves at all, and then it peels off in order.
	{ "Stacking Up",
	  { /*Shape*/ 0, /*Size*/ 0.46f, /*Stretch*/ 0.5f, /*Angle*/ 0.0f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Side*/ 1, /*Count*/ 0.408f, /*Travel*/ 0.9f,
	    /*Gap*/ 0.24f, /*Dwell*/ 0.85f, /*Speed*/ 0.5f,
	    /*ColMode*/ 2, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 0.7f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 0, /*Shade*/ 0.6f } },

	// Rungs falling down the frame and piling at the bottom: the same train
	// turned through ninety degrees, which the Angle-from-travel convention
	// makes a one-control change.
	{ "Ladder Fall",
	  { /*Shape*/ 7, /*Size*/ 0.62f, /*Stretch*/ 0.5f, /*Angle*/ 0.25f, /*Round*/ 0.4f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.06f, /*Side*/ 2, /*Count*/ 0.29f, /*Travel*/ 0.92f,
	    /*Gap*/ 0.3f, /*Dwell*/ 0.62f, /*Speed*/ 0.52f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 2, /*Shade*/ 0.0f } },

	// A dense hue-cycling stream of thin dashes from the right, barely stopping.
	// Made for a pixel-mapped run of fixtures rather than a screen.
	{ "Ticker",
	  { /*Shape*/ 7, /*Size*/ 0.34f, /*Stretch*/ 0.62f, /*Angle*/ 0.0f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Side*/ 1, /*Count*/ 0.56f, /*Travel*/ 1.0f,
	    /*Gap*/ 0.33f, /*Dwell*/ 0.1f, /*Speed*/ 0.66f,
	    /*ColMode*/ 3, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 1, /*Shade*/ 0.0f } },

	// Big overlapping discs, shaded, sliding over one another as they bank up —
	// the clearest demonstration that the newest one is on top.
	{ "Shingle",
	  { /*Shape*/ 0, /*Size*/ 0.66f, /*Stretch*/ 0.5f, /*Angle*/ 0.0f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Side*/ 0, /*Count*/ 0.25f, /*Travel*/ 0.78f,
	    /*Gap*/ 0.1f, /*Dwell*/ 0.55f, /*Speed*/ 0.5f,
	    /*ColMode*/ 2, /*RGB*/ 0.2f, 0.6f, 1.0f, /*HueSpr*/ 0.35f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 0, /*Shade*/ 1.0f } },

	// Found in the field on the first night: a thick Ring with an Outline wide
	// enough to stroke both of its edges, which leaves an outer ring and a dot
	// in the middle -- a target. Overlapping, and shaded so the overlap reads.
	{ "Bullseye",
	  { /*Shape*/ 6, /*Size*/ 0.66f, /*Stretch*/ 0.5f, /*Angle*/ 0.0f, /*Round*/ 0.7f,
	    /*Outline*/ 0.35f, /*Soft*/ 0.0f, /*Side*/ 0, /*Count*/ 0.315f, /*Travel*/ 0.8f,
	    /*Gap*/ 0.15f, /*Dwell*/ 0.55f, /*Speed*/ 0.55f,
	    /*ColMode*/ 1, /*RGB*/ 0.35f, 0.85f, 0.25f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 0, /*Shade*/ 0.6f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace shunt
