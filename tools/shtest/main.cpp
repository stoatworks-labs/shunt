/**
    shtest — the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context. Not a reimplementation of the queue and not a
    preview: the thing under test is `ShuntPlugin`, compiled from the same
    objects that go into the bundles, and every number below comes out of a
    frame it actually rendered.

        --out PATH        render a frame
        --shapes PATH     a contact sheet of all eight primitives
        --sides PATH      a contact sheet of all four entry sides
        --list            parameters, with their types and defaults
        --clock           the host clock unit, measured rather than guessed
        --speed           a Speed change does not teleport the train
        --presets         every factory preset survives every host behaviour
        --tile            every shape's run is exactly one cycle long
        --place           where every shape landed, against Queue.cpp
        --gap             standing shapes really are one gap apart, both units
        --travel          the head really stops at Travel% of the frame
        --dwell           a standing shape really stands for Dwell of its cycle
        --order           the newest shape is drawn on top
        --round           circles stay round, and stay put, off 1:1
        --mask            the four effect mask modes
        --cost            ms/frame at 720p, 1080p and 4K
        --effect          use the effect variant (with --out, --list)

    ## What is measured on the picture and what is not

    `--tile` is arithmetic on Queue.cpp with no GL at all: it is the one that
    proves the invariant the whole design rests on, and it does not need a
    frame to do it.

    Everything else looks at pixels. `--place`, `--gap`, `--travel` and `--dwell`
    all ask Queue.cpp what should happen, then go and look at what the renderer
    actually drew — so they exercise the solver, the uniform upload, the vertex
    transform, the aspect correction, the distance function and the blend at
    once, and they fail if any one of them is wrong, including in ways a mirror
    test structurally cannot see (an instance array uploaded off by one, say).

    `--round` is separate because it catches one specific mistake the others are
    blind to: the train runs in frame space and shapes are sized in short-edge
    fractions, and confusing the two is invisible on a square render and turns
    every circle into an ellipse on a real 16:9 output.

    None of them catches a dead uniform. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Controls.h"
#include "Presets.h"
#include "Queue.h"
#include "Shapes.h"
#include "Shunt.h"

using namespace shunt;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );// bit depth
	ihdr.push_back( 6 );// truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, bottom row first.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// A test clip for the effect: coloured quadrants over a gradient, so that a
/// mask mode getting its geometry or its UV flip wrong is obvious rather than
/// merely plausible.
GLuint makeTestClip( int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( width );
			const float v = static_cast< float >( y ) / static_cast< float >( height );

			unsigned char* p = &pixels[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ] = static_cast< unsigned char >( ( u < 0.5f ? 220.0f : 40.0f ) * ( 0.4f + 0.6f * v ) );
			p[ 1 ] = static_cast< unsigned char >( ( v < 0.5f ? 200.0f : 60.0f ) * ( 0.4f + 0.6f * u ) );
			p[ 2 ] = static_cast< unsigned char >( 255.0f * ( 0.3f + 0.7f * ( 1.0f - v ) ) );
			p[ 3 ] = 255;
		}
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// Parameters by name.
//---------------------------------------------------------------------------
std::map< std::string, unsigned int > parameterIndex( ShuntPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* name = plugin.GetParamName( i );
		if( name != nullptr )
			byName[ name ] = i;
	}
	return byName;
}

bool applySetting( ShuntPlugin& plugin, const std::string& assignment )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		fprintf( stderr, "--set wants Name=value, got '%s'\n", assignment.c_str() );
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	const auto found                                   = byName.find( name );
	if( found == byName.end() )
	{
		fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	plugin.SetFloatParameter( found->second, std::stof( value ) );
	return true;
}

//---------------------------------------------------------------------------
// Rendering
//---------------------------------------------------------------------------
void render( ShuntPlugin& plugin, const Target& target, GLuint input = 0 )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	plugin.Render( target.width, target.height, input, 1.0f, 1.0f );
	glFinish();
}

/// Set up a plugin, initialise its GL, and hand it back ready to draw.
bool prepare( ShuntPlugin& plugin, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.x      = 0;
	viewport.y      = 0;
	viewport.width  = static_cast< unsigned int >( width );
	viewport.height = static_cast< unsigned int >( height );

	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		fprintf( stderr, "InitGL failed -- see the log\n" );
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------
// Measuring what was drawn
//---------------------------------------------------------------------------
double luminanceAt( const std::vector< unsigned char >& image, int width, int height, int col, int glRow )
{
	if( col < 0 || col >= width || glRow < 0 || glRow >= height )
		return 0.0;

	const unsigned char* p = &image[ ( static_cast< size_t >( glRow ) * width + col ) * 4 ];
	return ( 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ] ) / 255.0;
}

struct Blob
{
	double weight = 0.0;   ///< Total luminance found.
	double x      = 0.0;   ///< Centroid, in frame space (0..1, y down).
	double y      = 0.0;
	double spanX  = 0.0;   ///< Extent in pixels, from the second moment.
	double spanY  = 0.0;
};

/**
    The luminance-weighted centroid of whatever is inside a window centred on
    where a shape was predicted to be.

    A window rather than a global blob search on purpose. A global search would
    have to decide which blob belongs to which shape, and the obvious way to do
    that — nearest to the prediction — is exactly the assumption the test is
    supposed to be checking. Looking only where the shape was promised means a
    shape that is somewhere else registers as an empty window and fails, which
    is the answer we want.
*/
Blob measure( const std::vector< unsigned char >& image, int width, int height,
              double predictedX, double predictedY, double radiusPx )
{
	const double centreCol   = predictedX * width;
	const double centreGlRow = ( 1.0 - predictedY ) * height;

	const int reach = static_cast< int >( std::ceil( radiusPx * 1.6 ) ) + 2;

	Blob blob;
	double sumX = 0.0;
	double sumY = 0.0;

	const int col0 = static_cast< int >( std::floor( centreCol ) ) - reach;
	const int col1 = static_cast< int >( std::ceil( centreCol ) ) + reach;
	const int row0 = static_cast< int >( std::floor( centreGlRow ) ) - reach;
	const int row1 = static_cast< int >( std::ceil( centreGlRow ) ) + reach;

	for( int row = row0; row <= row1; ++row )
	{
		for( int col = col0; col <= col1; ++col )
		{
			const double l = luminanceAt( image, width, height, col, row );
			if( l <= 0.0 )
				continue;

			blob.weight += l;
			sumX += l * ( col + 0.5 );
			sumY += l * ( row + 0.5 );
		}
	}

	if( blob.weight <= 0.0 )
		return blob;

	const double meanCol   = sumX / blob.weight;
	const double meanGlRow = sumY / blob.weight;

	double varX = 0.0;
	double varY = 0.0;
	for( int row = row0; row <= row1; ++row )
	{
		for( int col = col0; col <= col1; ++col )
		{
			const double l = luminanceAt( image, width, height, col, row );
			if( l <= 0.0 )
				continue;

			varX += l * ( col + 0.5 - meanCol ) * ( col + 0.5 - meanCol );
			varY += l * ( row + 0.5 - meanGlRow ) * ( row + 0.5 - meanGlRow );
		}
	}

	blob.x     = meanCol / width;
	blob.y     = 1.0 - meanGlRow / height;
	blob.spanX = std::sqrt( varX / blob.weight );
	blob.spanY = std::sqrt( varY / blob.weight );

	return blob;
}

/**
    A lit run along the direction of travel.

    The gap, the travel distance and the dwell are all one-dimensional claims —
    they are about where things sit ALONG the track — so the natural measurement
    is a one-dimensional one: collapse the frame onto the travel axis and find
    the stretches that are lit.

    Deliberately not a centroid search. Two shapes a third of a thickness apart
    have overlapping centroid windows and the measurement quietly becomes a
    measurement of both of them; a profile does not care, and edge-to-edge
    abutment — the thing "a gap of one thickness" actually means — has no
    centroid answer at all.
*/
struct Run
{
	double start  = 0.0;   ///< First lit position, in pixels along the travel axis.
	double end    = 0.0;
	double centre = 0.0;
	double length = 0.0;
};

/// The per-column (or per-row) mean luminance, in TRAVEL-AXIS order: index 0 is
/// the entry edge, whichever edge that is.
std::vector< double > travelProfile( const std::vector< unsigned char >& image,
                                     int width, int height, Side side )
{
	const bool horizontal = TravelsHorizontally( side );
	const int span        = horizontal ? width : height;
	const int across      = horizontal ? height : width;

	std::vector< double > profile( static_cast< size_t >( span ), 0.0 );

	for( int i = 0; i < span; ++i )
	{
		double sum = 0.0;
		for( int j = 0; j < across; ++j )
		{
			// glRow counts up from the BOTTOM of the image; frame y counts down
			// from the top. Everything below is stated in frame coordinates, so
			// the flip happens here and nowhere else.
			const int col   = horizontal ? i : j;
			const int frameRow = horizontal ? j : i;
			sum += luminanceAt( image, width, height, col, height - 1 - frameRow );
		}
		profile[ static_cast< size_t >( i ) ] = sum / static_cast< double >( across );
	}

	// Left and Top already run from the entry edge; Right and Bottom come in
	// from the far end, so reverse them and every measurement below reads as a
	// distance from the entry edge regardless of which side was chosen.
	if( side == Side::Right || side == Side::Bottom )
		std::reverse( profile.begin(), profile.end() );

	return profile;
}

