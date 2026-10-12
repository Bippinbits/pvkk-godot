/**************************************************************************/
/*  decal_math.h                                                          */
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

#include "core/math/math_funcs.h"
#include "core/math/transform_3d.h"

// Renderer-independent decal math shared by raster and path tracing.
namespace DecalMath {

// Maps the decal's local box (uv_local space: x/z in [0, 1], y in [-1, 1]) to world space.
inline Transform3D box_transform(const Transform3D &p_decal_xform, const Vector3 &p_size) {
	Transform3D scale_xform;
	scale_xform.basis.scale(p_size * 0.5);

	Transform3D uv_xform;
	uv_xform.basis.scale(Vector3(2.0, 1.0, 2.0));
	uv_xform.origin = Vector3(-1.0, 0.0, -1.0);

	return p_decal_xform * scale_xform * uv_xform;
}

// Blend order key. Lower keys are drawn on top.
inline float sort_key(float p_distance, float p_sorting_offset) {
	return p_distance - p_sorting_offset;
}

// Opacity multiplier for distance fade; 0 means fully faded out.
inline float distance_fade(bool p_enabled, float p_distance, float p_begin, float p_length) {
	if (!p_enabled || p_distance <= p_begin) {
		return 1.0f;
	}
	if (p_distance >= p_begin + p_length) {
		return 0.0f;
	}
	// Use `smoothstep()` to make opacity changes more gradual and less noticeable to the player.
	return Math::smoothstep(0.0f, 1.0f, 1.0f - (p_distance - p_begin) / p_length);
}

// Projected solid-angle proxy of a bounding sphere, clamped when the camera is inside it.
inline float solid_angle(float p_distance, float p_radius) {
	const float d = MAX(p_distance, p_radius);
	if (d <= 0.0f) {
		return 0.0f;
	}
	return float(Math::PI) * p_radius * p_radius / (d * d);
}

} // namespace DecalMath
