// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_lights_inc.glsl,
//   raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], motion_indices[], materials[], motion_transforms[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image,
//   DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped for back-face hits.
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec2 uv; // Raw UV (no material scale/offset applied).
	vec2 uv2; // Raw UV2 (no shader scale/offset applied).
	vec4 color; // Vertex color (white if not present).
	bool is_front_face;
	uint geometry_idx;
};

/// Fetch vertex attributes and transform to world space.
/// Requires hitAttributeEXT HitAttribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[h.geometry_idx];

	// Custom hit groups may reference vertex color in their fragment code, so
	// pull all attributes; default HGs can skip color for perf.
#ifdef RT_CUSTOM_HIT_GROUP
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_ALL);
#else
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_UV | FETCH_TBN);
#endif
	h.uv = attrs.uv;
	h.uv2 = attrs.uv2;
	h.color = attrs.color;

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		h.uv = hit_attribs.bary_or_uv;
		h.uv2 = h.uv;
		vec3 obj_normal = normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz);
		vec3 obj_tangent = normalize(unpackSnorm4x8(hit_attribs.packed_tangent).xyz);
		h.geometry_normal = normalize(normal_matrix * obj_normal);
		h.tangent = normalize(normal_matrix * obj_tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent);

		h.is_front_face = (dot(h.geometry_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	} else
#endif
	{
		// Triangle hit: reuse `attrs` from the top-level fetch (already has
		// UV / TBN from FETCH_UV | FETCH_TBN or FETCH_ALL).
		h.geometry_normal = normalize(normal_matrix * attrs.normal);
		h.tangent = normalize(normal_matrix * attrs.tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent) * attrs.bitangent_sign;

		h.is_front_face = (dot(h.geometry_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	}

	h.hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

	return h;
}

// ============================================================================
// HELPERS
// ============================================================================

/// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

void rt_store_ndc_depth(vec3 hit_pos) {
	mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 view_pos = (view_mat * vec4(hit_pos, 1.0)).xyz;
	vec4 clip_pos = scene_data_block.data.projection_matrix * vec4(view_pos, 1.0);
	float ndc_depth = clip_pos.z / clip_pos.w;
	imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(ndc_depth));
}

/// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
/// Peel rays defer to the dominant-layer logic in shade_and_bounce.
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags) &&
			(payload.packed_bounces_flags & PEEL_RAY_FLAG) == 0u) {
		rt_store_ndc_depth(hit_pos);
	}
}

// ============================================================================
// VELOCITY WRITE (primary ray only, MV-gated)
// ============================================================================

#ifdef ENABLE_INTERSECTION_SHADERS
/// Decode the FP16-compressed PREV_POSITION delta from HitAttribs.
vec3 decode_prev_pos_delta() {
	uint dx_low = (hit_attribs.packed_normal >> 24u) & 0xFFu;
	uint dx_high = (hit_attribs.packed_tangent >> 24u) & 0xFFu;
	float delta_x = unpackHalf2x16(dx_low | (dx_high << 8u)).x;
	vec2 delta_yz = unpackHalf2x16(hit_attribs.prev_pos_delta_yz);
	return vec3(delta_x, delta_yz.x, delta_yz.y);
}
#endif

