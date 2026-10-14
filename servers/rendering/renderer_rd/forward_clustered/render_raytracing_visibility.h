/**************************************************************************/
/*  render_raytracing_visibility.h                                        */
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

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"

// Per-frame inputs shared by the scheduler and the visibility estimators.
struct RTSchedulerFrameContext {
	// Absolute frame index; never wraps in practice.
	uint64_t frame = 0;
	uint32_t viewport_id = 0;
	// Camera-to-world transforms of this and the previous frame.
	Transform3D camera;
	Transform3D prev_camera;
};

// One dynamic item (particle system, billboarded MultiMesh, capture, deformed mesh) for this frame.
struct RTSchedulerItem {
	// RTDynamicEntry pool local index: stable and dense for the entry's lifetime.
	uint32_t slot = 0;
	// Generation of the pool index, bumped whenever it gets a new owner. Never 0.
	uint8_t gen = 1;
	// True when the entry has no valid BLAS for its current structure (new, amount/mesh changed).
	bool needs_build = false;
	// True when the cull found the item inside the camera frustum.
	bool in_frustum = false;
	// World-space bounds from the cull.
	AABB aabb;
	uint32_t triangle_count = 0;
};

// Answers how much a dynamic item matters for the image right now.
// Implementations hold no per-item registration: items are keyed by (slot, gen).
class RTVisibilityEstimator {
public:
	virtual ~RTVisibilityEstimator() = default;

	// Called once per scheduled viewport before any get_visibility call.
	virtual void begin_frame(const RTSchedulerFrameContext &p_frame) = 0;
	// 0..1, higher is updated first.
	virtual float get_visibility(const RTSchedulerItem &p_item) const = 0;
	// True only when the item is known to be unseen; the scheduler's safety nets can override it.
	virtual bool should_hold(const RTSchedulerItem &p_item) const { return false; }
	// True when hit shaders should write GeometryData.feedback_slot for this estimator.
	virtual bool wants_feedback() const { return false; }
	// Feedback buffer binding and readback; only GPU estimators do work here.
	virtual void record_gpu_work(uint32_t p_viewport_id) {}
	// Drops anything pending for a freed viewport.
	virtual void viewport_freed(uint32_t p_viewport_id) {}
};

// Pure CPU estimator: apparent angular size, weighted down outside the frustum.
class RTCpuVisibilityEstimator : public RTVisibilityEstimator {
	Vector3 camera_position;
	float offscreen_weight = 0.25f;

public:
	// Weight for items outside the frustum, which only secondary rays can see.
	void set_offscreen_weight(float p_weight) { offscreen_weight = p_weight; }
	float get_offscreen_weight() const { return offscreen_weight; }

	virtual void begin_frame(const RTSchedulerFrameContext &p_frame) override { camera_position = p_frame.camera.origin; }

	// (r / max(d, r))^2, with r the AABB radius and d the camera distance to its center.
	// Works for items behind the camera too.
	virtual float get_visibility(const RTSchedulerItem &p_item) const override {
		const real_t radius = p_item.aabb.size.length() * 0.5f;
		if (radius <= 0) {
			return 0.0f;
		}
		const real_t distance = MAX(camera_position.distance_to(p_item.aabb.get_center()), radius);
		const real_t ratio = radius / distance;
		return float(ratio * ratio) * (p_item.in_frustum ? 1.0f : offscreen_weight);
	}
};
