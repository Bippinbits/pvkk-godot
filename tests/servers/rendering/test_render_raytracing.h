/**************************************************************************/
/*  test_render_raytracing.h                                              */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/rendering_rt_material_state.h"

#include "tests/test_macros.h"

namespace TestRenderRaytracing {

using namespace RendererSceneRenderImplementation;

static constexpr uint32_t transparent(uint32_t p_blend_class) {
	return RT_MAT_FLAG_TRANSPARENT | (p_blend_class << RT_MAT_BLEND_CLASS_SHIFT);
}

TEST_CASE("[RenderRaytracing] Velocity ownership follows the dominant peel layer rule") {
	constexpr float dominant = 0.5f;
	CHECK(rt_material_can_own_velocity(0, dominant));
	CHECK(rt_material_can_own_velocity(RT_MAT_FLAG_ALPHA_SCISSOR, dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MIX), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_PREMULT), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MUL), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_OIT), dominant));
	CHECK_FALSE(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_ADD), dominant));
	CHECK_FALSE(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_SUB), dominant));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_ADD) | RT_MAT_FLAG_DEPTH_DRAW_ALWAYS, dominant));
}

TEST_CASE("[RenderRaytracing] A zero dominant layer alpha leaves velocity to opaque and depth_draw_always surfaces") {
	CHECK(rt_material_can_own_velocity(0, 0.0f));
	CHECK_FALSE(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MIX), 0.0f));
	CHECK(rt_material_can_own_velocity(transparent(RT_MAT_BLEND_CLASS_MIX) | RT_MAT_FLAG_DEPTH_DRAW_ALWAYS, 0.0f));
}

TEST_CASE("[RenderRaytracing] Emission flags carry the operator and UV2 addressing only when emission samples UV2") {
	CHECK(rt_material_emission_flags(false, false, false, false) == 0u);
	CHECK(rt_material_emission_flags(true, false, false, false) == uint32_t(RT_MAT_FLAG_EMISSION_ADD));
	CHECK(rt_material_emission_flags(false, false, true, true) == 0u);
	CHECK(rt_material_emission_flags(false, true, false, true) == uint32_t(RT_MAT_FLAG_EMISSION_UV2));
	CHECK(rt_material_emission_flags(false, true, true, false) == uint32_t(RT_MAT_FLAG_EMISSION_UV2 | RT_MAT_FLAG_UV2_TRIPLANAR));
	CHECK(rt_material_emission_flags(true, true, true, true) ==
			uint32_t(RT_MAT_FLAG_EMISSION_ADD | RT_MAT_FLAG_EMISSION_UV2 | RT_MAT_FLAG_UV2_TRIPLANAR | RT_MAT_FLAG_UV2_TRIPLANAR_WORLD));
}

TEST_CASE("[RenderRaytracing] BaseMaterial3D state survives the material parameter round trip") {
	CHECK(RTBaseMaterialState().encode() == 0);

	RTBaseMaterialState state;
	state.uv1_triplanar = true;
	state.uv2_world_triplanar = true;
	state.uv2_vertex_transform = true;
	state.distance_fade_mode = 3;
	state.emission = true;
	state.emission_on_uv2 = true;
	state.ambient_occlusion = true;
	state.roughness_texture_channel = 4;

	const int64_t packed = state.encode();
	CHECK(packed > 0);
	CHECK(packed <= INT32_MAX);

	const RTBaseMaterialState decoded = RTBaseMaterialState::decode(packed);
	CHECK(decoded.uv1_triplanar);
	CHECK_FALSE(decoded.uv1_world_triplanar);
	CHECK_FALSE(decoded.uv2_triplanar);
	CHECK(decoded.uv2_world_triplanar);
	CHECK(decoded.uv2_vertex_transform);
	CHECK_FALSE(decoded.proximity_fade);
	CHECK(decoded.distance_fade_mode == 3);
	CHECK(decoded.emission);
	CHECK_FALSE(decoded.emission_add);
	CHECK(decoded.emission_on_uv2);
	CHECK_FALSE(decoded.orm);
	CHECK(decoded.ambient_occlusion);
	CHECK(decoded.roughness_texture_channel == 4);
	CHECK(decoded.encode() == packed);
}

