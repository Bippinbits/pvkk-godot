// Shared data structures for raytracing shaders.
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, raytracing_inc.glsl (for OFFSET_NONE/FLAG_*)

// ============================================================================
// BUFFER REFERENCES
// ============================================================================
layout(buffer_reference, std430) readonly buffer FloatBuffer {
	float v[];
};
layout(buffer_reference, std430) readonly buffer Uint32Buffer {
	uint v[];
};

// ============================================================================
// GEOMETRY DATA (matches C++ RT_GeometryData, 144 bytes, 8-byte alignment)
// ============================================================================
struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;

	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;

	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed; // fp16 x, fp16 y
	uint index_format;
	uint primitive_count;
	uint flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;

	// Byte offset of the vertex color attribute inside `attribute_stride`,
	// or OFFSET_NONE if the mesh has no vertex colors.
	uint color_byte_offset;

	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;

	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint prev_vertex_address_lo;
	uint prev_vertex_address_hi;

	uint layers; // VisualInstance3D render layers of the owning instance.
	uint instance_uniforms_ofs; // Per-instance shader uniforms offset in the global buffer.
	uint uv2_byte_offset;
	uint uv2_scale_packed; // fp16 x, fp16 y
	uint instance_data_primitive_divisor; // Source triangle count for merged instances; 0 for fixed address.
	uint64_t instance_data_address; // First float of the instance record; 0 without instance data.
	uint instance_data_layout; // Bits 0-7 stride, 8-15 color offset, 16-23 custom offset, in floats; 0xff = absent.
	uint debug_state; // Dynamic Geometry debug view only: RT_DYNAMIC_GEOMETRY_* category, rest category above RT_DYNAMIC_GEOMETRY_REST_SHIFT, age above RT_DYNAMIC_GEOMETRY_AGE_SHIFT.
};

// ============================================================================
// PER-INSTANCE MOTION DATA (matches C++ RT_InstanceMotionData, 48 bytes)
// ============================================================================
struct InstanceMotionData {
	float prev_xform[12]; // Previous object-to-world (mat3x4, transposed 3x4)
};

// ============================================================================
// MATERIAL DATA (matches C++ layout, 128 bytes)
// ============================================================================

// MaterialData::flags bits (must match RT_MAT_FLAG_* in render_raytracing.h).
#define RT_MAT_FLAG_HAS_NORMAL_MAP 1u
#define RT_MAT_FLAG_HAS_EMISSION_TEX 2u
#define RT_MAT_FLAG_POINT_FILTER 4u
#define RT_MAT_FLAG_TRIPLANAR 8u
#define RT_MAT_FLAG_TRIPLANAR_WORLD 16u

// Blend class of transparent surfaces (bits 5-7). Opaque surfaces leave 0.
#define RT_MAT_BLEND_CLASS_SHIFT 5u
#define RT_MAT_BLEND_CLASS_MASK (7u << 5u)
#define RT_BLEND_CLASS_MIX 0u
#define RT_BLEND_CLASS_ADD 1u
#define RT_BLEND_CLASS_SUB 2u
#define RT_BLEND_CLASS_OIT 3u
#define RT_BLEND_CLASS_MUL 4u
#define RT_BLEND_CLASS_PREMULT 5u
#define RT_MAT_FLAG_DEPTH_DRAW_ALWAYS 256u
#define RT_MAT_FLAG_UNSHADED 512u
#define RT_MAT_FLAG_TRANSPARENT 1024u
#define RT_MAT_FLAG_ALPHA_SCISSOR 2048u
#define RT_MAT_FLAG_PROXIMITY_FADE 4096u
#define RT_MAT_FLAG_DISTANCE_FADE 8192u
#define RT_MAT_FLAG_FOG_DISABLED 16384u
#define RT_MAT_FLAG_NO_RAY_QUERY_ALPHA 32768u
#define RT_MAT_FLAG_EMISSION_ADD 65536u
#define RT_MAT_FLAG_EMISSION_UV2 131072u
#define RT_MAT_FLAG_UV2_TRIPLANAR 262144u
#define RT_MAT_FLAG_UV2_TRIPLANAR_WORLD 524288u
// Godot diffuse mode (bits 20-21) and specular mode (bits 22-23), values GODOT_DIFFUSE_* / GODOT_SPECULAR_*.
#define RT_MAT_DIFFUSE_MODE_SHIFT 20u
#define RT_MAT_SPECULAR_MODE_SHIFT 22u
#define RT_MAT_SHADING_MODE_MASK 3u

// MaterialData::texture_channels (must match RTTextureChannel in render_raytracing.h).
#define RT_TEXTURE_CHANNEL_GRAYSCALE 4u
#define RT_TEXTURE_CHANNEL_ROUGHNESS_SHIFT 0u
#define RT_TEXTURE_CHANNEL_METALLIC_SHIFT 3u
#define RT_TEXTURE_CHANNEL_AO_SHIFT 6u
#define RT_TEXTURE_CHANNEL_MASK 7u

struct MaterialData {
	uint albedo_texture_idx;
	uint normal_texture_idx;
	uint roughness_texture_idx; // texture_roughness, or texture_orm for ORMMaterial3D.
	uint emission_texture_idx;

	vec4 albedo_color;
	vec3 emission_color;
	float emission_strength;

	vec3 uv1_scale; // UV1 scale (default 1,1,1)
	float normal_map_depth; // Normal map strength (default 1.0)
	vec3 uv1_offset; // UV1 offset (default 0,0,0)
	float specular; // Dielectric specular [0..1], default 0.5 -> F0 = 0.04.

	float metallic;
	float roughness;
	uint uv2_scale_xy; // fp16x2. UV2 transform: identity unless triplanar or applied by the vertex stage.
	uint flags; // See RT_MAT_FLAG_*.

	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none)
	uint blend_sharpness; // fp16x2: UV1, UV2 triplanar per-axis blend exponents (default 1.0)
	float proximity_fade_distance;
	float distance_fade_min;
	float distance_fade_max;
	uint uv2_offset_xy; // fp16x2, modulo 1.
	uint uv2_scale_offset_z; // fp16x2: scale.z, offset.z (modulo 1).
	uint metallic_texture_idx; // texture_metallic, or texture_orm for ORMMaterial3D.
	uint ao_texture_idx; // AO texture when AO is enabled; 0 (white) otherwise.
	uint texture_channels; // RT_TEXTURE_CHANNEL_* for roughness, metallic, AO (3 bits each).
	uint pad;
};
