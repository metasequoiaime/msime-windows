# Assemble the TSF DLL as an Arm64X binary.
#
# Windows shares one 64-bit registry view between Arm64 and x64 hosts, so
# HKEY_CLASSES_ROOT\CLSID\{...}\InProcServer32 can name exactly one file. A plain Arm64 DLL would
# leave every x64-emulated host without an input method, exactly as today's x64 DLL leaves every
# native Arm64 host without one. Arm64X is the container that settles that: the file carries an
# Arm64 image and an Arm64EC image, and the loader maps whichever one matches the loading process.
#
# Microsoft's recipe is a two-pass build. The Arm64 pass links normally but also asks the linker to
# dump every input into a response file. The Arm64EC pass then hands those Arm64 objects, import
# libraries and .def file back to its own link as extra inputs under /machine:arm64x. Only the
# Arm64EC pass produces the Arm64X binary, in its own build directory; the Arm64 pass exists for
# its linker inputs alone.
#
# /LINKREPROFULLPATHRSP needs link.exe 14.38 or newer, which is Visual Studio 17.11 and later. The
# project already builds with Visual Studio 18 2026, and BUILD_AS_ARM64X is opt-in, so an ordinary
# x64 or Win32 build never touches this file.
#
# Build the two passes in this order, or the Arm64EC configure fails with the message below:
#
#   cmake -S windows -B windows/buildarm64-release  -A ARM64   -DBUILD_AS_ARM64X=ARM64   ...
#   cmake --build windows/buildarm64-release --config Release
#   cmake -S windows -B windows/buildarm64x-release -A ARM64EC -DBUILD_AS_ARM64X=ARM64EC ...
#   cmake --build windows/buildarm64x-release --config Release
#
# The response file lives in the source tree because both build directories have to agree on one
# path; <repo>/windows/repros/ is gitignored.

set(MSIME_ARM64X_REPRO_DIR "${CMAKE_SOURCE_DIR}/repros")

# Adds the Arm64 half of an Arm64X link: its objects, its import libraries and, under the name the
# linker expects, its module definition file.
function(msime_arm64x_add_arm64_inputs target)
    set(repro_file "${MSIME_ARM64X_REPRO_DIR}/${target}.rsp")
    if(NOT EXISTS "${repro_file}")
        message(FATAL_ERROR
            "There is no Arm64 linker response file at ${repro_file}. Build the same source with "
            "-DBUILD_AS_ARM64X=ARM64 into a different build directory first; the Arm64EC pass "
            "merges that pass's output and cannot run on its own.")
    endif()

    file(STRINGS "${repro_file}" arm64_objects REGEX "obj\"$")
    file(STRINGS "${repro_file}" arm64_def REGEX "def\"$")
    file(STRINGS "${repro_file}" arm64_libs REGEX "lib\"$")
    string(REPLACE "\"" ";" arm64_objects "${arm64_objects}")
    string(REPLACE "\"" ";" arm64_libs "${arm64_libs}")
    string(REPLACE "\"" ";" arm64_def "${arm64_def}")
    string(REPLACE "/def:" "/defArm64Native:" arm64_def "${arm64_def}")
    # Stripping the quotes leaves an empty element on both sides of every path. Empty link
    # options would reach the linker as bare quotes on the command line.
    foreach(list_name arm64_objects arm64_libs arm64_def)
        list(FILTER ${list_name} EXCLUDE REGEX "^$")
    endforeach()

    target_sources(${target} PRIVATE ${arm64_objects})
    target_link_options(${target} PRIVATE /machine:arm64x ${arm64_def} ${arm64_libs})
endfunction()

if(BUILD_AS_ARM64X STREQUAL "ARM64")
    add_custom_target(msime_arm64x_repro_dir ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MSIME_ARM64X_REPRO_DIR}")
    foreach(target ${ARM64X_TARGETS})
        add_dependencies(${target} msime_arm64x_repro_dir)
        target_link_options(${target} PRIVATE
            "/LINKREPROFULLPATHRSP:${MSIME_ARM64X_REPRO_DIR}/${target}.rsp")
    endforeach()
elseif(BUILD_AS_ARM64X STREQUAL "ARM64EC")
    foreach(target ${ARM64X_TARGETS})
        msime_arm64x_add_arm64_inputs(${target})
    endforeach()
else()
    message(FATAL_ERROR "BUILD_AS_ARM64X must be ARM64 or ARM64EC, got '${BUILD_AS_ARM64X}'")
endif()
