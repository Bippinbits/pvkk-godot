const MARK = "constant"
var mark_variable := "variable"

@m(1) var non_string: int = 0
@m(mark_variable) var non_constant: int = 0
@m("") var empty: int = 0
@m("a") @m(MARK) @m("a") var duplicate: int = 0

func test():
	pass
