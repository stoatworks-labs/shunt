#include "Shunt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include "Diag.h"
#include "Shaders.h"

namespace shunt
{
namespace
{
/// The GLSL declares `Xform[64]` as a literal, because the shader is a plain
/// string. Raising the C++ constant without raising the GLSL one would overrun
/// the uniform array, so make it a build error instead of a rendering one.
static_assert( kMaxShapes == 64, "Shaders.cpp declares Xform[64], Tint[64] and Cell[64] -- keep them in step" );

/// Read an option parameter. Option parameters do NOT hold 0..1: the host stores
/// the element value the operator chose, so this is an index and clamping it is
/// the only thing standing between a stale composition and an out-of-range enum.
int Option( float value, int count )
{
	const int i = static_cast< int >( std::lround( value ) );
	return std::min( count - 1, std::max( 0, i ) );
}

float Clamp01( float v )
{
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

/// Insert defines after the `#version` line, which must be first in a GLSL
/// source.
std::string WithDefines( const char* shader, const char* defines )
{
	std::string source( shader );
	if( defines == nullptr || *defines == '\0' )
		return source;

	const size_t afterVersion = source.find( '\n' );
	if( afterVersion != std::string::npos )
		source.insert( afterVersion + 1, defines );

	return source;
}

/// `fade` scales everything the shapes add to the frame, through the constant
/// blend colour rather than through the shader's alpha. That is what makes the
/// source's Mix a true fade of the whole composite -- background, shapes and
/// shadows together, `mix * ( shape over background )` -- rather than a fade of
/// each layer before compositing, which leaves every shape visibly too opaque
/// against whatever is on the layer below. The effect passes 1 and gets exactly
/// the blend it always had.
void ApplyBlend( Blend blend, float fade )
{
	glEnable( GL_BLEND );
	glBlendColor( 0.0f, 0.0f, 0.0f, fade );

	switch( blend )
	{
	case Blend::Add:
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_CONSTANT_ALPHA, GL_ONE );
		break;

	case Blend::Max:
		// GL_MAX ignores the factors entirely and takes the channel-wise maximum
		// of source and destination, alpha included. That is what a mask wants:
		// two overlapping white shapes stay white instead of clipping to a
		// brighter white that is no longer the same colour as either of them.
		//
		// It also throws away the draw order, which is the one thing this
		// plugin cares about — under Max there is no "newest on top", because
		// max is commutative. That is the correct trade for a mask and the
		// wrong one for a caterpillar, which is why Over is the default.
		//
		// GL_MAX ignores the factors, so `fade` cannot act here; the source
		// folds Mix into the shapes' own alpha under Max instead, and since max
		// commutes with a common scale that is still an exact fade.
		glBlendEquation( GL_MAX );
		glBlendFunc( GL_ONE, GL_ONE );
		break;

	case Blend::Over:
	default:
		// Premultiplied over, matching the shader's output and the rest of the
		// fleet. Draw order decides what is on top, and Queue.cpp sorts newest
		// last for exactly that reason.
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_CONSTANT_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		break;
	}
}

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, to calibrate the host's against. Steady rather than system, so
/// nothing here moves if the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
} // namespace

// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day the set of links changes,
// and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

ShuntPlugin::ShuntPlugin( bool overInput ) :
	overInput( overInput )
{
	// The source has no input; the effect takes one.
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	//-----------------------------------------------------------------------
	// Defaults.
	//
	// Set BEFORE the parameters are declared, because SetOptionParamInfo takes
	// the default as an argument and reads it from here.
	//
	// Every numeric default is a 0..1 host value that Controls.cpp maps to a
	// physical one -- see the note there on why a ranged parameter cannot carry
	// a ranged default. The physical quantity each one produces is in the
	// comment.
	//
	// Out of the box this is the "Caterpillar" preset: rungs entering from the
	// left, overlapping by a third of their own thickness, standing for about
	// half of each cycle.
	//-----------------------------------------------------------------------
	params[ PT_SHAPE ]     = static_cast< float >( Shape::Circle );
	params[ PT_SIZE ]      = 0.62f;   // 0.087 of the short edge
	params[ PT_STRETCH ]   = 0.5f;    // exactly 1.0
	params[ PT_ANGLE ]     = 0.0f;    // pointing the way it is going
	params[ PT_ROUNDNESS ] = 0.0f;
	params[ PT_OUTLINE ]   = 0.0f;
	params[ PT_SOFTNESS ]  = 0.0f;

	params[ PT_SIDE ]      = static_cast< float >( Side::Left );
	params[ PT_WAGONS ]    = 0.333f;  // 8 shapes
	params[ PT_TRAVEL ]    = 0.78f;   // the head stops 78% of the way across
	params[ PT_GAP_UNITS ] = static_cast< float >( GapUnits::Thickness );
	params[ PT_GAP ]       = 0.1875f; // 0.75 thicknesses: a quarter overlap
	params[ PT_DWELL ]     = 0.55f;   // ~0.52 of each cycle standing
	params[ PT_ACROSS ]    = 0.5f;    // centred

	params[ PT_SYNC ]  = static_cast< float >( Sync::Free );
	params[ PT_SPEED ] = 0.574f;      // ~0.2 cycles per second
	params[ PT_PHASE ] = 0.0f;

	params[ PT_COLOUR_MODE ]  = static_cast< float >( ColourMode::White );
	params[ PT_SHAPE_R ]      = 1.0f;
	params[ PT_SHAPE_G ]      = 1.0f;
	params[ PT_SHAPE_B ]      = 1.0f;
	params[ PT_HUE_SPREAD ]   = 1.0f;
	params[ PT_OPACITY ]      = 1.0f;
	params[ PT_BACK_R ]       = 0.0f;
	params[ PT_BACK_G ]       = 0.0f;
	params[ PT_BACK_B ]       = 0.0f;
	params[ PT_BACK_OPACITY ] = 1.0f;  // opaque black: what a mask wants
	params[ PT_BLEND ]        = static_cast< float >( Blend::Over );

	params[ PT_SHADE ] = 0.0f;
	params[ PT_LIGHT ] = 0.25f;        // from the top of the frame

	params[ PT_MASK_MODE ] = static_cast< float >( MaskMode::Over );
	params[ PT_MIX ]       = 1.0f;

	// Everything added in 0.2.0 defaults to OFF, so a composition saved with
	// 0.1.0 -- which has no values for these -- opens looking exactly as it did.
	params[ PT_LANES ]     = static_cast< float >( Lanes::Off );
	params[ PT_LANE_STEP ] = 0.7f;     // each set a fifth of the frame further down/right

	params[ PT_SHADOW ]          = 0.0f;
	params[ PT_SHADOW_DISTANCE ] = 0.25f;  // a quarter of the shape's radius
	params[ PT_SHADOW_BLUR ]     = 0.3f;

	params[ PT_IMAGE_SOURCE ] = static_cast< float >( ImageSource::Single );
	params[ PT_COLUMNS ]      = 2.0f;  // integer parameters: real values, not 0..1
	params[ PT_ROWS ]         = 2.0f;
	params[ PT_IMAGE_PICK ]   = static_cast< float >( ImagePick::Same );
	params[ PT_SPRITE ]       = 0.0f;
	params[ PT_IMAGE_MIX ]    = 1.0f;

	//-----------------------------------------------------------------------
	// Declaration. This order is the order the host shows them in.
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_SHAPE, "Shape", static_cast< int >( Shape::Count ), params[ PT_SHAPE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Shape::Count ); ++i )
		SetParamElementInfo( PT_SHAPE, i, ShapeName( static_cast< Shape >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_SIZE, "Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_STRETCH, "Stretch", FF_TYPE_STANDARD );
	SetParamInfof( PT_ANGLE, "Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROUNDNESS, "Roundness", FF_TYPE_STANDARD );
	SetParamInfof( PT_OUTLINE, "Outline", FF_TYPE_STANDARD );
	SetParamInfof( PT_SOFTNESS, "Softness", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SIDE, "From", static_cast< int >( Side::Count ), params[ PT_SIDE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Side::Count ); ++i )
		SetParamElementInfo( PT_SIDE, i, SideName( static_cast< Side >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_WAGONS, "Count", FF_TYPE_STANDARD );
	SetParamInfof( PT_TRAVEL, "Travel", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_GAP_UNITS, "Gap Units", static_cast< int >( GapUnits::Count ), params[ PT_GAP_UNITS ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( GapUnits::Count ); ++i )
		SetParamElementInfo( PT_GAP_UNITS, i, GapUnitsName( static_cast< GapUnits >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_GAP, "Gap", FF_TYPE_STANDARD );
	SetParamInfof( PT_DWELL, "Dwell", FF_TYPE_STANDARD );
	SetParamInfof( PT_ACROSS, "Across", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SYNC, "Sync", static_cast< int >( Sync::Count ), params[ PT_SYNC ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Sync::Count ); ++i )
		SetParamElementInfo( PT_SYNC, i, SyncName( static_cast< Sync >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_PHASE, "Phase", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_COLOUR_MODE, "Colour Mode", static_cast< int >( ColourMode::Count ), params[ PT_COLOUR_MODE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( ColourMode::Count ); ++i )
		SetParamElementInfo( PT_COLOUR_MODE, i, ColourModeName( static_cast< ColourMode >( i ) ), static_cast< float >( i ) );

	// FF_TYPE_RED carries the swatch; the green and blue components are separate
	// parameters that the host groups behind it by type, which is why only the
	// red one gets a human name.
	SetParamInfof( PT_SHAPE_R, "Colour", FF_TYPE_RED );
	SetParamInfof( PT_SHAPE_G, "Colour_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_SHAPE_B, "Colour_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_HUE_SPREAD, "Hue Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_BACK_R, "Background", FF_TYPE_RED );
	SetParamInfof( PT_BACK_G, "Background_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_BACK_B, "Background_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_BACK_OPACITY, "Background Alpha", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BLEND, "Blend", static_cast< int >( Blend::Count ), params[ PT_BLEND ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Blend::Count ); ++i )
		SetParamElementInfo( PT_BLEND, i, BlendName( static_cast< Blend >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_SHADE, "Shade", FF_TYPE_STANDARD );
	SetParamInfof( PT_LIGHT, "Light", FF_TYPE_STANDARD );

	// Same id and same name in both plugins, different elements: the effect
	// masks its clip, the source -- which has no clip -- draws a matte for
	// another layer to key against. See SourceOutput in Controls.h for why the
	// source's version exists at all.
	if( overInput )
	{
		SetOptionParamInfo( PT_MASK_MODE, "Mask Mode", static_cast< int >( MaskMode::Count ), params[ PT_MASK_MODE ] );
		for( unsigned int i = 0; i < static_cast< unsigned int >( MaskMode::Count ); ++i )
			SetParamElementInfo( PT_MASK_MODE, i, MaskModeName( static_cast< MaskMode >( i ) ), static_cast< float >( i ) );
	}
	else
	{
		SetOptionParamInfo( PT_MASK_MODE, "Mask Mode", static_cast< int >( SourceOutput::Count ), params[ PT_MASK_MODE ] );
		for( unsigned int i = 0; i < static_cast< unsigned int >( SourceOutput::Count ); ++i )
			SetParamElementInfo( PT_MASK_MODE, i, SourceOutputName( static_cast< SourceOutput >( i ) ), static_cast< float >( i ) );
	}

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	//-----------------------------------------------------------------------
	// 0.2.0. Declared in id order -- the SDK appends each declaration to a
	// list, and the host numbers parameters by their place in it.
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_LANES, "Lanes", static_cast< int >( Lanes::Count ), params[ PT_LANES ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Lanes::Count ); ++i )
		SetParamElementInfo( PT_LANES, i, LanesName( static_cast< Lanes >( i ) ), static_cast< float >( i ) );
	SetParamInfof( PT_LANE_STEP, "Lane Step", FF_TYPE_STANDARD );

	SetParamInfof( PT_SHADOW, "Shadow", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHADOW_DISTANCE, "Shadow Distance", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHADOW_BLUR, "Shadow Blur", FF_TYPE_STANDARD );

	{
		std::vector< std::string > extensions;
		for( int i = 0; i < kImageExtensionCount; ++i )
			extensions.emplace_back( kImageExtensions[ i ] );
		SetFileParamInfo( PT_IMAGE_FILE, "Image", extensions, "" );
	}

	SetOptionParamInfo( PT_IMAGE_SOURCE, "Image From", static_cast< int >( ImageSource::Count ), params[ PT_IMAGE_SOURCE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( ImageSource::Count ); ++i )
		SetParamElementInfo( PT_IMAGE_SOURCE, i, ImageSourceName( static_cast< ImageSource >( i ) ), static_cast< float >( i ) );

	// Integer parameters with real ranges, unlike everything above: a sheet's
	// grid is somebody else's, and a 0..1 slider that lands either side of 5 is
	// no way to read it. SetParamInfo only clamps FF_TYPE_STANDARD defaults, so
	// these carry their real defaults.
	SetParamInfof( PT_COLUMNS, "Columns", FF_TYPE_INTEGER );
	SetParamRange( PT_COLUMNS, 1.0f, static_cast< float >( kMaxGrid ) );
	SetParamInfof( PT_ROWS, "Rows", FF_TYPE_INTEGER );
	SetParamRange( PT_ROWS, 1.0f, static_cast< float >( kMaxGrid ) );

	SetOptionParamInfo( PT_IMAGE_PICK, "Pick", static_cast< int >( ImagePick::Count ), params[ PT_IMAGE_PICK ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( ImagePick::Count ); ++i )
		SetParamElementInfo( PT_IMAGE_PICK, i, ImagePickName( static_cast< ImagePick >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_SPRITE, "Sprite", FF_TYPE_INTEGER );
	SetParamRange( PT_SPRITE, 0.0f, static_cast< float >( kMaxSprite ) );

	SetParamInfof( PT_IMAGE_MIX, "Image Mix", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Groups. Thirty-odd parameters in one flat list is how somebody else's
	// inspector stops being readable. SetParamGroup collapses *runs* of
	// same-group parameters, which is why the ids in Controls.h have to stay in
	// this order.
	//-----------------------------------------------------------------------
	for( unsigned int id = PT_SHAPE; id <= PT_SOFTNESS; ++id )
		SetParamGroup( id, "Shape" );
	for( unsigned int id = PT_SIDE; id <= PT_ACROSS; ++id )
		SetParamGroup( id, "Queue" );
	for( unsigned int id = PT_SYNC; id <= PT_PHASE; ++id )
		SetParamGroup( id, "Timing" );
	for( unsigned int id = PT_COLOUR_MODE; id <= PT_BLEND; ++id )
		SetParamGroup( id, "Colour" );
	for( unsigned int id = PT_SHADE; id <= PT_LIGHT; ++id )
		SetParamGroup( id, "Shading" );
	for( unsigned int id = PT_MASK_MODE; id <= PT_MIX; ++id )
		SetParamGroup( id, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );
	for( unsigned int id = PT_LANES; id <= PT_LANE_STEP; ++id )
		SetParamGroup( id, "Lanes" );
	for( unsigned int id = PT_SHADOW; id <= PT_SHADOW_BLUR; ++id )
		SetParamGroup( id, "Shadow" );
	for( unsigned int id = PT_IMAGE_FILE; id <= PT_IMAGE_MIX; ++id )
		SetParamGroup( id, "Image" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );
}

//---------------------------------------------------------------------------
// GL lifetime
//---------------------------------------------------------------------------
bool ShuntPlugin::BuildShaders()
{
	const char* defines = overInput ? kEffectDefine : "";

	const std::string backgroundFragment = WithDefines( kBackgroundFragmentShader, defines );
	const std::string shapeFragment      = WithDefines( kShapeFragmentShader, defines );

	if( !backgroundShader.Compile( kBackgroundVertexShader, backgroundFragment.c_str() ) )
	{
		diag::error( "background shader would not compile" );
		return false;
	}

	if( !shapeShader.Compile( kShapeVertexShader, shapeFragment.c_str() ) )
	{
		diag::error( "shape shader would not compile" );
		return false;
	}

	// A uniform that does not resolve is a silent no-op -- glUniform on location
	// -1 is documented to do nothing -- and for these two the symptom is a
	// plugin that draws no shapes at all, which is indistinguishable from every
	// shape being off-screen. Say so in the log rather than leaving it to be
	// guessed at.
	if( shapeShader.FindUniform( "Xform" ) < 0 || shapeShader.FindUniform( "Tint" ) < 0 )
		diag::error( "shape uniform arrays did not resolve -- no shapes will be drawn" );

	return true;
}

FFResult ShuntPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();

	const GLubyte* version = glGetString( GL_VERSION );
	diag::info( std::string( "InitGL, GL " )
	            + ( version != nullptr ? reinterpret_cast< const char* >( version ) : "unknown" ) );

	if( !BuildShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	// A core profile refuses to draw with no vertex array bound, even though
	// both shaders build their geometry from gl_VertexID and source nothing.
	glGenVertexArrays( 1, &emptyVAO );

	// What the Imagery sampler reads while no image is loaded. The shader does
	// not sample it then -- HasImage is 0 -- but a sampler bound to texture 0
	// is "unloadable" to at least Apple's GL, which says so in the console on
	// every run, and a driver is within its rights to do worse. One white
	// texel costs nothing.
	{
		const unsigned char white[ 4 ] = { 255, 255, 255, 255 };
		glGenTextures( 1, &placeholderTexture );
		glBindTexture( GL_TEXTURE_2D, placeholderTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	xformScratch.assign( static_cast< size_t >( kMaxShapes ) * 4, 0.0f );
	tintScratch.assign( static_cast< size_t >( kMaxShapes ) * 4, 0.0f );
	cellScratch.assign( static_cast< size_t >( kMaxShapes ) * 4, 0.0f );

	currentViewport = *vp;
	return FF_SUCCESS;
}

FFResult ShuntPlugin::DeInitGL()
{
	backgroundShader.FreeGLResources();
	shapeShader.FreeGLResources();

	if( emptyVAO != 0 )
	{
		glDeleteVertexArrays( 1, &emptyVAO );
		emptyVAO = 0;
	}

	ReleaseImage();

	if( placeholderTexture != 0 )
	{
		glDeleteTextures( 1, &placeholderTexture );
		placeholderTexture = 0;
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------
char* ShuntPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call.
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}

	if( index == PT_IMAGE_FILE )
	{
		// Copied into a member under the lock: the host keeps the pointer past
		// this call, and imagePath itself may be replaced from another thread.
		std::lock_guard< std::mutex > lock( textMutex );
		imagePathReturn = imagePath;
		return const_cast< char* >( imagePathReturn.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult ShuntPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	// The same trap, and a sharper one: this is a FILE parameter whose default
	// is empty, and the host pushes that empty default through here on every
	// fresh instance. Falling through to the base class would fail it and the
	// plugin would not load at all.
	if( index == PT_IMAGE_FILE )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		imagePath = value != nullptr ? value : "";
		return FF_SUCCESS;
	}

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult ShuntPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	// A text parameter has no float value. Answered rather than stored, so a
	// host that sweeps every parameter as a float cannot write a number into
	// the slot the path lives beside.
	if( index == PT_IMAGE_FILE )
		return FF_SUCCESS;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// back to Custom -- which is what made presets look like they could not be
	// selected at all. See AGENTS.md.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters --
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				// Logged, unlike an ordinary parameter change: this one is a
				// state change an operator can be surprised by, and it happens
				// once rather than per frame.
				diag::info( "preset dropped to Custom: parameter "
				            + std::to_string( index ) + " moved to "
				            + std::to_string( value ) );
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* ShuntPlugin::PresetParamIDsForTest( int& count )
{
	count = presets::kParamCount;
	return kPresetParamIDs;
}

float ShuntPlugin::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return -1.0f;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

void ShuntPlugin::seedHostValues()
{
	// Seeded on first parameter traffic rather than in the constructor, so the
	// whole mechanism stays in one place. It has to happen BEFORE applyPreset
	// can run: seeding afterwards would record the preset's own values as the
	// host's opening position, and the host's very next restatement would then
	// look like an edit -- which is the bug this exists to prevent,
	// reintroduced.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool ShuntPlugin::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a UI,
	// a MIDI value or a saved composition -- hands back a number near ours
	// rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing it
		// would actively hurt: a host that quantises hands back a ROUNDED copy
		// of our own value, params[] would take the rounding, and the "did a
		// covered parameter move?" test above works to a tighter tolerance than
		// this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log nobody
	// reads.
	return true;
}

void ShuntPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float ShuntPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

void ShuntPlugin::SetPhaseOverride( float phase )
{
	phasePinned = true;
	pinnedPhase = phase;
}

//---------------------------------------------------------------------------
// Phase
//---------------------------------------------------------------------------
float ShuntPlugin::CurrentPhase() const
{
	const Sync sync    = static_cast< Sync >( Option( params[ PT_SYNC ], static_cast< int >( Sync::Count ) ) );
	const float speed  = SpeedFromParam( params[ PT_SPEED ] );
	const float manual = params[ PT_PHASE ];

	// Pinning replaces the CLOCK, not the whole phase. The Phase slider stays
	// live underneath it, which is what lets tools/sweep.py prove that slider
	// is connected -- a pin that swallowed it too would make it look dead.
	if( phasePinned )
		return pinnedPhase + manual;

	float driven = 0.0f;

	switch( sync )
	{
	case Sync::Free:
		// Not `hostSeconds * speed`: see UpdatePhaseAnchor. Until the operator
		// has moved Speed this is exactly that product, because the anchor
		// starts at clock zero with phase zero.
		driven = static_cast< float >( phaseAnchor + ( hostSeconds - anchorClock ) * speed );
		break;

	case Sync::Beat:
	case Sync::Bar:
	{
		//-------------------------------------------------------------------
		// The host hands us a tempo and a position *within* the current bar,
		// and never says which bar it is. A bar counter would be state, and
		// state is the thing this plugin does not have.
		//
		// So recover a continuous bar number without keeping one: the clock
		// gives an estimate of how many bars have passed, `barPhase` gives the
		// exact position inside the bar, and the whole number that reconciles
		// them is `round( estimate - barPhase )`. The result is continuous
		// across the bar line -- as `barPhase` wraps from 1 to 0 the rounded
		// integer steps up by one at the same moment -- and it stays exact even
		// if the clock estimate is off by up to half a bar.
		//
		// It can name the wrong absolute bar if the host's transport did not
		// start at time zero. That is invisible: the train repeats, so an
		// integer bar offset is not a thing anyone can see.
		//-------------------------------------------------------------------
		const float tempo      = bpm > 1.0f ? bpm : 120.0f;
		const float barSeconds = 240.0f / tempo;   // four beats to the bar
		const float estimate   = static_cast< float >( hostSeconds ) / barSeconds;
		const float within     = Clamp01( barPhase );

		const float bars = within + std::round( estimate - within );

		driven = ( sync == Sync::Beat ? bars * 4.0f : bars ) * speed;
		break;
	}

	case Sync::Manual:
	default:
		// Speed is deliberately ignored. This is the mode for driving Phase
		// from Resolume's own BPM-synced animation, or from a keyframe, or from
		// a MIDI fader -- and a second clock underneath it would fight whatever
		// is doing the driving.
		driven = 0.0f;
		break;
	}

	return driven + manual;
}

//---------------------------------------------------------------------------
// The clock
//---------------------------------------------------------------------------
void ShuntPlugin::UpdateClock()
{
	// FFGL never says what unit SetTime arrives in, and hosts disagree:
	// Resolume sends MILLISECONDS (measured live at 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. Reading it raw is a thousand times fast on
	// the one host that matters and exactly right on the one that gets tested,
	// which is how it stays hidden.
	//
	// So measure instead of guessing. steady_clock says how much real time
	// passed, the host says how much host time passed, and the ratio names the
	// unit outright. Nothing plausible sits between 1 and 1000, so both bands
	// are wide and a frame fitting neither simply does not vote.
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	// Never read `hostTime` before the host has set it: CFFGLPlugin's
	// constructor initialises bpm and barPhase and leaves hostTime
	// uninitialised, so until SetTime lands it is whatever was in that memory.
	const double raw = hostTimeSeen ? hostTime : -1.0;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			// Several frames rather than one, so a single odd frame -- the
			// first after a seek, say -- cannot decide it on its own.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				diag::info( std::string( "host clock is " )
				            + ( clockScale == 0.001 ? "milliseconds" : "seconds" )
				            + ", scale=" + std::to_string( clockScale ) );
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime at
	// all -- run on the real clock. Wrong in origin but right in rate, where
	// assuming seconds would be a thousand times fast on Resolume.
	hostSeconds = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

void ShuntPlugin::UpdatePhaseAnchor()
{
	const Sync sync   = static_cast< Sync >( Option( params[ PT_SYNC ], static_cast< int >( Sync::Count ) ) );
	const float speed = SpeedFromParam( params[ PT_SPEED ] );

	// Beat and Bar are meant to jump -- they re-lock to the transport, which is
	// the point of them. Keep the anchor following the clock while they are
	// selected so that returning to Free resumes rather than leaps.
	if( sync != Sync::Free )
	{
		anchorClock = hostSeconds;
		anchorSpeed = speed;
		return;
	}

	// First frame: leave the anchor at clock zero, phase zero. That makes the
	// expression in CurrentPhase identical to a plain `hostSeconds * speed` for
	// as long as nobody touches Speed, which is what keeps every rendered-frame
	// test and tools/sweep.py measuring the same thing.
	if( anchorSpeed < 0.0f )
	{
		anchorSpeed = speed;
		return;
	}

	if( speed != anchorSpeed )
	{
		// Once per speed change, not once per frame: this carries the exact
		// phase forward rather than integrating it, so a long session cannot
		// accumulate rounding into a drift.
		phaseAnchor += ( hostSeconds - anchorClock ) * anchorSpeed;
		anchorClock = hostSeconds;
		anchorSpeed = speed;
	}
}

FFResult ShuntPlugin::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

void ShuntPlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void ShuntPlugin::TickClockForTest()
{
	UpdateClock();
	UpdatePhaseAnchor();
}

double ShuntPlugin::ClockScaleForTest() const
{
	return clockScale;
}

float ShuntPlugin::CurrentPhaseForTest() const
{
	return CurrentPhase();
}

double ShuntPlugin::HostSecondsForTest() const
{
	return hostSeconds;
}

//---------------------------------------------------------------------------
// What the Output group means for this plugin
//---------------------------------------------------------------------------
SourceOutput ShuntPlugin::CurrentSourceOutput() const
{
	// The effect has its own four modes and never reads these.
	if( overInput )
		return SourceOutput::Normal;
	return static_cast< SourceOutput >( Option( params[ PT_MASK_MODE ], static_cast< int >( SourceOutput::Count ) ) );
}

Blend ShuntPlugin::CurrentBlend() const
{
	// A matte is only a matte if overlapping shapes stay one colour, so the
	// operator's Blend is set aside for it: Over, white on black or black on
	// white, is exactly that whatever the overlap.
	if( CurrentSourceOutput() != SourceOutput::Normal )
		return Blend::Over;
	return static_cast< Blend >( Option( params[ PT_BLEND ], static_cast< int >( Blend::Count ) ) );
}

bool ShuntPlugin::SourceFadesInBlend() const
{
	return !overInput && CurrentBlend() != Blend::Max;
}

//---------------------------------------------------------------------------
// Parameters to the queue
//---------------------------------------------------------------------------
QueueParams ShuntPlugin::CurrentQueue( int width, int height ) const
{
	QueueParams q;

	q.shape = static_cast< Shape >( Option( params[ PT_SHAPE ], static_cast< int >( Shape::Count ) ) );
	q.side  = static_cast< Side >( Option( params[ PT_SIDE ], static_cast< int >( Side::Count ) ) );
	q.count = CountFromParam( params[ PT_WAGONS ] );
	q.phase = CurrentPhase();

	q.travel = TravelFromParam( params[ PT_TRAVEL ] );
	q.dwell  = DwellFromParam( params[ PT_DWELL ] );
	q.across = AcrossFromParam( params[ PT_ACROSS ] );

	q.lanes    = static_cast< Lanes >( Option( params[ PT_LANES ], static_cast< int >( Lanes::Count ) ) );
	q.laneStep = LaneStepFromParam( params[ PT_LANE_STEP ] );

	q.gapUnits = static_cast< GapUnits >( Option( params[ PT_GAP_UNITS ], static_cast< int >( GapUnits::Count ) ) );
	q.gapValue = GapFromParam( params[ PT_GAP ], q.gapUnits == GapUnits::Pixels );

	// The pixel span is the raster's own count on the axis the train runs
	// along, so "40 pixels" means forty pixels of output whichever edge it
	// comes in from -- which is the only reading that is any use against a
	// pixel map.
	q.spanPixels = TravelsHorizontally( q.side ) ? std::max( 1, width ) : std::max( 1, height );

	q.size    = SizeFromParam( params[ PT_SIZE ] );
	q.stretch = StretchFromParam( params[ PT_STRETCH ] );
	q.angle   = AngleFromParam( params[ PT_ANGLE ] );

	q.colourMode = static_cast< ColourMode >( Option( params[ PT_COLOUR_MODE ], static_cast< int >( ColourMode::Count ) ) );
	q.r          = Clamp01( params[ PT_SHAPE_R ] );
	q.g          = Clamp01( params[ PT_SHAPE_G ] );
	q.b          = Clamp01( params[ PT_SHAPE_B ] );
	q.hueSpread  = HueSpreadFromParam( params[ PT_HUE_SPREAD ] );

	// Mix fades the shape layer. In the effect that mixes the effect against
	// the untouched clip; in the source, which has nothing underneath, it
	// fades the whole output -- background too, in Render -- to transparent.
	// Up to 0.1.0 the source ignored it, and an operator moving a slider that
	// did nothing reported it, fairly, as a bug.
	q.opacity = Clamp01( params[ PT_OPACITY ] ) * ( SourceFadesInBlend() ? 1.0f : Clamp01( params[ PT_MIX ] ) );

	q.aspect = ( width > 0 && height > 0 )
	           ? static_cast< float >( width ) / static_cast< float >( height )
	           : 1.0f;

	return q;
}

//---------------------------------------------------------------------------
// The Image
//---------------------------------------------------------------------------
void ShuntPlugin::ReleaseImage()
{
	if( imageTexture != 0 )
	{
		glDeleteTextures( 1, &imageTexture );
		imageTexture = 0;
	}
	imageCells.clear();

	// Forget the key too, so a context that comes back after DeInitGL reloads
	// rather than believing a texture it no longer has is still bound.
	loadedKey.clear();
}

void ShuntPlugin::UpdateImage()
{
	std::string path;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		path = imagePath;
	}

	const ImageSource source = static_cast< ImageSource >( Option( params[ PT_IMAGE_SOURCE ], static_cast< int >( ImageSource::Count ) ) );
	const int columns        = std::clamp( static_cast< int >( std::lround( params[ PT_COLUMNS ] ) ), 1, kMaxGrid );
	const int rows           = std::clamp( static_cast< int >( std::lround( params[ PT_ROWS ] ) ), 1, kMaxGrid );

	// "-" and not the empty string for "no image", so that clearing the path
	// is itself a change of key and releases the old texture.
	std::string key = path.empty() ? std::string( "-" ) : path;
	key += "|" + std::to_string( static_cast< int >( source ) );
	if( source == ImageSource::Sheet )
		key += "|" + std::to_string( columns ) + "x" + std::to_string( rows );

	if( key == loadedKey )
		return;

	ReleaseImage();
	loadedKey = key;

	if( path.empty() )
	{
		imageNote.clear();
		return;
	}

	// On the render thread, and synchronous: a frame is late while a folder of
	// photos decodes. Choosing a file is an edit, not a performance gesture, so
	// that is the right trade against a loader thread and the state it drags in.
	Imagery imagery = LoadImagery( path, source, columns, rows );
	imageNote       = imagery.note;

	if( !imagery.Valid() )
	{
		diag::error( "image not loaded: " + imagery.note );
		return;
	}

	// Drain anything the host left in the error queue, so the check after the
	// upload is about the upload.
	while( glGetError() != GL_NO_ERROR )
	{
	}

	glGenTextures( 1, &imageTexture );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, imageTexture );

	// Rows are always a multiple of four bytes, so the default unpack alignment
	// is right -- but a host is free to leave a row length or a skip set, and
	// those would shear the picture. Reset what matters and put it back.
	GLint rowLength = 0, skipRows = 0, skipPixels = 0, alignment = 4;
	glGetIntegerv( GL_UNPACK_ROW_LENGTH, &rowLength );
	glGetIntegerv( GL_UNPACK_SKIP_ROWS, &skipRows );
	glGetIntegerv( GL_UNPACK_SKIP_PIXELS, &skipPixels );
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
	glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );

	// Row 0 of the buffer is the top of the picture and lands at v = 0, which
	// is what the cell rectangles and the shader both assume: no flip anywhere.
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, imagery.width, imagery.height, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE, imagery.rgba.data() );

	glPixelStorei( GL_UNPACK_ROW_LENGTH, rowLength );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, skipRows );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, skipPixels );
	glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );

	// Not mipmapped: a mip level averages across cell boundaries, so every
	// sprite would faintly contain its neighbours. The half-texel inset keeps
	// the magnified case clean; minification aliases, which is the lesser
	// problem.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );

	if( glGetError() != GL_NO_ERROR )
	{
		diag::error( "image upload failed: " + imagery.note );
		glDeleteTextures( 1, &imageTexture );
		imageTexture = 0;
		return;
	}

	imageCells = std::move( imagery.cells );
	diag::info( "image loaded: " + imageNote );
}

//---------------------------------------------------------------------------
// Drawing
//---------------------------------------------------------------------------
void ShuntPlugin::Render( int width, int height, GLuint inputTexture, float maxU, float maxV )
{
	if( !backgroundShader.IsReady() || !shapeShader.IsReady() )
		return;

	UpdateClock();
	UpdatePhaseAnchor();

	UpdateImage();

	const QueueParams queue = CurrentQueue( width, height );
	Solve( queue, wagons );

	const MaskMode mask = overInput
	                      ? static_cast< MaskMode >( Option( params[ PT_MASK_MODE ], static_cast< int >( MaskMode::Count ) ) )
	                      : MaskMode::Over;

	const SourceOutput output = CurrentSourceOutput();
	const bool matte          = output != SourceOutput::Normal;
	const Blend blend         = CurrentBlend();
	const float mix           = Clamp01( params[ PT_MIX ] );

	// How much the blend itself scales what the shapes add -- see ApplyBlend.
	const float fade = SourceFadesInBlend() ? mix : 1.0f;

	// Shadows need somewhere to fall and a blend that can darken. Not in Hide,
	// whose blend function would punch them out of the clip as holes, and not
	// in a matte, where a grey fringe is a partial key nobody asked for.
	const float shadowAmount = Clamp01( params[ PT_SHADOW ] );
	const bool shadows       = shadowAmount > 0.001f && !matte && mask != MaskMode::Hide;

	const bool hasImage = imageTexture != 0 && !imageCells.empty();

	// Reveal and Colourise build their picture only where the shapes are, so
	// the clip behind them fades IN as the effect mixes OUT.
	const bool buildsFromClip = ( mask == MaskMode::Reveal || mask == MaskMode::Colourise );
	const float clipGain      = buildsFromClip ? 1.0f - mix : 1.0f;

	int sampleMode = 0;
	switch( mask )
	{
	case MaskMode::Reveal:    sampleMode = 1; break;
	case MaskMode::Colourise: sampleMode = 2; break;
	case MaskMode::Hide:      sampleMode = 3; break;
	case MaskMode::Over:
	default:                  sampleMode = 0; break;
	}

	glBindVertexArray( emptyVAO );

	//-----------------------------------------------------------------------
	// Pass 1: the background.
	//
	// glUseProgram directly rather than ffglex::ScopedShaderBinding, and an
	// explicit glBindTexture rather than ScopedTextureBinding, because every
	// ffglex Scoped* binding CLEARS to 0 when it leaves scope instead of
	// restoring what was there. That is survivable here but it is a trap worth
	// not stepping into twice, and the explicit form is no longer.
	//-----------------------------------------------------------------------
	glDisable( GL_BLEND );
	glUseProgram( backgroundShader.GetGLID() );

	if( overInput )
	{
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, inputTexture );
		backgroundShader.Set( "Clip", 0 );
		backgroundShader.Set( "MaxUV", maxU, maxV );
		backgroundShader.Set( "ClipGain", clipGain );
	}
	else if( matte )
	{
		// Opaque, whatever Background Alpha says: a matte with a transparent
		// surround keys as "no information" rather than as black. Mix still
		// fades it, as it fades everything the source draws.
		const float level = output == SourceOutput::InverseMatte ? 1.0f : 0.0f;
		backgroundShader.Set( "BackColour", level, level, level, mix );
	}
	else
	{
		backgroundShader.Set( "BackColour",
		                      Clamp01( params[ PT_BACK_R ] ),
		                      Clamp01( params[ PT_BACK_G ] ),
		                      Clamp01( params[ PT_BACK_B ] ),
		                      Clamp01( params[ PT_BACK_OPACITY ] ) * mix );
	}

	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	//-----------------------------------------------------------------------
	// Pass 2: the shapes, in the order Solve put them -- oldest first, so the
	// newest is drawn last and lands on top.
	//-----------------------------------------------------------------------
	const int count = static_cast< int >( wagons.size() );
	if( count > 0 )
	{
		glUseProgram( shapeShader.GetGLID() );

		for( int i = 0; i < count; ++i )
		{
			const Wagon& w = wagons[ i ];
			float* x       = &xformScratch[ static_cast< size_t >( i ) * 4 ];
			float* t       = &tintScratch[ static_cast< size_t >( i ) * 4 ];

			x[ 0 ] = w.x;
			x[ 1 ] = w.y;
			x[ 2 ] = w.scale;
			x[ 3 ] = w.rotation;

			t[ 0 ] = w.r;
			t[ 1 ] = w.g;
			t[ 2 ] = w.b;
			t[ 3 ] = w.a;

			// The rectangle of the picture this shape shows, picked by its
			// release number so it keeps the same one for its whole run.
			float* c = &cellScratch[ static_cast< size_t >( i ) * 4 ];
			if( hasImage )
			{
				const ImagePick pick = static_cast< ImagePick >( Option( params[ PT_IMAGE_PICK ], static_cast< int >( ImagePick::Count ) ) );
				const int sprite     = static_cast< int >( std::lround( std::max( 0.0f, params[ PT_SPRITE ] ) ) );
				const ImageCell& r   = imageCells[ static_cast< size_t >( PickCell( pick, w.release, sprite, static_cast< int >( imageCells.size() ) ) ) ];
				c[ 0 ] = r.u0;
				c[ 1 ] = r.v0;
				c[ 2 ] = r.u1;
				c[ 3 ] = r.v1;
			}
			else
			{
				c[ 0 ] = 0.0f;
				c[ 1 ] = 0.0f;
				c[ 2 ] = 1.0f;
				c[ 3 ] = 1.0f;
			}
		}

		const GLint xformLoc = shapeShader.FindUniform( "Xform" );
		const GLint tintLoc  = shapeShader.FindUniform( "Tint" );
		const GLint cellLoc  = shapeShader.FindUniform( "Cell" );
		if( xformLoc >= 0 )
			glUniform4fv( xformLoc, count, xformScratch.data() );
		if( tintLoc >= 0 )
			glUniform4fv( tintLoc, count, tintScratch.data() );
		if( cellLoc >= 0 )
			glUniform4fv( cellLoc, count, cellScratch.data() );

		const float outline  = Clamp01( params[ PT_OUTLINE ] );
		const float softness = SoftnessFromParam( params[ PT_SOFTNESS ] );

		// The quad has to contain the shape, its outline, and its feather. Slack
		// on top because overdrawing a few pixels costs nothing and a shape that
		// reaches past its quad silently loses a corner.
		const float bound = ShapeBound( queue.shape ) + outline * 0.5f + softness * 2.0f + 0.05f;

		shapeShader.Set( "Resolution", static_cast< float >( width ), static_cast< float >( height ) );
		shapeShader.Set( "Bound", bound );
		shapeShader.Set( "Stretch", queue.stretch );
		shapeShader.Set( "ShapeKind", static_cast< int >( queue.shape ) );
		shapeShader.Set( "Roundness", Clamp01( params[ PT_ROUNDNESS ] ) );
		shapeShader.Set( "Outline", outline );
		shapeShader.Set( "Softness", softness );
		shapeShader.Set( "Shade", Clamp01( params[ PT_SHADE ] ) );
		shapeShader.Set( "LightAngle", Clamp01( params[ PT_LIGHT ] ) );
		shapeShader.Set( "SampleMode", sampleMode );
		shapeShader.Set( "Matte", output == SourceOutput::Matte ? 1 : output == SourceOutput::InverseMatte ? 2 : 0 );

		//-------------------------------------------------------------------
		// Shadows. The Light control says where the light is, so the shadow
		// falls the other way: the same angle the shading uses, in the same
		// y-down screen convention, negated.
		//-------------------------------------------------------------------
		const float lightTurn = Clamp01( params[ PT_LIGHT ] ) * 6.28318531f;
		const float distance  = ShadowDistanceFromParam( params[ PT_SHADOW_DISTANCE ] );
		const float blur      = ShadowBlurFromParam( params[ PT_SHADOW_BLUR ] );
		shapeShader.Set( "ShadowPass", shadows ? 1 : 0 );
		shapeShader.Set( "ShadowOffset", -std::cos( lightTurn ) * distance, std::sin( lightTurn ) * distance );
		shapeShader.Set( "ShadowBlur", shadows ? blur : 0.0f );
		shapeShader.Set( "ShadowAmount", shadowAmount );

		//-------------------------------------------------------------------
		// The picture, on texture unit 1: unit 0 is the effect's clip.
		//-------------------------------------------------------------------
		shapeShader.Set( "HasImage", hasImage ? 1 : 0 );
		shapeShader.Set( "ImageMix", Clamp01( params[ PT_IMAGE_MIX ] ) );
		shapeShader.Set( "SideTurn", SideRotation( queue.side ) );
		shapeShader.Set( "Imagery", 1 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, hasImage ? imageTexture : placeholderTexture );
		glActiveTexture( GL_TEXTURE0 );

		if( overInput )
		{
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, inputTexture );
			shapeShader.Set( "Clip", 0 );
			shapeShader.Set( "MaxUV", maxU, maxV );
		}

		if( mask == MaskMode::Hide )
		{
			// Punch the shapes out of what is already there: keep no source, and
			// scale the destination by the shape's transparency. This is the
			// whole of Hide, and it is why there is no mask buffer anywhere in
			// this plugin.
			glEnable( GL_BLEND );
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_ALPHA );
		}
		else
		{
			ApplyBlend( blend, fade );
		}

		// Two instances per shape when there are shadows -- its shadow, then
		// itself -- so each shadow lands on the older shapes and not on its own.
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, shadows ? count * 2 : count );

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
	}

	//-----------------------------------------------------------------------
	// Leave the state as the host is entitled to find it.
	//-----------------------------------------------------------------------
	glDisable( GL_BLEND );
	glBlendEquation( GL_FUNC_ADD );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	glBlendColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glUseProgram( 0 );
	glBindVertexArray( 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

FFResult ShuntPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	int width           = 0;
	int height          = 0;
	GLuint inputTexture = 0;
	float maxU          = 1.0f;
	float maxV          = 1.0f;

	if( overInput )
	{
		if( pGL == nullptr || pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;

		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		inputTexture                     = texture.Handle;
		width                            = texture.Width;
		height                           = texture.Height;

		// The input texture can be bigger than the picture; MaxUV is the
		// fraction that was really drawn. The shapes are placed in frame space
		// and never touch this -- only the fetch of the clip does.
		const FFGLTexCoords coords = GetMaxGLTexCoords( texture );
		maxU                       = coords.s;
		maxV                       = coords.t;
	}
	else
	{
		width  = static_cast< int >( currentViewport.width );
		height = static_cast< int >( currentViewport.height );
	}

	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	Render( width, height, inputTexture, maxU, maxV );
	return FF_SUCCESS;
}

} // namespace shunt