/// Maximal stretches of the profile above `fraction` of its peak, with
/// sub-pixel ends from linear interpolation across the threshold crossing.
std::vector< Run > profileRuns( const std::vector< double >& profile, double fraction )
{
	std::vector< Run > runs;
	if( profile.empty() )
		return runs;

	const double peak = *std::max_element( profile.begin(), profile.end() );
	if( peak <= 1e-6 )
		return runs;

	const double level = peak * fraction;

	bool inside = false;
	double startPx = 0.0;

	for( size_t i = 0; i < profile.size(); ++i )
	{
		const bool lit = profile[ i ] >= level;

		if( lit && !inside )
		{
			// Where the profile crossed the level between i-1 and i.
			const double previous = i > 0 ? profile[ i - 1 ] : 0.0;
			const double t        = ( profile[ i ] > previous )
			                        ? ( level - previous ) / ( profile[ i ] - previous )
			                        : 0.0;
			startPx = static_cast< double >( i ) - 1.0 + std::min( 1.0, std::max( 0.0, t ) ) + 0.5;
			inside  = true;
		}
		else if( !lit && inside )
		{
			const double previous = profile[ i - 1 ];
			const double t        = ( previous > profile[ i ] )
			                        ? ( previous - level ) / ( previous - profile[ i ] )
			                        : 0.0;
			const double endPx    = static_cast< double >( i ) - 1.0 + std::min( 1.0, std::max( 0.0, t ) ) + 0.5;

			Run run;
			run.start  = startPx;
			run.end    = endPx;
			run.centre = 0.5 * ( startPx + endPx );
			run.length = endPx - startPx;
			runs.push_back( run );
			inside = false;
		}
	}

	if( inside )
	{
		Run run;
		run.start  = startPx;
		run.end    = static_cast< double >( profile.size() );
		run.centre = 0.5 * ( startPx + run.end );
		run.length = run.end - startPx;
		runs.push_back( run );
	}

	return runs;
}

/// The shape's radius in pixels, from the same convention Queue.h documents: a
/// fraction of the SHORT edge.
double radiusPixels( double scale, int width, int height )
{
	return scale * std::min( width, height );
}

