#pragma once

/**
    Host parameters, and what they mean.

    **Every numeric parameter Shunt declares is a plain 0..1 float**, even where
    it stands for a shape count, a pixel spacing or a fraction of a cycle. That
    is not a style preference. `CFFGLPluginManager::SetParamInfo` clamps an
    `FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
    can only be called afterwards because it finds the parameter by id — so a
    parameter declared in pixels cannot declare a default in pixels. There is no
    `SetParamDefault`. A default gap of 128 px becomes 1, silently, and the
    plugin starts up wrong in a way no build step notices.

    So the range lives here, in the conversion, and the host only ever sees
    0..1.

    Curves rather than straight lines wherever the useful part of a range is
    bunched at one end. Size is the clear case: for pixel mapping you are often
    placing marks a few thousandths of the frame across, and for a mask you want
    something that fills a good part of it. A linear slider would spend nine
    tenths of its travel between "large" and "slightly larger".

    **Gap is the exception, and deliberately linear in both of its units.** It is
    the one control an operator is likely to be matching to a number they
    already have — a fixture pitch, a pixel map's cell size — so predictable
    beats comfortable, and half a pixel per thousandth of the slider is fine
    enough to land on an exact value by typing it.
*/
namespace shunt
{
/// How the shapes are combined with each other and with what is behind them.
enum class Blend
{
	Over = 0,  ///< Ordinary alpha compositing, newest on top.
	Add,       ///< Additive. Overlaps brighten.
	Max,       ///< Channel-wise maximum. Overlaps do NOT brighten, which is what
	           ///< a mask wants: two overlapping white shapes stay white.

	Count
};

const char* BlendName( Blend blend );

/// What the effect variant does with the clip it is given. Ignored by the
/// source, which has no clip.
enum class MaskMode
{
	Over = 0,   ///< Shapes drawn on top of the clip, in their own colours.
	Reveal,     ///< The clip shows only where the shapes are.
	Hide,       ///< The clip shows everywhere except where the shapes are.
	Colourise,  ///< The clip, tinted by the shape colour, only inside the shapes.

	Count
};

const char* MaskModeName( MaskMode mode );

/**
    What the SOURCE does with the same `Mask Mode` parameter.

    Up to 0.1.0 the source declared Mask Mode and Mix and ignored both, on the
    grounds that it had no clip to mask. That was reported from the field as
    "Mask Mode in Output doesn't seem to do anything" — a control an operator
    can see and change and that changes nothing is a bug, whatever the reason.
    So on the source the parameter keeps its id and its name, and offers the
    thing a source *can* do for a mask: draw the train as a matte for another
    layer to key against.

    The element values line up with the effect's where they mean the same
    thing, so a composition moved from one plugin to the other lands somewhere
    sensible: Reveal (1) is Matte, and Hide (3) — which a source instance has
    only three elements to hold — clamps to Inverse Matte.
*/
enum class SourceOutput
{
	Normal = 0,    ///< The train in its own colours, over the Background.
	Matte,         ///< White shapes on opaque black: a luma matte.
	InverseMatte,  ///< Black shapes on opaque white: the same matte, inverted.

	Count
};

const char* SourceOutputName( SourceOutput mode );

/**
    Whether each new set of shapes comes in on the same line as the last.

    A **set** is one cycle's worth of releases — `Count` shapes. With Lanes off
    every set runs along `Across`, so at settings where the next set arrives
    while the last is still standing or leaving, the two run straight through
    each other. Asked for from the field in those words: "change the Y value
    for each set so that the next set doesn't overlap the previous".

    The set number is `floor( phase - slot / count )`, which is a pure function
    of (slot, phase) like everything else in the queue — so a lane is too, and
    no state is added.
*/
enum class Lanes
{
	Off = 0,  ///< Every set on `Across`.
	Step,     ///< Each set `Lane Step` further across than the last, wrapping.
	Random,   ///< Each set on a random lane at least one shape clear of the last.

	Count
};

const char* LanesName( Lanes lanes );

/// Where the shapes' image comes from. What the Image file parameter means.
enum class ImageSource
{
	Single = 0,  ///< The chosen file, whole, on every shape.
	Folder,      ///< Every image in the chosen file's folder, one per shape.
	Sheet,       ///< The chosen file cut into a Columns x Rows grid.

	Count
};

const char* ImageSourceName( ImageSource source );

/// Which image, or which cell of a sheet, each shape gets.
enum class ImagePick
{
	Same = 0,  ///< Every shape shows the one the Sprite control names.
	Random,    ///< Each shape its own, chosen at random when it is released.
	InOrder,   ///< Each shape the next one along from the shape before it.

	Count
};

const char* ImagePickName( ImagePick pick );

/// Where phase comes from.
enum class Sync
{
	Free = 0,  ///< The host clock. Speed is cycles per second.
	Beat,      ///< The host's beat. Speed is cycles per beat.
	Bar,       ///< The host's bar. Speed is cycles per bar.
	Manual,    ///< Speed is ignored; the Phase slider is the only driver, so the
	           ///< operator can key it, or let Resolume's own BPM-synced
	           ///< animation drive it.

	Count
};

const char* SyncName( Sync sync );

/**
    Parameter ids.

    The declaration order in Shunt.cpp is the order they appear in the host, and
    the groups depend on consecutive ids staying consecutive — `SetParamGroup`
    collapses *runs* of same-group parameters, so reordering these silently
    splits a group into two.
*/
enum ParamId : unsigned int
{
	// Shape
	PT_SHAPE = 0,
	PT_SIZE,
	PT_STRETCH,
	PT_ANGLE,
	PT_ROUNDNESS,
	PT_OUTLINE,
	PT_SOFTNESS,

