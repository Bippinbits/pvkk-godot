/**************************************************************************/
/*  render_forward_clustered_pt.h                                         */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/rt_fog_shadows.h"

namespace RendererSceneRenderImplementation {

// Path-tracing / DLSS variant of the clustered renderer. Inherits all of the
// raster setup and resource management from RenderForwardClustered and overrides
// only the scene render entry points to add the raytraced opaque path, DLSS Ray
// Reconstruction, and the associated debug visualizations.
class RenderForwardClusteredPT : public RenderForwardClustered {
	/* Raytracing */

	RenderRaytracing *raytracing = nullptr;
	RTFogShadows *fog_shadows = nullptr;

	bool _setup_rt();
	void _age_out_motion_vectors(const RenderDataRD *p_render_data);
	void _update_pt_volumetric_fog(RenderDataRD *p_render_data, RTViewportState *p_rt_state, uint32_t p_directional_light_count, uint32_t p_positional_light_count);

protected:
	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;
	virtual void _free_rt_viewport_state(RenderSceneBuffersRD *p_render_buffers) override;

public:
	virtual bool pathtracing_is_used(RID p_env, RID p_viewport) const override;

	RenderForwardClusteredPT();
	~RenderForwardClusteredPT();
};

} // namespace RendererSceneRenderImplementation