TEST_CASE("[RenderRaytracing] The default material is a non-emissive dielectric with identity UV transforms") {
	const RT_MaterialData d = rt_material_data_default();
	CHECK(d.flags == 0u);
	CHECK(d.emission_strength == 0.0f);
	CHECK(d.emission_texture.index == 0u);
	CHECK(d.albedo_color[3] == 1.0f);
	CHECK(d.albedo_texture.index == 0u);
	CHECK(d.albedo_texture.instance_transparency == 0u);
	CHECK(d.surface_params.roughness == 255u);
	CHECK(d.surface_params.metallic == 0u);
	CHECK(d.surface_params.specular == 128u);
	for (int i = 0; i < 3; i++) {
		CHECK(d.emission_color[i] == 0.0f);
		CHECK(d.uv1_scale[i] == 1.0f);
		CHECK(d.uv1_offset[i] == 0.0f);
	}
	CHECK(Math::half_to_float(d.uv2_scale_xy.x) == 1.0f);
	CHECK(Math::half_to_float(d.uv2_scale_xy.y) == 1.0f);
	CHECK(Math::half_to_float(d.uv2_scale_offset_z.scale) == 1.0f);
	CHECK(d.uv2_scale_offset_z.offset == 0u);
	CHECK(d.uv2_offset_xy.x == 0u);
	CHECK(d.uv2_offset_xy.y == 0u);
	CHECK(Math::half_to_float(d.blend_sharpness.uv1) == 1.0f);
	CHECK(Math::half_to_float(d.blend_sharpness.uv2) == 1.0f);
}

TEST_CASE("[RenderRaytracing] Packed UV fields round-trip, with the UV2 offset reduced modulo 1") {
	RT_MaterialData d = rt_material_data_default();
	rt_material_set_packed_uv(d, 8.0f, Vector3(2.0f, 0.5f, 4.0f), Vector3(3.25f, -0.75f, 1.5f), 40.0f);
	CHECK(Math::half_to_float(d.uv2_scale_xy.x) == 2.0f);
	CHECK(Math::half_to_float(d.uv2_scale_xy.y) == 0.5f);
	CHECK(Math::half_to_float(d.uv2_scale_offset_z.scale) == 4.0f);
	CHECK(Math::half_to_float(d.uv2_offset_xy.x) == 0.25f);
	CHECK(Math::half_to_float(d.uv2_offset_xy.y) == 0.25f);
	CHECK(Math::half_to_float(d.uv2_scale_offset_z.offset) == 0.5f);
	CHECK(Math::half_to_float(d.blend_sharpness.uv1) == 8.0f);
	CHECK(Math::half_to_float(d.blend_sharpness.uv2) == 40.0f);
}

template <typename T>
static uint32_t as_uint(const T &p_bitfields) {
	static_assert(sizeof(T) == sizeof(uint32_t));
	uint32_t word = 0;
	memcpy(&word, &p_bitfields, sizeof(word));
	return word;
}

TEST_CASE("[RenderRaytracing] Material bitfields fill from the low bit, as the GLSL accessors unpack them") {
	RT_MaterialData d = {};
	d.roughness_texture.index = BindlessBlock::MAX_BINDLESS_TEXTURES - 1;
	d.roughness_texture.channel = RT_TEXTURE_CHANNEL_GRAYSCALE;
	CHECK(as_uint(d.roughness_texture) == ((BindlessBlock::MAX_BINDLESS_TEXTURES - 1) | (uint32_t(RT_TEXTURE_CHANNEL_GRAYSCALE) << RT_MaterialData::TEXTURE_INDEX_BITS)));

	d.albedo_texture.index = 7;
	d.albedo_texture.instance_transparency = 0xAB;
	CHECK(as_uint(d.albedo_texture) == (7u | (0xABu << RT_MaterialData::TEXTURE_INDEX_BITS)));

	rt_material_set_surface_params(d, 1.0f, 0.0f, 0.5f);
	CHECK(as_uint(d.surface_params) == (255u | (128u << 16)));

	// unpackHalf2x16() reads x from the low half.
	d.uv2_scale_xy.x = 0x1234;
	d.uv2_scale_xy.y = 0xABCD;
	CHECK(as_uint(d.uv2_scale_xy) == 0xABCD1234u);
}

