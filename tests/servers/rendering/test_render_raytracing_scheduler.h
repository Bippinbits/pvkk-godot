/**************************************************************************/
/*  test_render_raytracing_scheduler.h                                    */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_scheduler.h"

#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "tests/test_macros.h"

namespace TestRenderRaytracingScheduler {

// Scripted visibility per slot, independent of any real estimator.
class MockEstimator : public RTVisibilityEstimator {
public:
	HashMap<uint32_t, float> visibility;
	HashSet<uint32_t> held;
	float default_visibility = 0.5f;

	virtual void begin_frame(const RTSchedulerFrameContext &p_frame) override {}
	virtual float get_visibility(const RTSchedulerItem &p_item) const override {
		const float *value = visibility.getptr(p_item.slot);
		return value ? *value : default_visibility;
	}
	virtual bool should_hold(const RTSchedulerItem &p_item) const override { return held.has(p_item.slot); }
};

static RTSchedulerItem make_item(uint32_t p_slot, uint32_t p_triangles = 100, uint8_t p_gen = 1) {
	RTSchedulerItem item;
	item.slot = p_slot;
	item.gen = p_gen;
	item.triangle_count = p_triangles;
	item.in_frustum = true;
	item.aabb = AABB(Vector3(p_slot * 10.0f, 0, 0), Vector3(1, 1, 1));
	return item;
}

static LocalVector<RTSchedulerItem> make_items(uint32_t p_count, uint32_t p_triangles = 100) {
	LocalVector<RTSchedulerItem> items;
	for (uint32_t i = 0; i < p_count; i++) {
		items.push_back(make_item(i, p_triangles));
	}
	return items;
}

struct Harness {
	RTDynamicScheduler scheduler;
	MockEstimator estimator;
	RTSchedulerFrameContext context;
	LocalVector<RTSchedulerAction> actions;

	const LocalVector<RTSchedulerAction> &run(const LocalVector<RTSchedulerItem> &p_items, uint32_t p_viewport = 0) {
		context.viewport_id = p_viewport;
		scheduler.schedule(estimator, context, p_items, actions);
		return actions;
	}
	const LocalVector<RTSchedulerAction> &frame(const LocalVector<RTSchedulerItem> &p_items) {
		context.frame++;
		scheduler.begin_frame(context.frame);
		return run(p_items);
	}
};

static uint32_t count_actions(const LocalVector<RTSchedulerAction> &p_actions, RTSchedulerAction p_action) {
	uint32_t count = 0;
	for (RTSchedulerAction action : p_actions) {
		count += action == p_action ? 1 : 0;
	}
	return count;
}

TEST_CASE("[RTDynamicScheduler] New items build, then refit until a rebuild is due") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.rebuild_interval_frames = 3;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> items = make_items(2);

	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 2);
	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REFIT) == 2);
	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REFIT) == 2);
	CHECK_MESSAGE(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 1, "Rebuild is due after rebuild_interval_frames, one item per frame for two items over three frames.");
	CHECK(h.scheduler.get_stats().builds == 1);
	CHECK(h.scheduler.get_stats().build_triangles == 100);
	CHECK_MESSAGE(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 1, "The deferred item rebuilds next.");
}

TEST_CASE("[RTDynamicScheduler] Periodic rebuilds of items created together are staggered") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.rebuild_interval_frames = 4;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> items = make_items(30);
	CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 30);

	HashSet<uint32_t> rebuilt;
	for (uint32_t f = 0; f < 8; f++) {
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_REBUILD) <= 8);
		CHECK(count_actions(actions, RT_SCHEDULER_HOLD) == 0);
		for (uint32_t i = 0; i < actions.size(); i++) {
			if (actions[i] == RT_SCHEDULER_REBUILD) {
				rebuilt.insert(i);
			}
		}
	}
	CHECK_MESSAGE(rebuilt.size() == 30, "Every item still gets its periodic rebuild.");
}

TEST_CASE("[RTDynamicScheduler] Structure changes force a build") {
	Harness h;
	LocalVector<RTSchedulerItem> items = make_items(1);
	h.frame(items);
	h.frame(items);
	items[0].needs_build = true;
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
}