/// Reconstruct previous-frame mat4 from a compact motion transform entry.
mat4 decode_prev_object_to_world(int motion_idx) {
	InstanceMotionData m = motion_transforms[motion_idx];
	return transpose(mat4(
			vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
			vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
			vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

/// Uses unjittered VP matrices matching the raster motion_vectors_store convention.
void rt_store_velocity(vec3 hit_pos) {
	uint geom_idx = RT_HIT_GEOMETRY_IDX;
	int mi = motion_indices[geom_idx];

	// Resolve previous-frame model matrix: compact entry if moved, current transform otherwise.
	mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : mat4(gl_ObjectToWorldEXT);

	vec3 obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(hit_pos, 1.0)).xyz;
	vec3 prev_obj_pos = obj_pos;

	GeometryData geom = geometries[geom_idx];

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		prev_obj_pos += decode_prev_pos_delta();
	}
#endif

#ifdef RT_HIT_ATTRIBS_DECLARED
	if ((geom.flags & FLAG_DEFORMED) != 0u) {
		uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
		if (prev_addr != 0ul) {
			uint i0, i1, i2;
			get_triangle_indices(geom, i0, i1, i2);
			vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
			FloatBuffer prev_vb = FloatBuffer(prev_addr);
			uint stride_floats = geom.position_stride >> 2;
			vec3 p0 = vec3(prev_vb.v[i0 * stride_floats + 0u],
					prev_vb.v[i0 * stride_floats + 1u],
					prev_vb.v[i0 * stride_floats + 2u]);
			vec3 p1 = vec3(prev_vb.v[i1 * stride_floats + 0u],
					prev_vb.v[i1 * stride_floats + 1u],
					prev_vb.v[i1 * stride_floats + 2u]);
			vec3 p2 = vec3(prev_vb.v[i2 * stride_floats + 0u],
					prev_vb.v[i2 * stride_floats + 1u],
					prev_vb.v[i2 * stride_floats + 2u]);
			prev_obj_pos = bary.x * p0 + bary.y * p1 + bary.z * p2;
		}
	}
#endif

	vec3 prev_world_pos = (prev_model * vec4(prev_obj_pos, 1.0)).xyz;

	vec2 curr_uv = project_uv(hit_pos, curr_vp_unjittered);
	vec2 prev_uv = project_uv(prev_world_pos, prev_vp_unjittered);

	imageStore(rt_velocity_image, ivec2(gl_LaunchIDEXT.xy), vec4(prev_uv - curr_uv, 0.0, 0.0));
}

/// Write motion vectors for primary ray hits (bounce 0, sample 0 only).
/// Peel rays defer to the dominant-layer logic in shade_and_bounce.
void write_primary_hit_velocity(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags) ||
			(payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		return;
	}
	rt_store_velocity(hit_pos);
}

// ============================================================================
// ENVIRONMENT FOG (per ray segment)
// ============================================================================

vec3 fog_get_directional_color(uint index) {
	return rt_lights[index].emission;
}

vec3 fog_get_directional_direction(uint index) {
	vec3 world_dir = -normalize(rt_lights[index].position);
	mat3 view_rot = transpose(mat3(
			scene_data_block.data.view_matrix[0].xyz,
			scene_data_block.data.view_matrix[1].xyz,
			scene_data_block.data.view_matrix[2].xyz));
	return view_rot * world_dir;
}

#define FOG_HAS_RADIANCE

vec3 fog_sample_radiance(vec3 vertex, float mip_level) {
	vec3 cube_view = scene_data_block.data.radiance_inverse_xform * vertex;
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 cube_uv = vec3_to_oct_with_border(cube_view, border);
	// mip_level is a normalized roughness (0..1); radiance_octmap_sample maps it
	// onto the prefiltered roughness array layers.
	return radiance_octmap_sample(cube_uv, mip_level);
}

#include "../fog_inc.glsl"
#include "raytracing_volumetric_fog_inc.glsl"

