@m("a") @m("a")
extends RefCounted

@warning_ignore("unused_signal")
@m("b") @m("b") signal duplicate_signal
@m("c") @m("c") func duplicate_function():
	pass
@m("d") @m("d") static func duplicate_static_function():
	pass
@m("e") @m("e") class DuplicateClass:
	pass

func test():
	pass
