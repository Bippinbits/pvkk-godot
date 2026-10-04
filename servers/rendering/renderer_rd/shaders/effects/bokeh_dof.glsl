#[compute]

#version 450

#VERSION_DEFINES

#define BLOCK_SIZE 8

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

#ifdef MODE_GEN_BLUR_SIZE
layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_image;
layout(set = 1, binding = 0) uniform sampler2D source_depth;
#endif

#if defined(MODE_BOKEH_BOX) || defined(MODE_BOKEH_HEXAGONAL) || defined(MODE_BOKEH_CIRCULAR) || defined(MODE_BOKEH_TILE_MAX)
layout(set = 1, binding = 0) uniform sampler2D color_texture;
layout(rgba16f, set = 0, binding = 0) uniform restrict writeonly image2D bokeh_image;
#endif

#ifdef MODE_BOKEH_CIRCULAR
layout(set = 2, binding = 0) uniform sampler2D tile_texture;
#endif

#if defined(MODE_BOKEH_CIRCULAR) || defined(MODE_BOKEH_TILE_MAX)
#define TILE_SIZE 16
shared uint tile_max;
#endif

#ifdef MODE_COMPOSITE_BOKEH
layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_image;
layout(set = 1, binding = 0) uniform sampler2D source_bokeh;
#endif

// based on https://www.shadertoy.com/view/Xd3GDl

#include "bokeh_dof_inc.glsl"

#ifdef MODE_GEN_BLUR_SIZE

float get_depth_at_pos(vec2 uv) {
	float depth = textureLod(source_depth, uv, 0.0).x * 2.0 - 1.0;
	if (params.orthogonal) {
		depth = -(depth * (params.z_far - params.z_near) - (params.z_far + params.z_near)) / 2.0;
	} else {
		depth = 2.0 * params.z_near * params.z_far / (params.z_far + params.z_near + depth * (params.z_far - params.z_near));
	}
	return depth;
}

float get_blur_size(float depth) {
	if (params.blur_near_active && depth < params.blur_near_begin) {
		if (params.use_physical_near) {
			// Physically-based.
			float d = abs(params.blur_near_begin - depth);
			return -(d / (params.blur_near_begin - d)) * params.blur_size_near - DEPTH_GAP; // Near blur is negative.
		} else {
			// Non-physically-based.
			return -(1.0 - smoothstep(params.blur_near_end, params.blur_near_begin, depth)) * params.blur_size - DEPTH_GAP; // Near blur is negative.
		}
	}

	if (params.blur_far_active && depth > params.blur_far_begin) {
		if (params.use_physical_far) {
			// Physically-based.
			float d = abs(params.blur_far_begin - depth);
			return (d / (params.blur_far_begin + d)) * params.blur_size_far + DEPTH_GAP;
		} else {
			// Non-physically-based.
			return smoothstep(params.blur_far_begin, params.blur_far_end, depth) * params.blur_size + DEPTH_GAP;
		}
	}

	return 0.0;
}

#endif

#if defined(MODE_BOKEH_BOX) || defined(MODE_BOKEH_HEXAGONAL)

vec4 weighted_filter_dir(vec2 dir, vec2 uv, vec2 pixel_size) {
	dir *= pixel_size;
	vec4 color = texture(color_texture, uv);

	vec4 accum = color;
	float total = 1.0;

	float blur_scale = params.blur_size / float(params.blur_steps);

	if (params.use_jitter) {
		uv += dir * (hash12n(uv + params.jitter_seed) - 0.5);
	}

	for (int i = -params.blur_steps; i <= params.blur_steps; i++) {
		if (i == 0) {
			continue;
		}
		float radius = float(i) * blur_scale;
		vec2 suv = uv + dir * radius;
		radius = abs(radius);

		vec4 sample_color = texture(color_texture, suv);
		float limit;

		if (sample_color.a < color.a) {
			limit = abs(sample_color.a);
		} else {
			limit = abs(color.a);
		}

		limit -= DEPTH_GAP;

		float m = smoothstep(radius - 0.5, radius + 0.5, limit);

		accum += mix(color, sample_color, m);

		total += 1.0;
	}

	return accum / total;
}

#endif

#ifdef MODE_BOKEH_CIRCULAR

float get_tile_blur_size() {
	ivec2 full_size = textureSize(color_texture, 0);
	vec2 scale = vec2(full_size) / vec2(params.size * 2);
	float max_scale = max(scale.x, scale.y);
	ivec2 tile_count = (full_size + TILE_SIZE - 1) / TILE_SIZE;
	ivec2 group = ivec2(gl_WorkGroupID.xy);
	vec2 group_begin = (vec2(group * TILE_SIZE) + 0.5) * scale;
	vec2 group_end = (vec2(group * TILE_SIZE) + float(TILE_SIZE) - 1.5) * scale;
	int reach = int(ceil((params.blur_size * max_scale + 1.5) / float(TILE_SIZE))) + 1;
	int width = reach * 2 + 1;

	if (gl_LocalInvocationIndex == 0) {
		tile_max = 0u;
	}
	barrier();

	for (int i = int(gl_LocalInvocationIndex); i < width * width; i += BLOCK_SIZE * BLOCK_SIZE) {
		ivec2 tile = group + ivec2(i % width, i / width) - reach;
		if (any(lessThan(tile, ivec2(0))) || any(greaterThanEqual(tile, tile_count))) {
			continue;
		}
		float size = texelFetch(tile_texture, tile, 0).r + 0.5;
		vec2 tile_begin = vec2(tile * TILE_SIZE) + 0.5;
		vec2 tile_end = tile_begin + float(TILE_SIZE - 1);
		vec2 gap = max(vec2(0.0), max(tile_begin - group_end, group_begin - tile_end));
		if (length(gap) < size * max_scale + 1.5) {
			atomicMax(tile_max, floatBitsToUint(size));
		}
	}

	barrier();
	return uintBitsToFloat(tile_max);
}

