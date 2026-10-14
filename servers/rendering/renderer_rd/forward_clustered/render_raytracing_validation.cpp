/**************************************************************************/
/*  render_raytracing_validation.cpp                                      */
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

#include "render_raytracing_validation.h"

#ifdef RT_VALIDATION_ENABLED

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/rendering_server_globals.h"

namespace RendererSceneRenderImplementation {

RTValidation &rt_validation() {
	static RTValidation validation;
	return validation;
}

void RTValidation::begin_view() {
	pending.clear();
}

void RTValidation::check_texture_slot(const String &p_name, uint32_t p_offset, uint32_t p_block_size, uint32_t p_index, uint32_t p_bindless_texture_count) {
	if (uint64_t(p_offset) + sizeof(uint32_t) > p_block_size) {
		pending.push_back(vformat("Custom material texture '%s': its bindless index slot at byte %d lies outside the %d-byte uniform block.", p_name, p_offset, p_block_size));
	}
	if (p_bindless_texture_count > 0 && p_index >= p_bindless_texture_count) {
		pending.push_back(vformat("Custom material texture '%s': bindless index %d is outside the %d bindless textures.", p_name, p_index, p_bindless_texture_count));
	}
}

// The buffer containing p_address, or null. Live buffers never overlap.
static const RD::BufferAddressRange *_rt_find_buffer(Span<RD::BufferAddressRange> p_sorted, uint64_t p_address) {
	uint32_t lo = 0;
	uint32_t hi = p_sorted.size();
	while (lo < hi) {
		const uint32_t mid = (lo + hi) / 2;
		if (p_sorted[mid].address <= p_address) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return nullptr;
	}
	const RD::BufferAddressRange &range = p_sorted[lo - 1];
	return p_address < range.address + range.size ? &range : nullptr;
}

void RTValidation::check_view(const RTValidationView &p_view, LocalVector<String> &r_issues) const {
	ERR_FAIL_COND(p_view.geometry_sources.size() != p_view.geometries.size() || p_view.material_sources.size() != p_view.materials.size());
	ERR_FAIL_NULL(p_view.declared);

	const auto check_address = [&](uint64_t p_address, uint64_t p_extent, RID p_source, const char *p_table, uint32_t p_index, const char *p_field) {
		if (p_address == 0) {
			return;
		}
		const String where = vformat("%s[%d].%s = 0x%s", p_table, p_index, p_field, String::num_uint64(p_address, 16));
		if (!p_source.is_valid()) {
			r_issues.push_back(where + " has no recorded source buffer: take it with _rt_buffer_address().");
			return;
		}
		const RD::BufferAddressRange *range = _rt_find_buffer(p_view.buffers, p_address);
		if (!range) {
			r_issues.push_back(where + vformat(" is not inside any live buffer: buffer (RID %d) it was taken from is freed.", (int64_t)p_source.get_id()));
			return;
		}
		if (range->buffer != p_source) {
			r_issues.push_back(where + vformat(" points into buffer (RID %d), but was taken from buffer (RID %d): that buffer was freed and its memory reused.",
											   (int64_t)range->buffer.get_id(), (int64_t)p_source.get_id()));
			return;
		}
		const uint64_t end = p_address - range->address + MAX(p_extent, (uint64_t)1);
		if (end > range->size) {
			r_issues.push_back(where + vformat(" reads %d bytes past the end of buffer (RID %d).", (int64_t)(end - range->size), (int64_t)range->buffer.get_id()));
		}
		if (range->written && !p_view.declared->has(range->buffer)) {
			r_issues.push_back(where + vformat(" reads buffer (RID %d), which this frame's graph writes, but no traversal declares it: the read is not ordered after the write.",
											   (int64_t)range->buffer.get_id()));
		}
	};

	for (uint32_t i = 0; i < p_view.geometries.size(); i++) {
		const RT_GeometryData &geometry = p_view.geometries[i];
		const RTGeometrySources &sources = p_view.geometry_sources[i];
		const uint64_t vertices = geometry.vertex_count;
		uint64_t vertex_extent = (geometry.flags & RT_GEOM_FLAG_PROCEDURAL) ? MAX(geometry.primitive_count, 1u) * 24ull : vertices * geometry.position_stride;
		if (geometry.normal_byte_offset != RT_OFFSET_NONE) {
			vertex_extent = MAX(vertex_extent, geometry.normal_byte_offset + vertices * geometry.normal_stride);
		}
		if (geometry.tangent_byte_offset != RT_OFFSET_NONE) {
			vertex_extent = MAX(vertex_extent, geometry.tangent_byte_offset + vertices * geometry.tangent_stride);
		}
		check_address(geometry.vertex_buffer_address, vertex_extent, sources.vertex, "geometries", i, "vertex_buffer_address");
		check_address(geometry.attribute_buffer_address, vertices * geometry.attribute_stride, sources.attribute, "geometries", i, "attribute_buffer_address");
		if (geometry.index_format != RT_INDEX_FORMAT_NONE) {
			const uint64_t index_size = geometry.index_format == RT_INDEX_FORMAT_UINT16 ? 2 : 4;
			check_address(geometry.index_buffer_address, uint64_t(geometry.primitive_count) * 3 * index_size, sources.index, "geometries", i, "index_buffer_address");
		}
		const uint64_t prev_address = (uint64_t(geometry.prev_vertex_buffer_address_hi) << 32) | geometry.prev_vertex_buffer_address_lo;
		check_address(prev_address, vertices * 12, sources.prev_vertex, "geometries", i, "prev_vertex_buffer_address");
		if (geometry.instance_data_address != 0) {
			const uint64_t stride_bytes = uint64_t(geometry.instance_data_layout & 0xFF) * sizeof(float);
			const uint64_t records = geometry.instance_data_primitive_divisor ? geometry.primitive_count / geometry.instance_data_primitive_divisor : 1;
			check_address(geometry.instance_data_address, records * stride_bytes, sources.instance_data, "geometries", i, "instance_data_address");
		}
	}

	for (uint32_t i = 0; i < p_view.materials.size(); i++) {
		const RT_MaterialData &material = p_view.materials[i];
		check_address(material.uniform_address, sizeof(uint32_t), p_view.material_sources[i], "materials", i, "uniform_address");
		if (p_view.bindless_texture_count == 0) {
			continue;
		}
		constexpr uint32_t TEXTURE_FIELDS = 6;
		const uint32_t indices[TEXTURE_FIELDS] = { material.albedo_texture_idx, material.normal_texture_idx, material.roughness_texture_idx,
			material.metallic_texture_idx, material.ao_texture_idx, material.emission_texture_idx };
		const char *names[TEXTURE_FIELDS] = { "albedo_texture_idx", "normal_texture_idx", "roughness_texture_idx",
			"metallic_texture_idx", "ao_texture_idx", "emission_texture_idx" };
		for (uint32_t t = 0; t < TEXTURE_FIELDS; t++) {
			if (indices[t] >= p_view.bindless_texture_count) {
				r_issues.push_back(vformat("materials[%d].%s = %d is outside the %d bindless textures.", i, names[t], indices[t], p_view.bindless_texture_count));
			}
		}
	}

	for (const RTInPlaceBlas &blas : p_view.in_place_blases) {
		bool ordered = false;
		for (uint32_t i = 0; !ordered && i < blas.written_inputs.size(); i++) {
			ordered = p_view.declared->has(blas.written_inputs[i]);
		}
		if (!ordered) {
			r_issues.push_back(vformat("BLAS (RID %d) is rewritten in place, but none of its inputs is written in this frame's graph and declared by the traversals: an earlier trace may still walk it.",
					(int64_t)blas.blas.get_id()));
		}
	}

	for (const String &issue : pending) {
		r_issues.push_back(issue);
	}
}

void RTValidation::check_tlas(Span<uint32_t> p_custom_indices, uint32_t p_geometry_count, const char *p_tlas_name, LocalVector<String> &r_issues) const {
	for (uint32_t i = 0; i < p_custom_indices.size(); i++) {
		if (p_custom_indices[i] >= p_geometry_count) {
			r_issues.push_back(vformat("%s instance %d starts at geometry %d of %d.", p_tlas_name, i, p_custom_indices[i], p_geometry_count));
		}
	}
}

bool RTValidation::is_new_issue(const String &p_issue) {
	// Bounded, so issues that differ every frame (such as addresses) stop being reported instead of growing the set.
	constexpr uint32_t MAX_REPORTED = 1024;
	if (reported.has(p_issue) || reported.size() >= MAX_REPORTED) {
		return false;
	}
	reported.insert(p_issue);
	return true;
}

// ---------------------------------------------------------------------------
// RenderRaytracing hooks
// ---------------------------------------------------------------------------

void RenderRaytracing::_rtv_begin_view() {
	RTValidation &validation = rt_validation();
	validation.strict = GLOBAL_GET_CACHED(bool, "rendering/pathtracing/debug/strict_validation");
	validation.begin_view();
	geometry_sources.clear();
	material_sources.clear();
}

RID RenderRaytracing::_rtv_material_source(const RTMaterialData *p_mat_data) const {
	if (p_mat_data->data.uniform_address == 0) {
		return RID();
	}
	return p_mat_data->uniform_buffer.is_valid() ? p_mat_data->uniform_buffer : mat_ubo_pool_buffer;
}

bool RenderRaytracing::_rtv_validate_view(const RTViewportState *p_state, Span<RID> p_built_blases, Span<RID> p_refit_blases) {
	RD *rd = RD::get_singleton();

	LocalVector<RID> declared_list;
	collect_device_address_buffers(p_state, declared_list);
	HashSet<RID> declared;
	for (const RID &buffer : declared_list) {
		declared.insert(buffer);
	}

	LocalVector<RD::BufferAddressRange> buffers;
	rd->buffer_get_device_address_ranges(buffers);

	LocalVector<RTInPlaceBlas> in_place;
	for (const Span<RID> &list : { p_built_blases, p_refit_blases }) {
		for (const RID &blas : list) {
			if (rd->blas_is_built(blas)) {
				in_place.push_back({ blas, {} });
				rd->blas_get_written_input_buffers(blas, in_place[in_place.size() - 1].written_inputs);
			}
		}
	}

	RTValidationView view;
	view.geometries = geometry_data;
	view.geometry_sources = geometry_sources;
	view.materials = material_data;
	view.material_sources = material_sources;
	view.bindless_texture_count = bindless_block ? bindless_block->get_texture_count() : 0;
	view.declared = &declared;
	view.buffers = buffers;
	view.in_place_blases = in_place;

	RTValidation &validation = rt_validation();
	LocalVector<String> issues;
	validation.check_view(view, issues);
	validation.check_tlas(tlas_instances.custom_indices, geometry_data.size(), "Opaque TLAS", issues);
	validation.check_tlas(tlas_transparent_instances.custom_indices, geometry_data.size(), "Transparent TLAS", issues);
	if (issues.is_empty()) {
		return true;
	}

	constexpr uint32_t MAX_PRINTED = 16;
	uint32_t printed = 0;
	for (const String &issue : issues) {
		if (printed < MAX_PRINTED && validation.is_new_issue(issue)) {
			ERR_PRINT("Path tracing strict validation: " + issue);
			printed++;
		}
	}
	WARN_PRINT_ONCE(vformat("Path tracing strict validation found %d problem(s); views with problems are not traced. Each problem is reported once.", (int64_t)issues.size()));
	return false;
}

} // namespace RendererSceneRenderImplementation

#endif // RT_VALIDATION_ENABLED
