@m("script")
@m("shared")
extends "./mark_annotations_base.notest.gd"

@warning_ignore("unused_signal")
@m("signal") @m("ui") signal changed(value: int)
@warning_ignore("unused_signal")
signal plain_signal

@m("member") var value: int = 0

@m("own") @m("base") func overridden():
	pass

func inherited():
	pass

@m("static") @m("tool") static func helper():
	pass

@m("inner") @m("shared")
class Inner:
	@m("inner_func") func run():
		pass

@m("derived")
class DerivedInner extends Inner:
	@m("derived_func") func run():
		pass

@m("outer")
class Outer:
	class Nested:
		pass

func test():
	var script: GDScript = get_script()
	print(script.get_script_marks())
	print(script.get_method_marks(&"overridden"))
	print(script.get_method_marks(&"inherited"))
	print(script.get_method_marks(&"helper"))
	print(script.get_method_marks(&"test"))
	print(script.get_signal_marks(&"changed"))
	print(script.get_signal_marks(&"base_signal"))
	print(script.get_signal_marks(&"plain_signal"))
	print(script.get_member_marks(&"value"))
	print(script.get_member_marks(&"overridden"))
	var base_script: GDScript = script.get_base_script()
	print(base_script.get_script_marks())
	print(base_script.get_method_marks(&"overridden"))
	var inner: GDScript = Inner
	print(inner.get_script_marks())
	print(inner.get_method_marks(&"run"))
	var derived: GDScript = DerivedInner
	print(derived.get_script_marks())
	print(derived.get_method_marks(&"run"))
	var outer: GDScript = Outer
	var nested: GDScript = Outer.Nested
	print(outer.get_script_marks())
	print(nested.get_script_marks())
