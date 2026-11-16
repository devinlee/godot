def can_build(env, platform):
    return platform == "windows" and env["d3d12"]


def configure(env):
    pass


def get_opts(platform):
    from SCons.Variables import BoolVariable
    return [("meton_contracts_dir", "Meton G01 generated C++ contracts (required)", ""),
            BoolVariable("meton_surface_fixture", "Build the trusted native surface fixture (tests only)", False),
            ("meton_fixture_config_dir", "Pinned surface fixture configuration directory", "")]
