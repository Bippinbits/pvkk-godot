@m("base_script")
@m("shared")
extends RefCounted

@warning_ignore("unused_signal")
@m("base_signal") signal base_signal

@m("base") func overridden():
	pass

@m("base_only") func inherited():
	pass
