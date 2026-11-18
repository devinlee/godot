from pathlib import Path


def can_build(env, platform):
    role = env.get("meton_js_role", "none")
    if role == "none":
        return False
    if platform != "windows" or env["arch"] != "x86_64":
        raise RuntimeError("当前原生 JS 角色只交接 Windows x64，不能静默改用其他执行器")
    return True


def configure(env):
    role = env["meton_js_role"]
    if role not in ("game", "task") or env.editor_build or env.get("module_meton_engine_enabled", False):
        raise RuntimeError("JS 游戏/任务必须独立构建，不能接入纯数据编辑器或 ContentTool")
    identity = Path(env["meton_js_identity_dir"])
    if not (identity / "meton_gw3_identity.generated.h").is_file():
        raise RuntimeError("GW3 需要本次实际源码与配置生成的角色身份，不能复用 JR2 候选身份")
    env.AppendUnique(CPPPATH=[str(identity.resolve())])
    env.AppendUnique(CPPDEFINES=["METON_JS_GAME_ENABLED" if role == "game" else "METON_JS_TASK_ENABLED"])
    if role == "game":
        env.AppendUnique(CPPDEFINES=["METON_GAME_ATLAS_ENABLED"])
    if env.get("meton_js_depfiles", False):
        # 每个对象单独记录编译器实际读取的头文件，避免并发覆盖同名源文件的回执。
        env.Append(CCFLAGS=["/sourceDependencies", "${TARGET}.sources.json"])
        # MSVC 要求选项与参数在响应文件同一行；SCons 的默认换行会触发 D8004。
        env["TEMPFILEARGJOIN"] = " "
        env.Append(LINKFLAGS=["/VERBOSE:LIB"])


def get_opts(platform):
    from SCons.Variables import BoolVariable, EnumVariable
    # 非运行配置默认不启用，路径只在选定角色后检查，避免影响 GW2 的原配置。
    return [EnumVariable("meton_js_role", "原生 JS 的唯一构建角色", "none", ("none", "game", "task")),
            BoolVariable("meton_js_depfiles", "记录本次实际编译输入", False),
            ("meton_js_identity_dir", "本组合生成的角色身份目录", ""),
            ("meton_v8_root", "已核对的 V8 源码与库目录", ""),
            ("meton_v8_builtins", "同一编译材料的 Windows compiler-rt", "")]
