/**************************************************************************/
/*  test_rendering_billboard.h                                            */
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

#include "servers/rendering/rendering_billboard.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

#include "tests/test_macros.h"

namespace TestRenderingBillboard {

static RenderingBillboard::Camera make_camera(const Vector3 &p_eye, const Vector3 &p_target, const Vector3 &p_up = Vector3(0, 1, 0)) {
	RenderingBillboard::Camera camera;
	camera.transform = Transform3D(Basis(), p_eye).looking_at(p_target, p_up);
	return camera;
}

static Transform3D make_model() {
	Transform3D model;
	model.basis = Basis(Vector3(0.3, 1.1, -0.2).normalized(), 0.7).scaled_local(Vector3(2, 3, 4));
	model.origin = Vector3(1, 2, -5);
	return model;
}

static bool is_finite(const Transform3D &p_xform) {
	return p_xform.basis.get_column(0).is_finite() && p_xform.basis.get_column(1).is_finite() && p_xform.basis.get_column(2).is_finite() && p_xform.origin.is_finite();
}

TEST_CASE("[RenderingBillboard] Disabled leaves the model untouched") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(4, 3, 6), Vector3());
	const Transform3D model = make_model();
	CHECK(RenderingBillboard::compute_world_transform(RenderingBillboard::Settings(), model, camera).is_equal_approx(model));
	RenderingBillboard::Settings keep;
	keep.keep_scale = true;
	CHECK_MESSAGE(RenderingBillboard::compute_world_transform(keep, model, camera).is_equal_approx(model), "keep_scale alone has no effect.");
	CHECK_FALSE(RenderingBillboard::Settings().is_camera_dependent());
}

TEST_CASE("[RenderingBillboard] Enabled copies the camera basis and keeps the origin") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(4, 3, 6), Vector3());
	const Transform3D model = make_model();
	RenderingBillboard::Settings settings;
	settings.mode = RenderingBillboard::MODE_ENABLED;
	CHECK(settings.is_camera_dependent());

	const Transform3D world = RenderingBillboard::compute_world_transform(settings, model, camera);
	CHECK(world.basis.is_equal_approx(camera.transform.basis));
	CHECK(world.origin.is_equal_approx(model.origin));
	CHECK_MESSAGE(RenderingBillboard::compute_world_transform(settings, world, camera).is_equal_approx(world), "Billboarding twice is idempotent.");
}

TEST_CASE("[RenderingBillboard] keep_scale restores the model scale per axis") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(4, 3, 6), Vector3());
	const Transform3D model = make_model();
	const RenderingBillboard::Mode modes[] = { RenderingBillboard::MODE_ENABLED, RenderingBillboard::MODE_FIXED_Y, RenderingBillboard::MODE_PARTICLES };
	for (RenderingBillboard::Mode mode : modes) {
		RenderingBillboard::Settings settings;
		settings.mode = mode;
		settings.keep_scale = true;
		const Transform3D world = RenderingBillboard::compute_world_transform(settings, model, camera, 0.4);
		CHECK(world.basis.get_scale_abs().is_equal_approx(Vector3(2, 3, 4)));
		CHECK(world.origin.is_equal_approx(model.origin));
	}
}

TEST_CASE("[RenderingBillboard] Fixed Y keeps world up and faces the camera horizontally") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(4, 3, 6), Vector3());
	RenderingBillboard::Settings settings;
	settings.mode = RenderingBillboard::MODE_FIXED_Y;

	const Transform3D world = RenderingBillboard::compute_world_transform(settings, make_model(), camera);
	CHECK(world.basis.get_column(1).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(world.basis.is_orthonormal());
	const Vector3 cam_back = camera.transform.basis.get_column(2);
	CHECK(Math::is_zero_approx(world.basis.get_column(0).dot(Vector3(cam_back.x, 0, cam_back.z))));
	CHECK(world.basis.get_column(2).dot(cam_back) > 0);
	CHECK_MESSAGE(RenderingBillboard::compute_world_transform(settings, world, camera).is_equal_approx(world), "Billboarding twice is idempotent.");
}

