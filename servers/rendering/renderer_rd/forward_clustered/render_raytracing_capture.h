/**************************************************************************/
/*  render_raytracing_capture.h                                           */
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

#include "core/math/transform_3d.h"
#include "core/string/string_name.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/span.h"
#include "core/templates/vector.h"

#include <cstdint>

// Decides which surfaces the path tracer builds from their captured raster vertex() output.
class RTVertexCapture {
public:
	// Value of the rendering/pathtracing/vertex_capture_mode project setting.
	enum Mode : uint8_t {
		MODE_AUTOMATIC,
		MODE_OPT_IN,
		MODE_DISABLED,
	};

	// Per-shader rt_vertex_* render mode, in the option order of the rt_vertex group.
	enum ShaderOverride : uint8_t {
		OVERRIDE_AUTO,
		OVERRIDE_DYNAMIC,
		OVERRIDE_STATIC,
		// Captured like OVERRIDE_DYNAMIC, then only recaptured when its source, deformation or instances change.
		OVERRIDE_FROZEN,
	};

	// Inputs read by vertex() that invalidate a capture (bitmask).
	enum Dependency : uint32_t {
		DEP_TIME = 1 << 0,
		DEP_CAMERA = 1 << 1,
		DEP_MODEL_TRANSFORM = 1 << 2,
		DEP_INSTANCE_UNIFORMS = 1 << 3,
		DEP_MATERIAL_UNIFORMS = 1 << 4,
		DEP_GLOBAL_UNIFORMS = 1 << 5,
		// Conservative mask, used when the dependencies can't be determined.
		DEP_ALL = (1 << 6) - 1,
		// Not computed yet.
		DEP_UNKNOWN = 0xFFFFFFFFu,
	};

	// What vertex() reads, from SceneShaderRaytracing::compute_vertex_capture_dependencies().
	struct VertexDependencies {
		// Dependency bits. DEP_ALL when the code doesn't parse, whose DEP_TIME then recaptures every frame.
		uint32_t mask = DEP_UNKNOWN;
		// Global uniforms vertex() reads, in first-read order. Only their values set the global_uniforms stamp.
		Vector<StringName> global_uniforms;
	};

	// What a shader's user code does to geometry. Recorded regardless of the mode so the mode can switch at runtime without recompiles.
	struct ShaderTraits {
		// From the rt_vertex_* render mode.
		ShaderOverride override_mode = OVERRIDE_AUTO;
		// User code writes VERTEX in vertex(). The injected fixed-function billboard doesn't count.
		bool writes_vertex = false;
		// User code writes MODELVIEW_MATRIX or PROJECTION_MATRIX in vertex().
		bool writes_modelview_or_projection = false;
		// The shader uses the particle_trails render mode.
		bool particle_trails = false;
		// Dependencies of vertex(), filled in by the path tracer on first capture. Reset with the traits when the code changes.
		VertexDependencies dependencies;
		// Unique per compiled code, assigned together with the dependencies; captures of older code are rebuilt.
		uint32_t code_version = 0;
	};

	// Dependencies of a captured surface: those of its vertex(), plus the camera and model transform the injected
	// fixed-function billboard reads. A frozen shader keeps its capture regardless of them, so it has none.
	static constexpr uint32_t surface_dependencies(const ShaderTraits &p_traits, bool p_camera_billboard) {
		if (p_traits.override_mode == OVERRIDE_FROZEN) {
			return 0;
		}
		return p_traits.dependencies.mask | (p_camera_billboard ? uint32_t(DEP_CAMERA | DEP_MODEL_TRANSFORM) : 0u);
	}

	// Inputs of vertex() when a capture was recorded. Each field is a hash or counter of one input; fields of
	// dependencies the shader doesn't have may stay 0.
	struct Stamps {
		uint64_t frame = 0; // DEP_TIME.
		uint64_t global_uniforms = 0; // DEP_GLOBAL_UNIFORMS: values of the global uniforms vertex() reads.
		uint64_t camera = 0; // DEP_CAMERA: view and projection of the capturing viewport.
		uint64_t model_transform = 0; // DEP_MODEL_TRANSFORM.
		uint64_t instance_uniforms = 0; // DEP_INSTANCE_UNIFORMS: the instance's slice of the global uniform buffer.
		uint64_t material = 0; // DEP_MATERIAL_UNIFORMS: material identity and parameter counter.
		uint64_t deformation = 0; // Skeleton or blend shape output vertex() reads; compared for every shader.
		uint64_t instances = 0; // MultiMesh or particle instance records vertex() reads; compared for every shader.
		uint64_t source = 0; // Mesh surface, shader code and layout; any change rebuilds.
		uint32_t mode_version = 0; // Version of the vertex_capture_mode setting; any change rebuilds.
	};