/// The 0..1 host value that produces a given shape count.
float countParam( int wanted )
{
	// CountFromParam is 1 + round( 63 v^2 ); invert it.
	const float v = std::sqrt( static_cast< float >( wanted - 1 ) / 63.0f );
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

/// The 0..1 host value that produces a given Gap, in whichever unit.
float gapParam( float wanted, bool pixels )
{
	return std::min( 1.0f, std::max( 0.0f, wanted / ( pixels ? 512.0f : 4.0f ) ) );
}

/// The 0..1 host value that produces a given Dwell fraction.
float dwellParam( float wanted )
{
	return std::min( 1.0f, std::max( 0.0f, wanted / 0.95f ) );
}

/// The 0..1 host value that produces a given Size, in short-edge fractions.
float sizeParam( float wanted )
{
	const float v = std::log( wanted / 0.005f ) / std::log( 0.5f / 0.005f );
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters( ShuntPlugin& plugin )
{
	printf( "%-4s %-20s %-10s %s\n", "id", "name", "type", "default" );
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* name        = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );

		const char* typeName = "standard";
		if( type == FF_TYPE_BOOLEAN )
			typeName = "boolean";
		else if( type == FF_TYPE_OPTION )
			typeName = "option";
		else if( type == FF_TYPE_RED || type == FF_TYPE_GREEN || type == FF_TYPE_BLUE )
			typeName = "colour";
		else if( type == FF_TYPE_TEXT )
			typeName = "text";
		else if( type == FF_TYPE_BUFFER )
			typeName = "buffer";

		printf( "%-4u %-20s %-10s %.4f\n", i, name != nullptr ? name : "?", typeName,
		        plugin.GetFloatParameter( i ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --tile
//
// The invariant the whole design rests on: every shape's run is exactly one
// cycle long, whatever the slot, the travel and the gap. No GL.
//---------------------------------------------------------------------------
int tileCheck()
{
	struct Case
	{
		const char* name;
		Side side;
		int count;
		float travel;
		float dwell;
		float gapValue;
		GapUnits units;
		Shape shape;
		float aspect;
	};

	const Case cases[] = {
		{ "default",        Side::Left,   8,  0.70f, 0.45f, 0.68f, GapUnits::Thickness, Shape::Bar,    16.0f / 9.0f },
		{ "no dwell",       Side::Left,   6,  0.60f, 0.00f, 1.00f, GapUnits::Thickness, Shape::Circle, 16.0f / 9.0f },
		{ "long dwell",     Side::Right,  5,  0.90f, 0.95f, 1.50f, GapUnits::Thickness, Shape::Square, 16.0f / 9.0f },
		{ "travel 0",       Side::Top,    4,  0.00f, 0.50f, 1.00f, GapUnits::Thickness, Shape::Circle, 1.0f },
		{ "travel 1",       Side::Bottom, 7,  1.00f, 0.30f, 0.50f, GapUnits::Thickness, Shape::Bar,    2.39f },
		{ "gap 0",          Side::Left,   9,  0.50f, 0.60f, 0.00f, GapUnits::Thickness, Shape::Star,   16.0f / 9.0f },
		{ "queue overruns", Side::Left,  40,  0.40f, 0.40f, 2.00f, GapUnits::Thickness, Shape::Circle, 16.0f / 9.0f },
		{ "pixel gap",      Side::Top,   12,  0.80f, 0.55f, 64.0f, GapUnits::Pixels,    Shape::Hexagon, 9.0f / 16.0f },
		{ "one shape",      Side::Right,  1,  0.75f, 0.25f, 1.00f, GapUnits::Thickness, Shape::Ring,   16.0f / 9.0f },
	};

	int failures = 0;
	int checked  = 0;

	for( const Case& c : cases )
	{
		QueueParams p;
		p.side      = c.side;
		p.count     = c.count;
		p.travel    = c.travel;
		p.dwell     = c.dwell;
		p.gapUnits  = c.units;
		p.gapValue  = c.gapValue;
		p.shape     = c.shape;
		p.aspect    = c.aspect;
		p.size      = 0.05f;
		p.stretch   = 1.0f;
		p.angle     = 0.25f;
		p.spanPixels = 1920;

		const float m = Margin( p );
		const float v = SlideSpeed( p );

		for( int slot = 0; slot < c.count; ++slot )
		{
			const float park   = SlotDepth( p, slot );
			const float arrive = ( park + m ) / v;
			const float leave  = ( 1.0f + m - park ) / v;

			//---------------------------------------------------------------
			// The claim, stated once: in, stand, out adds up to one cycle.
			//---------------------------------------------------------------
			const double total = static_cast< double >( arrive ) + c.dwell + static_cast< double >( leave );
			if( std::fabs( total - 1.0 ) > 2e-5 )
			{
				printf( "  FAIL %-15s slot %2d: run is %.6f cycles, not 1\n", c.name, slot, total );
				++failures;
			}

			//---------------------------------------------------------------
			// And the same thing seen from the renderer's side: the depth at
			// the very start of a run is off-stage at the near edge, and at the
			// very end it is off-stage at the far edge.
			//---------------------------------------------------------------
			QueueParams at = p;

			at.phase = static_cast< float >( slot ) / static_cast< float >( c.count );
			const Wagon first = SolveSlot( at, slot );
			if( std::fabs( first.depth - ( -m ) ) > 1e-4f )
			{
				printf( "  FAIL %-15s slot %2d: starts at depth %.5f, expected %.5f\n",
				        c.name, slot, first.depth, -m );
				++failures;
			}

			// A hair before the end of the run, which is a hair before fully
			// off-stage -- and at a long Dwell the slide speed is high, so that
			// hair is worth several thousandths of the frame. Predict it rather
			// than allowing for it, or the tolerance has to be loose enough to
			// hide a real error.
			constexpr float kEps = 1e-4f;
			at.phase = static_cast< float >( slot ) / static_cast< float >( c.count ) - kEps;
			const Wagon last     = SolveSlot( at, slot );
			const float endsAt   = 1.0f + m - v * kEps;
			if( std::fabs( last.depth - endsAt ) > 1e-4f )
			{
				printf( "  FAIL %-15s slot %2d: ends at depth %.5f, expected %.5f\n",
				        c.name, slot, last.depth, endsAt );
				++failures;
			}

			//---------------------------------------------------------------
			// Nothing ever moves backwards, and what it stands at is the slot
			// depth. Sampled densely rather than at the thresholds, because a
			// sign error inside one of the three branches would sit exactly
			// between them.
			//---------------------------------------------------------------
			float previous  = -m - 1.0f;
			int standingHits = 0;
			for( int step = 0; step < 2000; ++step )
			{
				at.phase = static_cast< float >( slot ) / static_cast< float >( c.count )
				           + static_cast< float >( step ) / 2000.0f;
				const Wagon w = SolveSlot( at, slot );

				if( w.depth < previous - 1e-4f )
				{
					printf( "  FAIL %-15s slot %2d: depth went backwards at age %.4f (%.5f after %.5f)\n",
					        c.name, slot, w.age, w.depth, previous );
					++failures;
					break;
				}
				previous = w.depth;

				if( w.standing )
				{
					++standingHits;
					if( std::fabs( w.depth - park ) > 1e-5f )
					{
						printf( "  FAIL %-15s slot %2d: standing at %.5f, slot is at %.5f\n",
						        c.name, slot, w.depth, park );
						++failures;
						break;
					}
				}
			}

			// The number of samples that found it standing is the dwell, and it
			// is the same dwell for every slot -- which is the half of the
			// invariant the depth checks above do not see.
			const double standingFraction = standingHits / 2000.0;
			if( std::fabs( standingFraction - c.dwell ) > 0.002 )
			{
				printf( "  FAIL %-15s slot %2d: stands for %.4f of its cycle, Dwell says %.4f\n",
				        c.name, slot, standingFraction, c.dwell );
				++failures;
			}

			++checked;
		}

		//-------------------------------------------------------------------
		// The runs TILE: at any phase there is exactly one shape in each
		// count-th of a cycle. That is what says there is never a hole in the
		// procession and never two shapes wanting one slot.
		//-------------------------------------------------------------------
		for( int step = 0; step < 97; ++step )
		{
			QueueParams at = p;
			at.phase       = 0.013f + static_cast< float >( step ) * 0.0137f;

			// Sorted ages, not buckets. The ages sit exactly ON the boundaries
			// of any count-wide bucketing whenever phase * count is near a whole
			// number, so a bucket count measures float rounding rather than the
			// invariant -- it duly reported 2-in-a-bucket at count 40. The gaps
			// between sorted ages say the same thing and cannot straddle
			// anything.
			std::vector< double > ages;
			ages.reserve( static_cast< size_t >( c.count ) );
			for( int slot = 0; slot < c.count; ++slot )
				ages.push_back( SlotAge( at, slot ) );
			std::sort( ages.begin(), ages.end() );

			const double want = 1.0 / c.count;
			for( size_t i = 0; i < ages.size(); ++i )
			{
				// Round the cycle, so the last shape's gap back to the first is
				// checked too -- that is the one a fencepost error lands in.
				const double next = ( i + 1 < ages.size() ) ? ages[ i + 1 ] : ages[ 0 ] + 1.0;
				const double gap  = next - ages[ i ];
				if( std::fabs( gap - want ) > 1e-4 )
				{
					printf( "  FAIL %-15s phase %.4f: shapes %.6f apart in the cycle, expected %.6f\n",
					        c.name, at.phase, gap, want );
					++failures;
					break;
				}
			}
		}

		if( failures == 0 )
			printf( "  ok   %-15s %2d slots tile the cycle exactly\n", c.name, c.count );
	}

	printf( "tile: %d slot runs across %zu configurations, %d failures\n",
	        checked, sizeof( cases ) / sizeof( cases[ 0 ] ), failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --place
//---------------------------------------------------------------------------
int placeCheck()
{
	struct Case
	{
		const char* name;
		Side side;
		int count;
		int width;
		int height;
		float phase;
		float dwell;
	};

	const Case cases[] = {
		{ "from left 16:9",   Side::Left,   6, 1280,  720, 0.000f, 0.50f },
		{ "from left late",   Side::Left,   6, 1280,  720, 3.137f, 0.50f },
		{ "from right 16:9",  Side::Right,  5, 1280,  720, 1.611f, 0.70f },
		{ "from top 4:3",     Side::Top,    4,  800,  600, 0.400f, 0.30f },
		{ "from bottom 1:1",  Side::Bottom, 7,  720,  720, 2.137f, 0.60f },
		{ "from left 2.39:1", Side::Left,   9, 1148,  480, 5.611f, 0.45f },
		{ "from top portrait",Side::Top,    5,  720, 1280, 0.800f, 0.55f },
	};

	// A shape has to be found within this of where Queue.cpp said it would be.
	// One and a half pixels: an antialiased centroid lands well inside a pixel,
	// so this is loose enough not to be flaky and tight enough that a genuine
	// half-cell error cannot hide in it.
	const double kTolerancePx = 1.5;

	int failures = 0;
	int checked  = 0;

	for( const Case& c : cases )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, c.width, c.height ) )
			return 1;

		plugin.SetFloatParameter( PT_SIDE, static_cast< float >( c.side ) );
		plugin.SetFloatParameter( PT_WAGONS, countParam( c.count ) );
		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Circle ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.03f ) );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( c.dwell ) );
		// Well clear of each other, so a centroid window holds one shape.
		plugin.SetFloatParameter( PT_GAP, gapParam( 2.2f, false ) );
		plugin.SetPhaseOverride( c.phase );

		Target target = makeTarget( c.width, c.height );
		render( plugin, target );
		const std::vector< unsigned char > image = readBytes( target );

		const QueueParams queue = plugin.CurrentQueue( c.width, c.height );

		//-------------------------------------------------------------------
		// A centroid in a window can only answer for a shape that is wholly in
		// the frame and on its own. Shapes in this plugin deliberately slide
		// over one another, and at any given phase some of them are halfway
		// off-stage -- so both of those have to be excluded here, and they are
		// covered instead by the checks that are built for them: --gap measures
		// overlapping shapes on a profile, --order measures which of two
		// overlapping shapes is on top, and --dwell watches one shape across a
		// whole cycle.
		//
		// Skipped, not silently passed: the count is printed, and a run that
		// skipped everything would say so.
		//-------------------------------------------------------------------
		int skipped = 0;

		for( int slot = 0; slot < c.count; ++slot )
		{
			// Predicted independently of what Render happened to place, so a
			// solver called with the wrong arguments is still caught.
			const Wagon expected = SolveSlot( queue, slot );

			const double radius = radiusPixels( expected.scale, c.width, c.height );

			// Wholly in frame, with a radius to spare: a shape clipped by the
			// edge has its centroid pulled inwards, and that is the measurement
			// being wrong rather than the plugin.
			const double px = expected.x * c.width;
			const double py = expected.y * c.height;
			if( px < radius * 1.6 || px > c.width - radius * 1.6
			    || py < radius * 1.6 || py > c.height - radius * 1.6 )
			{
				++skipped;
				continue;
			}

			// On its own: nothing else within three radii, or the window holds
			// two shapes and measures the pair.
			bool crowded = false;
			for( int other = 0; other < c.count && !crowded; ++other )
			{
				if( other == slot )
					continue;
				const Wagon w  = SolveSlot( queue, other );
				const double dx = ( w.x - expected.x ) * c.width;
				const double dy = ( w.y - expected.y ) * c.height;
				crowded = std::sqrt( dx * dx + dy * dy ) < radius * 3.0;
			}
			if( crowded )
			{
				++skipped;
				continue;
			}

			const Blob blob = measure( image, c.width, c.height, expected.x, expected.y, radius );

			// A filled circle of this radius, at full brightness. Half of it is
			// generous: what is being tested is "the shape is here", and the
			// window's edges clip a little of it.
			const double expectedWeight = 3.14159 * radius * radius * 0.5;

			if( blob.weight < expectedWeight )
			{
				printf( "  FAIL %-18s slot %2d: expected light at (%.4f, %.4f), found %.0f of %.0f\n",
				        c.name, slot, expected.x, expected.y, blob.weight, expectedWeight );
				++failures;
				continue;
			}

			const double dx = ( blob.x - expected.x ) * c.width;
			const double dy = ( blob.y - expected.y ) * c.height;
			const double d  = std::sqrt( dx * dx + dy * dy );

			if( d > kTolerancePx )
			{
				printf( "  FAIL %-18s slot %2d: %.2f px from the prediction (%.4f, %.4f) vs (%.4f, %.4f)\n",
				        c.name, slot, d, blob.x, blob.y, expected.x, expected.y );
				++failures;
			}

			++checked;
		}

		releaseTarget( target );
		plugin.DeInitGL();

		printf( "  ok   %-18s %dx%d, %d of %d shapes measured (%d overlapping or at the edge)\n",
		        c.name, c.width, c.height, c.count - skipped, c.count, skipped );
	}

	if( checked < 20 )
	{
		printf( "place: only %d shapes were measurable -- the test has gone stale\n", checked );
		return 1;
	}

	printf( "place: %d shapes measured, all within %.1f px%s\n",
	        checked, kTolerancePx, failures == 0 ? "" : " -- WITH FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --gap and --travel
//
// One rendered frame with the whole train standing, collapsed onto the travel
// axis. Both claims are read straight off the profile.
//---------------------------------------------------------------------------

/// Set up a plugin showing a fully standing train of bars, and find a phase at
/// which every shape in it is standing. Returns false if no such phase exists,
/// which is a legitimate answer for a short dwell and a failure for a long one.
bool standingPhase( const QueueParams& probe, int count, float& phaseOut )
{
	for( int step = 0; step < 4000; ++step )
	{
		QueueParams at = probe;
		at.phase       = static_cast< float >( step ) / 4000.0f;

		bool all = true;
		for( int slot = 0; slot < count && all; ++slot )
			all = SolveSlot( at, slot ).standing;

		if( all )
		{
			phaseOut = at.phase;
			return true;
		}
	}
	return false;
}

struct GapCase
{
	const char* name;
	Side side;
	int width;
	int height;
	int count;
	bool pixels;
	float gap;        ///< thicknesses, or pixels
	float size;       ///< short-edge fraction
};

int gapAndTravelCheck( bool wantGap, bool wantTravel )
{
	const GapCase cases[] = {
		// Thicknesses. 1.0 is the claim that matters: exactly touching.
		{ "abut, left 1080p",   Side::Left,  1920, 1080, 5, false, 1.0f,  0.030f },
		{ "abut, top 1080p",    Side::Top,   1920, 1080, 5, false, 1.0f,  0.030f },
		{ "separated 2.0x",     Side::Left,  1920, 1080, 5, false, 2.0f,  0.030f },
		{ "separated 1.6x",     Side::Right, 1280,  720, 4, false, 1.6f,  0.035f },
		{ "overlap 0.5x",       Side::Left,  1920, 1080, 4, false, 0.5f,  0.030f },

		// Pixels. The same number of pixels at two resolutions is the whole
		// point of the unit.
		{ "64 px at 1080p",     Side::Left,  1920, 1080, 5, true,  64.0f, 0.018f },
		{ "64 px at 720p",      Side::Left,  1280,  720, 5, true,  64.0f, 0.018f },
		{ "48 px from bottom",  Side::Bottom, 720, 1280, 5, true,  48.0f, 0.018f },
	};

	int failures = 0;

	for( const GapCase& c : cases )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, c.width, c.height ) )
			return 1;

		// Bars turned across the track. A rung has a flat profile along the
		// direction of travel, so a threshold crossing IS its edge — which is
		// what makes "these two are touching" a measurable statement rather
		// than an impression.
		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Bar ) );
		plugin.SetFloatParameter( PT_ANGLE, 0.25f );
		plugin.SetFloatParameter( PT_STRETCH, 0.75f );   // long across the track
		plugin.SetFloatParameter( PT_SIDE, static_cast< float >( c.side ) );
		plugin.SetFloatParameter( PT_WAGONS, countParam( c.count ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( c.size ) );
		plugin.SetFloatParameter( PT_TRAVEL, 0.8f );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( 0.9f ) );
		plugin.SetFloatParameter( PT_GAP_UNITS,
		                          static_cast< float >( c.pixels ? GapUnits::Pixels : GapUnits::Thickness ) );
		plugin.SetFloatParameter( PT_GAP, gapParam( c.gap, c.pixels ) );
		// Max, so that overlapping white bars stay one flat-topped run rather
		// than a brighter stripe in the middle that the threshold would read as
		// a separate shape.
		plugin.SetFloatParameter( PT_BLEND, static_cast< float >( Blend::Max ) );
		plugin.SetPhaseOverride( 0.0f );

		const QueueParams probe = plugin.CurrentQueue( c.width, c.height );

		float phase = 0.0f;
		if( !standingPhase( probe, c.count, phase ) )
		{
			printf( "  FAIL %-18s no phase has the whole train standing\n", c.name );
			++failures;
			plugin.DeInitGL();
			continue;
		}
		plugin.SetPhaseOverride( phase );

		Target target = makeTarget( c.width, c.height );
		render( plugin, target );
		const std::vector< unsigned char > image = readBytes( target );

		const QueueParams queue = plugin.CurrentQueue( c.width, c.height );
		const int span          = TravelsHorizontally( c.side ) ? c.width : c.height;

		const std::vector< double > profile = travelProfile( image, c.width, c.height, c.side );
		const std::vector< Run > runs       = profileRuns( profile, 0.5 );

		const double gapSpanPx      = static_cast< double >( GapSpan( queue ) ) * span;
		const double thicknessPx    = 2.0 * static_cast< double >( HalfThickness( queue ) ) * span;

		if( wantGap )
		{
			if( c.gap < 1.0f && !c.pixels )
			{
				//-----------------------------------------------------------
				// Overlapping: one merged run, and its length says how much
				// they overlap. N shapes at pitch g cover (N-1)g + thickness.
				//-----------------------------------------------------------
				if( runs.size() != 1 )
				{
					printf( "  FAIL %-18s overlapping shapes gave %zu runs, expected 1\n",
					        c.name, runs.size() );
					++failures;
				}
				else
				{
					const double expected = ( c.count - 1 ) * gapSpanPx + thicknessPx;
					const double got      = runs[ 0 ].length;
					if( std::fabs( got - expected ) > 2.0 )
					{
						printf( "  FAIL %-18s merged run is %.2f px, expected %.2f\n",
						        c.name, got, expected );
						++failures;
					}
					else
						printf( "  ok   %-18s %d shapes overlapping %.0f%% -> one run of %.1f px (%.1f predicted)\n",
						        c.name, c.count, ( 1.0 - c.gap ) * 100.0, got, expected );
				}
			}
			else if( std::fabs( c.gap - 1.0f ) < 1e-6f && !c.pixels )
			{
				//-----------------------------------------------------------
				// Exactly touching: the whole train is one run, N thicknesses
				// long. This is the definition of "a gap of one thickness",
				// and it is the one a centroid measurement cannot make.
				//-----------------------------------------------------------
				if( runs.size() != 1 )
				{
					printf( "  FAIL %-18s abutting shapes gave %zu runs, expected 1\n",
					        c.name, runs.size() );
					++failures;
				}
				else
				{
					const double expected = c.count * thicknessPx;
					const double got      = runs[ 0 ].length;
					if( std::fabs( got - expected ) > 2.0 )
					{
						printf( "  FAIL %-18s abutting run is %.2f px, expected %.2f (%d x %.2f)\n",
						        c.name, got, expected, c.count, thicknessPx );
						++failures;
					}
					else
						printf( "  ok   %-18s %d shapes edge to edge -> %.1f px, %d x %.2f predicted\n",
						        c.name, c.count, got, c.count, thicknessPx );
				}
			}
			else
			{
				//-----------------------------------------------------------
				// Separated: one run per shape, and the pitch between their
				// centres is the gap.
				//-----------------------------------------------------------
				if( static_cast< int >( runs.size() ) != c.count )
				{
					printf( "  FAIL %-18s expected %d separate shapes, found %zu runs\n",
					        c.name, c.count, runs.size() );
					++failures;
				}
				else
				{
					double worst = 0.0;
					for( size_t i = 1; i < runs.size(); ++i )
					{
						const double pitch = std::fabs( runs[ i ].centre - runs[ i - 1 ].centre );
						worst = std::max( worst, std::fabs( pitch - gapSpanPx ) );
					}

					if( worst > 1.0 )
					{
						printf( "  FAIL %-18s worst pitch error %.2f px against %.2f px expected\n",
						        c.name, worst, gapSpanPx );
						++failures;
					}
					else
						printf( "  ok   %-18s %d shapes, pitch %.2f px (asked %s), worst error %.2f px\n",
						        c.name, c.count, gapSpanPx,
						        c.pixels ? "in pixels" : "in thicknesses", worst );
				}
			}
		}

		if( wantTravel )
		{
			//---------------------------------------------------------------
			// The head of the queue stops at Travel of the way across, measured
			// from the entry edge in pixels. The head is the LAST run along the
			// travel axis, because everything else is banked up behind it.
			//---------------------------------------------------------------
			if( runs.empty() )
			{
				printf( "  FAIL %-18s nothing lit to measure Travel against\n", c.name );
				++failures;
			}
			else
			{
				const double expected = static_cast< double >( queue.travel ) * span;
				const double got      = runs.back().end - 0.5 * thicknessPx;

				if( std::fabs( got - expected ) > 1.5 )
				{
					printf( "  FAIL %-18s head stopped %.2f px in, Travel says %.2f\n",
					        c.name, got, expected );
					++failures;
				}
				else if( !wantGap )
					printf( "  ok   %-18s head stops %.1f px in (%.1f predicted, Travel %.0f%%)\n",
					        c.name, got, expected, queue.travel * 100.0f );
			}
		}

		releaseTarget( target );
		plugin.DeInitGL();
	}

	printf( "%s: %zu configurations, %d failures\n",
	        wantGap && wantTravel ? "gap+travel" : ( wantGap ? "gap" : "travel" ),
	        sizeof( cases ) / sizeof( cases[ 0 ] ), failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --dwell
//
// The one dial, measured at both ends, on rendered frames.
//---------------------------------------------------------------------------
int dwellCheck()
{
	const int width  = 1280;
	const int height = 720;
	const int frames = 240;   // frames per cycle in this measurement
	const int count  = 6;

	struct Case
	{
		const char* name;
		float dwell;
		bool expectFullStack;   ///< is there a phase with the whole train standing?
	};

	const Case cases[] = {
		{ "short", 0.10f, false },
		{ "half",  0.50f, false },
		{ "long",  0.90f, true  },
	};

	int failures = 0;

	for( const Case& c : cases )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, width, height ) )
			return 1;

		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Circle ) );
		plugin.SetFloatParameter( PT_SIDE, static_cast< float >( Side::Left ) );
		// ONE shape for the rendered half of this check. The whole point of the
		// plugin is that shapes pass over one another, so in a full train the
		// head's centroid window is crossed by every other shape in turn and
		// what gets measured is the crossing, not the standing -- it read 92 of
		// an expected 120 frames before this was cut back to one shape.
		//
		// That the dwell is the SAME for every slot is not taken on trust
		// because of it: --tile measures the standing fraction of every slot in
		// every configuration, without a renderer in the way.
		plugin.SetFloatParameter( PT_WAGONS, countParam( 1 ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.03f ) );
		plugin.SetFloatParameter( PT_TRAVEL, 0.8f );
		plugin.SetFloatParameter( PT_GAP, gapParam( 2.2f, false ) );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( c.dwell ) );

		Target target = makeTarget( width, height );

		//-------------------------------------------------------------------
		// Walk one cycle and watch the shape frame by frame, measuring where it
		// actually is rather than asking the solver.
		//-------------------------------------------------------------------
		int stillFrames  = 0;
		int seenFrames   = 0;
		double previousX = -1.0;

		for( int frame = 0; frame < frames; ++frame )
		{
			const float phase = static_cast< float >( frame ) / static_cast< float >( frames );
			plugin.SetPhaseOverride( phase );

			render( plugin, target );
			const std::vector< unsigned char > image = readBytes( target );

			const QueueParams queue = plugin.CurrentQueue( width, height );
			const Wagon head        = SolveSlot( queue, 0 );

			if( head.depth < 0.02f || head.depth > 0.98f )
			{
				previousX = -1.0;
				continue;   // off-stage or half off it: nothing to measure
			}

			const double radius = radiusPixels( head.scale, width, height );
			const Blob blob     = measure( image, width, height, head.x, head.y, radius );
			if( blob.weight <= 0.0 )
			{
				previousX = -1.0;
				continue;
			}

			const double x = blob.x * width;
			if( previousX >= 0.0 )
			{
				++seenFrames;
				if( std::fabs( x - previousX ) < 0.5 )
					++stillFrames;
			}
			previousX = x;
		}

		// The head is on stage for (1 - its entry run) of the cycle, and stands
		// for `dwell` of it -- so the frames it does not move should be
		// dwell * frames, within the couple of frames the two ends round away.
		const double expectedStill = c.dwell * frames;
		if( std::fabs( stillFrames - expectedStill ) > 4.0 )
		{
			printf( "  FAIL dwell %-6s the shape stood for %d of %d frames, Dwell says %.0f\n",
			        c.name, stillFrames, frames, expectedStill );
			++failures;
		}
		else
			printf( "  ok   dwell %-6s the shape stood for %d frames of %d, %.0f predicted (%d on stage)\n",
			        c.name, stillFrames, frames, expectedStill, seenFrames );

		//-------------------------------------------------------------------
		// And the behaviour either end of the dial, on a real train this time:
		// does it ever stand complete, and is the front ever leaving while the
		// back is still arriving?
		//-------------------------------------------------------------------
		plugin.SetFloatParameter( PT_WAGONS, countParam( count ) );
		const QueueParams probe = plugin.CurrentQueue( width, height );

		float unused = 0.0f;
		const bool fullStack = standingPhase( probe, count, unused );
		if( fullStack != c.expectFullStack )
		{
			printf( "  FAIL dwell %-6s whole train standing at some phase: %s, expected %s\n",
			        c.name, fullStack ? "yes" : "no", c.expectFullStack ? "yes" : "no" );
			++failures;
		}

		bool sawOverlap = false;
		for( int step = 0; step < 2000 && !sawOverlap; ++step )
		{
			QueueParams at = probe;
			at.phase       = static_cast< float >( step ) / 2000.0f;

			bool someoneLeaving  = false;
			bool someoneArriving = false;
			for( int slot = 0; slot < count; ++slot )
			{
				const Wagon w = SolveSlot( at, slot );
				if( w.standing )
					continue;
				if( w.depth > SlotDepth( at, slot ) )
					someoneLeaving = true;
				else
					someoneArriving = true;
			}
			sawOverlap = someoneLeaving && someoneArriving;
		}

		// A short dwell HAS to show it -- that is the end of the dial where the
		// queue empties from the front while it is still filling from the back.
		if( c.dwell <= 0.5f && !sawOverlap )
		{
			printf( "  FAIL dwell %-6s never has one shape leaving while another arrives\n", c.name );
			++failures;
		}
		else
			printf( "       dwell %-6s full stack %-3s, leaving-while-arriving %s\n",
			        c.name, fullStack ? "yes" : "no", sawOverlap ? "yes" : "no" );

		releaseTarget( target );
		plugin.DeInitGL();
	}

	printf( "dwell: %zu settings, %d failures\n", sizeof( cases ) / sizeof( cases[ 0 ] ), failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --order
//
// "The leading edge of #2 slides over the trailing edge of #1" -- the newest
// shape is drawn on top. Measured by colour, in the overlap.
//---------------------------------------------------------------------------
int orderCheck()
{
	const int width  = 1280;
	const int height = 720;
	const int count  = 5;

	int failures = 0;
	int checked  = 0;

	// A spread of phases, so the check sees the draw order AFTER it has rotated
	// as well as before. A fixed order would pass at one phase and be wrong for
	// most of the cycle, which is exactly the bug worth catching.
	const float phases[] = { 0.05f, 0.23f, 0.41f, 0.67f, 0.88f };

	for( float phase : phases )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, width, height ) )
			return 1;

		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Bar ) );
		plugin.SetFloatParameter( PT_ANGLE, 0.25f );
		plugin.SetFloatParameter( PT_STRETCH, 0.8f );
		plugin.SetFloatParameter( PT_SIDE, static_cast< float >( Side::Left ) );
		plugin.SetFloatParameter( PT_WAGONS, countParam( count ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.05f ) );
		plugin.SetFloatParameter( PT_TRAVEL, 0.8f );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( 0.9f ) );
		// Half a thickness, so consecutive shapes overlap by half.
		plugin.SetFloatParameter( PT_GAP, gapParam( 0.5f, false ) );
		// A hue per slot, so who is on top is a question the pixel can answer.
		plugin.SetFloatParameter( PT_COLOUR_MODE, static_cast< float >( ColourMode::HueSpread ) );
		plugin.SetFloatParameter( PT_HUE_SPREAD, 1.0f );
		// Over: Max is commutative and would throw the order away, which is the
		// documented trade and would make this test meaningless.
		plugin.SetFloatParameter( PT_BLEND, static_cast< float >( Blend::Over ) );
		plugin.SetPhaseOverride( phase );

		Target target = makeTarget( width, height );
		render( plugin, target );
		const std::vector< unsigned char > image = readBytes( target );

		const QueueParams queue = plugin.CurrentQueue( width, height );

		//-------------------------------------------------------------------
		// The draw order the renderer actually used, straight from the wagons
		// it uploaded. Ages must be non-increasing: oldest down first.
		//-------------------------------------------------------------------
		const std::vector< Wagon >& drawn = plugin.LastWagons();
		for( size_t i = 1; i < drawn.size(); ++i )
		{
			if( drawn[ i ].age > drawn[ i - 1 ].age + 1e-6f )
			{
				printf( "  FAIL phase %.2f: draw order is not oldest-first (%.4f then %.4f)\n",
				        phase, drawn[ i - 1 ].age, drawn[ i ].age );
				++failures;
				break;
			}
		}

		//-------------------------------------------------------------------
		// And on the picture: in the overlap between two neighbouring slots,
		// the colour is the YOUNGER one's.
		//-------------------------------------------------------------------
		for( int slot = 0; slot + 1 < count; ++slot )
		{
			const Wagon a = SolveSlot( queue, slot );
			const Wagon b = SolveSlot( queue, slot + 1 );

			if( !a.standing || !b.standing )
				continue;

			const Wagon& younger = ( a.age < b.age ) ? a : b;

			// Midway between two centres half a thickness apart is inside both.
			const double px = 0.5 * ( a.x + b.x ) * width;
			const double py = 0.5 * ( a.y + b.y ) * height;

			const int col   = static_cast< int >( px );
			const int glRow = static_cast< int >( ( 1.0 - py / height ) * height );

			if( col < 0 || col >= width || glRow < 0 || glRow >= height )
				continue;

			const unsigned char* p = &image[ ( static_cast< size_t >( glRow ) * width + col ) * 4 ];
			const double r = p[ 0 ] / 255.0;
			const double g = p[ 1 ] / 255.0;
			const double bl = p[ 2 ] / 255.0;

			if( r + g + bl < 0.1 )
				continue;   // not actually in the overlap at this phase

			const double dYoung = std::fabs( r - younger.r ) + std::fabs( g - younger.g ) + std::fabs( bl - younger.b );
			const Wagon& older  = ( a.age < b.age ) ? b : a;
			const double dOld   = std::fabs( r - older.r ) + std::fabs( g - older.g ) + std::fabs( bl - older.b );

			++checked;
			if( dYoung > dOld )
			{
				printf( "  FAIL phase %.2f slots %d/%d: overlap shows the OLDER shape "
				        "(pixel %.2f %.2f %.2f, younger %.2f %.2f %.2f)\n",
				        phase, slot, slot + 1, r, g, bl, younger.r, younger.g, younger.b );
				++failures;
			}
		}

		releaseTarget( target );
		plugin.DeInitGL();
	}

	printf( "order: %d overlaps measured, %d failures\n", checked, failures );
	if( checked == 0 )
	{
		printf( "order: nothing overlapped -- the test has gone stale\n" );
		return 1;
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --round
//---------------------------------------------------------------------------
int roundCheck()
{
	struct Case
	{
		const char* name;
		int width;
		int height;
	};

	const Case cases[] = {
		{ "1:1",      720,  720 },
		{ "16:9",    1280,  720 },
		{ "portrait", 720, 1280 },
		{ "2.39:1",  1148,  480 },
	};

	int failures = 0;

	for( const Case& c : cases )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, c.width, c.height ) )
			return 1;

		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Circle ) );
		plugin.SetFloatParameter( PT_WAGONS, countParam( 1 ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.08f ) );
		plugin.SetFloatParameter( PT_TRAVEL, 0.5f );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( 0.9f ) );
		plugin.SetFloatParameter( PT_SOFTNESS, 0.0f );

		const QueueParams probe = plugin.CurrentQueue( c.width, c.height );
		float phase             = 0.0f;
		if( !standingPhase( probe, 1, phase ) )
		{
			printf( "  FAIL round %-9s the one shape never stands\n", c.name );
			++failures;
			plugin.DeInitGL();
			continue;
		}
		plugin.SetPhaseOverride( phase );

		Target target = makeTarget( c.width, c.height );
		render( plugin, target );
		const std::vector< unsigned char > image = readBytes( target );

		const QueueParams queue = plugin.CurrentQueue( c.width, c.height );
		const Wagon w           = SolveSlot( queue, 0 );
		const double radius     = radiusPixels( w.scale, c.width, c.height );

		const Blob blob = measure( image, c.width, c.height, w.x, w.y, radius );
		if( blob.weight <= 0.0 )
		{
			printf( "  FAIL round %-9s nothing drawn at (%.3f, %.3f)\n", c.name, w.x, w.y );
			++failures;
			releaseTarget( target );
			plugin.DeInitGL();
			continue;
		}

		// A uniform disc of radius r has a second moment of r/2 on each axis.
		// Comparing the two axes is the roundness; comparing either against the
		// prediction is the SIZE, and it is a separate claim -- sizing off the
		// wrong edge gives a perfectly round circle of entirely the wrong
		// diameter, which a roundness test alone would pass.
		const double roundness = std::fabs( blob.spanX - blob.spanY ) / std::max( blob.spanX, blob.spanY );
		const double expected  = radius * 0.5;
		const double sizeError = std::fabs( blob.spanX - expected ) / expected;

		if( roundness > 0.02 )
		{
			printf( "  FAIL round %-9s %.2f%% out of round (%.2f x %.2f px)\n",
			        c.name, roundness * 100.0, blob.spanX, blob.spanY );
			++failures;
		}
		else if( sizeError > 0.03 )
		{
			printf( "  FAIL round %-9s %.2f%% wrong size (%.2f px vs %.2f expected)\n",
			        c.name, sizeError * 100.0, blob.spanX, expected );
			++failures;
		}
		else
			printf( "  ok   round %-9s %.2f%% out of round, %.2f%% off the expected size\n",
			        c.name, roundness * 100.0, sizeError * 100.0 );

		releaseTarget( target );
		plugin.DeInitGL();
	}

	printf( "round: %zu aspect ratios, %d failures\n", sizeof( cases ) / sizeof( cases[ 0 ] ), failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mask
//---------------------------------------------------------------------------
int maskCheck()
{
	const int width  = 640;
	const int height = 360;

	//-----------------------------------------------------------------------
	// Set up an effect instance with one big shape standing in the middle, so
	// there is an unambiguous inside and outside to sample.
	//-----------------------------------------------------------------------
	auto configure = []( ShuntPlugin& plugin ) {
		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Circle ) );
		plugin.SetFloatParameter( PT_WAGONS, countParam( 1 ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.25f ) );
		plugin.SetFloatParameter( PT_TRAVEL, 0.5f );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( 0.9f ) );
		plugin.SetFloatParameter( PT_SOFTNESS, 0.0f );
		plugin.SetFloatParameter( PT_MIX, 1.0f );
	};

	ShuntPlugin probePlugin( true );
	configure( probePlugin );
	const QueueParams probe = probePlugin.CurrentQueue( width, height );
	float phase             = 0.0f;
	if( !standingPhase( probe, 1, phase ) )
	{
		printf( "mask: the one shape never stands -- the test has gone stale\n" );
		return 1;
	}

	const Wagon centre = [&] {
		QueueParams at = probe;
		at.phase       = phase;
		return SolveSlot( at, 0 );
	}();

	const int insideCol   = static_cast< int >( centre.x * width );
	const int insideGlRow = static_cast< int >( ( 1.0f - centre.y ) * height );
	const int outsideCol  = 8;
	const int outsideGlRow = 8;

	//-----------------------------------------------------------------------
	// The reference clip is CAPTURED by rendering at zero opacity rather than
	// predicted, because predicting it would mean reimplementing the UV flip in
	// the test -- and a test that reimplements what it tests agrees with its own
	// mistakes.
	//-----------------------------------------------------------------------
	GLuint clip   = makeTestClip( width, height );
	Target target = makeTarget( width, height );

	std::vector< unsigned char > reference;
	{
		ShuntPlugin plugin( true );
		if( !prepare( plugin, width, height ) )
			return 1;
		configure( plugin );
		plugin.SetFloatParameter( PT_OPACITY, 0.0f );
		plugin.SetFloatParameter( PT_MASK_MODE, static_cast< float >( MaskMode::Over ) );
		plugin.SetPhaseOverride( phase );
		render( plugin, target, clip );
		reference = readBytes( target );
		plugin.DeInitGL();
	}

	auto sample = []( const std::vector< unsigned char >& image, int w, int col, int glRow ) {
		const unsigned char* p = &image[ ( static_cast< size_t >( glRow ) * w + col ) * 4 ];
		return std::array< double, 4 >{ p[ 0 ] / 255.0, p[ 1 ] / 255.0, p[ 2 ] / 255.0, p[ 3 ] / 255.0 };
	};

	const auto clipInside  = sample( reference, width, insideCol, insideGlRow );
	const auto clipOutside = sample( reference, width, outsideCol, outsideGlRow );

	struct ModeCase
	{
		MaskMode mode;
		const char* name;
		// Shape colour, so Colourise has something to multiply by. Red, because
		// against white it is arithmetically identical to Reveal and would pass
		// whether or not the multiply happened.
		bool red;
	};

	const ModeCase modes[] = {
		{ MaskMode::Over,      "Over",      false },
		{ MaskMode::Reveal,    "Reveal",    false },
		{ MaskMode::Hide,      "Hide",      false },
		{ MaskMode::Colourise, "Colourise", true  },
	};

	int failures = 0;

	for( const ModeCase& m : modes )
	{
		ShuntPlugin plugin( true );
		if( !prepare( plugin, width, height ) )
			return 1;
		configure( plugin );
		plugin.SetFloatParameter( PT_MASK_MODE, static_cast< float >( m.mode ) );
		if( m.red )
		{
			plugin.SetFloatParameter( PT_COLOUR_MODE, static_cast< float >( ColourMode::Solid ) );
			plugin.SetFloatParameter( PT_SHAPE_R, 1.0f );
			plugin.SetFloatParameter( PT_SHAPE_G, 0.0f );
			plugin.SetFloatParameter( PT_SHAPE_B, 0.0f );
		}
		plugin.SetPhaseOverride( phase );

		render( plugin, target, clip );
		const std::vector< unsigned char > image = readBytes( target );

		const auto in  = sample( image, width, insideCol, insideGlRow );
		const auto out = sample( image, width, outsideCol, outsideGlRow );

		auto near = []( double a, double b ) { return std::fabs( a - b ) < 0.06; };
		auto sameAs = [&]( const std::array< double, 4 >& a, const std::array< double, 4 >& b ) {
			return near( a[ 0 ], b[ 0 ] ) && near( a[ 1 ], b[ 1 ] ) && near( a[ 2 ], b[ 2 ] );
		};

		bool ok = false;
		const char* why = "";

		switch( m.mode )
		{
		case MaskMode::Over:
			// The shape's own colour inside, the clip untouched outside.
			ok  = in[ 0 ] > 0.8 && in[ 1 ] > 0.8 && in[ 2 ] > 0.8 && sameAs( out, clipOutside );
			why = "white inside, clip outside";
			break;

		case MaskMode::Reveal:
			// The clip inside, nothing outside.
			ok  = sameAs( in, clipInside ) && out[ 3 ] < 0.06;
			why = "clip inside, transparent outside";
			break;

		case MaskMode::Hide:
			// Nothing inside, the clip outside.
			ok  = in[ 3 ] < 0.06 && sameAs( out, clipOutside );
			why = "transparent inside, clip outside";
			break;

		case MaskMode::Colourise:
			// The clip's red only, inside; nothing outside.
			ok = near( in[ 0 ], clipInside[ 0 ] ) && in[ 1 ] < 0.06 && in[ 2 ] < 0.06
			     && out[ 3 ] < 0.06;
			why = "clip's red only, inside";
			break;

		default:
			break;
		}

		if( ok )
			printf( "  ok   mask %-10s %s\n", m.name, why );
		else
		{
			printf( "  FAIL mask %-10s expected %s; inside %.2f %.2f %.2f a%.2f, outside %.2f %.2f %.2f a%.2f\n",
			        m.name, why, in[ 0 ], in[ 1 ], in[ 2 ], in[ 3 ],
			        out[ 0 ], out[ 1 ], out[ 2 ], out[ 3 ] );
			++failures;
		}

		plugin.DeInitGL();
	}

	releaseTarget( target );
	glDeleteTextures( 1, &clip );

	printf( "mask: 4 modes, %d failures\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --clock
//
// No GL. The host clock's unit is MEASURED against a real one rather than
// guessed from the size of a single delta.
//---------------------------------------------------------------------------
int clockCheck()
{
	struct Case
	{
		const char* name;
		double perFrame;    ///< what the host advances by, in its own unit
		double expected;    ///< the scale that should be settled on
	};

	const Case cases[] = {
		{ "seconds at 60 fps",       1.0 / 60.0, 1.0   },
		{ "milliseconds at 50 fps",  20.0,       0.001 },
		{ "milliseconds at 60 fps",  1000.0 / 60.0, 0.001 },
	};

	int failures = 0;

	for( const Case& c : cases )
	{
		ShuntPlugin plugin( false );

		double host = 0.0;
		for( int frame = 0; frame < 30; ++frame )
		{
			plugin.SetTime( host );
			plugin.TickClockForTest();
			host += c.perFrame;

			// Real time has to pass, or the ratio is undefined. Short enough
			// that the whole check is a fraction of a second.
			std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
		}

		const double scale = plugin.ClockScaleForTest();
		if( std::fabs( scale - c.expected ) > 1e-9 )
		{
			printf( "  FAIL clock %-24s settled on %.6f, expected %.6f\n", c.name, scale, c.expected );
			++failures;
		}
		else
			printf( "  ok   clock %-24s scale %.6f\n", c.name, scale );
	}

	printf( "clock: %zu hosts, %d failures\n", sizeof( cases ) / sizeof( cases[ 0 ] ), failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --speed
//
// No GL. Changing Speed must not move the train: the phase either side of the
// change has to be the same number.
//---------------------------------------------------------------------------
int speedCheck()
{
	ShuntPlugin plugin( false );
	plugin.SetClockScaleForTest( 1.0 );   // seconds, said out loud

	// Ten minutes in, which is where the bug this guards against actually
	// bites: `phase = clock * speed` moves by `clock * delta`, and after ten
	// minutes that is hundreds of cycles.
	plugin.SetTime( 600.0 );
	plugin.TickClockForTest();

	const float before = plugin.CurrentPhaseForTest();

	plugin.SetFloatParameter( PT_SPEED, 0.8f );
	plugin.TickClockForTest();

	const float after = plugin.CurrentPhaseForTest();

	if( std::fabs( after - before ) > 1e-3f )
	{
		printf( "  FAIL speed: phase jumped from %.4f to %.4f on a Speed change\n", before, after );
		printf( "speed: 1 check, 1 failure\n" );
		return 1;
	}

	// And it must then run at the NEW rate, or the anchor has frozen it.
	plugin.SetTime( 610.0 );
	plugin.TickClockForTest();
	const float later = plugin.CurrentPhaseForTest();

	const float rate = ( later - after ) / 10.0f;
	const float want = SpeedFromParam( 0.8f );
	if( std::fabs( rate - want ) > 1e-3f )
	{
		printf( "  FAIL speed: after the change it runs at %.4f cycles/s, Speed says %.4f\n", rate, want );
		printf( "speed: 1 check, 1 failure\n" );
		return 1;
	}

	printf( "  ok   speed: phase held at %.4f across a change, then ran at %.4f cycles/s\n", after, rate );
	printf( "speed: 1 check, 0 failures\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --presets
//
// No GL. Three host behaviours x every preset. See AGENTS.md for why the host
// restating its own values is the case that matters.
//---------------------------------------------------------------------------
int presetCheck()
{
	using namespace shunt::presets;

	int coveredCount            = 0;
	const unsigned int* covered = ShuntPlugin::PresetParamIDsForTest( coveredCount );

	enum class Host
	{
		Honours,
		Ignores,
		Quantises
	};
	struct HostCase
	{
		Host kind;
		const char* name;
	};
	const HostCase hosts[] = {
		{ Host::Honours, "honours value events" },
		{ Host::Ignores, "ignores value events" },
		{ Host::Quantises, "honours, 1/1000 steps" },
	};

	int failures = 0;

	for( const HostCase& host : hosts )
	{
		for( int preset = 1; preset <= kCount; ++preset )
		{
			// The source build; the effect declares the same parameters.
			ShuntPlugin plugin( false );

			int presetIndex = -1;
			for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			{
				const char* declared = plugin.GetParamName( i );
				if( declared != nullptr && std::strcmp( declared, "Preset" ) == 0 )
				{
					presetIndex = int( i );
					break;
				}
			}
			if( presetIndex < 0 )
			{
				std::fprintf( stderr, "presets: no parameter is called \"Preset\"\n" );
				return 1;
			}

			// What the host thinks the sliders say before the operator reaches
			// for the dropdown.
			std::vector< float > hostOwn;
			for( int j = 0; j < coveredCount; ++j )
				hostOwn.push_back( plugin.GetFloatParameter( covered[ j ] ) );

			// The operator picks a preset.
			plugin.SetFloatParameter( unsigned( presetIndex ), float( preset ) );

			// And now the host says its piece.
			for( int j = 0; j < coveredCount; ++j )
			{
				float back = 0.0f;
				switch( host.kind )
				{
				case Host::Honours:
					back = plugin.GetFloatParameter( covered[ j ] );
					break;
				case Host::Ignores:
					back = hostOwn[ size_t( j ) ];
					break;
				case Host::Quantises:
					back = std::round( plugin.GetFloatParameter( covered[ j ] ) * 1000.0f ) / 1000.0f;
					break;
				}
				plugin.SetFloatParameter( covered[ j ], back );
			}

			const int still = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			bool ok         = still == preset;

			// Still selected is not enough -- it has to be what renders.
			for( int j = 0; j < coveredCount; ++j )
			{
				const float want = kPresets[ preset - 1 ].v[ j ];
				const float got  = plugin.GetFloatParameter( covered[ j ] );
				ok               = ok && std::fabs( got - want ) <= 1e-4f;
			}

			if( !ok )
			{
				std::printf( "presets %-22s %-14s FAILED (shows %d)\n",
				             host.name, kPresets[ preset - 1 ].name, still );
				++failures;
				continue;
			}

			// An operator turning a covered knob must still drop to Custom -- a
			// preset that cannot be left is no better than one that will not
			// stick. Move it somewhere neither the preset nor the host named.
			const float moved = kPresets[ preset - 1 ].v[ 1 ] > 0.5f ? 0.123f : 0.877f;
			plugin.SetFloatParameter( covered[ 1 ], moved );
			const int after = int( std::lround( plugin.GetFloatParameter( unsigned( presetIndex ) ) ) );
			if( after != 0 )
			{
				std::printf( "presets %-22s %-14s FAILED (an edit left it on %d)\n",
				             host.name, kPresets[ preset - 1 ].name, after );
				++failures;
				continue;
			}

			std::printf( "presets %-22s %-14s ok\n", host.name, kPresets[ preset - 1 ].name );
		}
	}

	std::printf( "%s\n", failures == 0 ? "presets: all ok" : "presets: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --cost
//---------------------------------------------------------------------------
int costCheck()
{
	struct Size
	{
		const char* name;
		int width;
		int height;
	};

	const Size sizes[] = {
		{ "720p",  1280,  720 },
		{ "1080p", 1920, 1080 },
		{ "4K",    3840, 2160 },
	};

	for( const Size& s : sizes )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, s.width, s.height ) )
			return 1;

		// A full train of large shapes: the worst case the plugin can be put in
		// without leaving the controls' own ranges.
		plugin.SetFloatParameter( PT_WAGONS, 1.0f );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.08f ) );
		plugin.SetFloatParameter( PT_SHADE, 1.0f );

		Target target = makeTarget( s.width, s.height );

		// Warm up: the first frame compiles nothing but does allocate, and it
		// would otherwise be the number reported.
		for( int i = 0; i < 10; ++i )
		{
			plugin.SetPhaseOverride( 0.01f * i );
			render( plugin, target );
		}

		const int frames = 120;
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i )
		{
			plugin.SetPhaseOverride( 0.003f * i );
			render( plugin, target );
		}
		const auto end = std::chrono::steady_clock::now();

		const double ms = std::chrono::duration< double, std::milli >( end - start ).count() / frames;
		printf( "  %-6s %5d x %-5d  %.3f ms/frame\n", s.name, s.width, s.height, ms );

		releaseTarget( target );
		plugin.DeInitGL();
	}

	return 0;
}

//---------------------------------------------------------------------------
// Contact sheets. They assert nothing, and they are still worth regenerating
// whenever a shape or a side changes -- orrery's inverted star was found by
// looking at one, not by any assertion.
//---------------------------------------------------------------------------
int contactSheet( const std::string& path, bool byShape )
{
	const int cellWidth  = 480;
	const int cellHeight = 270;
	const int columns    = byShape ? 4 : 2;
	const int cells      = byShape ? static_cast< int >( Shape::Count ) : static_cast< int >( Side::Count );
	const int rows       = ( cells + columns - 1 ) / columns;

	const int width  = cellWidth * columns;
	const int height = cellHeight * rows;

	std::vector< unsigned char > sheet( static_cast< size_t >( width ) * height * 4, 0 );

	Target target = makeTarget( cellWidth, cellHeight );

	for( int cell = 0; cell < cells; ++cell )
	{
		ShuntPlugin plugin( false );
		if( !prepare( plugin, cellWidth, cellHeight ) )
			return 1;

		plugin.SetFloatParameter( PT_WAGONS, countParam( 5 ) );
		plugin.SetFloatParameter( PT_SIZE, sizeParam( 0.09f ) );
		plugin.SetFloatParameter( PT_DWELL, dwellParam( 0.75f ) );
		plugin.SetFloatParameter( PT_GAP, gapParam( 0.8f, false ) );
		plugin.SetFloatParameter( PT_COLOUR_MODE, static_cast< float >( ColourMode::HueSpread ) );
		plugin.SetFloatParameter( PT_HUE_SPREAD, 0.6f );
		plugin.SetFloatParameter( PT_SHAPE_R, 0.2f );
		plugin.SetFloatParameter( PT_SHAPE_G, 0.7f );
		plugin.SetFloatParameter( PT_SHAPE_B, 1.0f );

		if( byShape )
		{
			// Angle 0, so each primitive appears as it is drawn rather than
			// turned -- the point of this sheet is to look at the shapes. The
			// pitch differs from cell to cell on purpose: the gap is a multiple
			// of the shape's OWN thickness, so a train of bars is tight and a
			// train of discs is wide at the very same setting.
			plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( cell ) );
			plugin.SetFloatParameter( PT_ANGLE, 0.0f );
		}
		else
		{
			plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Bar ) );
			plugin.SetFloatParameter( PT_ANGLE, 0.25f );
			plugin.SetFloatParameter( PT_SIDE, static_cast< float >( cell ) );
		}

		plugin.SetPhaseOverride( 0.62f );
		render( plugin, target );

		const std::vector< unsigned char > cellImage = flipRows( readBytes( target ), cellWidth, cellHeight );

		const int col = cell % columns;
		const int row = cell / columns;
		for( int y = 0; y < cellHeight; ++y )
		{
			unsigned char* dst = &sheet[ ( static_cast< size_t >( row * cellHeight + y ) * width
			                               + static_cast< size_t >( col * cellWidth ) ) * 4 ];
			const unsigned char* src = &cellImage[ static_cast< size_t >( y ) * cellWidth * 4 ];
			std::memcpy( dst, src, static_cast< size_t >( cellWidth ) * 4 );
		}

		plugin.DeInitGL();
	}

	releaseTarget( target );

	if( !writePng( path, width, height, sheet ) )
	{
		fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	printf( "wrote %s (%dx%d)\n", path.c_str(), width, height );
	return 0;
}

