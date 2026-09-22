#pragma once

/**
    The primitives.

    Each is a signed distance function evaluated per pixel in the fragment
    shader, in the shape's own local space where it has unit radius. An SDF is
    the right representation here for three reasons that are all load-bearing
    rather than aesthetic:

    - **Antialiasing is one line.** `smoothstep` across a distance of one screen
      pixel, obtained from `fwidth`, gives a correctly filtered edge at any size
      and any rotation without multisampling the whole frame.
    - **Outline is free.** A stroke of width w around any shape is
      `abs( d ) - w`. That is one expression that works for all eight shapes,
      rather than eight more sets of geometry.
    - **Softness is free.** Widening the smoothstep turns a hard edge into a
      feathered one, which is the difference between a shape that pixel-maps
      cleanly to a fixture and one that reads as a soft mask.

    ## Why the C++ side carries no SDFs, but does carry the extents

    The shader owns the distance functions and there is no mirrored C++ copy of
    them: the offline harness checks *where each shape landed* and *how far
    apart the standing ones are*, not what the edge did pixel by pixel, and a
    mirrored SDF would be a second implementation to keep in step in exchange
    for a test that largely restates the shader.

    What the C++ side does need is how big each shape is, and it needs it twice
    over for two different jobs:

    - `ShapeBound` sizes the quad the shape is rasterised on. Erring high costs
      overdraw; erring low silently cuts a corner off.
    - `ShapeHalfExtents` is the shape's **true** axis-aligned half extents, and
      it is what the Gap control means by "thickness". A gap of one thickness
      has to put two shapes edge to edge, so this one cannot be padded — the
      quad bound would leave a visible space and call it touching.

    Derived from the same repo's orrery, which is where these eight primitives
    and their distance functions were first written.
*/
namespace shunt
{
enum class Shape
{
	Circle = 0,
	Square,
	Triangle,
	Hexagon,
	Star,
	Cross,
	Ring,
	Bar,

	Count
};

/// The name shown in the host's dropdown.
const char* ShapeName( Shape shape );

/**
    How far the shape reaches from its own centre, at unit radius, before
    rounding and outline are applied.

    This sizes the quad each shape is drawn on, and **it is the one number here
    that can produce a visibly wrong picture if it is too small**: the quad is
    the only thing being rasterised, so a shape that reaches past its own quad
    is not clipped in a way that looks like clipping — it loses a corner, and a
    lost corner on a rotated square reads as the shape "wobbling" rather than as
    a bounds bug.

    Erring high costs only overdraw on a few hundred pixels per shape, so these
    are the true extents rounded up, and Shunt.cpp adds a further margin for the
    outline and the feather on top.
*/
float ShapeBound( Shape shape );

/**
    The shape's true axis-aligned half extents in its own space, at unit radius.

    The **full** extent on each axis, halved — not the distance from the centre
    to the furthest point in each direction, which for a triangle is not the
    same number. A gap measured in thicknesses puts consecutive shapes a full
    extent apart so that they touch, and for two identical shapes that distance
    is the sum of one's reach forwards and the next one's reach backwards, which
    is the full extent however lopsided the shape is about its own centre.

    Unpadded, unlike `ShapeBound`. Padding here would open a visible space
    between two shapes the operator asked to have touching.
*/
void ShapeHalfExtents( Shape shape, float& halfX, float& halfY );

} // namespace shunt
