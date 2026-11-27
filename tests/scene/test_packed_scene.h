/**************************************************************************/
/*  test_packed_scene.h                                                   */
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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_uid.h"
#include "scene/resources/packed_scene.h"

#include "tests/test_macros.h"
#include "tests/test_tools.h"
#include "tests/test_utils.h"

namespace TestPackedScene {

TEST_CASE("[PackedScene] Pack Scene and Retrieve State") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	const Error err = packed_scene.pack(scene);
	CHECK(err == OK);

	// Retrieve the packed state.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state->get_node_count() == 1);
	CHECK(state->get_node_name(0) == "TestScene");

	memdelete(scene);
}

TEST_CASE("[PackedScene] Signals Preserved when Packing Scene") {
	// Create main scene
	// root
	// `- sub_node (local)
	// `- sub_scene (instance of another scene)
	//    `- sub_scene_node (owned by sub_scene)
	Node *main_scene_root = memnew(Node);
	Node *sub_node = memnew(Node);
	Node *sub_scene_root = memnew(Node);
	Node *sub_scene_node = memnew(Node);

	main_scene_root->add_child(sub_node);
	sub_node->set_owner(main_scene_root);

	sub_scene_root->add_child(sub_scene_node);
	sub_scene_node->set_owner(sub_scene_root);

	main_scene_root->add_child(sub_scene_root);
	sub_scene_root->set_owner(main_scene_root);

	SUBCASE("Signals that should be saved") {
		int main_flags = Object::CONNECT_PERSIST;
		// sub node to a node in main scene
		sub_node->connect("ready", callable_mp(main_scene_root, &Node::is_ready), main_flags);
		// subscene root to a node in main scene
		sub_scene_root->connect("ready", callable_mp(main_scene_root, &Node::is_ready), main_flags);
		//subscene root to subscene root (connected within main scene)
		sub_scene_root->connect("ready", callable_mp(sub_scene_root, &Node::is_ready), main_flags);

		// Pack the scene.
		Ref<PackedScene> packed_scene;
		packed_scene.instantiate();
		const Error err = packed_scene->pack(main_scene_root);
		CHECK(err == OK);

		// Make sure the right connections are in packed scene.
		Ref<SceneState> state = packed_scene->get_state();
		CHECK_EQ(state->get_connection_count(), 3);
	}

	/*
	// FIXME: This subcase requires GH-48064 to be fixed.
	SUBCASE("Signals that should not be saved") {
		int subscene_flags = Object::CONNECT_PERSIST | Object::CONNECT_INHERITED;
		// subscene node to itself
		sub_scene_node->connect("ready", callable_mp(sub_scene_node, &Node::is_ready), subscene_flags);
		// subscene node to subscene root
		sub_scene_node->connect("ready", callable_mp(sub_scene_root, &Node::is_ready), subscene_flags);
		//subscene root to subscene root (connected within sub scene)
		sub_scene_root->connect("ready", callable_mp(sub_scene_root, &Node::is_ready), subscene_flags);

		// Pack the scene.
		Ref<PackedScene> packed_scene;
		packed_scene.instantiate();
		const Error err = packed_scene->pack(main_scene_root);
		CHECK(err == OK);

		// Make sure the right connections are in packed scene.
		Ref<SceneState> state = packed_scene->get_state();
		CHECK_EQ(state->get_connection_count(), 0);
	}
	*/

	memdelete(main_scene_root);
}

