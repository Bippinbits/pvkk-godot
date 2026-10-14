/**************************************************************************/
/*  render_raytracing_validation.h                                        */
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

// ---------------------------------------------------------------------------
// RT validation
// ---------------------------------------------------------------------------
//
// The hit shaders read geometry, attributes, indices, instance data and material uniforms through raw device
// addresses stored in RT_GeometryData / RT_MaterialData. Neither RD nor the draw graph can see those reads, so a
// stale address, a freed buffer or a missing barrier shows up as a GPU fault or garbage instead of an error.
// RTValidation catches these on the CPU before the view is submitted.
//
// It keeps no registry of its own. Buffer liveness, addresses, sizes and writes in the draw graph being recorded
// come from RD::buffer_get_device_address_ranges(); BLAS state and inputs from RD::blas_is_built() and
// RD::blas_get_written_input_buffers(). Freeing, creating or writing a buffer therefore needs no bookkeeping.
//
// Availability:
//   - Exists only in DEV_ENABLED builds (profile debug or fulldebug). Elsewhere every hook compiles to nothing.
//   - Runs only while rendering/pathtracing/debug/strict_validation is on. A view with problems is not traced:
//     _rtv_validate_view() fails and build_tlas() returns null. Each distinct problem is printed once.
//
// Rules for render_raytracing.cpp:
//   1. Every device address written into RT_GeometryData comes from _rt_buffer_address(buffer, sources.field), which
//      records the buffer it was taken from in the entry's RTGeometrySources. push_tlas_entry() carries the sources
//      into the frame tables. Material uniform addresses are matched against RTMaterialData::uniform_buffer, or the
//      material UBO pool when it is unset.
//   2. Every buffer read through a device address and written in the draw graph before the trace must be returned by
//      collect_device_address_buffers(), which feeds the raytracing list's buffer dependencies.
//   3. Custom material texture slots written into uniform blocks go through RT_VALIDATE(rt_validation().check_texture_slot(...)).
//
// What gets reported: addresses outside any live buffer (stale or freed), addresses inside a different buffer than the
// one recorded (freed and memory reused), addresses with no recorded buffer (rule 1 missed), reads past the end of a
// buffer, buffers written in the graph but not declared (rule 2 missed), bindless texture indices out of range, TLAS
// custom indices past the geometry table, and BLASes rewritten in place without a written and declared input, which
// earlier traces of the frame may still walk. RD itself rejects TLAS instances of freed or unbuilt BLASes.
//
// Please run with strict validation on after any change to buffer lifetime, table layout or BLAS scheduling.

#ifdef DEV_ENABLED
#define RT_VALIDATION_ENABLED
#endif

#ifdef RT_VALIDATION_ENABLED

#include "core/string/ustring.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/span.h"
#include "servers/rendering/rendering_device.h"

// Runs m_code only while strict validation is on.
#define RT_VALIDATE(m_code)                                                \
	do {                                                                   \
		if (::RendererSceneRenderImplementation::rt_validation().strict) { \
			m_code;                                                        \
		}                                                                  \
	} while (false)

namespace RendererSceneRenderImplementation {

struct RT_GeometryData;
struct RT_MaterialData;
struct RTGeometrySources;

// A BLAS rebuilt or refit in place: it was built before, so traces recorded earlier in the frame may still walk it.
struct RTInPlaceBlas {
	RID blas;
	LocalVector<RID> written_inputs; // Its input buffers written in the draw graph being recorded.
};

// What check_view() inspects for one view.
struct RTValidationView {
	Span<RT_GeometryData> geometries;
	Span<RTGeometrySources> geometry_sources; // Parallel to `geometries`.
	Span<RT_MaterialData> materials;
	Span<RID> material_sources; // Parallel to `materials`: the buffer each uniform_address was taken from.
	uint32_t bindless_texture_count = 0;
	const HashSet<RID> *declared = nullptr; // Buffers the traversals declare as read through device addresses.
	Span<RD::BufferAddressRange> buffers; // From RD::buffer_get_device_address_ranges().
	Span<RTInPlaceBlas> in_place_blases;
};

// CPU-side checks of what the path tracer hands to the GPU, run before submission.
class RTValidation {
public:
	// Mirrors the rendering/pathtracing/debug/strict_validation project setting for the view being built.
	bool strict = false;

	// Starts a view: drops problems recorded for the previous one.
	void begin_view();
	// Checks a bindless texture index written into a custom material's uniform block of p_block_size bytes.
	// Problems are reported with the current view.
	void check_texture_slot(const String &p_name, uint32_t p_offset, uint32_t p_block_size, uint32_t p_index, uint32_t p_bindless_texture_count);

	// Checks the view's geometry and material tables and its in-place BLAS writes. Appends one message per problem to r_issues.
	void check_view(const RTValidationView &p_view, LocalVector<String> &r_issues) const;
	// Checks that each custom index of a TLAS's instances is inside the geometry table.
	void check_tlas(Span<uint32_t> p_custom_indices, uint32_t p_geometry_count, const char *p_tlas_name, LocalVector<String> &r_issues) const;

	// True the first time p_issue is passed, so persistent problems are reported once.
	bool is_new_issue(const String &p_issue);

private:
	LocalVector<String> pending; // This view: problems found while the tables were built.
	HashSet<String> reported;
};

// The validation state shared by all path tracer code.
RTValidation &rt_validation();

} // namespace RendererSceneRenderImplementation

#else

#define RT_VALIDATE(m_code) ((void)0)

#endif // RT_VALIDATION_ENABLED
