/**************************************************************************/
/*  render_raytracing_plan.h                                              */
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
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/span.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_capture.h"
#include "servers/rendering/rendering_billboard.h"
#include "servers/rendering/rendering_server.h"

namespace RendererSceneRenderImplementation {

// TLAS instance state that surfaces must share to be merged into one multi-geometry BLAS.
struct RTSurfaceGroupKey {
	RS::CullMode cull_mode = RS::CULL_MODE_BACK; // Picks the TLAS instance facing bits, see rt_triangle_facing().
	uint32_t sbt_offset = 0;
	uint8_t tlas_mask = 0;
	RenderingBillboard::Settings billboard; // Camera-dependent surfaces get their own TLAS transform.
	bool opaque = true; // No any-hit. Not part of the key: opacity is a per-geometry flag in merged BLASes.
	bool groupable = false; // False keeps the surface on the single-surface path.

	bool operator==(const RTSurfaceGroupKey &p_other) const {
		return groupable && p_other.groupable && cull_mode == p_other.cull_mode &&
				sbt_offset == p_other.sbt_offset && tlas_mask == p_other.tlas_mask && billboard == p_other.billboard;
	}
};

// Global inputs of a frame's plans, shared by every record of an RTPlanArena. Any difference means its plans may be stale.
struct RTPlanVersions {
	uint64_t material_version = 0; // MaterialStorage::material_get_rt_version().
	uint64_t mesh_version = 0; // MeshStorage::mesh_get_rt_invalidation_version().
	uint32_t ready_version = 0; // SceneShaderRaytracing::get_ready_version(), plus RenderRaytracing's hit group slot changes.
	uint32_t capture_mode_version = 0; // RenderRaytracing::capture_mode_version.
};

// Per-instance inputs a cached plan was built from. Any difference means the plan is stale.
struct RTPlanKey {
	uint64_t surfaces_version = 0; // GeometryInstanceForwardClustered::rt_surfaces_version; unique, so it also identifies the instance.
	uint8_t mask_opaque = 0; // TLAS mask of opaque surfaces: camera lane plus shadow groups.
	uint8_t mask_shadow = 0; // Shadow-caster group bits.
	uint8_t transparency = 0; // rt_instance_transparency() of the instance alpha.
	bool camera_visible = false;
};
static_assert(sizeof(RTPlanKey) == 16, "RTPlanKey must stay 16 bytes to keep RTPlanInstance at 32.");

// How a cached plan compares with the current inputs.
enum RTPlanKeyMatch : uint8_t {
	RT_PLAN_KEY_MATCH, // Reuse the plan as is.
	RT_PLAN_KEY_GLOBAL_ONLY, // Only material_version or mesh_version moved: reuse after checking each surface's counters.
	RT_PLAN_KEY_MISMATCH, // Re-plan.
};

// Flags of an RTPlanInstance.
enum RTPlanInstanceFlags : uint8_t {
	RT_PLAN_INSTANCE_VALID = 1 << 0, // The record holds a plan; false for skipped instances.
	RT_PLAN_INSTANCE_HIT_GROUPS_READY = 1 << 1, // Every traced surface's hit group is live, so grouping is allowed.
	RT_PLAN_INSTANCE_DYNAMIC = 1 << 2, // Has deformed or captured surfaces: its TLAS part is re-planned every frame.
	RT_PLAN_INSTANCE_TLAS_STALE = 1 << 3, // The capture pass re-planned the record, so the TLAS part must be re-planned too.
	RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES = 1 << 4, // Set by the capture plan: no surface is captured under the record's capture mode.
};

// Plan of one instance of a view. Its groups, surfaces and captures share one reserved range.
struct RTPlanInstance {
	RTPlanKey key;
	uint32_t range_begin = 0; // First element of this instance in RTPlanArena::groups, surfaces and captures.
	uint16_t range_capacity = 0; // Elements reserved per array; counts never exceed it.
	uint16_t group_count = 0;
	uint16_t surface_count = 0;
	uint16_t capture_count = 0;
	uint16_t pending_surfaces = 0; // Surfaces left out until their hit group or capture is ready.
	uint8_t flags = 0; // RTPlanInstanceFlags.
};
static_assert(sizeof(RTPlanInstance) == 32, "RTPlanInstance must stay compact.");

// Surface deformation, resolved again by apply every frame.
enum RTPlanDeformedKind : uint8_t {
	RT_PLAN_DEFORMED_NONE, // Traced from its mesh buffers.
	RT_PLAN_DEFORMED_MESH_INSTANCE, // Skeleton or blend shape output of the instance's mesh instance.
	RT_PLAN_DEFORMED_CAPTURED, // Vertex capture output.
};

// Flags of an RTPlanSurface.
enum RTPlanSurfaceFlags : uint8_t {
	RT_PLAN_SURFACE_TRANSPARENT = 1 << 0, // Uses the transparent camera lane.
};

// A traced surface of a planned instance. Members of one group are contiguous and in surface order.
struct RTPlanSurface {
	const void *surf = nullptr; // RenderForwardClustered::GeometryInstanceSurfaceDataCache; only read on a BLAS miss or for captures.
	uint32_t material_slot = UINT32_MAX; // RenderRaytracing material cache slot; UINT32_MAX is the default material.
	uint32_t cache_key = 0; // RenderRaytracing surface cache key: mesh RID local index and surface index.
	uint32_t mesh_rid_version = 0; // Version of the mesh RID behind cache_key.
	uint32_t surface_counter = 0; // Mesh surface RT invalidation counter.
	uint32_t material_flags = 0; // OR-ed into the material entry.
	uint16_t material_counter = 0; // Material RT invalidation counter.
	uint8_t surface_index = 0;
	uint8_t deformed_kind = RT_PLAN_DEFORMED_NONE; // RTPlanDeformedKind.
	uint8_t flags = 0; // RTPlanSurfaceFlags.
};
static_assert(sizeof(RTPlanSurface) <= 48, "RTPlanSurface must stay compact.");

// Surfaces of a planned instance sharing one TLAS instance.
struct RTPlanGroup {
	RTSurfaceGroupKey key; // Of the leader; equal for every member.
	uint64_t surface_mask = 0; // Bit i set when surface i is a member (RTMeshGroupKey::surface_mask).
	uint64_t opaque_mask = 0; // Bit i set when member i is opaque (RTMeshGroupKey::opaque_mask).
	uint16_t surface_begin = 0; // First member, relative to the instance's range_begin.
	uint16_t surface_count = 0;
	bool transparent = false; // Of the leader.
};
static_assert(sizeof(RTPlanGroup) <= 64, "RTPlanGroup must stay within a cache line.");

// Flags of an RTPlanCapture.
enum RTPlanCaptureFlags : uint8_t {
	RT_PLAN_CAPTURE_MERGED = 1 << 0, // MultiMesh or particle surface, merged into one BLAS.
	RT_PLAN_CAPTURE_CAMERA_DEPENDENT = 1 << 1, // Captured with this viewport's camera.
	RT_PLAN_CAPTURE_EVALUATED = 1 << 2, // RTPlanArena::capture_evals holds apply's decision for the memo entry.
	RT_PLAN_CAPTURE_IN_FRUSTUM = 1 << 3, // The world AABB intersects the view frustum.
	RT_PLAN_CAPTURE_IDENTITY_FLAGS = RT_PLAN_CAPTURE_MERGED | RT_PLAN_CAPTURE_CAMERA_DEPENDENT, // Flags that select the entry.
};

// What apply decides for a capture whose memo entry is live, evaluated by the plan.
struct RTPlanCaptureEval {
	RTVertexCapture::Stamps inputs; // Current inputs of the capture.
	RTVertexCapture::Recapture recapture = RTVertexCapture::RECAPTURE_REBUILD; // Decision against the entry's kept capture.
};

// A vertex-captured surface of a planned instance. Its world AABB is in RTPlanArena::capture_aabbs at the same index.
struct RTPlanCapture {
	const void *surf = nullptr; // RenderForwardClustered::GeometryInstanceSurfaceDataCache.
	RID entry; // Resolved capture entry; re-resolved through the handle map when no longer valid.
	RTVertexCapture::Layout layout;
	uint32_t dependencies = 0; // RTVertexCapture::surface_dependencies().
	uint32_t triangle_count = 0;
	uint32_t surface_key = 0; // RTMMSurfaceKey::surface_index.
	uint8_t flags = 0; // RTPlanCaptureFlags.
};
static_assert(sizeof(RTPlanCapture) <= 64, "RTPlanCapture must stay within a cache line.");

// Plans of every instance of one view in one frame, in rt_instances order (one record per instance, skipped or not).
struct RTPlanArena {
	RTPlanVersions versions; // Every record was planned or validated against these. Kept by clear().
	LocalVector<RTPlanInstance> instances;
	LocalVector<RTPlanGroup> groups;
	LocalVector<RTPlanSurface> surfaces;
	LocalVector<RTPlanCapture> captures;
	LocalVector<AABB> capture_aabbs; // Parallel to captures, so the frustum test streams only AABBs.
	LocalVector<RTPlanCaptureEval> capture_evals; // Parallel to captures; valid with RT_PLAN_CAPTURE_EVALUATED.

