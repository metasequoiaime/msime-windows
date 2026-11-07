# 最小 BoostConfig.cmake
#
# CMake 4 起不再自带 FindBoost 模块，只走 config 模式；本机的 Boost 是解压版，
# 官方下载包里没有 BoostConfig.cmake，于是 configure 阶段直接失败。
#
# 这个文件只做一件事：把项目实际用到的三个目标（headers / locale / json）指向
# 本机的 Boost 安装，其余组件一律不声明。放在仓库里而不是工具链目录，是为了让
# 构建方式可复现——换一台机器只要改 BOOST_ROOT。
#
# 用法：cmake ... -DBoost_DIR=<仓库>/build-support

set(_boost_root "")

if(DEFINED ENV{BOOST_ROOT} AND EXISTS "$ENV{BOOST_ROOT}/boost/version.hpp")
  set(_boost_root "$ENV{BOOST_ROOT}")
elseif(EXISTS "D:/Tools/boost/boost/version.hpp")
  set(_boost_root "D:/Tools/boost")
elseif(EXISTS "C:/Boost/boost/version.hpp")
  set(_boost_root "C:/Boost")
endif()

if(_boost_root STREQUAL "")
  set(Boost_FOUND FALSE)
  set(Boost_NOT_FOUND_MESSAGE "找不到 Boost 头文件：请设置 BOOST_ROOT 指向 Boost 解压目录")
  return()
endif()

set(_boost_lib_dir "${_boost_root}/lib64-msvc-14.3")
set(_boost_version 108600)
set(_boost_version_string "1.86.0")

if(NOT EXISTS "${_boost_lib_dir}")
  set(Boost_FOUND FALSE)
  set(Boost_NOT_FOUND_MESSAGE "在 ${_boost_root} 下找不到 lib64-msvc-14.3")
  return()
endif()

set(Boost_INCLUDE_DIRS "${_boost_root}")
set(Boost_INCLUDE_DIR "${_boost_root}")
set(Boost_VERSION ${_boost_version})
set(Boost_VERSION_STRING "${_boost_version_string}")
set(Boost_LIBRARY_DIRS "${_boost_lib_dir}")

if(NOT TARGET Boost::headers)
  add_library(Boost::headers INTERFACE IMPORTED)
  set_target_properties(Boost::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
endif()
if(NOT TARGET Boost::boost)
  add_library(Boost::boost INTERFACE IMPORTED)
  set_target_properties(Boost::boost PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
endif()

# 静态库 + 动态运行时（/MD），与 CMake 的默认 MSVC 运行时一致。
function(_boost_imported_target name filename)
  if(NOT TARGET Boost::${name})
    if(NOT EXISTS "${_boost_lib_dir}/${filename}")
      message(FATAL_ERROR "缺少 ${_boost_lib_dir}/${filename}")
    endif()
    add_library(Boost::${name} STATIC IMPORTED)
    set_target_properties(Boost::${name} PROPERTIES
      IMPORTED_LOCATION "${_boost_lib_dir}/${filename}"
      INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
  endif()
  set(Boost_${name}_FOUND TRUE PARENT_SCOPE)
endfunction()

_boost_imported_target(locale "libboost_locale-vc143-mt-x64-1_86.lib")
_boost_imported_target(json "libboost_json-vc143-mt-x64-1_86.lib")
# Boost.Locale 与 Boost.JSON 的静态库都会 auto-link boost_container。
_boost_imported_target(container "libboost_container-vc143-mt-x64-1_86.lib")
set_target_properties(Boost::locale PROPERTIES INTERFACE_LINK_LIBRARIES Boost::container)
set_target_properties(Boost::json PROPERTIES INTERFACE_LINK_LIBRARIES Boost::container)

set(Boost_locale_FOUND TRUE)
set(Boost_json_FOUND TRUE)
set(Boost_LIBRARIES Boost::locale Boost::json)
set(Boost_FOUND TRUE)