	// Queue
	PT_SIDE,
	PT_WAGONS,
	PT_TRAVEL,
	PT_GAP_UNITS,
	PT_GAP,
	PT_DWELL,
	PT_ACROSS,

	// Timing
	PT_SYNC,
	PT_SPEED,
	PT_PHASE,

	// Colour
	PT_COLOUR_MODE,
	PT_SHAPE_R,
	PT_SHAPE_G,
	PT_SHAPE_B,
	PT_HUE_SPREAD,
	PT_OPACITY,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,
	PT_BACK_OPACITY,
	PT_BLEND,

	// Shading
	PT_SHADE,
	PT_LIGHT,

	// Output. Both plugins declare both of these so that a composition can be
	// moved between the source and the effect without the parameter list
	// shifting underneath it; the source simply has nothing to mask against and
	// ignores them.
	PT_MASK_MODE,
	PT_MIX,

	// Preset. Declared after the real controls so their ids — which a saved
	// composition refers to — do not shift under existing users if a preset is
	// ever added or removed.
	PT_PRESET,

	//-----------------------------------------------------------------------
	// Added in 0.2.0, from the first field report. After PT_PRESET rather than
	// in with the groups they belong beside, for the same reason Preset is
	// where it is: a saved composition refers to parameters by id, and every
	// id before these is one a 0.1.0 composition already uses.
	//-----------------------------------------------------------------------

	// Lanes
	PT_LANES,
	PT_LANE_STEP,

	// Shadow
	PT_SHADOW,
	PT_SHADOW_DISTANCE,
	PT_SHADOW_BLUR,

	// Image
	PT_IMAGE_FILE,
	PT_IMAGE_SOURCE,
	PT_COLUMNS,
	PT_ROWS,
	PT_IMAGE_PICK,
	PT_SPRITE,
	PT_IMAGE_MIX,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line, then one button per link the block carries:
	// the guide, the project page, the source, the funding page. A button opens
	// a browser and stores nothing.
	//
	// How many buttons there are is decided by which URLs StoatworksAbout.h
	// actually holds, so Shunt.cpp static_asserts this run against
	// `about::kParamCount`.
	//
	// Last in the enum so no saved composition's parameter ids shift.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,
	PT_COUNT
};

/// Shapes in the train. 1..64, quadratic — the difference between three and
/// four is a different-looking effect and the difference between 51 and 52 is
/// nothing, so a linear slider would spend most of its travel on choices nobody
/// makes.
int CountFromParam( float value );

/// Shape radius as a fraction of the short edge. 0.005..0.5, exponential.
float SizeFromParam( float value );

/// Long-axis stretch. 0.1..10, logarithmic, exactly 1 at the centre of the
/// slider so that "not stretched" is a place you can find by feel.
float StretchFromParam( float value );

/// Shape rotation in turns, measured from the direction of travel. 0..1.
float AngleFromParam( float value );

/// Feather width as a fraction of the shape's radius. 0..0.5.
///
/// Zero matters more here than at most sliders: for pixel mapping you want a
/// hard edge, because a feathered one means a fixture at the boundary reads a
/// half-brightness colour that was never in the design.
float SoftnessFromParam( float value );

/// Cycles per unit of the sync source. 0..2, exponential, with a dead zone at
/// the bottom so that "stopped" is reachable by dragging to zero rather than by
/// luck.
float SpeedFromParam( float value );

/// Where the head of the queue stops, as a fraction of the frame's span along
/// the direction of travel. 0..1, linear.
float TravelFromParam( float value );

/// The Gap slider, in whichever unit is selected: 0..4 thicknesses, or
/// 0..512 pixels. Linear in both — see the note at the top of this file.
float GapFromParam( float value, bool pixels );

/// The fraction of a shape's own cycle it spends standing. 0..0.95.
///
/// Stopped short of 1 because the slide speed is `(1 + 2m) / (1 - dwell)` and
/// at exactly 1 that is infinite. 0.95 already means a shape crosses the whole
/// frame in a twentieth of a cycle, which is as close to "it teleports" as
/// anything anyone would use.
float DwellFromParam( float value );

/// Where the run sits on the cross axis, in frame space. -0.25..1.25, so it can
/// sit partly off-frame and be layered with another instance.
float AcrossFromParam( float value );

/// Hue range spanned along the train, in turns. 0..1.
float HueSpreadFromParam( float value );

/// How far across each new set moves, as a fraction of the frame's span
/// ACROSS the direction of travel. -0.5..0.5, linear, exactly 0 at the centre
/// of the slider. Positive is right for a vertical train and down for a
/// horizontal one — frame space runs y-down.
float LaneStepFromParam( float value );

/// Shadow offset, in multiples of the shape's own radius. 0..1.
float ShadowDistanceFromParam( float value );

/// Shadow feather, in multiples of the shape's own radius. 0..1.
float ShadowBlurFromParam( float value );

/// The grid a sprite sheet is cut into. Integer parameters with a real range,
/// because a sheet is somebody else's grid and a slider that lands either side
/// of 5 cannot read it. `FF_TYPE_INTEGER` with a range is proven in Arena by
/// burin's Shape Count.
constexpr int kMaxGrid = 16;

/// The highest sprite index the Sprite control offers. Wraps past the end.
constexpr int kMaxSprite = 255;

} // namespace shunt
