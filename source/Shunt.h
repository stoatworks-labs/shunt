#pragma once

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

#include "Controls.h"
#include "Imagery.h"
#include "Presets.h"
#include "Queue.h"
#include "Shapes.h"

/**
    Shunt — shapes that slide in, bank up and are drawn away, for Resolume.

    What is worth knowing about how it works is in two other files and not
    repeated here:

    - **Queue.h** — an accumulating conveyor. A shape's place is a pure function
      of (slot, phase), every shape's run is exactly one cycle long, and Dwell
      is the single continuous dial between "the train stacks up before anything
      moves" and "the front is still leaving as the back arrives".
    - **Shaders.h** — two passes and no framebuffer, and why instanced quads
      rather than one fullscreen pass over all the distance functions.

    This class is the part that talks to the host: it declares the parameters,
    turns them into a `QueueParams`, works out what phase the host is asking
    for, and draws.

    Both plugins are this class. The source draws the train over its own
    background; the effect draws it over, or into, the incoming clip. They
    differ by a constructor flag, a `#define` handed to the shader compiler, and
    their input count — little enough that keeping them as one class is what
    stops them drifting apart.

    See AGENTS.md for the traps.
*/
namespace shunt
{
class ShuntPlugin : public CFFGLPlugin
{
public:
	explicit ShuntPlugin( bool overInput );

	// CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure — so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Test hook: the parameter ids a preset covers, in presets::Param order.
	/// Handed out rather than copied into the harness, so a second list cannot
	/// go quietly out of step with this one.
	static const unsigned int* PresetParamIDsForTest( int& count );

	/// Render one frame into whatever is currently bound, at `width` x `height`.
	///
	/// Exposed for the offline harness, which drives this class rather than a
	/// copy of it — a test that exercises a reimplementation tests the
	/// reimplementation.
	void Render( int width, int height, GLuint inputTexture, float maxU, float maxV );

	/// The queue parameters as they would be resolved at this size, for the
	/// harness to predict where each shape should land.
	QueueParams CurrentQueue( int width, int height ) const;

	/// The shapes placed by the last Render, in draw order.
	const std::vector< Wagon >& LastWagons() const { return wagons; }

	/// Pin the driven phase, ignoring the host clock and the beat.
	///
	/// The harness needs this because a test that renders "the frame at phase
	/// 3.25" and then predicts where the shapes are has to be certain that both
	/// halves used the same 3.25 — and `hostTime` is a double that arrives from
	/// outside.
	///
	/// The Phase parameter is still added on top, so pinning does not make that
	/// slider look dead to a sweep.
	void SetPhaseOverride( float phase );

	/// What the last attempt to load the Image said -- the same line Diag
	/// logs. Empty until an image has been asked for.
	std::string ImageNoteForTest() const { return imageNote; }

	/// How many cells the loaded image has; 0 when none is loaded.
	int ImageCellsForTest() const { return imageTexture != 0 ? static_cast< int >( imageCells.size() ) : 0; }

private:
	/// The ParamId each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ presets::kParamCount ] = {
		PT_SHAPE, PT_SIZE, PT_STRETCH, PT_ANGLE, PT_ROUNDNESS, PT_OUTLINE, PT_SOFTNESS,
		PT_SIDE, PT_WAGONS, PT_TRAVEL, PT_GAP, PT_DWELL, PT_SPEED,
		PT_COLOUR_MODE, PT_SHAPE_R, PT_SHAPE_G, PT_SHAPE_B, PT_HUE_SPREAD, PT_OPACITY,
		PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY, PT_BLEND, PT_SHADE
	};

	/// The active preset's value for `id`, or -1 when no preset is active or
	/// this one has no opinion about `id`. Preset values are all 0..1, so a
	/// negative is unambiguous. `presetIndex` is 1-based; 0 is Custom.
	float presetValue( int presetIndex, unsigned int id ) const;

	/// True when this write is the HOST restating a value it still believes in
	/// rather than the operator moving anything — in which case it must not
	/// reach params[] and must not disturb the preset.
	bool hostIsRestatingItself( unsigned int index, float value );

	/// Record the defaults as the host's opening position, once, before
	/// anything has had a chance to move them.
	void seedHostValues();

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders.
	void applyPreset( int presetIndex );

	/// What the HOST last sent for each parameter, which is not the same thing
	/// as what the plugin is rendering with.
	///
	/// FFGL's host owns parameter state. It pushes its own values back down
	/// whenever it likes, and nothing obliges it to act on the value events
	/// applyPreset raises — Resolume does not. So a preset that writes params[]
	/// and trusts the host to follow is relying on behaviour the specification
	/// never promised, and when the host instead restates the values it still
	/// believes in, the rule that a covered parameter changing means the
	/// operator has taken over fires on the host's own echo and drops straight
	/// back to Custom. Reported against vertigo as its issue #2 and fixed
	/// across the fleet; this is that fix, carried in from the start.
	///
	/// Keeping the host's own last word separately is what tells the two apart.
	float hostValues[ PT_COUNT ] = {};
	bool hostValuesSeeded        = false;

