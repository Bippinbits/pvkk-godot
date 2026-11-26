class Base:
	@save var base_value: int = 1
	@m("transient") var base_cache: Array = []

class Derived extends Base:
	@save @m("ui") @m("profile") var derived_value := 2.0
	var plain: int = 0
	@save var derived_name: String = ""

const MARK = &"constant"

@save var health: int = 100
@save var offset := Vector2.ZERO
@m(MARK) @m("debug") static var counter: int = 0
var untouched: int = 0

func test():
	var script: GDScript = get_script()
	print(script.get_saved_members())
	print(script.get_member_marks(&"counter"))
	print(script.get_member_marks(&"untouched"))
	var base_script: GDScript = Base
	var derived_script: GDScript = Derived
	print(base_script.get_saved_members())
	print(derived_script.get_saved_members())
	print(derived_script.get_member_marks(&"derived_value"))
	print(derived_script.get_member_marks(&"base_cache"))
	print(derived_script.get_member_marks(&"missing"))