TEST_CASE("[RenderingBillboard] Fixed Y stays finite when looking straight down") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(0, 10, 0), Vector3(), Vector3(0, 0, -1));
	RenderingBillboard::Settings settings;
	settings.mode = RenderingBillboard::MODE_FIXED_Y;

	const Transform3D world = RenderingBillboard::compute_world_transform(settings, make_model(), camera);
	CHECK(is_finite(world));
	CHECK(world.basis.is_orthonormal());
	CHECK(world.basis.get_column(0).is_equal_approx(camera.transform.basis.get_column(0)));
}

TEST_CASE("[RenderingBillboard] Particles rotate around the view axis by INSTANCE_CUSTOM.x") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(4, 3, 6), Vector3());
	const Basis &cam = camera.transform.basis;
	RenderingBillboard::Settings settings;
	settings.mode = RenderingBillboard::MODE_PARTICLES;

	CHECK(RenderingBillboard::compute_world_transform(settings, make_model(), camera, 0.0).basis.is_equal_approx(cam));

	const Transform3D quarter = RenderingBillboard::compute_world_transform(settings, make_model(), camera, Math::PI * 0.5);
	CHECK(quarter.basis.get_column(0).is_equal_approx(-cam.get_column(1)));
	CHECK(quarter.basis.get_column(1).is_equal_approx(cam.get_column(0)));
	CHECK(quarter.basis.get_column(2).is_equal_approx(cam.get_column(2)));
}

TEST_CASE("[RenderingBillboard] fixed_size scales by view depth or orthogonal height") {
	const RenderingBillboard::Camera camera = make_camera(Vector3(0, 0, 10), Vector3());
	const Transform3D model(Basis(), Vector3(0, 0, -2));
	RenderingBillboard::Settings settings;
	settings.mode = RenderingBillboard::MODE_ENABLED;
	settings.fixed_size = true;
	CHECK(settings.is_camera_dependent());

	const Transform3D world = RenderingBillboard::compute_world_transform(settings, model, camera);
	CHECK(world.basis.get_scale_abs().is_equal_approx(Vector3(12, 12, 12)));
	CHECK(world.origin.is_equal_approx(model.origin));

	RenderingBillboard::Camera ortho = camera;
	ortho.orthogonal = true;
	ortho.projection_y_scale = 0.25;
	CHECK(RenderingBillboard::compute_world_transform(settings, model, ortho).basis.get_scale_abs().is_equal_approx(Vector3(4, 4, 4)));

	RenderingBillboard::Settings size_only;
	size_only.fixed_size = true;
	CHECK(size_only.is_camera_dependent());
	const Transform3D sized = RenderingBillboard::compute_world_transform(size_only, make_model(), camera);
	const real_t depth = 10 - make_model().origin.z;
	CHECK(sized.basis.get_scale_abs().is_equal_approx(Vector3(2, 3, 4) * depth));
}

static Error compile_spatial_render_modes(const String &p_render_modes) {
	const ShaderTypes *types = ShaderTypes::get_singleton();
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = types->get_functions(RS::SHADER_SPATIAL);
	info.render_modes = types->get_modes(RS::SHADER_SPATIAL);
	info.stencil_modes = types->get_stencil_modes(RS::SHADER_SPATIAL);
	info.shader_types = types->get_types();
	ShaderLanguage language;
	return language.compile("shader_type spatial;\nrender_mode " + p_render_modes + ";\nvoid vertex() {}\n", info);
}

// ShaderLanguage reads RSG::utilities, which only a live RenderingServer keeps valid.
TEST_CASE("[SceneTree][RenderingBillboard] Billboard render modes parse and conflict") {
	REQUIRE(ShaderTypes::get_singleton() != nullptr);
	CHECK(compile_spatial_render_modes("billboard") == OK);
	CHECK(compile_spatial_render_modes("billboard_y, billboard_keep_scale") == OK);
	CHECK(compile_spatial_render_modes("billboard_particles, billboard_keep_scale, fixed_size") == OK);
	CHECK(compile_spatial_render_modes("fixed_size") == OK);

	ERR_PRINT_OFF;
	CHECK(compile_spatial_render_modes("billboard, billboard_y") != OK);
	CHECK(compile_spatial_render_modes("billboard_particles, billboard") != OK);
	CHECK(compile_spatial_render_modes("billboard_y, billboard_particles") != OK);
	ERR_PRINT_ON;
}

} // namespace TestRenderingBillboard
