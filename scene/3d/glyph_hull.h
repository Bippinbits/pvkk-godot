/**************************************************************************/
/*  glyph_hull.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/io/image.h"
#include "core/math/rect2.h"
#include "core/math/rect2i.h"
#include "core/templates/vector.h"

// Triangulated outline enclosing every point of a glyph atlas region that the
// glyph material can draw, so glyphs can be meshed tighter than their padded
// atlas rect without clipping ink. Coordinates are atlas texels, y-down.
struct GlyphHull {
	// Atlas value the material turns into alpha.
	enum Channel : uint8_t {
		CHANNEL_ALPHA, // Coverage bitmaps (LA8 or RGBA8): a.
		CHANNEL_MSDF, // MSDF fill (RGBA8): median(r, g, b).
		CHANNEL_MSDF_OUTLINE, // MSDF outline (RGBA8): min(median(r, g, b), a).
	};

	// A point can draw only where the bilinearly filtered channel value
	// exceeds `threshold` (0 to 1, like the shader's texture reads).
	struct AlphaTest {
		Channel channel = CHANNEL_ALPHA;
		float threshold = 0.0f;
	};

	// Hull vertices in atlas texels.
	Vector<Vector2> points;
	// Triangle list indexing `points`, wound like a glyph quad.
	Vector<int> indices;

	// Returns the alpha test matching BaseMaterial3D's glyph shading. MSDF
	// pixel range and outline size are in atlas texels.
	static AlphaTest make_alpha_test(bool p_msdf, float p_msdf_pixel_range, float p_msdf_outline_size);

	// Builds the hull of the atlas texels in `p_rect`. Up to `p_tolerance`
	// texels of extra area are allowed to keep the vertex count low, but never
	// past the bounding box of what can draw, so neighbouring glyphs keep their
	// distance. Returns false with an empty hull when the atlas format is
	// unsupported, nothing in the rect can draw, or triangulation fails;
	// callers should then draw the full rect.
	static bool build(const Ref<Image> &p_atlas, const Rect2i &p_rect, const AlphaTest &p_test, real_t p_tolerance, GlyphHull &r_hull);

	// Builds the hull of a cell mask, clipped to `p_clip` and to the bounding
	// box of the flagged cells. `p_cells` is row-major with `p_size`; a
	// non-zero cell (x, y) covers the square of side `p_cell_size` at
	// `p_origin + (x, y) * p_cell_size`. Returns false like build().
	static bool build_from_cells(const Vector<uint8_t> &p_cells, const Size2i &p_size, const Vector2 &p_origin, real_t p_cell_size, const Rect2 &p_clip, real_t p_tolerance, GlyphHull &r_hull);
};
