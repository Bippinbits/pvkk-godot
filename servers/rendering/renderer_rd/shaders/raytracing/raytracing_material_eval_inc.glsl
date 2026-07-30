// Material evaluation result - common output format for all hit groups.
// Standard materials and custom ShaderMaterials both produce this struct.
//
// Requires (before this file): raytracing_data_inc.glsl, bindless_textures[],
// raytracing_samplers_inc.glsl.

struct MaterialResult {
	vec3 albedo;
	float alpha;
	float roughness;
	float metalness;
	float specular; // Dielectric specular reflectance control [0..1], default 0.5 -> F0 = 0.04.
	vec3 emissive;
	vec3 normal; // Final shading normal (world space, after normal mapping).
};

/// Sensible default for a mid-grey diffuse surface.
MaterialResult default_material_result(vec3 geometry_normal) {
	MaterialResult r;
	r.albedo = vec3(0.8);
	r.alpha = 1.0;
	r.roughness = 0.5;
	r.metalness = 0.0;
	r.specular = 0.5;
	r.emissive = vec3(0.0);
	r.normal = geometry_normal;
	return r;
}

// ============================================================================
// TEXTURE SAMPLING
// ============================================================================

vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv);
}

/// Sample with point/nearest filtering (for pixel art textures).
vec4 sample_bindless_texture_point(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv);
}

/// Sample with the appropriate filter based on material flags.
vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags) {
	if ((mat_flags & RT_MAT_FLAG_POINT_FILTER) != 0u) {
		return sample_bindless_texture_point(tex_idx, uv);
	}
	return sample_bindless_texture(tex_idx, uv);
}

// ============================================================================
// UV ADDRESSING (plain UV1 or triplanar)
// ============================================================================

/// Resolved texture addressing for one hit. Mirrors the UV setup that
/// BaseMaterial3D generates for the rasterizer, so both mapping modes can be
/// sampled through a single entry point.
struct MaterialUV {
	vec2 uv; // UV1 with scale/offset applied (plain mapping).
	vec3 triplanar_pos; // Scaled/offset position, object or world space.
	vec3 triplanar_weight; // Per-axis blend weights, normalized to sum 1.
	vec3 blend_normal; // Normal the weights came from, in the mapping space.
	bool triplanar;
	bool object_space; // Triplanar mapping runs in model space, not world space.
};

/// Build the addressing for a hit. `object_pos`/`object_normal` must be in
/// decompressed model space (what the rasterizer sees as VERTEX/NORMAL);
/// `world_pos`/`world_normal` are only used by world triplanar mode.
MaterialUV material_uv_compute(MaterialData mat, vec2 mesh_uv,
		vec3 object_pos, vec3 object_normal, vec3 world_pos, vec3 world_normal) {
	MaterialUV muv;
	muv.uv = mesh_uv * mat.uv1_scale.xy + mat.uv1_offset.xy;
	muv.triplanar_pos = vec3(0.0);
	muv.triplanar_weight = vec3(0.0);
	muv.blend_normal = world_normal;
	muv.triplanar = (mat.flags & RT_MAT_FLAG_TRIPLANAR) != 0u;
	muv.object_space = (mat.flags & RT_MAT_FLAG_TRIPLANAR_WORLD) == 0u;

	if (muv.triplanar) {
		vec3 pos = muv.object_space ? object_pos : world_pos;
		muv.blend_normal = muv.object_space ? object_normal : world_normal;

		vec3 weight = pow(abs(muv.blend_normal), vec3(mat.uv1_blend_sharpness));
		muv.triplanar_weight = weight / max(dot(weight, vec3(1.0)), 0.0001);

		muv.triplanar_pos = (pos * mat.uv1_scale + mat.uv1_offset) * vec3(1.0, -1.0, 1.0);
	}

	return muv;
}

/// Ray-tracing stages only carry world-space hit data, so recover the model
/// space that local triplanar mapping needs (undoing both the instance
/// transform and any AABB position compression baked into it).
MaterialUV material_uv_from_world(MaterialData mat, GeometryData geom, vec2 mesh_uv,
		vec3 world_pos, vec3 world_normal) {
	vec3 object_pos = vec3(0.0);
	vec3 object_normal = vec3(0.0);

	bool needs_model_space = (mat.flags & RT_MAT_FLAG_TRIPLANAR) != 0u &&
			(mat.flags & RT_MAT_FLAG_TRIPLANAR_WORLD) == 0u;
	if (needs_model_space) {
		mat4 aabb_xform;
		mat4 inv_aabb_xform;
		get_aabb_compression_xforms(geom, aabb_xform, inv_aabb_xform);
		mat4 world_to_model = aabb_xform * mat4(gl_WorldToObjectEXT);
		object_pos = (world_to_model * vec4(world_pos, 1.0)).xyz;
		object_normal = normalize(mat3(world_to_model) * world_normal);
	}

	return material_uv_compute(mat, mesh_uv, object_pos, object_normal, world_pos, world_normal);
}

/// Sample a material texture through the resolved addressing.
vec4 material_uv_sample(uint tex_idx, MaterialUV muv, uint mat_flags) {
	if (!muv.triplanar) {
		return sample_material_texture(tex_idx, muv.uv, mat_flags);
	}

	vec3 p = muv.triplanar_pos;
	vec3 w = muv.triplanar_weight;
	vec4 samp = sample_material_texture(tex_idx, p.xy, mat_flags) * w.z;
	samp += sample_material_texture(tex_idx, p.xz, mat_flags) * w.y;
	samp += sample_material_texture(tex_idx, p.zy * vec2(-1.0, 1.0), mat_flags) * w.x;
	return samp;
}

/// World-space tangent frame implied by triplanar mapping. Normal maps on a
/// triplanar material are authored against these axes rather than the mesh
/// tangents, so they replace the fetched frame.
void material_uv_triplanar_tangents(MaterialUV muv, out vec3 tangent, out vec3 binormal) {
	vec3 n = abs(muv.blend_normal);

	tangent = vec3(0.0, 0.0, -1.0) * n.x;
	tangent += vec3(1.0, 0.0, 0.0) * n.y;
	tangent += vec3(1.0, 0.0, 0.0) * n.z;
	tangent = normalize(tangent);

	binormal = vec3(0.0, 1.0, 0.0) * n.x;
	binormal += vec3(0.0, 0.0, -1.0) * n.y;
	binormal += vec3(0.0, 1.0, 0.0) * n.z;
	binormal = normalize(binormal);

	if (muv.object_space) {
		// Match compute_hit_data: normalized columns keep uniformly scaled
		// instances from skewing the frame.
		mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
		mat3 object_to_world = mat3(
				normalize(model_rotation[0]),
				normalize(model_rotation[1]),
				normalize(model_rotation[2]));
		tangent = normalize(object_to_world * tangent);
		binormal = normalize(object_to_world * binormal);
	}
}
