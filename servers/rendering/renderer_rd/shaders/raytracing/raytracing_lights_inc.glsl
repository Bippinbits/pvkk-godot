// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0.
// Note: ray_query_alpha_test() still requires GL_EXT_ray_query (used by DLSS-RR path or when USE_RAY_QUERY_SHADOWS is defined).

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_SPOT 3 // Spot light with cone falloff

// Reservoir sampling batch size for stochastic light selection.
#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

// ============================================================================
// Light Data (matches C++ RT_LightData, 80 bytes, std430)
// ============================================================================

struct RTLightData {
	vec3 position; // World pos (omni/spot) or direction (directional).
	uint type; // RT_LIGHT_TYPE_*.
	vec3 emission; // HDR emission color (color * energy).
	float radius; // Light size (omni/spot) or angular radius (directional).
	float attenuation; // Attenuation exponent (2=inverse-square, 1=linear).
	float inv_max_range; // 1/range (-1 = infinite).
	float max_range_squared; // range^2 (0 = infinite).
	float specular_amount; // Godot specular multiplier [0..1].
	float indirect_energy; // Godot indirect energy multiplier.
	float inv_spot_attenuation; // Spot cone softness.
	float cos_spot_angle; // Cosine of spot cone half-angle.
	uint light_masks; // Bits 0-19: light_cull_mask (lighting test). Bits 24-31: TLAS shadow-ray mask.
	vec3 spot_direction; // Spot direction (normalized, world space).
	uint pad0;
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightData rt_lights[];
};

// ============================================================================
// Unified Cone Sampling (for sphere, directional, spot lights)
// ============================================================================

struct LightSample {
	vec3 cone_axis; // Direction to sample around (normalized).
	float cos_theta_max; // Cone half-angle cosine (1.0 = point, 0.0 = hemisphere).
	vec3 emission; // Light radiance.
	float distance_sq; // Squared distance to light (0 = directional).
	float max_distance; // Max shadow ray distance.
};

