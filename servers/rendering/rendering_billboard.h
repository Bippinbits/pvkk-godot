/**************************************************************************/
/*  rendering_billboard.h                                                 */
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

#include "core/math/transform_3d.h"

// CPU mirror of billboard_inc.glsl. Keep both in sync: the path tracer bakes
// billboards into TLAS transforms with this, while raster applies the GLSL copy.
class RenderingBillboard {
public:
	enum Mode : uint8_t {
		MODE_DISABLED,
		MODE_ENABLED,
		MODE_FIXED_Y,
		MODE_PARTICLES,
	};

	// Camera state a billboard depends on.
	struct Camera {
		// Camera-to-world transform (inverse view matrix).
		Transform3D transform;
		// True for orthogonal projections (PROJECTION_MATRIX[3][3] != 0).
		bool orthogonal = false;
		// PROJECTION_MATRIX[1][1]; only read for orthogonal fixed_size.
		real_t projection_y_scale = 1.0;
	};

	// Per-instance billboard settings, as set by the render modes.
	struct Settings {
		Mode mode = MODE_DISABLED;
		bool keep_scale = false;
		bool fixed_size = false;

		// True when the result depends on the camera.
		_FORCE_INLINE_ bool is_camera_dependent() const { return mode != MODE_DISABLED || fixed_size; }

		_FORCE_INLINE_ bool operator==(const Settings &p_other) const {
			return mode == p_other.mode && keep_scale == p_other.keep_scale && fixed_size == p_other.fixed_size;
		}
	};

	// Y-billboard basis: normalize(cross(Y, cam_z)), Y, normalize(cross(cam_x, Y)).
	// Each horizontal axis falls back to the other one's construction when its
	// cross product degenerates (camera looking straight up/down, or rolled 90 degrees).
	static _FORCE_INLINE_ Basis fixed_y_basis(const Basis &p_camera_basis) {
		const Vector3 up(0, 1, 0);
		Vector3 right = up.cross(p_camera_basis.get_column(2));
		Vector3 back = p_camera_basis.get_column(0).cross(up);
		if (right.length_squared() < CMP_EPSILON2) {
			right = up.cross(back);
		}
		if (back.length_squared() < CMP_EPSILON2) {
			back = right.cross(up);
		}
		return Basis(right.normalized(), up, back.normalized());
	}

	// World basis a camera-facing mode starts from, before the particle rotation,
	// keep_scale and fixed_size. Identity for MODE_DISABLED.
	static _FORCE_INLINE_ Basis facing_basis(Mode p_mode, const Basis &p_camera_basis) {
		switch (p_mode) {
			case MODE_ENABLED:
				return p_camera_basis;
			case MODE_FIXED_Y:
				return fixed_y_basis(p_camera_basis);
			case MODE_PARTICLES:
				return Basis(p_camera_basis.get_column(0).normalized(), p_camera_basis.get_column(1).normalized(), p_camera_basis.get_column(2).normalized());
			case MODE_DISABLED:
				break;
		}
		return Basis();
	}

	// Rotation of the particles mode around the facing Z axis by INSTANCE_CUSTOM.x.
	static _FORCE_INLINE_ Basis particle_rotation(real_t p_rotation) {
		const real_t c = Math::cos(p_rotation);
		const real_t s = Math::sin(p_rotation);
		return Basis(Vector3(c, -s, 0), Vector3(s, c, 0), Vector3(0, 0, 1));
	}

	// fixed_size scale as an affine function of the world origin: gradient.dot(origin) + offset.
	// Perspective cameras scale by view depth; orthogonal ones by a constant.
	struct FixedSizeScale {
		Vector3 gradient;
		real_t offset = 1.0;

		_FORCE_INLINE_ real_t at(const Vector3 &p_origin) const { return gradient.dot(p_origin) + offset; }
	};

	static _FORCE_INLINE_ FixedSizeScale fixed_size_scale(const Camera &p_camera) {
		FixedSizeScale result;
		if (p_camera.orthogonal) {
			result.offset = Math::abs(1.0f / (2.0f * p_camera.projection_y_scale)) * 2.0f;
		} else {
			const Transform3D view = p_camera.transform.affine_inverse();
			result.gradient = -view.basis.rows[2];
			result.offset = -view.origin.z;
		}
		return result;
	}

	// Returns the world transform the raster vertex stage would produce for this
	// instance (MODELVIEW_MATRIX premultiplied by the inverse view matrix).
	// p_model is MODEL_MATRIX; p_rotation is INSTANCE_CUSTOM.x (particles mode only).
	// Applying the result again as p_model yields the same transform, except for
	// keep_scale and fixed_size, which read the scale of p_model.
	static Transform3D compute_world_transform(const Settings &p_settings, const Transform3D &p_model, const Camera &p_camera, real_t p_rotation = 0.0) {
		Transform3D world = p_model;

		if (p_settings.mode != MODE_DISABLED) {
			world.basis = facing_basis(p_settings.mode, p_camera.transform.basis);
			if (p_settings.mode == MODE_PARTICLES) {
				world.basis = world.basis * particle_rotation(p_rotation);
			}
			if (p_settings.keep_scale) {
				world.basis.scale_local(p_model.basis.get_scale_abs());
			}
		}

		if (p_settings.fixed_size) {
			const real_t scale = fixed_size_scale(p_camera).at(world.origin);
			world.basis.scale(Vector3(scale, scale, scale));
		}

		return world;
	}

};
