#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Controls.h"

/**
    Pictures on the shapes: one image, a folder of them, or a sprite sheet.

    Asked for from the field as "load an image and that is applied to the
    shapes. Bonus points for being able to load a folder of images and it
    chooses at random, or load a sprite sheet and after declaring grid size it
    loads either a specific sprite into all the objects or chooses a random
    sprite for each."

    ## One texture, and a list of rectangles

    All three sources end up as the same thing: **one RGBA texture and a list
    of cells**, each cell a rectangle in that texture. The renderer never asks
    which source it was, only "which rectangle does this shape get?" — and
    `Queue.cpp` has already given every shape the number that answers that
    (`Wagon::release`), so a random pick is a pure function of the shape and
    needs no state either.

    - **Single** is one cell, the whole image.
    - **Sprite Sheet** is the operator's Columns x Rows grid over the image.
    - **Folder** decodes every image in the chosen file's folder and packs them
      into a grid of equal square cells here, so the GPU still sees one texture.
      Resampled, because the images can be any size and a texture array wants
      them equal; a sheet is never resampled, because a sheet is somebody
      else's grid and resampling it is how a hard sprite edge goes soft.

    ## Square, and centred

    A shape is drawn over a square in its own space, so each cell is cropped to
    its largest centred square. A 16:9 photo on a circle is a circle of the
    middle of the photo, not an ellipse squeezed out of all of it — the same
    choice every avatar picker makes. The crop is only a narrower rectangle, so
    it costs nothing at draw time.

    ## Inset by half a texel

    Sampling a cell at exactly its boundary with GL_LINEAR blends in the first
    texel of the *next* cell: a one-pixel seam of the neighbouring sprite down
    one edge, which looks like a decode fault and is not one. Every rectangle
    here is pulled in by half a texel. Carried over from flipbook, where it was
    the trap that cost the most.

    ## Straight alpha

    Kept as stb hands it over. The fragment shader multiplies the picture by
    the shape's own coverage and premultiplies once, at the end.
*/
namespace shunt
{
/// Extensions offered by the file parameter. stb_image's 8-bit set.
extern const char* const kImageExtensions[];
extern const int kImageExtensionCount;

/// The largest image accepted on either side. A safe value everywhere Resolume
/// runs; the true GL limit is a runtime query and is usually double this.
constexpr int kMaxImagePx = 8192;

/// How many images a folder contributes at most, and the square each is
/// resampled to. 36 cells of 512 px is a 3072 px texture.
constexpr int kMaxFolderImages = 36;
constexpr int kFolderCellPx    = 512;

struct ImageCell
{
	float u0 = 0.0f;  ///< Texture coordinates, origin at the TOP left.
	float v0 = 0.0f;
	float u1 = 1.0f;
	float v1 = 1.0f;
};

struct Imagery
{
	int width  = 0;
	int height = 0;

	/// Straight-alpha RGBA8, `width * height * 4` bytes, row 0 at the top.
	std::vector< uint8_t > rgba;

	std::vector< ImageCell > cells;

	/// One line for the log: what was loaded and anything that was dropped.
	std::string note;

	bool Valid() const { return width > 0 && height > 0 && !rgba.empty() && !cells.empty(); }
};

/// Load `path` as `source`. `columns` and `rows` are used only for a sheet.
/// A failure comes back with `Valid()` false and a `note` saying why.
Imagery LoadImagery( const std::string& path, ImageSource source, int columns, int rows );

/// Which cell a shape gets. `release` is `Wagon::release`, `sprite` the Sprite
/// control. Wraps rather than clamps, so a Sprite past the end comes round.
int PickCell( ImagePick pick, long long release, int sprite, int cellCount );

} // namespace shunt