// Build orthonormal basis from a single direction.
void lights_build_basis(vec3 dir, out vec3 tangent, out vec3 bitangent) {
	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

// Transform local direction to world space around axis.
vec3 lights_local_to_world(vec3 local_dir, vec3 axis) {
	vec3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

// Prepare unified cone sample from any light type.
LightSample lights_prepare_sample(vec3 hit_pos, RTLightData light) {
	LightSample s;

	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = inversesqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;

	// Cone axis.
	vec3 sphere_axis = to_light * inv_dist;
	vec3 dir_axis = -normalize(light.position);
	s.cone_axis = mix(sphere_axis, dir_axis, is_directional);

	// Distance (0 for directional = no falloff).
	s.distance_sq = mix(dist_sq, 0.0, is_directional);

	// Cone angle from subtended solid angle.
	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = mix(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	// Max shadow ray distance.
	float sphere_max = dist + light.radius;
	float dir_max = 10000.0;
	s.max_distance = mix(sphere_max, dir_max, is_directional);

	return s;
}

// Sample a direction within the light's cone.
vec3 lights_sample_cone(LightSample ls, vec2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

// ============================================================================
// Attenuation
// ============================================================================

// Godot-style windowed distance attenuation.
// window = (1 - (d/range)^4)^2, combined with pow(d, -decay).
float lights_get_attenuation(LightSample ls, float inv_max_range, float decay) {
	if (ls.distance_sq <= 0.0) {
		return 1.0; // Directional: no distance attenuation.
	}

	float distance = sqrt(ls.distance_sq);
	float atten = min(pow(max(distance, 0.0001), -decay), 1.0);

	// Windowed falloff if range is finite (inv_max_range >= 0).
	if (inv_max_range >= 0.0) {
		float nd = distance * inv_max_range;
		nd *= nd;
		nd *= nd; // nd^4
		nd = max(1.0 - nd, 0.0);
		nd *= nd; // nd^2 window
		return atten * nd;
	}

	return atten;
}

// Per-light specular multiplier.
float lights_get_specular_multiplier(float specular_amount, float roughness) {
	if (specular_amount >= 0.0) {
		return specular_amount;
	} else {
		float r3 = roughness * roughness * roughness;
		return mix(0.0, r3, -specular_amount);
	}
}

// ============================================================================
// Inline Alpha Test (shared by all ray query proceed loops)
// ============================================================================

vec4 ray_query_surface(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);

	MaterialData mat = materials[geometry_idx];
	uv = uv * mat.uv1_scale.xy + mat.uv1_offset.xy;
	vec4 albedo = texture(sampler2D(bindless_textures[nonuniformEXT(mat.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
	albedo *= mat.albedo_color;
	if ((mat.flags & RT_MAT_BLEND_CLASS_MASK) == RT_BLEND_CLASS_PREMULT && albedo.a > 1e-6) {
		albedo.rgb /= albedo.a;
	}

	return clamp(albedo, vec4(0.0), vec4(1.0));
}

float ray_query_alpha(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	return ray_query_surface(geometry_idx, primitive_id, candidate_bary).a;
}

/// Inline alpha test for non-transparent ray query candidates.
bool ray_query_alpha_test(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	return ray_query_alpha(geometry_idx, primitive_id, candidate_bary) >= 0.5;
}

// ============================================================================
// Shadow Ray (traceRayEXT pipeline)
// ============================================================================

/// Returns colored light transmittance.
/// Uses SkipClosestHitShader so only any_hit (alpha test) and miss are invoked.
/// TerminateOnFirstHit exits on the first opaque or stochastic alpha blocker.
/// p_shadow_mask is the light's 8-bit shadow-caster group mask.
vec3 lights_trace_shadow_ray(vec3 origin, vec3 direction, float max_dist, uint p_shadow_mask, inout uint rng_state) {
#ifdef USE_RAY_QUERY_SHADOWS
	// Ray queries are faster but approximate custom or animated material outputs
	// with a direct albedo texture sample.
	vec3 transmittance = vec3(1.0);
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, origin, 0.001, direction, max_dist - 0.001);

	while (rayQueryProceedEXT(shadow_rq)) {
		if (rayQueryGetIntersectionTypeEXT(shadow_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			uint geometry_idx = rayQueryGetIntersectionInstanceCustomIndexEXT(shadow_rq, false);
			uint primitive_idx = rayQueryGetIntersectionPrimitiveIndexEXT(shadow_rq, false);
			vec2 bary = rayQueryGetIntersectionBarycentricsEXT(shadow_rq, false);
			vec4 surface = ray_query_surface(geometry_idx, primitive_idx, bary);
			float alpha = surface.a;
			bool transparent_shadow = (materials[geometry_idx].flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
			if (transparent_shadow && rand(rng_state) >= alpha) {
				transmittance *= mix(vec3(1.0), surface.rgb, alpha);
			} else if (transparent_shadow || alpha >= 0.5) {
				rayQueryConfirmIntersectionEXT(shadow_rq);
			}
		}
	}

	return rayQueryGetIntersectionTypeEXT(shadow_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT
			? transmittance
			: vec3(0.0);
#elif defined(USE_SER)
	PathPayload saved_payload = payload;
	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(1.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = rng_state;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

	hitObjectNV hitObject;
	hitObjectTraceRayNV(hitObject, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	shadow_ps = path_unpack(payload);
	rng_state = shadow_ps.rng_state;
	payload = saved_payload;
	return hitObjectIsHitNV(hitObject) ? vec3(0.0) : shadow_ps.throughput;
#else
	/// The miss shader copies accumulated transmittance to radiance.
	/// If an opaque hit occurs, miss is never called and radiance stays vec3(0.0).
	// Save full payload, set up shadow ray, then restore after trace.
	PathPayload saved_payload = payload;

	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(1.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = rng_state;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

	traceRayEXT(tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	// Unpack the colored visibility written by the miss shader.
	shadow_ps = path_unpack(payload);
	vec3 transmittance = shadow_ps.radiance;
	rng_state = shadow_ps.rng_state;

	payload = saved_payload;

	return transmittance;
#endif
}

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

// Evaluate direct lighting using NEE with stochastic light selection.
// Uses mini-batch reservoir sampling for importance-weighted light selection.
vec3 lights_evaluate_direct_lighting(
		vec3 hit_pos,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		vec3 backlight,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count,
		uint instance_layers) {
	if (light_count == 0u) {
		return vec3(0.0);
	}

	// Mini-batch reservoir sampling: sample k lights, pick the best valid one.
	const uint k = uint(min(RT_LIGHT_RESERVOIR_SIZE, int(light_count)));
	uint valid_found = 0u;
	uint selected_idx = 0u;

	for (uint i = 0u; i < k; i++) {
		uint idx = min(uint(rand(rng_state) * float(light_count)), light_count - 1u);
		RTLightData test_light = rt_lights[idx];

		// light_cull_mask: the light only affects intersecting layers.
		bool is_valid = (test_light.light_masks & instance_layers & 0xFFFFFu) != 0u;

		// Range check for positional lights.
		bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
		if (is_valid && is_positional) {
			vec3 to_l = test_light.position - hit_pos;
			float d2 = dot(to_l, to_l);
			is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
		}

		if (is_valid) {
			valid_found++;
			if (rand(rng_state) < 1.0 / float(valid_found)) {
				selected_idx = idx;
			}
		}
	}

	if (valid_found == 0u) {
		return vec3(0.0);
	}

	// Estimate valid count from sample ratio, PDF = 1/validCount.
	float valid_count_estimate = float(light_count) * (float(valid_found) / float(k));
	float light_select_pdf = 1.0 / max(valid_count_estimate, 1.0);

	RTLightData light = rt_lights[selected_idx];
	vec2 u = rand2(rng_state);

	// === POSITIONAL LIGHT PATH (omni + spot) ===
	if (light.type == RT_LIGHT_TYPE_OMNI || light.type == RT_LIGHT_TYPE_SPOT) {
		vec3 to_light = light.position - hit_pos;
		float dist_sq = dot(to_light, to_light);

		// Early out: outside max range.
		if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
			return vec3(0.0);
		}

		float dist = sqrt(dist_sq);
		vec3 L;
		float shadow_dist;

		if (light.radius <= 0.01) {
			// True point light: exact direction.
			L = to_light / max(dist, 0.0001);
			shadow_dist = dist;
		} else {
			// Sphere light: cone sampling for soft shadows.
			LightSample ls = lights_prepare_sample(hit_pos, light);
			float light_pdf;
			L = lights_sample_cone(ls, u, light_pdf);
			float t_center = dot(to_light, L);
			vec3 perp = to_light - t_center * L;
			float perp_sq = dot(perp, perp);
			float dt = sqrt(max(0.0, light.radius * light.radius - perp_sq));
			shadow_dist = max(0.0, t_center - dt);
		}

		// Spot cone early-out.
		float spot_atten = 1.0;
		if (light.type == RT_LIGHT_TYPE_SPOT) {
			float scos = dot(-L, light.spot_direction);
			if (scos <= light.cos_spot_angle) {
				return vec3(0.0);
			}
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
			spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		}

		vec3 shadow_transmittance = lights_trace_shadow_ray(
				hit_pos, L, shadow_dist, (light.light_masks >> 24u) & 0xFFu, rng_state);

		// Evaluate BRDF (diffuse + specular separately for specular_amount control).
		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		// Backlight/translucency: light "wraps around" from the far side of the
		// surface. Nonzero exactly where NdotL<=0 would otherwise zero the diffuse
		// term. Mirrors the rasterizer's (1/PI - diffuse_brdf_NL) * backlight formula.
		float NdotL = dot(N, L);
		vec3 backlight_term = backlight * max(0.0, 1.0 - clamp(NdotL, -1.0, 1.0)) * (1.0 / PI);

		// Distance attenuation.
		LightSample ls_atten;
		ls_atten.distance_sq = dist_sq;
		float atten = lights_get_attenuation(ls_atten, light.inv_max_range, light.attenuation) * spot_atten;

		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
		vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul + backlight_term;

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		// NdotL is already included in brdf_value (evalLambertian/evalMicrofacet bake it in).
		vec3 contribution = brdf_value * light.emission * shadow_transmittance * atten * indirect_mul;
		return contribution / max(light_select_pdf, 1e-10);
	}
	// === CONE LIGHT PATH (directional) ===
	else {
		LightSample ls = lights_prepare_sample(hit_pos, light);
		float light_pdf;
		vec3 L = lights_sample_cone(ls, u, light_pdf);

		float NdotL = dot(N, L);
		bool has_backlight = any(greaterThan(backlight, vec3(0.0)));
		if (NdotL <= 0.0 && !has_backlight) {
			return vec3(0.0);
		}

		vec3 shadow_transmittance = lights_trace_shadow_ray(
				hit_pos, L, ls.max_distance, (light.light_masks >> 24u) & 0xFFu, rng_state);

		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		vec3 backlight_term = has_backlight ? backlight * max(0.0, 1.0 - clamp(NdotL, -1.0, 1.0)) * (1.0 / PI) : vec3(0.0);

		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
		vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul + backlight_term;

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		return brdf_value * light.emission * shadow_transmittance * indirect_mul / max(light_select_pdf, 1e-10);
	}
}
