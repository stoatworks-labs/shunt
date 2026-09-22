#include "Queue.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace shunt
{
namespace
{
constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

float Fract( float x )
{
	return x - std::floor( x );
}

float Clamp01( float v )
{
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

/// The shape's rotation on screen, in radians, clockwise — the direction of
/// travel plus whatever the Angle control adds.
///
/// Measuring Angle from the direction of travel rather than from the frame is
/// what makes a Bar behave the same way whichever edge the train comes in
/// from: at 0 it is a dash pointing the way it is going, at a quarter turn it
/// is a rung across the train. Measured from the frame, the operator would have
/// to re-dial it every time they changed the entry side.
float ScreenRotation( const QueueParams& p )
{
	return SideRotation( p.side ) + p.angle * kTau;
}

/// A well-mixed 0..1 from an integer, so a set's random lane is a pure function
/// of its number. splitmix64's finaliser: every input bit reaches every output
/// bit, which matters because consecutive set numbers differ in one or two.
double Hash01( long long n )
{
	uint64_t x = static_cast< uint64_t >( n ) + 0x9E3779B97F4A7C15ull;
	x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
	x = x ^ ( x >> 31 );
	return static_cast< double >( x >> 11 ) * ( 1.0 / 9007199254740992.0 );
}

double FractD( double x )
{
	return x - std::floor( x );
}

/// The rotated box's half extents on the frame's two axes, in frame-span units.
void RotatedHalfExtents( const QueueParams& p, float& onX, float& onY )
{
	float ex = 1.0f;
	float ey = 1.0f;
	ShapeHalfExtents( p.shape, ex, ey );

	// Stretch acts on the shape's own x before it is rotated, exactly as the
	// vertex shader applies it.
	const float hx = ex * std::max( 0.0f, p.stretch );
	const float hy = ey;

	const float theta = ScreenRotation( p );
	const float c     = std::fabs( std::cos( theta ) );
	const float s     = std::fabs( std::sin( theta ) );

	// The support function of a rotated box, projected onto each axis. Rotation
	// happens in shape space and the anisotropic frame scaling comes after it,
	// which is why the two are applied in this order and not the other.
	float rx = 0.0f;
	float ry = 0.0f;
	FrameRadius( p.size, p.aspect, rx, ry );

	onX = rx * ( hx * c + hy * s );
	onY = ry * ( hx * s + hy * c );
}

} // namespace

float SideRotation( Side side )
{
	switch( side )
	{
	case Side::Left:   return 0.0f;         // travelling +x
	case Side::Right:  return kPi;          // travelling -x
	case Side::Top:    return kPi * 0.5f;   // travelling +y, which is DOWN
	case Side::Bottom: return -kPi * 0.5f;  // travelling -y
	default:           return 0.0f;
	}
}

const char* SideName( Side side )
{
	switch( side )
	{
	case Side::Left:   return "Left";
	case Side::Right:  return "Right";
	case Side::Top:    return "Top";
	case Side::Bottom: return "Bottom";
	default:           return "Left";
	}
}

const char* GapUnitsName( GapUnits units )
{
	switch( units )
	{
	case GapUnits::Thickness: return "Thickness";
	case GapUnits::Pixels:    return "Pixels";
	default:                  return "Thickness";
	}
}

const char* ColourModeName( ColourMode mode )
{
	switch( mode )
	{
	case ColourMode::White:     return "White";
	case ColourMode::Solid:     return "Solid";
	case ColourMode::HueSpread: return "Hue Spread";
	case ColourMode::HueCycle:  return "Hue Cycle";
	default:                    return "White";
	}
}

bool TravelsHorizontally( Side side )
{
	return side == Side::Left || side == Side::Right;
}

void FrameRadius( float scale, float aspect, float& rx, float& ry )
{
	if( aspect >= 1.0f )
	{
		// Landscape: the short edge is the height.
		rx = scale / aspect;
		ry = scale;
	}
	else
	{
		// Portrait: the short edge is the width.
		rx = scale;
		ry = scale * aspect;
	}
}

float HalfThickness( const QueueParams& p )
{
	float onX = 0.0f;
	float onY = 0.0f;
	RotatedHalfExtents( p, onX, onY );
	return TravelsHorizontally( p.side ) ? onX : onY;
}

float CrossHalfThickness( const QueueParams& p )
{
	float onX = 0.0f;
	float onY = 0.0f;
	RotatedHalfExtents( p, onX, onY );
	return TravelsHorizontally( p.side ) ? onY : onX;
}

long long SetOf( const QueueParams& p, int slot )
{
	const int count = std::max( 1, p.count );

	// The same expression SlotAge takes the fraction of, so a shape's set turns
	// over at exactly the instant its age wraps back to zero -- the instant it
	// is released again from off-stage, where nobody can see it change lanes.
	const double released = static_cast< double >( p.phase )
	                        - static_cast< double >( slot ) / static_cast< double >( count );
	return static_cast< long long >( std::floor( released ) );
}

float LaneAcross( const QueueParams& p, long long set )
{
	if( p.lanes == Lanes::Off )
		return p.across;

	// The band a whole shape fits in. Capped so a shape as big as the frame
	// still has a band to wrap in rather than a division by zero.
	const double e    = std::min( 0.45, static_cast< double >( CrossHalfThickness( p ) ) );
	const double band = 1.0 - 2.0 * e;
	const double u0   = ( static_cast< double >( p.across ) - e ) / band;

	double u = 0.0;

	if( p.lanes == Lanes::Step )
	{
		// Doubles, because `set` counts every cycle since the composition
		// opened: at a few cycles a second that is six figures within a day,
		// and a float step times a six-figure set loses the lane to rounding.
		u = FractD( u0 + static_cast< double >( set ) * static_cast< double >( p.laneStep ) / band );
	}
	else
	{
		// See the header. Half the band per set, plus a jitter small enough that
		// two consecutive sets can never come closer than one shape.
		const double sep    = std::min( 1.0, 2.0 * e / band );
		const double jitter = std::max( 0.0, 0.5 - sep );
		u = FractD( u0 + 0.5 * static_cast< double >( set ) + ( Hash01( set ) - 0.5 ) * jitter );
	}

	return static_cast< float >( e + u * band );
}

float GapSpan( const QueueParams& p )
{
	if( p.gapUnits == GapUnits::Pixels )
	{
		// Along the direction of travel, so the operator's number means the same
		// thing whichever edge the train comes in from. The span is the raster's
		// own pixel count on that axis, which is what a pixel map is measured in.
		const float span = static_cast< float >( std::max( 1, p.spanPixels ) );
		return std::max( 0.0f, p.gapValue ) / span;
	}

	// Twice the half-thickness at gapValue 1.0, so that "1" means edge to edge
	// and anything below it is the overlap the effect exists for.
	return std::max( 0.0f, p.gapValue ) * 2.0f * HalfThickness( p );
}

float Margin( const QueueParams& p )
{
	return HalfThickness( p ) + kClearance;
}

float SlideSpeed( const QueueParams& p )
{
	const float m = Margin( p );

	// Controls.cpp clamps dwell below 1; the floor here is belt and braces
	// against a host handing over something out of range, because the
	// alternative is a division by zero and a frame of NaNs.
	const float idle = std::max( 0.02f, 1.0f - Clamp01( p.dwell ) );

	return ( 1.0f + 2.0f * m ) / idle;
}

float SlotDepth( const QueueParams& p, int slot )
{
	const float depth = p.travel - static_cast< float >( slot ) * GapSpan( p );

	// A train longer than the run backs up out of the frame rather than trying
	// to stand behind its own starting point. Those shapes have no entry run at
	// all — they wait off-stage and then cross the whole frame — and the
	// one-cycle invariant still holds for them, because the entry they lose is
	// exactly the exit they gain.
	return std::max( -Margin( p ), depth );
}

float SlotAge( const QueueParams& p, int slot )
{
	const int count = std::max( 1, p.count );

	// Slot 0 leads. Slot k was released k/count of a cycle later, so it is that
	// much less far through its own run.
	return Fract( p.phase - static_cast< float >( slot ) / static_cast< float >( count ) );
}

Wagon SolveSlot( const QueueParams& p, int slot )
{
	Wagon out;

	const int count = std::max( 1, p.count );

	const float m     = Margin( p );
	const float v     = SlideSpeed( p );
	const float park  = SlotDepth( p, slot );
	const float dwell = Clamp01( p.dwell );
	const float age   = SlotAge( p, slot );

	const long long set = SetOf( p, slot );
	const float across  = LaneAcross( p, set );

	//-----------------------------------------------------------------------
	// The run: in, stand, out. Three straight lines and two thresholds, which
	// is the whole of the motion.
	//-----------------------------------------------------------------------
	const float arrive = ( park + m ) / v;   // cycles spent running in

	float depth = park;
	bool standing = false;

	if( age < arrive )
	{
		depth = -m + v * age;
	}
	else if( age < arrive + dwell )
	{
		standing = true;
	}
	else
	{
		depth = park + v * ( age - arrive - dwell );
	}

	//-----------------------------------------------------------------------
	// Depth and cross-position to frame space. y runs DOWN, so Top enters at
	// y = 0 and travels towards 1.
	//-----------------------------------------------------------------------
	switch( p.side )
	{
	case Side::Left:   out.x = depth;        out.y = across;       break;
	case Side::Right:  out.x = 1.0f - depth; out.y = across;       break;
	case Side::Top:    out.x = across;       out.y = depth;        break;
	case Side::Bottom: out.x = across;       out.y = 1.0f - depth; break;
	default:           out.x = depth;        out.y = across;       break;
	}

	//-----------------------------------------------------------------------
	// Colour. Keyed off the SLOT, not the draw order: the slot is a shape's
	// fixed place in the train, so a hue spread reads as a stable pattern
	// marching through rather than as the whole set flickering every time the
	// queue turns over.
	//-----------------------------------------------------------------------
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;

	switch( p.colourMode )
	{
	case ColourMode::White:
		break;

	case ColourMode::Solid:
		r = p.r;
		g = p.g;
		b = p.b;
		break;

	case ColourMode::HueSpread:
	case ColourMode::HueCycle:
	{
		float h = 0.0f;
		float s = 0.0f;
		float val = 0.0f;
		RgbToHsv( p.r, p.g, p.b, h, s, val );

		// The swatch defaults to white, and white has no hue and no saturation
		// — so a straight reading of it would make both hue modes produce a
		// train of identical white shapes and look completely broken. Treat an
		// achromatic swatch as "full saturation, hue from the spread".
		if( s < 0.01f )
		{
			s = 1.0f;
			if( val < 0.01f )
				val = 1.0f;
		}

		const float along = count > 1 ? static_cast< float >( slot ) / static_cast< float >( count ) : 0.0f;
		float hue         = h + p.hueSpread * along;

		if( p.colourMode == ColourMode::HueCycle )
			hue += p.phase;

		HsvToRgb( hue, s, val, r, g, b );
		break;
	}

	default:
		break;
	}

	out.scale    = std::max( 0.0f, p.size );
	out.rotation = ScreenRotation( p );
	out.r        = r;
	out.g        = g;
	out.b        = b;
	out.a        = Clamp01( p.opacity );

	out.slot     = slot;
	out.age      = age;
	out.depth    = depth;
	out.standing = standing;
	out.set      = set;
	out.release  = set * static_cast< long long >( count ) + slot;

	return out;
}

void Solve( const QueueParams& p, std::vector< Wagon >& out )
{
	const int count = std::min( kMaxShapes, std::max( 1, p.count ) );

	out.clear();
	out.reserve( static_cast< size_t >( count ) );

	for( int slot = 0; slot < count; ++slot )
		out.push_back( SolveSlot( p, slot ) );

	//-----------------------------------------------------------------------
	// Draw order: oldest first, newest last.
	//
	// "The leading edge of #2 slides over the trailing edge of #1" is the whole
	// reason this sort is here — the newest shape has to be on top, and with
	// ordinary alpha compositing that means drawn last. Age runs 0..1 through a
	// shape's own run, so the largest age is the one that has been going
	// longest and it goes down first.
	//
	// The order rotates once per release, because which slot is newest turns
	// over as the phase advances. A fixed order would put the same shape on top
	// for ever and the train would look wrong for most of every cycle.
	//
	// stable_sort so that the instant two shapes share an age — reachable by
	// pinning the phase to an exact multiple of 1/count, which every rendered
	// test does — the picture is still the same one on every machine.
	//-----------------------------------------------------------------------
	std::stable_sort( out.begin(), out.end(),
	                  []( const Wagon& a, const Wagon& b ) { return a.age > b.age; } );
}

void HsvToRgb( float h, float s, float v, float& r, float& g, float& b )
{
	h = Fract( h ) * 6.0f;
	s = Clamp01( s );
	v = Clamp01( v );

	const int sector = static_cast< int >( std::floor( h ) ) % 6;
	const float f    = h - std::floor( h );
	const float p    = v * ( 1.0f - s );
	const float q    = v * ( 1.0f - s * f );
	const float t    = v * ( 1.0f - s * ( 1.0f - f ) );

	switch( sector )
	{
	case 0:  r = v; g = t; b = p; break;
	case 1:  r = q; g = v; b = p; break;
	case 2:  r = p; g = v; b = t; break;
	case 3:  r = p; g = q; b = v; break;
	case 4:  r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
}

void RgbToHsv( float r, float g, float b, float& h, float& s, float& v )
{
	const float maxC = std::max( r, std::max( g, b ) );
	const float minC = std::min( r, std::min( g, b ) );
	const float d    = maxC - minC;

	v = maxC;
	s = maxC > 0.0f ? d / maxC : 0.0f;

	if( d <= 0.0f )
	{
		h = 0.0f;
		return;
	}

	if( maxC == r )
		h = ( g - b ) / d + ( g < b ? 6.0f : 0.0f );
	else if( maxC == g )
		h = ( b - r ) / d + 2.0f;
	else
		h = ( r - g ) / d + 4.0f;

	h /= 6.0f;
}

} // namespace shunt
