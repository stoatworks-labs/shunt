#include "Imagery.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

// stb_image is compiled here and only here. STBI_NO_HDR and STBI_NO_PIC drop
// the float paths this 8-bit pipeline could not use anyway; failure strings are
// kept, because stbi_failure_reason() is the only thing that tells "not an
// image" from "truncated" in the log.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#include "stb_image.h"

namespace shunt
{
const char* const kImageExtensions[] = { "png", "jpg", "jpeg", "gif", "bmp", "tga", "psd" };
const int kImageExtensionCount       = static_cast< int >( sizeof( kImageExtensions ) / sizeof( kImageExtensions[ 0 ] ) );

namespace
{
namespace fs = std::filesystem;

/// The host hands paths over as UTF-8. Through `u8path` so that a folder with
/// an accent in its name opens on Windows, where a narrow `fopen` would read
/// the bytes in the ANSI code page and find nothing.
fs::path FromUtf8( const std::string& path )
{
	return fs::u8path( path );
}

std::string Basename( const fs::path& path )
{
	return path.filename().u8string();
}

bool ReadFile( const fs::path& path, std::vector< uint8_t >& out )
{
	std::ifstream file( path, std::ios::binary );
	if( !file )
		return false;

	out.assign( std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >() );
	return !out.empty();
}

bool HasImageExtension( const fs::path& path )
{
	std::string ext = path.extension().u8string();
	if( ext.empty() )
		return false;

	ext.erase( 0, 1 );
	std::transform( ext.begin(), ext.end(), ext.begin(),
	                []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );

	for( int i = 0; i < kImageExtensionCount; ++i )
		if( ext == kImageExtensions[ i ] )
			return true;
	return false;
}

struct Decoded
{
	int w = 0;
	int h = 0;
	std::vector< uint8_t > rgba;
	std::string why;
};

Decoded Decode( const fs::path& path )
{
	Decoded d;

	std::vector< uint8_t > bytes;
	if( !ReadFile( path, bytes ) )
	{
		d.why = "cannot be read";
		return d;
	}

	int channels    = 0;
	stbi_uc* pixels = stbi_load_from_memory( bytes.data(), static_cast< int >( bytes.size() ),
	                                         &d.w, &d.h, &channels, 4 );
	if( pixels == nullptr )
	{
		const char* reason = stbi_failure_reason();
		d.why              = std::string( "will not decode (" ) + ( reason != nullptr ? reason : "unknown" ) + ")";
		d.w = d.h = 0;
		return d;
	}

	if( d.w > kMaxImagePx || d.h > kMaxImagePx )
	{
		d.why = std::to_string( d.w ) + "x" + std::to_string( d.h ) + " is larger than the "
		        + std::to_string( kMaxImagePx ) + " px limit";
		d.w = d.h = 0;
		stbi_image_free( pixels );
		return d;
	}

	d.rgba.assign( pixels, pixels + static_cast< size_t >( d.w ) * d.h * 4 );
	stbi_image_free( pixels );
	return d;
}

/// The largest centred square inside the pixel rectangle (x0, y0, w, h), in
/// texture coordinates of a `texW` x `texH` texture, inset by half a texel.
ImageCell SquareCell( double x0, double y0, double w, double h, int texW, int texH )
{
	const double side = std::min( w, h );
	const double cx   = x0 + ( w - side ) * 0.5;
	const double cy   = y0 + ( h - side ) * 0.5;

	// Half a texel in from every edge -- see Imagery.h. Never past the middle,
	// so a one-pixel cell still samples its own pixel.
	const double inset = std::min( 0.5, side * 0.5 );

	ImageCell c;
	c.u0 = static_cast< float >( ( cx + inset ) / texW );
	c.v0 = static_cast< float >( ( cy + inset ) / texH );
	c.u1 = static_cast< float >( ( cx + side - inset ) / texW );
	c.v1 = static_cast< float >( ( cy + side - inset ) / texH );
	return c;
}

/// Resample the centred square of `src` into an `n` x `n` block of `dst` at
/// (dx, dy). Bilinear, in PREMULTIPLIED space and back, because averaging
/// straight-alpha pixels drags the colour of fully transparent texels -- often
/// black, sometimes anything at all -- into the edge of every sprite.
void ResampleSquare( const Decoded& src, std::vector< uint8_t >& dst, int dstW, int dx, int dy, int n )
{
	const double side = std::min( src.w, src.h );
	const double ox   = ( src.w - side ) * 0.5;
	const double oy   = ( src.h - side ) * 0.5;
	const double step = side / n;

	auto texel = [&]( int x, int y, double* out ) {
		x = std::clamp( x, 0, src.w - 1 );
		y = std::clamp( y, 0, src.h - 1 );
		const uint8_t* p = &src.rgba[ ( static_cast< size_t >( y ) * src.w + x ) * 4 ];
		const double a   = p[ 3 ] / 255.0;
		out[ 0 ] = p[ 0 ] * a;
		out[ 1 ] = p[ 1 ] * a;
		out[ 2 ] = p[ 2 ] * a;
		out[ 3 ] = p[ 3 ];
	};

	for( int y = 0; y < n; ++y )
	{
		for( int x = 0; x < n; ++x )
		{
			// Box-average when shrinking a lot, so a 4000 px photo in a 512 px
			// cell is not a field of aliasing; one tap per destination texel
			// is enough when it is not.
			const int taps = std::clamp( static_cast< int >( std::ceil( step ) ), 1, 4 );
			double acc[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };

			for( int ty = 0; ty < taps; ++ty )
			{
				for( int tx = 0; tx < taps; ++tx )
				{
					const double sx = ox + ( x + ( tx + 0.5 ) / taps ) * step - 0.5;
					const double sy = oy + ( y + ( ty + 0.5 ) / taps ) * step - 0.5;
					const int x0    = static_cast< int >( std::floor( sx ) );
					const int y0    = static_cast< int >( std::floor( sy ) );
					const double fx = sx - x0;
					const double fy = sy - y0;

					double a[ 4 ], b[ 4 ], c[ 4 ], d[ 4 ];
					texel( x0, y0, a );
					texel( x0 + 1, y0, b );
					texel( x0, y0 + 1, c );
					texel( x0 + 1, y0 + 1, d );

					for( int k = 0; k < 4; ++k )
						acc[ k ] += ( a[ k ] * ( 1 - fx ) + b[ k ] * fx ) * ( 1 - fy )
						            + ( c[ k ] * ( 1 - fx ) + d[ k ] * fx ) * fy;
				}
			}

			const double inv   = 1.0 / ( taps * taps );
			const double alpha = acc[ 3 ] * inv;
			const double un    = alpha > 0.0 ? 255.0 / alpha : 0.0;

			uint8_t* out = &dst[ ( static_cast< size_t >( dy + y ) * dstW + ( dx + x ) ) * 4 ];
			for( int k = 0; k < 3; ++k )
				out[ k ] = static_cast< uint8_t >( std::clamp( acc[ k ] * inv * un, 0.0, 255.0 ) + 0.5 );
			out[ 3 ] = static_cast< uint8_t >( std::clamp( alpha, 0.0, 255.0 ) + 0.5 );
		}
	}
}

Imagery LoadFolder( const fs::path& chosen )
{
	Imagery out;

	const fs::path dir = chosen.parent_path();

	std::vector< fs::path > files;
	std::error_code ec;
	for( fs::directory_iterator it( dir, ec ), end; !ec && it != end; it.increment( ec ) )
	{
		std::error_code typeError;
		if( it->is_regular_file( typeError ) && HasImageExtension( it->path() ) )
			files.push_back( it->path() );
	}

	if( ec && files.empty() )
	{
		out.note = Basename( dir ) + ": folder cannot be listed (" + ec.message() + ")";
		return out;
	}

	// Sorted by name, so In Order runs in the order the operator sees in a
	// file browser and the same folder picks the same images on every machine.
	std::sort( files.begin(), files.end(), []( const fs::path& a, const fs::path& b ) {
		std::string x = a.filename().u8string();
		std::string y = b.filename().u8string();
		auto lower    = []( std::string s ) {
			std::transform( s.begin(), s.end(), s.begin(),
			                   []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
			return s;
		};
		return lower( x ) < lower( y );
	} );

	std::vector< Decoded > images;
	int skipped = 0;
	for( const fs::path& file : files )
	{
		if( static_cast< int >( images.size() ) >= kMaxFolderImages )
			break;

		Decoded d = Decode( file );
		if( d.rgba.empty() )
			++skipped;
		else
			images.push_back( std::move( d ) );
	}

	if( images.empty() )
	{
		out.note = Basename( dir ) + ": no image in the folder would decode";
		return out;
	}

	const int n    = static_cast< int >( images.size() );
	const int cols = static_cast< int >( std::ceil( std::sqrt( static_cast< double >( n ) ) ) );
	const int rows = ( n + cols - 1 ) / cols;
	const int cell = kFolderCellPx;

	out.width  = cols * cell;
	out.height = rows * cell;
	out.rgba.assign( static_cast< size_t >( out.width ) * out.height * 4, 0 );

	for( int i = 0; i < n; ++i )
	{
		const int cx = ( i % cols ) * cell;
		const int cy = ( i / cols ) * cell;
		ResampleSquare( images[ i ], out.rgba, out.width, cx, cy, cell );
		out.cells.push_back( SquareCell( cx, cy, cell, cell, out.width, out.height ) );
	}

	out.note = Basename( dir ) + "/: " + std::to_string( n ) + " images";
	if( static_cast< int >( files.size() ) - skipped > n )
		out.note += " (the first " + std::to_string( kMaxFolderImages ) + " by name; "
		            + std::to_string( files.size() - skipped - n ) + " more left out)";
	if( skipped > 0 )
		out.note += ", " + std::to_string( skipped ) + " would not decode";
	return out;
}

} // namespace

Imagery LoadImagery( const std::string& path, ImageSource source, int columns, int rows )
{
	Imagery out;

	if( path.empty() )
	{
		out.note = "no image chosen";
		return out;
	}

	const fs::path chosen = FromUtf8( path );

	if( source == ImageSource::Folder )
		return LoadFolder( chosen );

	Decoded d = Decode( chosen );
	if( d.rgba.empty() )
	{
		out.note = Basename( chosen ) + ": " + d.why;
		return out;
	}

	out.width  = d.w;
	out.height = d.h;
	out.rgba   = std::move( d.rgba );

	if( source == ImageSource::Sheet )
	{
		const int c = std::clamp( columns, 1, kMaxGrid );
		const int r = std::clamp( rows, 1, kMaxGrid );

		const double cw = static_cast< double >( out.width ) / c;
		const double ch = static_cast< double >( out.height ) / r;

		for( int j = 0; j < r; ++j )
			for( int i = 0; i < c; ++i )
				out.cells.push_back( SquareCell( i * cw, j * ch, cw, ch, out.width, out.height ) );

		out.note = Basename( chosen ) + ": " + std::to_string( out.width ) + "x" + std::to_string( out.height )
		           + ", grid " + std::to_string( c ) + "x" + std::to_string( r );

		// Worth saying, because an inexact division is the commonest reason a
		// sprite shows a sliver of its neighbour, and nothing else explains it.
		if( out.width % c != 0 || out.height % r != 0 )
			out.note += " (does not divide exactly)";
	}
	else
	{
		out.cells.push_back( SquareCell( 0, 0, out.width, out.height, out.width, out.height ) );
		out.note = Basename( chosen ) + ": " + std::to_string( out.width ) + "x" + std::to_string( out.height );
	}

	return out;
}

int PickCell( ImagePick pick, long long release, int sprite, int cellCount )
{
	if( cellCount <= 0 )
		return 0;

	long long index = 0;

	switch( pick )
	{
	case ImagePick::Random:
	{
		// The same finaliser Queue.cpp uses for lanes, on a different stream so
		// a shape's picture and its set's lane are not the same coin toss.
		uint64_t x = static_cast< uint64_t >( release ) * 0xD1B54A32D192ED03ull + 0x632BE59BD9B4E019ull;
		x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
		x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
		x = x ^ ( x >> 31 );
		index = static_cast< long long >( x % static_cast< uint64_t >( cellCount ) );
		break;
	}

	case ImagePick::InOrder:
		// Offset by Sprite, so In Order can start the run anywhere.
		index = release + sprite;
		break;

	case ImagePick::Same:
	default:
		index = sprite;
		break;
	}

	long long cell = index % cellCount;
	if( cell < 0 )
		cell += cellCount;
	return static_cast< int >( cell );
}

} // namespace shunt