TEST_CASE("[RenderRaytracing] Instance transparency is zero when opaque and saturates outside [0, 1]") {
	CHECK(rt_instance_transparency(1.0f) == 0u);
	CHECK(rt_instance_transparency(0.0f) == 255u);
	CHECK(rt_instance_transparency(0.5f) == 127u);
	CHECK(rt_instance_transparency(2.0f) == 0u);
	CHECK(rt_instance_transparency(-1.0f) == 255u);
}

TEST_CASE("[RenderRaytracing] Light distance fade is full before the begin distance and smoothstepped to zero over the length") {
	using RendererRD::LightStorage;
	CHECK(LightStorage::distance_fade_factor(10.0f, 40.0f, 10.0f) == 1.0f);
	CHECK(LightStorage::distance_fade_factor(40.0f, 40.0f, 10.0f) == 1.0f);
	CHECK(LightStorage::distance_fade_factor(45.0f, 40.0f, 10.0f) == doctest::Approx(0.5f));
	CHECK(LightStorage::distance_fade_factor(42.0f, 40.0f, 10.0f) == doctest::Approx(Math::smoothstep(0.0f, 1.0f, 0.8f)));
	CHECK(LightStorage::distance_fade_factor(50.0f, 40.0f, 10.0f) == 0.0f);
	CHECK(LightStorage::distance_fade_factor(60.0f, 40.0f, 10.0f) == 0.0f);
	CHECK(LightStorage::distance_fade_factor(41.0f, 40.0f, 0.0f) == 0.0f);
}

#ifdef RT_VALIDATION_ENABLED
static RT_GeometryData rtv_geometry(uint64_t p_vertex_address, uint32_t p_vertex_count) {
	RT_GeometryData geometry = {};
	geometry.vertex_buffer_address = p_vertex_address;
	geometry.vertex_count = p_vertex_count;
	geometry.position_stride = 12;
	geometry.normal_byte_offset = RT_OFFSET_NONE;
	geometry.tangent_byte_offset = RT_OFFSET_NONE;
	geometry.index_format = RT_INDEX_FORMAT_NONE;
	return geometry;
}

TEST_CASE("[RenderRaytracing] Strict validation requires owned address buffers to be declared and large enough") {
	RTValidation validation;
	const RID buffer = RID::from_uint64(1);
	validation.track_buffer(buffer, 0x10000, 1200, "vertices");
	validation.begin_view(1);
	const RT_GeometryData geometries[] = { rtv_geometry(0x10000, 100) };
	HashSet<RID> declared;
	LocalVector<String> issues;

	validation.check_view(geometries, Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.size() == 1);

	declared.insert(buffer);
	issues.clear();
	validation.check_view(geometries, Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.is_empty());

	const RT_GeometryData past_end[] = { rtv_geometry(0x10000, 101) };
	validation.check_view(past_end, Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.size() == 1);
}

TEST_CASE("[RenderRaytracing] Strict validation rejects addresses outside the live buffers of the frame") {
	RTValidation validation;
	validation.begin_view(1);
	const RT_GeometryData geometries[] = { rtv_geometry(0x20000, 10) };
	const HashSet<RID> declared;
	LocalVector<String> issues;

	validation.check_view(geometries, Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.size() == 1);

	validation.note_external(0x20000, 0);
	issues.clear();
	validation.check_view(geometries, Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.is_empty());

	validation.begin_view(2);
	validation.check_view(geometries, Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.size() == 1);
}

