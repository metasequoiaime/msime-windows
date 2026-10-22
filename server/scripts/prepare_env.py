import os
import re


def normpath(path):
    return path.replace("\\", "/")


cur_file_path = os.path.dirname(os.path.abspath(__file__))
project_root_path = os.path.dirname(cur_file_path)

user_home = os.path.expanduser("~")

# vcpkg location. Prefer the VCPKG_ROOT environment variable so that installs
# outside Scoop work as well; fall back to the historical Scoop path.
vcpkg_root = normpath(
    os.environ.get("VCPKG_ROOT") or os.path.join(user_home, "scoop", "apps", "vcpkg", "current")
).rstrip("/")

MetasequoiaImeServer_root_path = normpath(project_root_path)
MetasequoiaImeServer_src_path = normpath(os.path.join(MetasequoiaImeServer_root_path, "src"))
vcpkg_include_path = normpath(
    os.path.join(
        MetasequoiaImeServer_root_path,
        "build",
        "vcpkg_installed",
        "x64-windows",
        "include",
    )
)
monorepo_root_path = normpath(os.path.dirname(project_root_path))
engine_path = normpath(os.path.join(monorepo_root_path, "engine"))
utfcpp_path = normpath(os.path.join(engine_path, "utfcpp", "source"))
wil_path = normpath(
    os.path.join(
        user_home,
        ".nuget",
        "packages",
        "microsoft.windows.implementationlibrary",
        "1.0.240803.1",
        "include",
    )
)
# Boost location. CMakeLists.txt already honours the BOOST_ROOT environment
# variable; mirror that here instead of assuming a Scoop install.
boost_path = normpath(
    os.environ.get("BOOST_ROOT") or os.path.join(user_home, "scoop", "apps", "boost", "current")
).rstrip("/")

#
# project_root/.clangd
#
# The template carries @NAME@ placeholders rather than fixed line numbers. The
# previous version addressed lines by index, and the indices had drifted, so
# paths were written over the wrong include lines and comments and clangd could
# not resolve the headers they were meant for.
#
clangd_substitutions = {
    "@SERVER_ROOT@": MetasequoiaImeServer_root_path,
    "@SERVER_SRC@": MetasequoiaImeServer_src_path,
    "@VCPKG_INCLUDE@": vcpkg_include_path,
    "@HOST_API_INCLUDE@": os.path.join(MetasequoiaImeServer_root_path, "build", "host-api", "include"),
    "@ENGINE_ROOT@": engine_path,
    "@UTFCPP@": utfcpp_path,
    "@VOICE_INCLUDE@": engine_path + "/voice/include",
    "@MINIAUDIO@": engine_path + "/voice/third_party/miniaudio",
    "@GUI_INCLUDE@": monorepo_root_path + "/ui/include",
    "@WIL@": wil_path,
    "@BOOST@": boost_path,
}
dot_clangd_file = os.path.join(
    MetasequoiaImeServer_root_path, "scripts", "config_files", ".clangd"
)
dot_clangd_output_file = os.path.join(MetasequoiaImeServer_root_path, ".clangd")
with open(dot_clangd_file, "r", encoding="utf-8") as f:
    content = f.read()
for placeholder, value in clangd_substitutions.items():
    content = content.replace(placeholder, value)
leftover = re.findall(r"@[A-Z0-9_]+@", content)
if leftover:
    raise SystemExit(f"unsubstituted placeholders in the .clangd template: {sorted(set(leftover))}")
with open(dot_clangd_output_file, "w", encoding="utf-8") as f:
    f.write(content)

#
# project_root/CMakePresets.json
#
# Rewritten in place rather than copied from a template. The template that used
# to be copied here had fallen behind the tracked file and would have dropped
# the vcpkg-release preset that llaunch-release.ps1 depends on.
#
CMakePresets_file = os.path.join(MetasequoiaImeServer_root_path, "CMakePresets.json")
with open(CMakePresets_file, "r", encoding="utf-8") as f:
    content = f.read()
content = re.sub(
    r'("VCPKG_ROOT":\s*")[^"]*(")',
    lambda m: f"{m.group(1)}{vcpkg_root}/{m.group(2)}",
    content,
)
content = re.sub(
    r'("CMAKE_TOOLCHAIN_FILE":\s*")[^"]*(")',
    lambda m: f"{m.group(1)}{vcpkg_root}/scripts/buildsystems/vcpkg.cmake{m.group(2)}",
    content,
)
with open(CMakePresets_file, "w", encoding="utf-8") as f:
    f.write(content)
