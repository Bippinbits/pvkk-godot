/**************************************************************************/
/*  render_raytracing_debug.h                                             */
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

#include "core/templates/hash_map.h"
#include "core/templates/rid.h"

/// Bookkeeping of the Dynamic Geometry path tracing debug view (Environment::RT_DEBUG_DYNAMIC_GEOMETRY).
/// Remembers the frame each BLAS was last built or refit and classifies TLAS geometry from those stamps,
/// so items the dynamic scheduler updates only on some frames stay visible while their work fades out.
/// Dynamic BLASes whose last work was a build keep showing it while they idle.
class RTDynamicGeometryDebug {
public:
	/// Color category of a geometry. Must match RT_DYNAMIC_GEOMETRY_* in raytracing_closest_hit_common_inc.glsl.
	enum Category : uint32_t {
		CATEGORY_STATIC, ///< No acceleration structure work.
		CATEGORY_MOVED, ///< Only its TLAS transform changed this frame.
		CATEGORY_REFIT, ///< BLAS refit.
		CATEGORY_BUILD, ///< BLAS built.
		CATEGORY_CAPTURED_REFIT, ///< BLAS refit from a vertex capture.
		CATEGORY_CAPTURED_BUILD, ///< BLAS built from a vertex capture.
		CATEGORY_BUILT_IDLE, ///< Dynamic BLAS idle since its last build.
		CATEGORY_CAPTURED_BUILT_IDLE, ///< Dynamic BLAS idle since its last build from a vertex capture.
	};

	/// Frames BLAS work stays visible. Must match RT_DYNAMIC_GEOMETRY_FADE_FRAMES.
	static constexpr uint32_t FADE_FRAMES = 60;
	/// Bit position of State::rest in a packed state. Must match RT_DYNAMIC_GEOMETRY_REST_SHIFT.
	static constexpr uint32_t REST_SHIFT = 4;
	/// Bits of a category in a packed state.
	static constexpr uint32_t CATEGORY_MASK = (1u << REST_SHIFT) - 1u;
	/// Bit position of the age in a packed state. Must match RT_DYNAMIC_GEOMETRY_AGE_SHIFT.
	static constexpr uint32_t AGE_SHIFT = 8;
	/// Stamp of work that never happened.
	static constexpr uint64_t NEVER = UINT64_MAX;

	/// Frames of the last BLAS work, and what is known about the BLAS.
	struct Stamps {
		uint64_t last_build = NEVER;
		uint64_t last_refit = NEVER;
		uint64_t last_seen = NEVER; ///< Last frame a TLAS instance used the BLAS.
		/// Built from renderer-written vertices (RT_GEOM_FLAG_DEFORMED). Only these settle on CATEGORY_BUILT_IDLE:
		/// every other BLAS is built once at load.
		bool dynamic = false;
	};

	/// What a geometry shows: a category, the frames since the work that picked it (0 is this frame), and the
	/// category it fades to.
	struct State {
		Category category = CATEGORY_STATIC;
		Category rest = CATEGORY_STATIC;
		uint32_t age = 0;

		bool operator==(const State &p_other) const { return category == p_other.category && rest == p_other.rest && age == p_other.age; }
	};

	/// The most recent BLAS work within FADE_FRAMES of p_frame wins, a build on ties. Without any, a geometry
	/// whose TLAS transform changed this frame is CATEGORY_MOVED, others show their rest category: a build
	/// idle category for a dynamic BLAS whose last work was a build, CATEGORY_STATIC otherwise.
	static State classify(const Stamps &p_stamps, bool p_captured, bool p_moved, uint64_t p_frame);
	/// p_state packed for RT_GeometryData::debug_state.
	static uint32_t pack(const State &p_state) {
		return uint32_t(p_state.category) | (uint32_t(p_state.rest) << REST_SHIFT) | (p_state.age << AGE_SHIFT);
	}

	/// Starts p_frame, forgetting BLASes with nothing left to show. Further calls for the same frame do nothing.
	void begin_frame(uint64_t p_frame);
	/// Stamps a build of p_blas in the current frame.
	void record_build(RID p_blas);
	/// Stamps a refit of p_blas in the current frame.
	void record_refit(RID p_blas);
	/// State of a geometry of p_blas in the current frame. Marks p_blas as seen; p_dynamic is its Stamps::dynamic.
	State observe(RID p_blas, bool p_captured, bool p_moved, bool p_dynamic);
	/// Forgets every BLAS.
	void clear() { blas_stamps.clear(); }
	/// BLASes with work that hasn't faded out yet, or idling since a build.
	uint32_t get_tracked_count() const { return blas_stamps.size(); }

private:
	HashMap<uint64_t, Stamps> blas_stamps; // By RID id.
	uint64_t frame = 0;

	// Frames from p_stamp to p_frame, or UINT32_MAX when the work faded out or never happened.
	static uint32_t _age(uint64_t p_stamp, uint64_t p_frame);
	// True when p_stamps settle on a build idle category.
	static bool _settles(const Stamps &p_stamps);
};