TEST_CASE("[RenderRaytracing] Strict validation orders in-place BLAS writes after a declared input write") {
	RTValidation validation;
	const RID input = RID::from_uint64(1);
	const RID blas = RID::from_uint64(2);
	const RID inputs[] = { input };
	validation.track_blas(blas, inputs, "blas");
	HashSet<RID> declared;
	declared.insert(input);
	LocalVector<String> issues;

	validation.begin_view(1);
	validation.note_blas_queued(blas);
	validation.check_view(Span<RT_GeometryData>(), Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.is_empty());

	validation.begin_view(2);
	validation.note_blas_queued(blas);
	validation.check_view(Span<RT_GeometryData>(), Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.size() == 1);

	validation.begin_view(3);
	validation.note_write(input);
	validation.note_blas_queued(blas);
	issues.clear();
	validation.check_view(Span<RT_GeometryData>(), Span<RT_MaterialData>(), 0, declared, issues);
	CHECK(issues.is_empty());
}

TEST_CASE("[RenderRaytracing] Strict validation checks TLAS instances and bindless texture indices") {
	RTValidation validation;
	const RID blas = RID::from_uint64(3);
	validation.track_blas(blas, Span<RID>(), "blas");
	validation.begin_view(1);
	const RID instance_blases[] = { blas, RID::from_uint64(4) };
	const uint32_t custom_indices[] = { 0, 5 };
	LocalVector<String> issues;

	// Never queued, untracked, and a custom index past the geometry table.
	validation.check_tlas(instance_blases, custom_indices, 1, "TLAS", issues);
	CHECK(issues.size() == 3);

	RT_MaterialData material = rt_material_data_default();
	material.albedo_texture.index = 7;
	const RT_MaterialData materials[] = { material };
	issues.clear();
	validation.check_view(Span<RT_GeometryData>(), materials, 4, HashSet<RID>(), issues);
	CHECK(issues.size() == 1);

	// The instance transparency is not part of the bindless index.
	RT_MaterialData faded = rt_material_data_default();
	faded.albedo_texture.index = 3;
	faded.albedo_texture.instance_transparency = rt_instance_transparency(0.0f);
	const RT_MaterialData faded_materials[] = { faded };
	issues.clear();
	validation.check_view(Span<RT_GeometryData>(), faded_materials, 4, HashSet<RID>(), issues);
	CHECK(issues.is_empty());

	issues.clear();
	validation.check_texture_slot("tex", 60, 64, 3, 4);
	validation.check_view(Span<RT_GeometryData>(), Span<RT_MaterialData>(), 4, HashSet<RID>(), issues);
	CHECK(issues.is_empty());
	validation.check_texture_slot("tex", 62, 64, 9, 4);
	validation.check_view(Span<RT_GeometryData>(), Span<RT_MaterialData>(), 4, HashSet<RID>(), issues);
	CHECK(issues.size() == 2);
}
#endif

TEST_CASE("[RenderRaytracing] Material buffer uniform sizes match the shader compiler layout") {
	using Uniform = ShaderLanguage::ShaderNode::Uniform;
	Uniform uniform;
	uniform.type = ShaderLanguage::TYPE_FLOAT;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 4);

	uniform.array_size = 3;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 48);

	uniform.type = ShaderLanguage::TYPE_MAT4;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 192);

	uniform.type = ShaderLanguage::TYPE_VEC4;
	uniform.array_size = 0;
	uniform.scope = Uniform::SCOPE_GLOBAL;
	CHECK(SceneShaderRaytracing::material_buffer_uniform_size(uniform) == 4);
}

TEST_CASE("[RenderRaytracing] Shadow renderers only apply while shadows are enabled and visible") {
	constexpr RS::LightShadowRenderer RASTER = RS::LIGHT_SHADOW_RENDERER_RASTERIZER;
	constexpr RS::LightShadowRenderer PT = RS::LIGHT_SHADOW_RENDERER_PATH_TRACER;
	constexpr uint32_t ALL = RS::LIGHT_SHADOW_RENDERER_ALL;

	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(false, 1.0f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(false, 1.0f, ALL, PT));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, ALL, RASTER));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, ALL, PT));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, PT, PT));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, PT, RASTER));
	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, RASTER, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, RASTER, PT));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 1.0f, 0, RASTER));

	CHECK(RendererLightStorage::is_shadow_drawn_by(true, 0.01f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 0.0f, ALL, RASTER));
	CHECK_FALSE(RendererLightStorage::is_shadow_drawn_by(true, 0.0f, ALL, PT));
}

