@save var untyped = 0

func get_value() -> Variant:
	return 1

@warning_ignore("inference_on_variant")
@save var inferred_variant := get_value()
@save static var static_value: int = 0
@save @save var twice: int = 0

func test():
	pass