TEST_CASE("[RTDynamicScheduler] Budgets cap operations and triangles") {
	SUBCASE("max_operations") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.max_operations = 3;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items(10);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_HOLD) == 7);
		CHECK(h.scheduler.get_stats().get_updated() == 3);
		CHECK(h.scheduler.get_stats().budget_limited == 7);
	}
	SUBCASE("build triangles downgrade rebuild-due items to refits") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.build_triangles = 250;
		budgets.rebuild_interval_frames = 1;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items(4);
		// Mandatory builds spread over two frames under the 250-triangle budget.
		CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 2);
		CHECK(count_actions(h.frame(items), RT_SCHEDULER_REBUILD) == 2);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_REBUILD) == 2);
		CHECK(count_actions(actions, RT_SCHEDULER_REFIT) == 2);
		CHECK(h.scheduler.get_stats().build_triangles <= 250);
	}
	SUBCASE("update triangles") {
		Harness h;
		RTSchedulerBudgets budgets;
		budgets.update_triangles = 300;
		h.scheduler.set_budgets(budgets);
		const LocalVector<RTSchedulerItem> items = make_items(5);
		h.frame(items);
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		CHECK(count_actions(actions, RT_SCHEDULER_REFIT) == 3);
		CHECK(h.scheduler.get_stats().refit_triangles == 300);
		CHECK(h.scheduler.get_stats().budget_limited == 2);
	}
}

TEST_CASE("[RTDynamicScheduler] An oversized new item still builds when it comes first") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.build_triangles = 500000;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items = make_items(3000, 10);
	RTSchedulerItem big = make_item(5000, 400000 * 2);
	h.estimator.visibility[5000] = 1.0f;
	items.push_back(big);
	const LocalVector<RTSchedulerAction> &actions = h.frame(items);
	CHECK(actions[items.size() - 1] == RT_SCHEDULER_REBUILD);
	CHECK(h.scheduler.get_stats().get_updated() <= budgets.max_operations);
}

TEST_CASE("[RTDynamicScheduler] Higher visibility is updated first") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 1;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items = make_items(3);
	h.estimator.visibility[0] = 0.1f;
	h.estimator.visibility[1] = 0.9f;
	h.estimator.visibility[2] = 0.5f;
	CHECK(h.frame(items)[1] == RT_SCHEDULER_REBUILD);
	CHECK(h.frame(items)[2] == RT_SCHEDULER_REBUILD);
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
}

TEST_CASE("[RTDynamicScheduler] Equal scores take turns") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 2;
	budgets.max_stale_frames = 1000;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> items = make_items(8);
	for (int i = 0; i < 4; i++) {
		h.frame(items); // Initial builds, two per frame.
	}
	HashMap<uint32_t, int> updates;
	for (int f = 0; f < 8; f++) {
		const LocalVector<RTSchedulerAction> &actions = h.frame(items);
		for (uint32_t i = 0; i < actions.size(); i++) {
			if (actions[i] != RT_SCHEDULER_HOLD) {
				updates[i]++;
			}
		}
	}
	for (uint32_t i = 0; i < items.size(); i++) {
		CHECK_MESSAGE(updates[i] == 2, vformat("Item %d was updated %d times in 8 frames.", i, updates[i]));
	}
}

TEST_CASE("[RTDynamicScheduler] Estimator holds and safety nets") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_stale_frames = 5;
	h.scheduler.set_budgets(budgets);
	LocalVector<RTSchedulerItem> items = make_items(1);
	h.estimator.held.insert(0);

	CHECK_MESSAGE(h.frame(items)[0] == RT_SCHEDULER_REBUILD, "Mandatory builds ignore holds.");
	for (int i = 0; i < 4; i++) {
		CHECK(h.frame(items)[0] == RT_SCHEDULER_HOLD);
		CHECK(h.scheduler.get_stats().invisible == 1);
	}
	CHECK_MESSAGE(h.frame(items)[0] != RT_SCHEDULER_HOLD, "max_stale_frames forces an update.");

	CHECK(h.frame(items)[0] == RT_SCHEDULER_HOLD);
	items[0].aabb.position += Vector3(5, 0, 0);
	CHECK_MESSAGE(h.frame(items)[0] != RT_SCHEDULER_HOLD, "Moving more than its radius forces an update.");
}