TEST_CASE("[RenderRaytracing] Instance data layout packs stride and field offsets") {
	CHECK(rt_pack_instance_data_layout(20, 12, 16) == (20u | (12u << 8) | (16u << 16)));
	const uint32_t no_fields = rt_pack_instance_data_layout(12, UINT32_MAX, UINT32_MAX);
	CHECK((no_fields & 0xFFu) == 12u);
	CHECK(((no_fields >> 8) & 0xFFu) == RT_INSTANCE_DATA_FIELD_NONE);
	CHECK(((no_fields >> 16) & 0xFFu) == RT_INSTANCE_DATA_FIELD_NONE);
}

// What one camera ray sees of a triangle: whether it is drawn, its FRONT_FACING and whether its normal is flipped.
struct FacingResult {
	bool visible = false;
	bool front_facing = false;
	bool normal_flipped = false;

	bool operator==(const FacingResult &p_other) const {
		return visible == p_other.visible && (!visible || (front_facing == p_other.front_facing && normal_flipped == p_other.normal_flipped));
	}
};

// The rasterizer: culls by the screen winding, reversed for mirrored instances; FRONT_FACING is the screen winding.
static FacingResult raster_facing(RS::CullMode p_cull_mode, bool p_raster_mirror, bool p_world_front) {
	const bool culled_front = p_world_front != p_raster_mirror;
	FacingResult result;
	result.visible = p_cull_mode == RS::CULL_MODE_DISABLED || culled_front == (p_cull_mode == RS::CULL_MODE_BACK);
	result.front_facing = p_world_front;
	result.normal_flipped = p_cull_mode != RS::CULL_MODE_BACK && !p_world_front;
	return result;
}

// The ray tracer: decides facing in BLAS space, where the default front face is the rasterizer's back face, culls back
// faces unless the instance disables it, and the hit shader applies the geometry flags.
static FacingResult traced_facing(const RTTriangleFacing &p_facing, bool p_blas_front) {
	const bool flip = p_facing.instance_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
	const bool hit_front = p_blas_front == flip;
	FacingResult result;
	result.visible = hit_front || (p_facing.instance_flags & RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT);
	result.front_facing = hit_front != bool(p_facing.geometry_flags & RT_GEOM_FLAG_FACING_INVERTED);
	result.normal_flipped = !result.front_facing && (p_facing.geometry_flags & RT_GEOM_FLAG_SIDE_CHECK);
	return result;
}

TEST_CASE("[RenderRaytracing] Path traced triangle facing matches raster for every cull mode and mirroring") {
	const RS::CullMode cull_modes[] = { RS::CULL_MODE_BACK, RS::CULL_MODE_FRONT, RS::CULL_MODE_DISABLED };
	for (RS::CullMode cull_mode : cull_modes) {
		for (int bits = 0; bits < 8; bits++) {
			const bool raster_mirror = bits & 1;
			// Billboards, captures and merged instances can have a TLAS transform mirrored unlike the node.
			const bool tlas_mirror = bits & 2;
			const bool blas_front = bits & 4;
			const bool world_front = blas_front != tlas_mirror;
			const FacingResult traced = traced_facing(rt_triangle_facing(cull_mode, raster_mirror, tlas_mirror), blas_front);
			CHECK_MESSAGE(traced == raster_facing(cull_mode, raster_mirror, world_front),
					vformat("cull mode %d, raster mirror %d, TLAS mirror %d, BLAS front %d", int(cull_mode), int(raster_mirror), int(tlas_mirror), int(blas_front)));
		}
	}
}

TEST_CASE("[RenderRaytracing] Unmirrored back face culling keeps the TLAS facing bits of a plain mesh") {
	const RTTriangleFacing facing = rt_triangle_facing(RS::CULL_MODE_BACK, false, false);
	CHECK(facing.instance_flags == uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT));
	CHECK(facing.geometry_flags == 0);
}