	// Drops every record, keeping the allocations.
	void clear() {
		instances.clear();
		groups.clear();
		surfaces.clear();
		captures.clear();
		capture_aabbs.clear();
		capture_evals.clear();
	}
};

// Sizes r_arena to p_capacities.size() empty records, record i reserving p_capacities[i] elements in groups,
// surfaces, captures and capture_aabbs. Ranges are contiguous and in record order.
void rt_plan_reserve(RTPlanArena &r_arena, Span<uint32_t> p_capacities);

// Copies record p_src_index of p_src, with its groups, surfaces and captures, into the reserved range of record
// p_dst_index of r_dst. False, leaving the destination untouched, when the counts exceed its range_capacity.
bool rt_plan_copy_forward(const RTPlanArena &p_src, uint32_t p_src_index, RTPlanArena &r_dst, uint32_t p_dst_index);

// Compares a cached plan's key and its arena's versions with the current inputs.
RTPlanKeyMatch rt_plan_key_match(const RTPlanKey &p_cached, const RTPlanVersions &p_cached_versions, const RTPlanKey &p_current, const RTPlanVersions &p_current_versions);

// Finds last frame's record of the instance with p_rt_index and p_surfaces_version. False when it has none, or the
// record belongs to an earlier instance with the same index.
bool rt_plan_lookup(Span<uint32_t> p_lookup, const RTPlanArena &p_prev, uint32_t p_rt_index, uint64_t p_surfaces_version, uint32_t &r_prev_index);

// Records that the instance with p_rt_index has record p_index, growing r_lookup with UINT32_MAX as needed.
void rt_plan_set_lookup(LocalVector<uint32_t> &r_lookup, uint32_t p_rt_index, uint32_t p_index);

} // namespace RendererSceneRenderImplementation