void usage()
{
	printf(
		"shtest -- the offline harness for shunt\n\n"
		"  --out PATH        render a frame\n"
		"  --shapes PATH     a contact sheet of all eight primitives\n"
		"  --sides PATH      a contact sheet of all four entry sides\n"
		"  --list            parameters, with their types and defaults\n"
		"  --clock           the host clock unit, measured rather than guessed\n"
		"  --speed           a Speed change does not teleport the train\n"
		"  --presets         every factory preset survives every host behaviour\n"
		"  --tile            every shape's run is exactly one cycle long\n"
		"  --place           where every shape landed, against Queue.cpp\n"
		"  --gap             standing shapes really are one gap apart, both units\n"
		"  --travel          the head really stops at Travel%% of the frame\n"
		"  --dwell           a standing shape really stands for Dwell of its cycle\n"
		"  --order           the newest shape is drawn on top\n"
		"  --round           circles stay round, and stay put, off 1:1\n"
		"  --mask            the four effect mask modes\n"
		"  --cost            ms/frame at 720p, 1080p and 4K\n\n"
		"  --effect          use the effect variant\n"
		"  --set \"Name=v\"    set any parameter by name\n"
		"  --phase P         pin the phase (default)\n"
		"  --time T          drive the real clock in seconds instead\n"
		"  --size WxH        output size (default 1280x720)\n" );
}

} // namespace

