/**************************************************************************/
/*  raytracing_decal_selection.h                                          */
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
#include "core/templates/local_vector.h"
#include "core/templates/radix_sort.h"
#include "core/templates/sort_array.h"

// Budgeting of path traced scene elements. Renderer independent so it can be unit tested.
namespace RTSelection {

// Weight of elements behind the camera. Ramps from 1 at the camera plane down to
// BEHIND_CAMERA_PENALTY at BEHIND_CAMERA_BUFFER_M behind it, so nearby elements are not punished harshly.
static constexpr float BEHIND_CAMERA_PENALTY = 0.05f;
static constexpr float BEHIND_CAMERA_BUFFER_M = 20.0f;
// Score bonus of decals selected last frame, so the set does not flicker at the budget edge.
static constexpr float DECAL_HYSTERESIS = 1.25f;

// p_forward_depth: signed distance along the camera forward axis (positive in front).
inline float behind_camera_factor(float p_forward_depth) {
	const float t = CLAMP((p_forward_depth + BEHIND_CAMERA_BUFFER_M) / BEHIND_CAMERA_BUFFER_M, 0.0f, 1.0f);
	return Math::lerp(BEHIND_CAMERA_PENALTY, 1.0f, t);
}

// Priority of a decal: projected size times distance fade and camera weight.
inline float decal_score(float p_solid_angle, float p_fade, float p_forward_depth, bool p_was_selected) {
	return p_solid_angle * p_fade * behind_camera_factor(p_forward_depth) * (p_was_selected ? DECAL_HYSTERESIS : 1.0f);
}

struct DecalCandidate {
	uint32_t source = 0; ///< Caller index of the decal.
	float score = 0.0f; ///< Priority; higher survives the budget.
	float sort_key = 0.0f; ///< Blend order; lower is drawn on top.
};

struct DecalByScore {
	bool operator()(const DecalCandidate &p_a, const DecalCandidate &p_b) const { return p_a.score > p_b.score; }
};

struct DecalSortKeyBits {
	uint32_t operator()(const DecalCandidate &p_candidate) const { return radix_sort_key_float(p_candidate.sort_key); }
};

// Keeps the p_budget highest scoring candidates and orders them by blend key (index 0 on top).
// r_scratch is reused between calls to avoid per-frame allocations.
inline void select_decals(LocalVector<DecalCandidate> &r_candidates, uint32_t p_budget, LocalVector<DecalCandidate> &r_scratch) {
	if (r_candidates.size() > p_budget) {
		SortArray<DecalCandidate, DecalByScore> by_score;
		by_score.nth_element(0, r_candidates.size(), p_budget, r_candidates.ptr());
		r_candidates.resize(p_budget);
	}
	r_scratch.resize(r_candidates.size());
	RadixSort<DecalCandidate, DecalSortKeyBits> by_key;
	by_key.sort(r_candidates.ptr(), r_scratch.ptr(), r_candidates.size());
}

} // namespace RTSelection