using DynamicDebug = RTDynamicGeometryDebug;

static DynamicDebug::State dynamic_state(DynamicDebug::Category p_category, uint32_t p_age = 0, DynamicDebug::Category p_rest = DynamicDebug::CATEGORY_STATIC) {
	DynamicDebug::State state;
	state.category = p_category;
	state.rest = p_rest;
	state.age = p_age;
	return state;
}

static DynamicDebug::Stamps dynamic_stamps(uint64_t p_build, uint64_t p_refit, bool p_dynamic = false) {
	DynamicDebug::Stamps stamps;
	stamps.last_build = p_build;
	stamps.last_refit = p_refit;
	stamps.dynamic = p_dynamic;
	return stamps;
}

TEST_CASE("[RTDynamicGeometryDebug] Geometry without BLAS work is static, or moved when its transform changed") {
	const DynamicDebug::Stamps none;
	CHECK(DynamicDebug::classify(none, false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(none, true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(none, false, true, 100) == dynamic_state(DynamicDebug::CATEGORY_MOVED));
}

TEST_CASE("[RTDynamicGeometryDebug] The most recent BLAS work picks the category, a build on ties") {
	CHECK(DynamicDebug::classify(dynamic_stamps(100, DynamicDebug::NEVER), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD));
	CHECK(DynamicDebug::classify(dynamic_stamps(DynamicDebug::NEVER, 100), false, true, 100) == dynamic_state(DynamicDebug::CATEGORY_REFIT));
	CHECK(DynamicDebug::classify(dynamic_stamps(96, 99), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_REFIT, 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(99, 96), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(100, 100), false, false, 100) == dynamic_state(DynamicDebug::CATEGORY_BUILD));
}

TEST_CASE("[RTDynamicGeometryDebug] Captured geometry gets its own build and refit categories") {
	CHECK(DynamicDebug::classify(dynamic_stamps(100, DynamicDebug::NEVER), true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILD));
	CHECK(DynamicDebug::classify(dynamic_stamps(90, 98), true, false, 100) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT, 2));
}

TEST_CASE("[RTDynamicGeometryDebug] BLAS work fades out after FADE_FRAMES") {
	const uint64_t frame = 1000;
	const uint64_t last_visible = frame - (DynamicDebug::FADE_FRAMES - 1);
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible, DynamicDebug::NEVER), false, false, frame) == dynamic_state(DynamicDebug::CATEGORY_BUILD, DynamicDebug::FADE_FRAMES - 1));
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible - 1, DynamicDebug::NEVER), false, false, frame) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(dynamic_stamps(last_visible - 1, DynamicDebug::NEVER), false, true, frame) == dynamic_state(DynamicDebug::CATEGORY_MOVED));
}