	// What to do with a captured surface this frame.
	enum Recapture : uint8_t {
		RECAPTURE_NONE, // Keep the capture and its BLAS.
		RECAPTURE_UPDATE, // Capture again and update the BLAS.
		RECAPTURE_REBUILD, // No usable capture: drop it, capture again and build a new BLAS.
	};

	// Dependencies whose stamp differs between p_previous and p_current.
	static constexpr uint32_t changed_dependencies(const Stamps &p_previous, const Stamps &p_current) {
		return (p_previous.frame != p_current.frame ? uint32_t(DEP_TIME) : 0u) |
				(p_previous.global_uniforms != p_current.global_uniforms ? uint32_t(DEP_GLOBAL_UNIFORMS) : 0u) |
				(p_previous.camera != p_current.camera ? uint32_t(DEP_CAMERA) : 0u) |
				(p_previous.model_transform != p_current.model_transform ? uint32_t(DEP_MODEL_TRANSFORM) : 0u) |
				(p_previous.instance_uniforms != p_current.instance_uniforms ? uint32_t(DEP_INSTANCE_UNIFORMS) : 0u) |
				(p_previous.material != p_current.material ? uint32_t(DEP_MATERIAL_UNIFORMS) : 0u);
	}

	// Decides whether a surface with p_dependencies must be captured again. p_has_capture is false before its first capture.
	static constexpr Recapture needs_recapture(bool p_has_capture, const Stamps &p_previous, const Stamps &p_current, uint32_t p_dependencies) {
		if (!p_has_capture || p_previous.mode_version != p_current.mode_version || p_previous.source != p_current.source) {
			return RECAPTURE_REBUILD;
		}
		if ((changed_dependencies(p_previous, p_current) & p_dependencies) != 0 || p_previous.deformation != p_current.deformation ||
				p_previous.instances != p_current.instances) {
			return RECAPTURE_UPDATE;
		}
		return RECAPTURE_NONE;
	}

	// Current value of a global uniform: its floats in the global uniform buffer, or the texture of a sampler global.
	// A global that doesn't exist has neither.
	struct GlobalUniformValue {
		Span<const float> buffer;
		uint64_t texture = 0;
	};

	// Stamp of the values of the global uniforms p_names; p_get_value(name) returns the GlobalUniformValue of one.
	// Names are looked up on every call, so globals that were added, removed or retyped since are read correctly.
	template <typename GetValue>
	static uint64_t global_uniforms_stamp(const Vector<StringName> &p_names, const GetValue &p_get_value) {
		uint32_t lanes[2] = { HASH_MURMUR3_SEED, 0x9E3779B9u };
		for (const StringName &name : p_names) {
			const GlobalUniformValue value = p_get_value(name);
			const int bytes = int(value.buffer.size() * sizeof(float));
			for (uint32_t &lane : lanes) {
				lane = hash_murmur3_one_64(value.texture, hash_murmur3_one_32(uint32_t(bytes), hash_murmur3_buffer(value.buffer.ptr(), bytes, lane)));
			}
		}
		return (uint64_t(lanes[1]) << 32) | lanes[0];
	}

	// True when surfaces using a shader with p_traits must be vertex-captured for the path tracer.
	static constexpr bool needs_capture(const ShaderTraits &p_traits, Mode p_mode) {
		if (p_mode == MODE_DISABLED || p_traits.override_mode == OVERRIDE_STATIC) {
			return false;
		}
		if (p_traits.override_mode == OVERRIDE_DYNAMIC || p_traits.override_mode == OVERRIDE_FROZEN || p_traits.particle_trails) {
			return true;
		}
		return p_mode == MODE_AUTOMATIC && (p_traits.writes_vertex || p_traits.writes_modelview_or_projection);
	}