#endif

void main() {
#ifdef MODE_BOKEH_TILE_MAX
	if (gl_LocalInvocationIndex == 0) {
		tile_max = 0u;
	}
	barrier();

	ivec2 tile_pos = ivec2(gl_WorkGroupID.xy) * TILE_SIZE + ivec2(gl_LocalInvocationID.xy) * 2;
	ivec2 last_pixel = textureSize(color_texture, 0) - 1;
	float max_size = 0.0;
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			max_size = max(max_size, abs(texelFetch(color_texture, min(tile_pos + ivec2(x, y), last_pixel), 0).a));
		}
	}
	atomicMax(tile_max, floatBitsToUint(max_size));
	barrier();

	if (gl_LocalInvocationIndex == 0) {
		imageStore(bokeh_image, ivec2(gl_WorkGroupID.xy), vec4(uintBitsToFloat(tile_max)));
	}
	return;
#endif

#ifdef MODE_BOKEH_CIRCULAR
	float tile_blur_size = min(params.blur_size, get_tile_blur_size());
#endif

	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThan(pos, params.size))) { //too large, do nothing
		return;
	}

	vec2 pixel_size = 1.0 / vec2(params.size);
	vec2 uv = vec2(pos) / vec2(params.size);

#ifdef MODE_GEN_BLUR_SIZE
	uv += pixel_size * 0.5;
	//precompute size in alpha channel
	float depth = get_depth_at_pos(uv);
	float size = get_blur_size(depth);

	vec4 color = imageLoad(color_image, pos);
	color.a = size;
	imageStore(color_image, pos, color);
#endif

#ifdef MODE_BOKEH_BOX

	//pixel_size*=0.5; //resolution is doubled
	if (params.second_pass || !params.half_size) {
		uv += pixel_size * 0.5; //half pixel to read centers
	} else {
		uv += pixel_size * 0.25; //half pixel to read centers from full res
	}

	vec2 dir = (params.second_pass ? vec2(0.0, 1.0) : vec2(1.0, 0.0));

	vec4 color = weighted_filter_dir(dir, uv, pixel_size);

	imageStore(bokeh_image, pos, color);

#endif

#ifdef MODE_BOKEH_HEXAGONAL

	//pixel_size*=0.5; //resolution is doubled
	if (params.second_pass || !params.half_size) {
		uv += pixel_size * 0.5; //half pixel to read centers
	} else {
		uv += pixel_size * 0.25; //half pixel to read centers from full res
	}

	vec2 dir = (params.second_pass ? normalize(vec2(1.0, 0.577350269189626)) : vec2(0.0, 1.0));

	vec4 color = weighted_filter_dir(dir, uv, pixel_size);

	if (params.second_pass) {
		dir = normalize(vec2(-1.0, 0.577350269189626));

		vec4 color2 = weighted_filter_dir(dir, uv, pixel_size);

		color.rgb = min(color.rgb, color2.rgb);
		color.a = (color.a + color2.a) * 0.5;
	}

	imageStore(bokeh_image, pos, color);

#endif

#ifdef MODE_BOKEH_CIRCULAR

	if (params.half_size) {
		pixel_size *= 0.5; //resolution is doubled
	}

	uv += pixel_size * 0.5; //half pixel to read centers

	vec4 color = texture(color_texture, uv);
	float initial_blur = color.a;
	float accum = 1.0;
	float radius = params.blur_scale;

	for (float ang = 0.0; radius < tile_blur_size; ang += GOLDEN_ANGLE) {
		vec2 suv = uv + vec2(cos(ang), sin(ang)) * pixel_size * radius;
		vec4 sample_color = texture(color_texture, suv);
		float sample_size = abs(sample_color.a);
		if (sample_color.a > initial_blur) {
			sample_size = clamp(sample_size, 0.0, abs(initial_blur) * 2.0);
		}

		float m = smoothstep(radius - 0.5, radius + 0.5, sample_size);
		color += mix(color / accum, sample_color, m);
		accum += 1.0;
		radius += params.blur_scale / radius;
	}

	color /= accum;

	imageStore(bokeh_image, pos, color);
#endif

#ifdef MODE_COMPOSITE_BOKEH

	uv += pixel_size * 0.5;
	vec4 color = imageLoad(color_image, pos);
	vec4 bokeh = texture(source_bokeh, uv);

	float mix_amount;
	if (bokeh.a < color.a) {
		mix_amount = min(1.0, max(0.0, max(abs(color.a), abs(bokeh.a)) - DEPTH_GAP));
	} else {
		mix_amount = min(1.0, max(0.0, abs(color.a) - DEPTH_GAP));
	}

	color.rgb = mix(color.rgb, bokeh.rgb, mix_amount); //blend between hires and lowres

	color.a = 0; //reset alpha
	imageStore(color_image, pos, color);
#endif
}