/// Analytic and, on camera segments, froxel fog at a hit, as premultiplied
/// inscatter and transmittance.
vec4 rt_fog_at(vec3 world_pos, vec3 view_vertex, bool camera_segment) {
	bool use_fog = (RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u;
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
	if (use_fog) {
		fog = fog_process(scene_data_block.data, view_vertex);
		fog = vec4(fog.rgb * fog.a, 1.0 - fog.a);
	}
	if (camera_segment && rt_volumetric_fog_enabled()) {
		fog = rt_combine_fog(fog, use_fog, rt_volumetric_fog_at(world_pos, view_vertex));
	}
	return fog;
}

vec3 rt_world_to_view(vec3 p_world) {
	mat4 view_mat = transpose(mat4(
			scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	return (view_mat * vec4(p_world, 1.0)).xyz;
}

/// Apply environment fog for the ray segment that was just traversed.
/// Attenuates throughput and adds in-scattered fog color.
void apply_segment_fog(float segment_dist, uint mat_flags, bool camera_segment, inout vec3 radiance, inout vec3 throughput) {
	if ((mat_flags & RT_MAT_FLAG_FOG_DISABLED) != 0u) {
		return;
	}
	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u && !(camera_segment && rt_volumetric_fog_enabled())) {
		return;
	}

	vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * segment_dist;
	vec4 fog = rt_fog_at(world_hit, rt_world_to_view(world_hit), camera_segment);
	radiance += throughput * fog.rgb;
	throughput *= fog.a;
}

/// Converts specular parameter [0..1] to dielectric F0.
float specular_to_f0(float specular) {
	return 0.16 * specular * specular;
}

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
#define RT_VIS_DIRECTIONAL_SHADOW_BLOCKER 27

bool debug_is_camera_visible(uint p_geometry_idx, vec3 p_origin, vec3 p_direction, float p_t) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, RT_VIS_MASK, p_origin, max(p_t - 0.01, 0.0), p_direction, p_t + 0.01);
	while (rayQueryProceedEXT(rq)) {
		if (RT_RQ_GEOMETRY_IDX(rq, false) == p_geometry_idx) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// White: sun visible. Yellow: only transparent casters (scaled by transmittance).
// Red: opaque blocker the camera cannot see (raster ignores it). Blue: opaque blocker the camera sees.
// Darker means a farther blocker. Grey: facing away from the sun. Magenta: no directional light.
vec3 debug_directional_shadow_blocker(vec3 p_geometry_normal) {
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	int sun = -1;
	for (uint i = 0u; i < light_count; i++) {
		if (rt_lights[i].type == RT_LIGHT_TYPE_DIRECTIONAL) {
			sun = int(i);
			break;
		}
	}
	if (sun < 0) {
		return vec3(1.0, 0.0, 1.0);
	}
	vec3 to_sun = -normalize(rt_lights[sun].position);
	if (dot(p_geometry_normal, to_sun) <= 0.0) {
		return vec3(0.1);
	}

	vec3 origin = offset_ray_origin(gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT, p_geometry_normal);
	vec3 transmittance = vec3(1.0);
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, 0xFCu, origin, 0.001, to_sun, 10000.0);
	while (rayQueryProceedEXT(rq)) {
		uint geometry_idx = RT_RQ_GEOMETRY_IDX(rq, false);
		uint mat_flags = materials[geometry_idx].flags;
		if ((mat_flags & (RT_MAT_FLAG_TRANSPARENT | RT_MAT_FLAG_ALPHA_SCISSOR)) == 0u) {
			rayQueryConfirmIntersectionEXT(rq);
			continue;
		}
		bool transparent = (mat_flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
		if (transparent && (mat_flags & RT_MAT_FLAG_NO_RAY_QUERY_ALPHA) != 0u) {
			continue;
		}
		vec4 surface = ray_query_surface(geometry_idx, rayQueryGetIntersectionPrimitiveIndexEXT(rq, false), rayQueryGetIntersectionBarycentricsEXT(rq, false), 0.0);
		if (transparent) {
			// Expected value of ray_query_shadow_transmittance's stochastic coverage.
			transmittance *= (1.0 - surface.a) * mix(vec3(1.0), surface.rgb, surface.a);
		} else if (surface.a >= 0.5) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		return transmittance == vec3(1.0) ? vec3(1.0) : vec3(1.0, 1.0, 0.0) * luminance(transmittance);
	}
	float t = rayQueryGetIntersectionTEXT(rq, true);
	uint blocker = RT_RQ_GEOMETRY_IDX(rq, true);
	float brightness = clamp(1.0 - log2(1.0 + t) / 8.0, 0.25, 1.0);
	vec3 color = debug_is_camera_visible(blocker, origin, to_sun, t) ? vec3(0.0, 0.3, 1.0) : vec3(1.0, 0.0, 0.0);
	return color * brightness;
}

void debug_visualize(
		int vis_mode,
		bool is_front_face,
		vec3 geometry_normal,
		vec3 final_normal,
		vec3 tangent_space_normal,
		vec3 world_tangent,
		vec3 world_bitangent,
		vec2 uv,
		vec3 albedo,
		vec3 orm,
		float metalness,
		float roughness,
		float specular,
		vec3 emissive,
		vec3 V,
		float NdotV) {
	PathState ps = path_unpack(payload);

	if (vis_mode == 1) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ps.hit_t = gl_HitTEXT;
			ps.offset_normal = geometry_normal;
			ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, geometry_normal);
			path_pack(payload, ps);
			return;
		} else {
			ps.radiance = geometry_normal * 0.5 + 0.5;
		}
	} else if (vis_mode == 2) {
		ps.radiance = geometry_normal * 0.5 + 0.5;
	} else if (vis_mode == 3) {
		ps.radiance = final_normal * 0.5 + 0.5;
	} else if (vis_mode == 4) {
		ps.radiance = tangent_space_normal * 0.5 + 0.5;
	} else if (vis_mode == 5) {
		ps.radiance = world_tangent * 0.5 + 0.5;
	} else if (vis_mode == 6) {
		ps.radiance = world_bitangent * 0.5 + 0.5;
	} else if (vis_mode == 7) {
		ps.radiance = vec3(fract(uv), 0.0);
	} else if (vis_mode == 8) {
		ps.radiance = albedo;
	} else if (vis_mode == 9) {
		ps.radiance = orm;
	} else if (vis_mode == 10) {
		ps.radiance = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
	} else if (vis_mode == 11) {
		ps.radiance = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
	} else if (vis_mode == 12) {
		ps.radiance = (final_normal * 0.5 + 0.5) * (1.0 - roughness * 0.5);
	} else if (vis_mode == 13) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			if (roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
				ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
				ps.radiance = vec3(0.1, 0.1, 0.4);
				ps.hit_t = gl_HitTEXT;
				ps.offset_normal = final_normal;
				ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, final_normal);
				path_pack(payload, ps);
				return;
			} else {
				ps.radiance = vec3(0.1, 0.1, 0.4);
			}
		} else {
			float spec_hit_t = gl_HitTEXT;
			float v = clamp(log(spec_hit_t + 1.0) / log(1000.0), 0.0, 1.0);
			vec3 color;
			if (v < 0.33) {
				color = mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), v * 3.0);
			} else if (v < 0.66) {
				color = mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), (v - 0.33) * 3.0);
			} else {
				color = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), (v - 0.66) * 3.0);
			}
			ps.radiance = color;
		}
	} else if (vis_mode == 14) {
		ps.radiance = vec3(metalness);
	} else if (vis_mode == 15) {
		ps.radiance = vec3(roughness);
	} else if (vis_mode == 16) {
		mat3 world_to_view = mat3(scene_data_block.data.inv_view_matrix);
		ps.radiance = normalize(world_to_view * final_normal) * 0.5 + 0.5;
	} else if (vis_mode == 17) {
		vec3 diffuse_albedo = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
		ps.radiance = mix(diffuse_albedo, specular_albedo, metalness);
	} else if (vis_mode == 18) {
		ps.radiance = baseColorToSpecularF0(albedo, metalness, specular_to_f0(specular));
	} else if (vis_mode == 19) {
		ps.radiance = is_front_face ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	} else if (vis_mode == 20) {
		float depth_range = scene_data_block.data.z_far - scene_data_block.data.z_near;
		float d = clamp(gl_HitTEXT / depth_range, 0.0, 1.0);
		ps.radiance = vec3(d);
	} else if (vis_mode == 21) {
		ps.radiance = emissive;
	} else if (vis_mode == RT_VIS_DIRECTIONAL_SHADOW_BLOCKER) {
		ps.radiance = debug_directional_shadow_blocker(geometry_normal);
	} else if (vis_mode == RT_VIS_VOLUMETRIC_FOG || vis_mode == RT_VIS_VOLUMETRIC_FOG_TRANSMITTANCE) {
		vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
		ps.radiance = rt_volumetric_fog_debug(vis_mode, rt_volumetric_fog_at(world_hit, rt_world_to_view(world_hit)));
	} else if (vis_mode == 22) {
		// BRDF below-hemisphere fallback visualization. Reflects the two-layer
		// recovery applied in shade_and_bounce:
		//   Layer 1 - shading-normal clamp toward geometry (energy preserving
		//             but flattens detail at grazing angles)
		//   Layer 2 - mirror rejected directions across the geometry plane
		//             (biased; reuses the rejected sample's BRDF weight)
		//
		//
		// Color legend (ordered cleanest -> most biased):
		//   green   = no rejection in either pass (no fallback needed)
		//   blue    = clamp alone resolves rejection (energy preserving)
		//   yellow  = clamp + mirror (one lobe still mirrored after clamp)
		//   red     = clamp + mirror (both lobes mirrored - most biased pixel)
		//
		// Production output is never black anymore -- yellow/red just indicate
		// where the cheaper Layer-1 clamp could not catch the bump and the
		// biased Layer-2 mirror had to step in. Raise
		// RT_SHADING_NORMAL_CLAMP_THRESHOLD if yellow/red dominate.
		MaterialProperties dbg_mat;
		dbg_mat.baseColor = albedo;
		dbg_mat.metalness = metalness;
		dbg_mat.roughness = roughness;
		dbg_mat.dielectricF0 = specular_to_f0(specular);
		dbg_mat.emissive = vec3(0.0);
		dbg_mat.transmissivness = 0.0;
		dbg_mat.opacity = 1.0;

		vec3 dbg_dir;
		vec3 dbg_weight;
		// Draw both random pairs up front so each (specular/diffuse) test in
		// the un-clamped and clamped pass uses the exact same numbers.
		vec2 u_spec_rng = rand2(ps.rng_state);
		vec2 u_diff_rng = rand2(ps.rng_state);

		bool u_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, final_normal, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool u_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, final_normal, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		// Uses the same threshold as shade_and_bounce so the visualization
		// stays in sync with production sampling.
		vec3 N_clamped = clampShadingNormal(final_normal, geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
		bool c_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, N_clamped, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool c_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, N_clamped, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		bool all_uncl_ok = u_spec_ok && u_diff_ok;
		bool all_cl_ok = c_spec_ok && c_diff_ok;
		bool any_cl_ok = c_spec_ok || c_diff_ok;

		if (all_uncl_ok) {
			ps.radiance = vec3(0.0, 1.0, 0.0);
		} else if (all_cl_ok) {
			ps.radiance = vec3(0.0, 0.4, 1.0);
		} else if (any_cl_ok) {
			ps.radiance = vec3(1.0, 1.0, 0.0);
		} else {
			ps.radiance = vec3(1.0, 0.0, 0.0);
		}
	}

	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	path_pack(payload, ps);
}
#endif // RT_DEBUG_ENABLED

