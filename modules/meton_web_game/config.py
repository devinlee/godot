from pathlib import Path


def can_build(env, platform):
    if not env.get("meton_web_runtime", False):
        return False
    if platform != "web":
        raise RuntimeError("Web 游戏角色只接受 Web 工具链")
    return True


def configure(env):
    if env.editor_build:
        raise RuntimeError("Web 游戏角色不包含创作编辑器")
    identity = Path(env["meton_web_identity_dir"])
    if not (identity / "meton_gw3_identity.generated.h").is_file():
        raise RuntimeError("Web 游戏需要本次实际源码与配置生成的角色身份")
    env.AppendUnique(CPPPATH=[str(identity.resolve())], CPPDEFINES=["METON_GAME_ATLAS_ENABLED"])
    # 沿用已验 Wasm 硬上限，实际目标安全与停止仍由对应运行验收证明。
    env.Append(LINKFLAGS=["-sMAXIMUM_MEMORY=268435456"])


def get_opts(platform):
    from SCons.Variables import BoolVariable
    return [BoolVariable("meton_web_runtime", "显式启用 Web 游戏角色", False),
            ("meton_web_identity_dir", "本组合生成的 Web 角色身份目录", "")]