	// Byte layout of one capture in the capture arena: float3 positions of every point (instance * V + vertex),
	// then the packed TBN block (octahedral normal, then the tangent with the binormal sign) when the mesh has normals.
	// Matches the deformed vertex buffers and the merged MultiMesh vertex buffers the path tracer builds BLASes from.
	struct Layout {
		uint32_t vertex_count = 0; // V, vertices per instance.
		uint32_t instance_count = 1; // N, instances drawn.
		bool has_normal = false;
		bool has_tangent = false; // Only stored together with the normal.

		constexpr uint64_t point_count() const { return uint64_t(vertex_count) * instance_count; }
		constexpr uint64_t point_index(uint32_t p_instance, uint32_t p_vertex) const { return uint64_t(p_instance) * vertex_count + p_vertex; }
		constexpr uint32_t tbn_stride() const { return has_normal ? (has_tangent ? 8u : 4u) : 0u; }
		constexpr uint64_t position_bytes() const { return point_count() * 12u; }
		constexpr uint64_t total_bytes() const { return point_count() * (12u + tbn_stride()); }
	};

	// Instances of a MultiMesh or particle draw. A particle_trails shader draws one instance per particle and reads all
	// of its trail_steps consecutive records, so each instance spans that many records.
	struct Instances {
		uint32_t count = 0; // N.
		uint32_t records_per_instance = 1;
		uint32_t stride = 0; // Floats per instance in the instance buffer.
	};

	// Instances drawn from p_records instance records of p_record_stride floats each, in groups of p_trail_steps
	// records when the shader uses particle trails.
	static constexpr Instances drawn_instances(uint32_t p_records, uint32_t p_record_stride, uint32_t p_trail_steps, bool p_uses_trails) {
		const uint32_t per_instance = p_uses_trails && p_trail_steps > 1 ? p_trail_steps : 1u;
		return Instances{ p_records / per_instance, per_instance, p_record_stride * per_instance };
	}

	enum DrawFlags : uint32_t {
		DRAW_FLAG_NORMAL = 1u << 0,
		DRAW_FLAG_TANGENT = 1u << 1,
	};

	// Per-draw parameters of the capture pass. Must match RTCaptureDraw in rt_capture_inc.glsl (std430).
	struct DrawParams {
		// Rows of the affine transform from raster view space to capture space; its basis also maps normals and tangents.
		float view_to_local[12] = {};
		uint32_t position_offset = 0; // First position word in the arena.
		uint32_t tbn_offset = 0; // First TBN word in the arena.
		uint32_t vertex_count = 0; // V; the output index of a point is instance * V + vertex.
		uint32_t flags = 0; // DrawFlags.
		// Capture-space point (w unused) that points of culled instances collapse to: particles the particle system
		// hid with a non-finite transform, and any point vertex() moved to infinity.
		float dead_position[4] = {};
	};
	static_assert(sizeof(DrawParams) == 80, "Must match RTCaptureDraw in rt_capture_inc.glsl.");

	// Parameters for capturing p_layout at byte p_arena_offset (4-byte aligned) into the space p_local_to_world maps
	// to world space, as seen by a raster pass with camera p_camera. The hit shader maps captured normals with the
	// basis of p_local_to_world, so they use the same inverse as positions. p_dead_position is in capture space.
	static DrawParams make_draw_params(const Layout &p_layout, uint32_t p_arena_offset, const Transform3D &p_local_to_world, const Transform3D &p_camera, const Vector3 &p_dead_position) {
		DrawParams params;
		const Transform3D view_to_local = p_local_to_world.affine_inverse() * p_camera;
		for (int row = 0; row < 3; row++) {
			params.view_to_local[row * 4 + 0] = float(view_to_local.basis.rows[row][0]);
			params.view_to_local[row * 4 + 1] = float(view_to_local.basis.rows[row][1]);
			params.view_to_local[row * 4 + 2] = float(view_to_local.basis.rows[row][2]);
			params.view_to_local[row * 4 + 3] = float(view_to_local.origin[row]);
		}
		params.position_offset = p_arena_offset / 4u;
		params.tbn_offset = uint32_t((p_arena_offset + p_layout.position_bytes()) / 4u);
		params.vertex_count = p_layout.vertex_count;
		params.flags = (p_layout.has_normal ? DRAW_FLAG_NORMAL : 0u) | (p_layout.has_normal && p_layout.has_tangent ? DRAW_FLAG_TANGENT : 0u);
		params.dead_position[0] = float(p_dead_position.x);
		params.dead_position[1] = float(p_dead_position.y);
		params.dead_position[2] = float(p_dead_position.z);
		return params;
	}
};