// ============================================================================
// SHADE AND BOUNCE
// ============================================================================

bool surface_is_unshaded(uint geometry_idx) {
#ifdef MODE_UNSHADED
	return true;
#else
	return (materials[geometry_idx].flags & RT_MAT_FLAG_UNSHADED) != 0u;
#endif
}

MaterialProperties material_properties_from_result(MaterialResult m) {
	MaterialProperties material;
	material.baseColor = m.albedo;
	material.metalness = m.metalness;
	material.roughness = m.roughness;
	material.dielectricF0 = specular_to_f0(m.specular);
	material.emissive = m.emissive;
	material.transmissivness = 0.0;
	material.opacity = 1.0;
	return material;
}

/// Ambient and reflection lighting for max_bounces == 0, which never traces the
/// sky. Mirrors the rasterizer (scene_forward_clustered.glsl): ambient source,
/// sky contribution, reflection source and specular occlusion. The DFG lookup
/// texture is not bound here, so envBRDFApprox stands in for it.
vec3 ambient_approx(vec3 N, vec3 V, MaterialProperties material) {
	uint flags = scene_data_block.data.flags;
	vec4 ambient_color_energy = scene_data_block.data.ambient_light_color_energy;
	float sky_scale = scene_data_block.data.IBL_exposure_normalization * ambient_color_energy.a;
	float roughness = material.roughness;

	vec3 ambient = vec3(0.0);
	if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT) != 0u) {
		ambient = ambient_color_energy.rgb;
		if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP) != 0u) {
			vec3 sky_ambient = radiance_octmap_sample(rt_sky_oct_uv(N), 1.0) * sky_scale;
			ambient = mix(ambient, sky_ambient, scene_data_block.data.ambient_color_sky_mix);
		}
	}

	vec3 specular = vec3(0.0);
	if ((flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP) != 0u) {
		vec3 ref_vec = mix(reflect(-V, N), N, roughness * roughness);
		float horizon = min(1.0 + dot(ref_vec, N), 1.0);
		specular = radiance_octmap_sample(rt_sky_oct_uv(ref_vec), sqrt(roughness)) * sky_scale * horizon * horizon;

		float specular_occlusion = min(dot(ambient, vec3(0.3, 0.59, 0.11)) * 8.0, 1.0);
		float reflective_f = (1.0 - roughness) * material.metalness;
		specular_occlusion = max(min(reflective_f * specular_occlusion * 10.0, 1.0), specular_occlusion);

		vec3 specular_f0 = baseColorToSpecularF0(material.baseColor, material.metalness, material.dielectricF0);
		specular *= specular_occlusion * envBRDFApprox(specular_f0, roughness, max(dot(N, V), 1e-4));
	}

	return baseColorToDiffuseReflectance(material.baseColor, material.metalness) * ambient + specular;
}

