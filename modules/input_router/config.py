def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "GamepadBakedMaps",
        "GamepadContext",
        "GamepadMap",
        "GlyphFontMap",
        "InputRouter",
        "RouteBindingResource",
    ]


def get_doc_path():
    return "doc_classes"
