/**************************************************************************/
/*  render_raytracing_scheduler.cpp                                       */
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

#include "render_raytracing_scheduler.h"

#include "core/templates/sort_array.h"

namespace {

// Keeps items the estimator rates at 0 ageing, so they still get their turn.
constexpr float VISIBILITY_FLOOR = 1e-4f;
// Safety-net items rank above everything the estimator scores.
constexpr float FORCED_SCORE_BONUS = 1e6f;

struct CandidateSort {
	template <typename T>
	_FORCE_INLINE_ bool operator()(const T &p_a, const T &p_b) const {
		if (p_a.score != p_b.score) {
			return p_a.score > p_b.score;
		}
		return p_a.order < p_b.order;
	}
};

} // namespace

void RTDynamicScheduler::begin_frame(uint64_t p_frame) {
	frame = p_frame;
	stats = RTSchedulerStats();
}

void RTDynamicScheduler::_ensure_slot(uint32_t p_slot) {
	if (p_slot < slot_gen.size()) {
		return;
	}
	const uint32_t size = MAX(p_slot + 1, slot_gen.size() * 2);
	const uint32_t old_size = slot_gen.size();
	slot_gen.resize(size);
	slot_last_update.resize(size);
	slot_last_rebuild.resize(size);
	slot_last_center.resize(size);
	for (uint32_t i = old_size; i < size; i++) {
		slot_gen[i] = 0;
	}
}

bool RTDynamicScheduler::_has_history(const RTSchedulerItem &p_item) const {
	return p_item.slot < slot_gen.size() && slot_gen[p_item.slot] == p_item.gen;
}

uint32_t RTDynamicScheduler::get_frames_since_update(uint32_t p_slot, uint8_t p_gen) const {
	if (p_slot >= slot_gen.size() || slot_gen[p_slot] != p_gen) {
		return UINT32_MAX;
	}
	return uint32_t(MIN(frame - slot_last_update[p_slot], uint64_t(UINT32_MAX - 1)));
}

bool RTDynamicScheduler::_try_rebuild(const RTSchedulerItem &p_item, bool p_ignore_triangle_budget) {
	if (stats.get_updated() >= budgets.max_operations) {
		return false;
	}
	if (!p_ignore_triangle_budget && stats.build_triangles + p_item.triangle_count > budgets.build_triangles) {
		return false;
	}
	stats.builds++;
	stats.build_triangles += p_item.triangle_count;
	return true;
}

bool RTDynamicScheduler::_try_refit(const RTSchedulerItem &p_item) {
	if (stats.get_updated() >= budgets.max_operations) {
		return false;
	}
	if (stats.refit_triangles + p_item.triangle_count > budgets.update_triangles) {
		return false;
	}
	stats.refits++;
	stats.refit_triangles += p_item.triangle_count;
	return true;
}

void RTDynamicScheduler::_record(const RTSchedulerItem &p_item, RTSchedulerAction p_action) {
	slot_gen[p_item.slot] = p_item.gen;
	slot_last_update[p_item.slot] = frame;
	slot_last_center[p_item.slot] = p_item.aabb.get_center();
	if (p_action == RT_SCHEDULER_REBUILD) {
		slot_last_rebuild[p_item.slot] = frame;
	}
}

void RTDynamicScheduler::schedule(RTVisibilityEstimator &p_estimator, const RTSchedulerFrameContext &p_frame, const LocalVector<RTSchedulerItem> &p_items, LocalVector<RTSchedulerAction> &r_actions) {
	r_actions.resize(p_items.size());
	mandatory.clear();
	optional.clear();
	p_estimator.begin_frame(p_frame);

	uint32_t &cursor = viewport_cursor[p_frame.viewport_id];

	for (uint32_t i = 0; i < p_items.size(); i++) {
		const RTSchedulerItem &item = p_items[i];
		r_actions[i] = RT_SCHEDULER_HOLD;
		_ensure_slot(item.slot);

		Candidate candidate;
		candidate.index = i;
		candidate.order = item.slot - cursor;
		const float visibility = MAX(p_estimator.get_visibility(item), VISIBILITY_FLOOR);

		if (item.needs_build || !_has_history(item)) {
			candidate.score = visibility;
			mandatory.push_back(candidate);
			continue;
		}

		const uint64_t since_update = frame - slot_last_update[item.slot];
		if (since_update == 0) {
			continue; // Already updated this frame by another viewport (shared entry).
		}
		const real_t radius = item.aabb.size.length() * 0.5f;
		candidate.forced = since_update >= budgets.max_stale_frames || slot_last_center[item.slot].distance_to(item.aabb.get_center()) > radius;
		if (!candidate.forced && p_estimator.should_hold(item)) {
			stats.held++;
			stats.invisible++;
			continue;
		}
		candidate.score = visibility * float(1 + MIN(since_update, uint64_t(budgets.max_stale_frames)));
		if (candidate.forced) {
			candidate.score += FORCED_SCORE_BONUS;
		}
		optional.push_back(candidate);
	}

	SortArray<Candidate, CandidateSort> sorter;
	if (mandatory.size() > 1) {
		sorter.sort(mandatory.ptr(), mandatory.size());
	}
	if (optional.size() > 1) {
		sorter.sort(optional.ptr(), optional.size());
	}

	const uint32_t updated_before = stats.get_updated();

	// Items without a valid BLAS. One that alone exceeds the build budget still builds when
	// it is the first build of the frame, so oversized systems are never starved.
	for (const Candidate &candidate : mandatory) {
		const RTSchedulerItem &item = p_items[candidate.index];
		if (_try_rebuild(item, stats.build_triangles == 0)) {
			r_actions[candidate.index] = RT_SCHEDULER_REBUILD;
			_record(item, RT_SCHEDULER_REBUILD);
		} else {
			stats.held++;
			stats.budget_limited++;
		}
	}

	// Spreads periodic rebuilds over the interval, so items created together don't all rebuild on the same frame.
	const uint32_t interval = MAX(budgets.rebuild_interval_frames, 1u);
	uint32_t rebuild_quota = (optional.size() + interval - 1) / interval;
	for (const Candidate &candidate : optional) {
		const RTSchedulerItem &item = p_items[candidate.index];
		const bool rebuild_due = frame - slot_last_rebuild[item.slot] >= interval;
		RTSchedulerAction action = RT_SCHEDULER_HOLD;
		if (rebuild_due && rebuild_quota > 0 && _try_rebuild(item, false)) {
			action = RT_SCHEDULER_REBUILD;
			rebuild_quota--;
		} else if (_try_refit(item)) {
			action = RT_SCHEDULER_REFIT;
		}
		r_actions[candidate.index] = action;
		if (action == RT_SCHEDULER_HOLD) {
			stats.held++;
			stats.budget_limited++;
		} else {
			_record(item, action);
		}
	}

	cursor += stats.get_updated() - updated_before;
}
