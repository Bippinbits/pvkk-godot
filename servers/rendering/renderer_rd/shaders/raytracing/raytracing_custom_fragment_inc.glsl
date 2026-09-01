// Custom shader fragment setup for RT hit groups (closest-hit and any-hit).
// Include inside main() under #ifdef RT_CUSTOM_HIT_GROUP.
//
// Required variables before inclusion:
//   uint  rt_geometry_idx   -- geometry/material index
//   vec3  rt_hit_pos        -- world-space hit position
//   vec2  rt_uv             -- interpolated UV
//   vec3  rt_normal         -- world-space geometry normal (flipped for back-face)
//   vec3  rt_tangent        -- world-space tangent
//   vec3  rt_bitangent      -- world-space bitangent
//   bool  rt_front_face     -- true if front-face hit
//
// Required bindings/types:
//   materials[], CustomMaterialUniforms, scene_data_block

MaterialData rt_mat = materials[rt_geometry_idx];
material = CustomMaterialUniforms(rt_mat.uniform_address);

// Matrices.
mat4 rt_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
		scene_data_block.data.view_matrix[1],
		scene_data_block.data.view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));

GeometryData rt_geom = geometries[rt_geometry_idx];
mat4 rt_aabb_xform;
mat4 rt_inv_aabb_xform;
get_aabb_compression_xforms(rt_geom, rt_aabb_xform, rt_inv_aabb_xform);

read_model_matrix = mat4(gl_ObjectToWorldEXT) * rt_inv_aabb_xform;
// Matches the rasterizer's uniform-scale case; instance scale flags that would
// select the inverse-transpose form are not available to hit shaders.
model_normal_matrix = mat3(read_model_matrix);
read_view_matrix = rt_view_matrix;
inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
		scene_data_block.data.inv_view_matrix[1],
		scene_data_block.data.inv_view_matrix[2],
		vec4(0.0, 0.0, 0.0, 1.0)));
projection_matrix = scene_data_block.data.projection_matrix;
inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
read_viewport_size = scene_data_block.data.viewport_size;
global_time = scene_data_block.data.time;

mat4 rt_world_to_object_decomp = rt_aabb_xform * mat4(gl_WorldToObjectEXT);

vertex = (rt_world_to_object_decomp * vec4(rt_hit_pos, 1.0)).xyz;
normal = mat3(rt_world_to_object_decomp) * rt_normal;
tangent = mat3(rt_world_to_object_decomp) * rt_tangent;
binormal = mat3(rt_world_to_object_decomp) * rt_bitangent;
uv_interp = rt_uv;
uv2_interp = rt_uv;
color_interp = rt_color;
// VIEW is view-space in the rasterizer; rotate the incoming ray to match.
view = -normalize(mat3(rt_view_matrix) * gl_WorldRayDirectionEXT);
rt_front_facing = rt_front_face;
rt_screen_uv = vec2(gl_LaunchIDEXT.xy) / vec2(gl_LaunchSizeEXT.xy);
rt_frag_coord = vec4(gl_LaunchIDEXT.xy, 0.0, 1.0);

rt_modelview_matrix = rt_view_matrix * read_model_matrix;
rt_modelview_normal_matrix = mat3(rt_modelview_matrix);

// Run vertex shader (computes varyings, may modify built-ins).
/* RT_CUSTOM_VERTEX_CALL */

// Post-vertex transform: object-space -> view-space (mirrors rasterizer post-vertex,
// honoring MODELVIEW_MATRIX overrides from the vertex code).
vertex = (rt_modelview_matrix * vec4(vertex, 1.0)).xyz;
normal = normalize(rt_modelview_normal_matrix * normal);
tangent = normalize(rt_modelview_normal_matrix * tangent);
binormal = normalize(rt_modelview_normal_matrix * binormal);

// Fragment outputs with sensible defaults.
vec3 albedo = vec3(1.0);
float alpha = 1.0;
float metallic = 0.0;
float roughness = 0.5;
float specular = 0.5;
vec3 emission = vec3(0.0);
vec3 normal_map = vec3(0.5, 0.5, 1.0);
float normal_map_depth = 1.0;
float ao = 1.0;
float ao_light_affect = 0.0;
vec3 backlight = vec3(0.0);
float sss_strength = 0.0;
float rim = 0.0;
float rim_tint = 0.0;
float clearcoat = 0.0;
float clearcoat_roughness = 0.0;
float anisotropy = 0.0;
vec2 anisotropy_flow = vec2(1.0, 0.0);
float alpha_scissor_threshold = 0.0;
float alpha_hash_scale = 1.0;
float alpha_antialiasing_edge = 0.0;
vec2 alpha_texture_coordinate = vec2(0.0);
vec3 light_vertex = vertex;
vec2 rt_point_coord = vec2(0.0);
float rt_depth = 0.0;
float premul_alpha = 1.0;
vec4 custom_radiance = vec4(0.0);
vec4 custom_irradiance = vec4(0.0);
vec4 transmittance_color = vec4(0.0);
float transmittance_depth = 0.0;
float transmittance_boost = 0.0;
// Raster-only builtins. FOG writes are discarded. VOLUMETRIC_FOG reads see the
// analytic fog at this hit in the raster froxel convention (rgb = premultiplied
// inscatter, a = transmittance); neutral no-fog is a = 1.
vec4 fog = vec4(0.0);
vec4 volumetric_fog_rt = vec4(0.0, 0.0, 0.0, 1.0);
#ifdef RT_STAGE_CLOSEST_HIT
if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u) {
	vec4 rt_analytic_fog = fog_process(scene_data_block.data, vertex);
	volumetric_fog_rt = vec4(rt_analytic_fog.rgb * rt_analytic_fog.a, 1.0 - rt_analytic_fog.a);
}
#endif

{
	/* RT_CUSTOM_FRAGMENT_CODE */
}
