#include "Shapes.h"

namespace shunt
{
const char* ShapeName( Shape shape )
{
	switch( shape )
	{
	case Shape::Circle:   return "Circle";
	case Shape::Square:   return "Square";
	case Shape::Triangle: return "Triangle";
	case Shape::Hexagon:  return "Hexagon";
	case Shape::Star:     return "Star";
	case Shape::Cross:    return "Cross";
	case Shape::Ring:     return "Ring";
	case Shape::Bar:      return "Bar";
	default:              return "Circle";
	}
}

float ShapeBound( Shape shape )
{
	switch( shape )
	{
	// Inscribed in the unit circle by construction.
	case Shape::Circle:
	case Shape::Ring:
	case Shape::Star:
		return 1.0f;

	// The square SDF is built from a half-extent of 1 on each axis, so the
	// corners sit at sqrt(2). Getting this wrong is exactly the failure the
	// header describes: at 45 degrees the corners are what you see.
	case Shape::Square:
		return 1.4143f;

	// Equilateral triangle with circumradius 1: the vertices are the bound.
	case Shape::Triangle:
		return 1.0f;

	// Hexagon with circumradius 1.
	case Shape::Hexagon:
		return 1.0f;

	// The cross arms run to 1 on each axis and the corners of the arms sit a
	// little beyond, so round up rather than assume.
	case Shape::Cross:
		return 1.05f;

	// The bar is a 1 x 0.15 box, so its corners sit a shade past 1 — and the
	// long axis is scaled by Stretch in the shader on top of this.
	case Shape::Bar:
		return 1.05f;

	default:
		return 1.4143f;
	}
}

void ShapeHalfExtents( Shape shape, float& halfX, float& halfY )
{
	switch( shape )
	{
	case Shape::Circle:
	case Shape::Ring:
		halfX = 1.0f;
		halfY = 1.0f;
		break;

	// sdBox with a half-extent of 1 on each axis.
	case Shape::Square:
	case Shape::Cross:
		halfX = 1.0f;
		halfY = 1.0f;
		break;

	// Equilateral, circumradius 1, apex up. It reaches 1 above its centre and
	// 0.5 below, so the full height is 1.5 and the half extent used for spacing
	// is 0.75 — not 1. Half the width is sin(60).
	case Shape::Triangle:
		halfX = 0.8660254f;
		halfY = 0.75f;
		break;

	// sdHexagon's apothem is 0.8660254, giving a circumradius of 1: flats top
	// and bottom, vertices left and right.
	case Shape::Hexagon:
		halfX = 1.0f;
		halfY = 0.8660254f;
		break;

	// Five points, outer radius 1, one point up. The widest pair sits at 18
	// degrees either side of the horizontal; the height runs from the top point
	// at 1 to the two lower points at -0.809, so the half extent is 0.9045.
	case Shape::Star:
		halfX = 0.9510565f;
		halfY = 0.9045085f;
		break;

	// sdBox( q, vec2( 1.0, 0.15 ) ).
	case Shape::Bar:
		halfX = 1.0f;
		halfY = 0.15f;
		break;

	default:
		halfX = 1.0f;
		halfY = 1.0f;
		break;
	}
}

} // namespace shunt