vec3 shade_local_surface(HitData h, MaterialResult m, vec3 N, vec3 V, bool p_is_indirect, inout uint rng_state) {
	if (surface_is_unshaded(h.geometry_idx)) {
		return m.albedo + m.emissive;
	}
	vec3 lit = m.emissive;
	MaterialProperties material = material_properties_from_result(m);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	vec3 hit_pos_offset = offset_ray_origin(h.hit_pos, h.geometry_normal);
	uint layers = geometries[h.geometry_idx].layers;
	if (RT_GET_MAX_BOUNCES() == 0u) {
		// Direct only: one ray per light group, plus the rasterizer's ambient.
		lit += lights_evaluate_direct_lighting_per_group(hit_pos_offset, N, V, material, m.backlight,
				rng_state, p_is_indirect, light_count, layers);
		lit += ambient_approx(N, V, material);
	} else {
		lit += lights_evaluate_direct_lighting(hit_pos_offset, N, V, material, m.backlight,
				rng_state, p_is_indirect, light_count, layers);
	}
	return lit;
}

bool sample_surface_continuation(HitData h, MaterialResult m, vec3 N, vec3 V, inout PathState ps) {
	if (surface_is_unshaded(h.geometry_idx)) {
		return false;
	}
	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		return false;
	}

	MaterialProperties material = material_properties_from_result(m);
	vec3 specular_f0 = baseColorToSpecularF0(material.baseColor, material.metalness, material.dielectricF0);
	vec3 diffuse_reflectance = baseColorToDiffuseReflectance(material.baseColor, material.metalness);
	float specular_lum = luminance(specular_f0);
	float diffuse_lum = luminance(diffuse_reflectance);

	int brdf_type;
	if (diffuse_lum < 0.0001) {
		brdf_type = SPECULAR_TYPE;
	} else if (specular_lum < 0.0001) {
		brdf_type = DIFFUSE_TYPE;
	} else {
		float brdf_probability = clamp(specular_lum / (specular_lum + diffuse_lum), 0.01, 0.99);
		if (rand(ps.rng_state) < brdf_probability) {
			brdf_type = SPECULAR_TYPE;
			ps.throughput /= brdf_probability;
		} else {
			brdf_type = DIFFUSE_TYPE;
			ps.throughput /= (1.0 - brdf_probability);
		}
	}

	vec3 next_dir;
	vec3 brdf_weight;
	if (!evalIndirectCombinedBRDF(rand2(ps.rng_state), N, h.geometry_normal, V, material, brdf_type, next_dir, brdf_weight, vec4(0.0))) {
		vec3 recovered_dir;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
			return false;
		}
		next_dir = recovered_dir;
	}

	ps.throughput *= brdf_weight;
	ps.packed_bounces_flags = (brdf_type == DIFFUSE_TYPE)
			? inc_diffuse_bounce(ps.packed_bounces_flags)
			: inc_total_bounce(ps.packed_bounces_flags);
	if (brdf_type == DIFFUSE_TYPE || material.roughness > SINGULAR_ROUGHNESS_MAX) {
		ps.packed_bounces_flags |= NON_SINGULAR_PATH_FLAG;
	}
	ps.offset_normal = h.geometry_normal;
	ps.next_ray_dir = next_dir;
	return true;
}

/// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
/// Also handles DLSS-RR G-buffer output on primary ray.
void shade_and_bounce(HitData h, MaterialResult m) {
	PathState ps = path_unpack(payload);
	// Every surface bounds this ray segment, even if it cannot continue the path.
	ps.hit_t = gl_HitTEXT;

	vec3 V = -gl_WorldRayDirectionEXT;

	// Clamp shading normal toward geometry at grazing view angles to keep BRDF above the geometry hemisphere.
	vec3 N = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	float NdotV = max(dot(N, V), 0.0001);

	if ((ps.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		// Peel layer: shade like an opaque hit, apply its blend operation, no bounce.
		// Raygen pre-scales throughput by the running transmittance and owns
		// path continuation; this only reports (t, alpha) and its own light.
		uint mat_flags = materials[h.geometry_idx].flags;
		uint bclass = (mat_flags & RT_MAT_BLEND_CLASS_MASK) >> RT_MAT_BLEND_CLASS_SHIFT;
		bool depth_barrier = (mat_flags & RT_MAT_FLAG_DEPTH_DRAW_ALWAYS) != 0u;
		float material_alpha = clamp(m.alpha, 0.0, 1.0);
		float peel_alpha;
		float continuation_alpha = 0.0;
		vec3 fogged_throughput = ps.throughput;
		vec3 branch_fog = vec3(0.0);
		apply_segment_fog(gl_HitTEXT, mat_flags, get_total_bounces(ps.packed_bounces_flags) == 0u, branch_fog, fogged_throughput);
		if (bclass == RT_BLEND_CLASS_MUL) {
			// Mul contributes no light; its color folds into the scalar T.
			peel_alpha = clamp(1.0 - luminance(m.albedo), 0.0, 1.0);
		} else {
			// Add/Sub scale their source by alpha but do not attenuate the
			// destination. Premultiplied alpha already includes alpha in RGB.
			peel_alpha = (bclass == RT_BLEND_CLASS_ADD || bclass == RT_BLEND_CLASS_SUB) ? 0.0 : material_alpha;
			ps.throughput = fogged_throughput;
			path_pack(payload, ps);
			vec3 lit = shade_local_surface(h, m, N, V,
					get_diffuse_bounces(ps.packed_bounces_flags) > 0u, ps.rng_state);
			float source_alpha = (bclass == RT_BLEND_CLASS_PREMULT) ? 1.0 : material_alpha;
			vec3 contribution = ps.throughput * source_alpha * lit;
			if (bclass == RT_BLEND_CLASS_SUB) {
				ps.radiance -= contribution;
			} else {
				ps.radiance += contribution;
			}
			// Add/Sub don't attenuate the destination (peel_alpha above), but they
			// still need a chance at their own indirect bounce so they can pick up
			// GI/sky ambient like every other blend class does.
			continuation_alpha = material_alpha;
			if (bclass != RT_BLEND_CLASS_ADD && bclass != RT_BLEND_CLASS_SUB) {
				ps.radiance += branch_fog * continuation_alpha;
			}

			// Dominant surface: the first peel layer on the primary segment
			// with alpha >= 0.5 owns depth/velocity and blends into RR guides.
			// depth_draw_always owns depth regardless of its alpha.
			if (get_total_bounces(ps.packed_bounces_flags) == 0u && is_sample_zero(ps.packed_bounces_flags) &&
					(ps.packed_bounces_flags & PEEL_FIRST_LAYER_FLAG) != 0u && (depth_barrier || peel_alpha >= 0.5)) {
				rt_store_ndc_depth(h.hit_pos);
				rt_store_velocity(h.hit_pos);
#ifdef DLSS_RR_ENABLED
				ivec2 peel_pixel = ivec2(gl_LaunchIDEXT.xy);
				float guide_alpha = depth_barrier ? material_alpha : peel_alpha;
				vec3 layer_diffuse = DLSSRR_encodeDiffuseAlbedo(DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness));
				vec4 prev_diffuse = imageLoad(dlss_rr_diffuse_albedo, peel_pixel);
				imageStore(dlss_rr_diffuse_albedo, peel_pixel, vec4(mix(prev_diffuse.rgb, layer_diffuse, guide_alpha), 1.0));
				vec4 prev_nr = imageLoad(dlss_rr_normal_roughness, peel_pixel);
				vec3 blended_n = normalize(mix(prev_nr.xyz, N, guide_alpha));
				imageStore(dlss_rr_normal_roughness, peel_pixel, vec4(blended_n, mix(prev_nr.w, m.roughness, guide_alpha)));
				vec3 layer_specular = DLSSRR_computeSpecularAlbedo(
						m.albedo, m.metalness, specular_to_f0(m.specular), m.roughness, NdotV);
				vec4 prev_specular = imageLoad(dlss_rr_specular_albedo, peel_pixel);
				imageStore(dlss_rr_specular_albedo, peel_pixel,
						vec4(mix(prev_specular.rgb, clamp(layer_specular, vec3(0.04), vec3(1.0)), guide_alpha), 1.0));

				if (m.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
					vec3 spec_dir = reflect(-V, N);
					vec3 spec_origin = offset_ray_origin(h.hit_pos, spec_dir);
					rayQueryEXT spec_rq;
					rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
							RT_VIS_MASK | RT_TMASK_TRANSPARENT, spec_origin, 0.001, spec_dir, 10000.0);
					while (rayQueryProceedEXT(spec_rq)) {
						if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
								ray_query_alpha_test(
										RT_RQ_GEOMETRY_IDX(spec_rq, false),
										rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
										rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
							rayQueryConfirmIntersectionEXT(spec_rq);
						}
					}
					if (rayQueryGetIntersectionTypeEXT(spec_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
						float layer_spec_hit_dist = rayQueryGetIntersectionTEXT(spec_rq, true);
						vec4 prev_spec_hit_dist = imageLoad(dlss_rr_specular_hit_dist, peel_pixel);
						imageStore(dlss_rr_specular_hit_dist, peel_pixel,
								vec4(mix(prev_spec_hit_dist.x, layer_spec_hit_dist, guide_alpha)));
					}
				}
#endif
			}
		}
		ps.packed_bounces_flags = set_peel_alpha(ps.packed_bounces_flags, peel_alpha);
		if (depth_barrier) {
			ps.packed_bounces_flags |= PEEL_DEPTH_BARRIER_HIT_FLAG;
		}
		ps.packed_bounces_flags &= ~PEEL_HAS_CONTINUATION_FLAG;
		// Continue with probability alpha rather than scaling throughput by it, so faint
		// layers rarely spawn a full indirect path.
		if (rand(ps.rng_state) < continuation_alpha && sample_surface_continuation(h, m, N, V, ps)) {
			ps.packed_bounces_flags |= PEEL_HAS_CONTINUATION_FLAG;
		}
		path_pack(payload, ps);
		return;
	}

	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);

	// Environment fog for this ray segment (before surface contribution).
	apply_segment_fog(gl_HitTEXT, materials[h.geometry_idx].flags, total_bounces == 0u, ps.radiance, ps.throughput);

	path_pack(payload, ps);
	ps.radiance += ps.throughput * shade_local_surface(
			h, m, N, V, diffuse_bounces > 0u, ps.rng_state);

	// BRDF material setup.
	MaterialProperties brdf_mat = material_properties_from_result(m);

	// =================================================================
	// DLSS Ray Reconstruction output (primary ray, sample 0 only)
	// =================================================================