TEST_CASE("[PackedScene] Clear Packed Scene") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Clear the packed scene.
	packed_scene.clear();

	// Check if it has been cleared.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK_FALSE(state->get_node_count() == 1);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Can Instantiate Packed Scene") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Check if the packed scene can be instantiated.
	const bool can_instantiate = packed_scene.can_instantiate();
	CHECK(can_instantiate == true);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Instantiate Packed Scene") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Instantiate the packed scene.
	Node *instance = packed_scene.instantiate();
	CHECK(instance != nullptr);
	CHECK(instance->get_name() == "TestScene");

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Instantiate Packed Scene With Children") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Add persisting child nodes to the scene.
	Node *child1 = memnew(Node);
	child1->set_name("Child1");
	scene->add_child(child1);
	child1->set_owner(scene);

	Node *child2 = memnew(Node);
	child2->set_name("Child2");
	scene->add_child(child2);
	child2->set_owner(scene);

	// Add non persisting child node to the scene.
	Node *child3 = memnew(Node);
	child3->set_name("Child3");
	scene->add_child(child3);

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Instantiate the packed scene.
	Node *instance = packed_scene.instantiate();
	CHECK(instance != nullptr);
	CHECK(instance->get_name() == "TestScene");

	// Validate the child nodes of the instantiated scene.
	CHECK(instance->get_child_count() == 2);
	CHECK(instance->get_child(0)->get_name() == "Child1");
	CHECK(instance->get_child(1)->get_name() == "Child2");
	CHECK(instance->get_child(0)->get_owner() == instance);
	CHECK(instance->get_child(1)->get_owner() == instance);

	memdelete(scene);
	memdelete(instance);
}

TEST_CASE("[PackedScene] Set Path") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Set a new path for the packed scene.
	const String new_path = "NewTestPath";
	packed_scene.set_path(new_path);

	// Check if the path has been set correctly.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state->get_path() == new_path);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Replace State") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Create another scene state to replace with.
	Ref<SceneState> new_state = memnew(SceneState);
	new_state->set_path("NewPath");

	// Replace the state.
	packed_scene.replace_state(new_state);

	// Check if the state has been replaced.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state == new_state);

	memdelete(scene);
}

TEST_CASE("[PackedScene] Recreate State") {
	// Create a scene to pack.
	Node *scene = memnew(Node);
	scene->set_name("TestScene");

	// Pack the scene.
	PackedScene packed_scene;
	packed_scene.pack(scene);

	// Recreate the state.
	packed_scene.recreate_state();

	// Check if the state has been recreated.
	Ref<SceneState> state = packed_scene.get_state();
	CHECK(state.is_valid());
	CHECK(state->get_node_count() == 0); // Since the state was recreated, it should be empty.

	memdelete(scene);
}