int main( int argc, char** argv )
{
	std::string outPath;
	std::string shapesPath;
	std::string sidesPath;
	std::vector< std::string > settings;

	bool wantList   = false;
	bool wantClock  = false;
	bool wantSpeed  = false;
	bool wantPreset = false;
	bool wantTile   = false;
	bool wantPlace  = false;
	bool wantGap    = false;
	bool wantTravel = false;
	bool wantDwell  = false;
	bool wantOrder  = false;
	bool wantRound  = false;
	bool wantMask   = false;
	bool wantCost   = false;
	bool wantEffect = false;

	float phase    = 0.0f;
	float hostTime = -1.0f;   // negative means "pin the phase instead"
	int width      = 1280;
	int height     = 720;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		const bool hasNext    = ( i + 1 < argc );

		if( arg == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( arg == "--shapes" && hasNext )
			shapesPath = argv[ ++i ];
		else if( arg == "--sides" && hasNext )
			sidesPath = argv[ ++i ];
		else if( arg == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( arg == "--phase" && hasNext )
			phase = std::stof( argv[ ++i ] );
		else if( arg == "--time" && hasNext )
			hostTime = std::stof( argv[ ++i ] );
		else if( arg == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x != std::string::npos )
			{
				width  = std::stoi( size.substr( 0, x ) );
				height = std::stoi( size.substr( x + 1 ) );
			}
		}
		else if( arg == "--list" )    wantList = true;
		else if( arg == "--clock" )   wantClock = true;
		else if( arg == "--speed" )   wantSpeed = true;
		else if( arg == "--presets" ) wantPreset = true;
		else if( arg == "--tile" )    wantTile = true;
		else if( arg == "--place" )   wantPlace = true;
		else if( arg == "--gap" )     wantGap = true;
		else if( arg == "--travel" )  wantTravel = true;
		else if( arg == "--dwell" )   wantDwell = true;
		else if( arg == "--order" )   wantOrder = true;
		else if( arg == "--round" )   wantRound = true;
		else if( arg == "--mask" )    wantMask = true;
		else if( arg == "--cost" )    wantCost = true;
		else if( arg == "--effect" )  wantEffect = true;
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			fprintf( stderr, "unrecognised argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( outPath.empty() && shapesPath.empty() && sidesPath.empty()
	    && !wantList && !wantClock && !wantSpeed && !wantPreset && !wantTile
	    && !wantPlace && !wantGap && !wantTravel && !wantDwell && !wantOrder
	    && !wantRound && !wantMask && !wantCost )
	{
		usage();
		return 2;
	}

	// Before any GL: none of these touches the GPU, and a self-test that needed
	// a context would not run on a CI box without one.
	int status = 0;
	if( wantClock )
		status |= clockCheck();
	if( wantSpeed )
		status |= speedCheck();
	if( wantPreset )
		status |= presetCheck();
	if( wantTile )
		status |= tileCheck();

	const bool needsGL = !outPath.empty() || !shapesPath.empty() || !sidesPath.empty()
	                     || wantList || wantPlace || wantGap || wantTravel || wantDwell
	                     || wantOrder || wantRound || wantMask || wantCost;
	if( !needsGL )
		return status;

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		fprintf( stderr, "could not create an OpenGL 4 core context\n" );
		return 1;
	}

	if( wantList )
	{
		ShuntPlugin plugin( wantEffect );
		status |= listParameters( plugin );
	}

	if( wantPlace )
		status |= placeCheck();

	if( wantGap || wantTravel )
		status |= gapAndTravelCheck( wantGap, wantTravel );

	if( wantDwell )
		status |= dwellCheck();

	if( wantOrder )
		status |= orderCheck();

	if( wantRound )
		status |= roundCheck();

	if( wantMask )
		status |= maskCheck();

	if( wantCost )
		status |= costCheck();

	if( !shapesPath.empty() )
		status |= contactSheet( shapesPath, true );

	if( !sidesPath.empty() )
		status |= contactSheet( sidesPath, false );

	if( !outPath.empty() )
	{
		ShuntPlugin plugin( wantEffect );
		if( !prepare( plugin, width, height ) )
		{
			CGLDestroyContext( context );
			return 1;
		}

		for( const std::string& setting : settings )
		{
			if( !applySetting( plugin, setting ) )
			{
				CGLDestroyContext( context );
				return 2;
			}
		}

		if( hostTime >= 0.0f )
		{
			// Drive the real clock rather than pinning, so that Speed and Sync
			// do something. Without this they are correctly inert -- a pinned
			// phase ignores both -- and a dead-control sweep would report two
			// working parameters as broken.
			plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud
			plugin.SetTime( static_cast< double >( hostTime ) );
			plugin.SetBeatInfo( 120.0f, 0.25f );
		}
		else
		{
			plugin.SetPhaseOverride( phase );
		}

		GLuint clip = wantEffect ? makeTestClip( width, height ) : 0;

		Target target = makeTarget( width, height );
		render( plugin, target, clip );

		const std::vector< unsigned char > image = flipRows( readBytes( target ), width, height );
		if( writePng( outPath, width, height, image ) )
			printf( "wrote %s (%dx%d, phase %.3f)\n", outPath.c_str(), width, height, phase );
		else
		{
			fprintf( stderr, "could not write %s\n", outPath.c_str() );
			status |= 1;
		}

		releaseTarget( target );
		if( clip != 0 )
			glDeleteTextures( 1, &clip );
		plugin.DeInitGL();
	}

	CGLDestroyContext( context );
	return status;
}