TEST_CASE("[RTDynamicScheduler] A reused slot with a new generation starts over") {
	Harness h;
	LocalVector<RTSchedulerItem> items = make_items(1);
	h.frame(items);
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REFIT);
	items[0].gen = 2;
	CHECK(h.scheduler.get_frames_since_update(0, 2) == UINT32_MAX);
	CHECK(h.frame(items)[0] == RT_SCHEDULER_REBUILD);
	CHECK(h.scheduler.get_frames_since_update(0, 1) == UINT32_MAX);
	CHECK(h.scheduler.get_frames_since_update(0, 2) == 0);
}

TEST_CASE("[RTDynamicScheduler] Viewports share the frame's budgets and shared entries") {
	Harness h;
	RTSchedulerBudgets budgets;
	budgets.max_operations = 5;
	h.scheduler.set_budgets(budgets);
	const LocalVector<RTSchedulerItem> shared = make_items(3);
	h.frame(shared);
	h.context.frame++;
	h.scheduler.begin_frame(h.context.frame);
	CHECK(count_actions(h.run(shared, 0), RT_SCHEDULER_REFIT) == 3);
	CHECK_MESSAGE(count_actions(h.run(shared, 1), RT_SCHEDULER_HOLD) == 3, "Entries updated by viewport 0 this frame are not redone.");

	LocalVector<RTSchedulerItem> own;
	for (uint32_t i = 0; i < 4; i++) {
		own.push_back(make_item(100 + i));
	}
	CHECK(count_actions(h.run(own, 1), RT_SCHEDULER_REBUILD) == 2);
	CHECK(h.scheduler.get_stats().get_updated() == 5);
}

TEST_CASE("[RTDynamicScheduler] CPU estimator ranks by apparent size") {
	RTCpuVisibilityEstimator estimator;
	RTSchedulerFrameContext context;
	estimator.begin_frame(context);

	RTSchedulerItem near_item = make_item(0);
	near_item.aabb = AABB(Vector3(-0.5, -0.5, -5.5), Vector3(1, 1, 1));
	RTSchedulerItem far_item = near_item;
	far_item.aabb.position.z = -50.5;
	CHECK(estimator.get_visibility(near_item) > estimator.get_visibility(far_item));

	RTSchedulerItem behind = near_item;
	behind.aabb.position.z = 4.5;
	behind.in_frustum = false;
	CHECK(estimator.get_visibility(behind) > 0.0f);
	CHECK(estimator.get_visibility(behind) == doctest::Approx(estimator.get_visibility(near_item) * estimator.get_offscreen_weight()));

	RTSchedulerItem inside = near_item;
	inside.aabb = AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4));
	CHECK_MESSAGE(estimator.get_visibility(inside) == doctest::Approx(1.0f), "A camera inside the bounds sees it fully.");
	CHECK(estimator.should_hold(near_item) == false);
}

TEST_CASE("[RTDynamicScheduler] 5000 items schedule quickly") {
	Harness h;
	RTCpuVisibilityEstimator estimator;
	const LocalVector<RTSchedulerItem> items = make_items(5000);
	LocalVector<RTSchedulerAction> actions;
	RTDynamicScheduler &scheduler = h.scheduler;

	const int frames = 50;
	uint64_t total_usec = 0;
	for (int f = 1; f <= frames; f++) {
		scheduler.begin_frame(f);
		h.context.frame = f;
		const uint64_t start = OS::get_singleton()->get_ticks_usec();
		scheduler.schedule(estimator, h.context, items, actions);
		total_usec += OS::get_singleton()->get_ticks_usec() - start;
	}
	const double average_usec = double(total_usec) / frames;
	MESSAGE(vformat("RTDynamicScheduler: %.1f usec per frame for 5000 items.", average_usec));
	CHECK(average_usec < 200.0);
}

} // namespace TestRenderRaytracingScheduler