TEST_CASE("[PackedScene][SceneState] scan_text_scene reads the node structure without loading anything") {
	ResourceUID::ID known_uid = ResourceUID::get_singleton()->create_id();
	ResourceUID::get_singleton()->add_id(known_uid, "res://resolved_root.gd");
	const String unknown_uid = ResourceUID::get_singleton()->id_to_text(ResourceUID::get_singleton()->create_id());

	const String scene_path = TestUtils::get_temp_path("scan_text_scene.tscn");
	{
		Ref<FileAccess> f = FileAccess::open(scene_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string(vformat(R"([gd_scene load_steps=4 format=3]

[ext_resource type="Script" uid="%s" path="res://stale_root.gd" id="1_script"]
[ext_resource type="Texture2D" path="res://a.png" id="2_tex"]
[ext_resource type="PackedScene" uid="%s" path="res://child.tscn" id="3_scene"]
[ext_resource type="Script" path="res://child.gd" id="4_script"]

[sub_resource type="GDScript" id="GDScript_1"]
script/source = "extends Node
"

[sub_resource type="Resource" id="Resource_1"]
script = ExtResource("1_script")
textures = Array[Texture2D]([ExtResource("2_tex")])

[node name="Root" type="Node3D" groups=["saveable", "level"] unique_id=1234]
script = ExtResource("1_script")
textures = Array[Texture2D]([ExtResource("2_tex"), ExtResource("2_tex")])
by_name = Dictionary[String, Texture2D]({
"a": ExtResource("2_tex")
})
helper = Object(Node3D,"name":"Helper","script":ExtResource("1_script"))
data = SubResource("Resource_1")
metadata/note = "[node name=\"Fake\"]"

[node name="Child" parent="." unique_id=5 instance=ExtResource("3_scene")]
script = ExtResource("4_script")

[node name="Inner" parent="Child/Body" type="Node"]
script = null

[node name="BuiltIn" parent="." type="Node"]
script = SubResource("GDScript_1")

[node name="Lazy" parent="." instance_placeholder="res://lazy.tscn"]

[editable path="Child"]

[connection signal="ready" from="." to="Child" method="_on_ready" binds=[Object(Node3D,"name":"Bound")]]
)",
				ResourceUID::get_singleton()->id_to_text(known_uid), unknown_uid));
	}

	const String inherited_path = TestUtils::get_temp_path("scan_text_scene_inherited.tscn");
	{
		Ref<FileAccess> f = FileAccess::open(inherited_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string(R"([gd_scene format=3]

[ext_resource type="PackedScene" path="res://base.tscn" id="1_base"]

[node name="Derived" instance=ExtResource("1_base")]

[node name="Extra" parent="." type="Node" groups=["g"]]
)");
	}

	const int object_count = ObjectDB::get_object_count();
	ErrorDetector ed;
	TypedArray<Dictionary> nodes = SceneState::scan_text_scene(scene_path);
	TypedArray<Dictionary> inherited = SceneState::scan_text_scene(inherited_path);
	CHECK_FALSE(ed.has_error);
	CHECK(ObjectDB::get_object_count() == object_count);

	const auto make_node = [](const String &p_path, const String &p_name, const String &p_type, const String &p_parent, const String &p_instance, bool p_placeholder, const String &p_script, const PackedStringArray &p_groups, int p_unique_id) {
		Dictionary node;
		node["path"] = p_path;
		node["name"] = p_name;
		node["type"] = p_type;
		node["parent"] = p_parent;
		node["instance"] = p_instance;
		node["placeholder"] = p_placeholder;
		node["script"] = p_script;
		node["groups"] = p_groups;
		node["unique_id"] = p_unique_id;
		return node;
	};

	REQUIRE(nodes.size() == 5);
	CHECK(Dictionary(nodes[0]) == make_node(".", "Root", "Node3D", "", "", false, "res://resolved_root.gd", { "saveable", "level" }, 1234));
	CHECK(Dictionary(nodes[1]) == make_node("Child", "Child", "", ".", "res://child.tscn", false, "res://child.gd", {}, 5));
	CHECK(Dictionary(nodes[2]) == make_node("Child/Body/Inner", "Inner", "Node", "Child/Body", "", false, "", {}, 0));
	CHECK(Dictionary(nodes[3]) == make_node("BuiltIn", "BuiltIn", "Node", ".", "", false, "", {}, 0));
	CHECK(Dictionary(nodes[4]) == make_node("Lazy", "Lazy", "", ".", "res://lazy.tscn", true, "", {}, 0));

	REQUIRE(inherited.size() == 2);
	CHECK(Dictionary(inherited[0]) == make_node(".", "Derived", "", "", "res://base.tscn", false, "", {}, 0));
	CHECK(Dictionary(inherited[1]) == make_node("Extra", "Extra", "Node", ".", "", false, "", { "g" }, 0));

	ResourceUID::get_singleton()->remove_id(known_uid);
	DirAccess::remove_absolute(scene_path);
	DirAccess::remove_absolute(inherited_path);
}

TEST_CASE("[PackedScene][SceneState] scan_text_scene fails on non-scenes and parse errors") {
	const String broken_path = TestUtils::get_temp_path("scan_text_scene_broken.tscn");
	{
		Ref<FileAccess> f = FileAccess::open(broken_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string(R"([gd_scene format=3]

[node name="Root" type="Node"]
position = Vector2(1, 2]
)");
	}

	ERR_PRINT_OFF;
	ErrorDetector ed;
	CHECK(SceneState::scan_text_scene("res://not_a_scene.tres").is_empty());
	CHECK(ed.has_error);

	ed.clear();
	CHECK(SceneState::scan_text_scene(broken_path).is_empty());
	CHECK(ed.has_error);
	ERR_PRINT_ON;

	DirAccess::remove_absolute(broken_path);
}

} // namespace TestPackedScene