TEST_CASE("[RTDynamicGeometryDebug] A dynamic BLAS whose last work was a build fades to a build idle category") {
	const uint64_t built = 100;
	const uint64_t idle = built + DynamicDebug::FADE_FRAMES;
	const DynamicDebug::Stamps dynamic_build = dynamic_stamps(built, DynamicDebug::NEVER, true);
	CHECK(DynamicDebug::classify(dynamic_build, false, false, built) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, false, false, idle) == dynamic_state(DynamicDebug::CATEGORY_BUILT_IDLE, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, true, false, idle) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE, 0, DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK(DynamicDebug::classify(dynamic_build, false, true, idle) == dynamic_state(DynamicDebug::CATEGORY_MOVED, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	// A build after a refit settles; a refit after the build fades to static, as does a BLAS built once at load.
	CHECK(DynamicDebug::classify(dynamic_stamps(built, built - 1, true), false, false, idle).rest == DynamicDebug::CATEGORY_BUILT_IDLE);
	CHECK(DynamicDebug::classify(dynamic_stamps(built, built + 1, true), false, false, idle + 1) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(DynamicDebug::classify(dynamic_stamps(built, DynamicDebug::NEVER), false, false, idle) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
}

TEST_CASE("[RTDynamicGeometryDebug] Packing keeps the category in the low bits, the rest category and the age above") {
	const uint32_t packed = DynamicDebug::pack(dynamic_state(DynamicDebug::CATEGORY_CAPTURED_BUILD, 17, DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK((packed & DynamicDebug::CATEGORY_MASK) == uint32_t(DynamicDebug::CATEGORY_CAPTURED_BUILD));
	CHECK(((packed >> DynamicDebug::REST_SHIFT) & DynamicDebug::CATEGORY_MASK) == uint32_t(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE));
	CHECK((packed >> DynamicDebug::AGE_SHIFT) == 17u);
	CHECK(DynamicDebug::pack(DynamicDebug::State()) == 0u);
	static_assert(DynamicDebug::CATEGORY_CAPTURED_BUILT_IDLE <= DynamicDebug::CATEGORY_MASK, "Categories must fit their bits.");
	static_assert(DynamicDebug::REST_SHIFT * 2 <= DynamicDebug::AGE_SHIFT, "The rest category must fit below the age.");
	static_assert(DynamicDebug::FADE_FRAMES <= (UINT32_MAX >> DynamicDebug::AGE_SHIFT), "Ages must fit above the categories.");
}

TEST_CASE("[RTDynamicGeometryDebug] The tracker stamps BLASes per frame and forgets faded ones") {
	DynamicDebug tracker;
	const RID skinned = RID::from_uint64(0x100000001ULL);
	const RID captured = RID::from_uint64(0x100000002ULL);
	const RID static_blas = RID::from_uint64(0x100000003ULL);

	tracker.begin_frame(10);
	tracker.record_build(skinned);
	tracker.record_refit(captured);
	CHECK(tracker.get_tracked_count() == 2);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_BUILD, 0, DynamicDebug::CATEGORY_BUILT_IDLE));
	CHECK(tracker.observe(captured, true, false, true) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT));
	CHECK(tracker.observe(static_blas, false, false, false) == dynamic_state(DynamicDebug::CATEGORY_STATIC));
	CHECK(tracker.get_tracked_count() == 2);

	tracker.begin_frame(11);
	tracker.record_refit(skinned);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT));
	CHECK(tracker.observe(captured, true, false, true) == dynamic_state(DynamicDebug::CATEGORY_CAPTURED_REFIT, 1));

	// Repeated calls within a frame (one per viewport) keep the frame's stamps.
	tracker.begin_frame(11);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT));

	tracker.begin_frame(10 + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 1);
	CHECK(tracker.observe(skinned, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_REFIT, DynamicDebug::FADE_FRAMES - 1));
	tracker.begin_frame(11 + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 0);

	tracker.record_build(static_blas);
	tracker.clear();
	CHECK(tracker.get_tracked_count() == 0);
}

TEST_CASE("[RTDynamicGeometryDebug] The tracker keeps idle built dynamic BLASes while TLAS instances use them") {
	DynamicDebug tracker;
	const RID idle = RID::from_uint64(0x100000001ULL);
	const RID loaded = RID::from_uint64(0x100000002ULL);

	tracker.begin_frame(10);
	tracker.record_build(idle);
	tracker.record_build(loaded);
	tracker.observe(idle, false, false, true);
	tracker.observe(loaded, false, false, false);

	const uint64_t settled = 10 + 2 * DynamicDebug::FADE_FRAMES;
	for (uint64_t frame = 11; frame <= settled; frame++) {
		tracker.begin_frame(frame);
		tracker.observe(idle, false, false, true);
		tracker.observe(loaded, false, false, false);
	}
	CHECK(tracker.get_tracked_count() == 1);
	CHECK(tracker.observe(idle, false, false, true) == dynamic_state(DynamicDebug::CATEGORY_BUILT_IDLE, 0, DynamicDebug::CATEGORY_BUILT_IDLE));

	tracker.begin_frame(settled + DynamicDebug::FADE_FRAMES - 1);
	CHECK(tracker.get_tracked_count() == 1);
	tracker.begin_frame(settled + DynamicDebug::FADE_FRAMES);
	CHECK(tracker.get_tracked_count() == 0);
}

} // namespace TestRenderRaytracing