	bool BuildShaders();

	/// The source's reading of Mask Mode; Normal for the effect.
	SourceOutput CurrentSourceOutput() const;

	/// The blend actually used: the operator's, except that a matte is Over.
	Blend CurrentBlend() const;

	/// True when the source's Mix is applied as a constant blend factor over
	/// the whole composite rather than folded into the shapes' alpha. See
	/// ApplyBlend in Shunt.cpp.
	bool SourceFadesInBlend() const;

	//---------------------------------------------------------------------
	// The Image.
	//
	// The path arrives through SetTextParameter, which a host may call from a
	// thread that is not the render thread, so it is held under a mutex and
	// only ever READ on the render thread -- which is also the only place the
	// decode and the upload happen, because the upload needs the GL context.
	//
	// What was last loaded is remembered as a key (path, source, grid), and
	// the image is reloaded only when the key changes. Columns and Rows are in
	// the key only for a sheet: they mean nothing to a single image or a
	// folder, and dragging them should not reload a folder of 36 photos.
	//---------------------------------------------------------------------
	void UpdateImage();
	void ReleaseImage();

	mutable std::mutex textMutex;
	std::string imagePath;         ///< as the host last set it
	std::string imagePathReturn;   ///< what GetTextParameter hands back

	std::string loadedKey;         ///< empty: nothing loaded, or not yet tried
	std::string imageNote;
	GLuint imageTexture       = 0;
	GLuint placeholderTexture = 0;  ///< one white texel, for while there is no image
	std::vector< ImageCell > imageCells;

	/// Phase, in cycles, for the current parameters and host state.
	float CurrentPhase() const;

	const bool overInput;

	ffglex::FFGLShader backgroundShader;
	ffglex::FFGLShader shapeShader;

	/// A core profile refuses to draw with no vertex array bound, even when the
	/// vertex shader sources nothing and builds its quad from `gl_VertexID`.
	/// This is that array: created empty, bound to draw, never filled.
	GLuint emptyVAO = 0;

	float params[ PT_COUNT ] = {};

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live at 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. UpdateClock decides by MEASUREMENT rather
	// than by the magnitude of one delta — steady_clock says how much real time
	// passed, the host says how much host time passed, and the ratio names the
	// unit. Carried in from orrery, where guessing from one delta was a bug
	// reported from the field.
	//---------------------------------------------------------------------
	void UpdateClock();

	//---------------------------------------------------------------------
	// Phase continuity across a Speed change.
	//
	// A shape's place stays a pure function of (slot, phase) — that is the
	// whole design and none of it changes here. What changes is only which
	// phase a given clock reading maps to.
	//
	// `phase = clock * speed` means a speed change moves the phase by
	// `clock * delta`, and `clock` is however long the composition has been
	// open. Nudging Speed an hour in is a jump of hundreds of cycles and the
	// whole train teleports. So remember the phase reached so far and count
	// from there at the new rate.
	//
	// Free only. Beat and Bar deliberately keep jumping: their contract is that
	// phase 0 lands on the bar line, and an offset that made a speed change
	// seamless would slide the train off the grid it exists to sit on.
	//---------------------------------------------------------------------
	void UpdatePhaseAnchor();

	double phaseAnchor = 0.0; ///< phase already reached at `anchorClock`
	double anchorClock = 0.0; ///< the clock reading that phase belongs to
	float anchorSpeed  = -1.0f;///< speed in force since then; < 0 until the first frame

public:
	FFResult SetTime( double time ) override;

	//---------------------------------------------------------------------
	// Clock test hooks.
	//
	// The offline harness DECLARES its unit rather than leaving UpdateClock to
	// infer one. A single absolute time handed over in one frame is genuinely
	// ambiguous — 2.0 is two seconds or two milliseconds and nothing in it says
	// which — so inference is only possible against a live host's frame deltas.
	//---------------------------------------------------------------------
	void SetClockScaleForTest( double scale );
	void TickClockForTest();
	double ClockScaleForTest() const;
	double HostSecondsForTest() const;

	/// The phase the next frame would be drawn at. `--speed` needs it: the
	/// thing being tested is that a speed change does NOT move the picture, and
	/// reading the phase either side of one says so directly, where a
	/// rendered-frame comparison would only say the two frames match.
	float CurrentPhaseForTest() const;

private:

	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double hostSeconds  = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen   = false;

	bool phasePinned  = false;
	float pinnedPhase = 0.0f;

	std::vector< Wagon > wagons;

	/// Scratch for the uniform upload, kept as a member so that a frame does not
	/// allocate. Xform, Tint and Cell, kMaxShapes of each.
	std::vector< float > xformScratch;
	std::vector< float > tintScratch;
	std::vector< float > cellScratch;
};

} // namespace shunt