#ifdef DLSS_RR_ENABLED
	if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

		vec3 diffuse_albedo = DLSSRR_encodeDiffuseAlbedo(DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness));
		imageStore(dlss_rr_diffuse_albedo, pixel, vec4(diffuse_albedo, 1.0));

		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(m.albedo, m.metalness, brdf_mat.dielectricF0, m.roughness, NdotV);
		imageStore(dlss_rr_specular_albedo, pixel, vec4(clamp(specular_albedo, vec3(0.04), vec3(1.0)), 1.0)); // match UNORM8 like before - fixes some issues with garbling..

		imageStore(dlss_rr_normal_roughness, pixel, vec4(N, m.roughness));

		// Specular hit distance via inline ray query (only for smooth surfaces).
		float spec_hit_dist = -1.0;
		if (m.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
			vec3 spec_dir = reflect(-V, N);
			vec3 spec_origin = offset_ray_origin(h.hit_pos, spec_dir);

			rayQueryEXT spec_rq;
			rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
					RT_VIS_MASK, spec_origin, 0.001, spec_dir, 10000.0);
			while (rayQueryProceedEXT(spec_rq)) {
				if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
					if (ray_query_alpha_test(
								RT_RQ_GEOMETRY_IDX(spec_rq, false),
								rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
								rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
						rayQueryConfirmIntersectionEXT(spec_rq);
					}
				}
			}
			if (rayQueryGetIntersectionTypeEXT(spec_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
				spec_hit_dist = rayQueryGetIntersectionTEXT(spec_rq, true);
			}
		}
		imageStore(dlss_rr_specular_hit_dist, pixel, vec4(spec_hit_dist));
	}
#endif

	if (!sample_surface_continuation(h, m, N, V, ps)) {
		ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	}
	path_pack(payload, ps);
}
