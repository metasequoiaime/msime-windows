cmake_minimum_required(VERSION 3.25)

project(msimeui VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(
    /D_UNICODE=1
    /DUNICODE=1
    /DNOMINMAX=1
    /DWIN32_LEAN_AND_MEAN=1
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(msimeui STATIC
    src/Accordion.cpp
    src/AnimatedImage.cpp
    src/Animation.cpp
    src/Application.cpp
    src/Brush.cpp
    src/Button.cpp
    src/CandidateList.cpp
    src/ComboBox.cpp
    src/ControlsInternal.cpp
    src/DebugLog.cpp
    src/DeviceResources.cpp
    src/Fonts.cpp
    src/Image.cpp
    src/ImageMask.cpp
    src/ListView.cpp
    src/Menu.cpp
    src/PagerArrows.cpp
    src/Popup.cpp
    src/RangeControls.cpp
    src/Separator.cpp
    src/TabControl.cpp
    src/TextBox.cpp
    src/TreeView.cpp
    src/Layout.cpp
    src/Scene.cpp
    src/ScrollAxis.cpp
    src/Theme.cpp
    src/Window.cpp
    src/WindowClass.cpp
    src/tsf/DisplayAttribute.cpp
    src/tsf/TextContainer.cpp
    src/tsf/TextEditor.cpp
    src/tsf/TextEditSink.cpp
    src/tsf/TextLayout.cpp
    src/tsf/TextStore.cpp
    src/tsf/TsfTextServices.cpp
    include/msimeui/AnimatedImage.h
    include/msimeui/Animation.h
    include/msimeui/Application.h
    include/msimeui/Brush.h
    src/ControlsInternal.h
    src/DebugLog.h
    include/msimeui/Controls.h
    include/msimeui/DeviceResources.h
    include/msimeui/Fonts.h
    include/msimeui/ImageMask.h
    include/msimeui/Layout.h
    include/msimeui/Scene.h
    include/msimeui/ScrollAxis.h
    include/msimeui/Theme.h
    include/msimeui/Types.h
    include/msimeui/Window.h
    src/tsf/DisplayAttribute.h
    src/tsf/private.h
    src/tsf/TextContainer.h
    src/tsf/TextEditor.h
    src/tsf/TextEditSink.h
    src/tsf/TextLayout.h
    src/tsf/TextStore.h
    src/tsf/TsfTextServices.h
)

target_include_directories(msimeui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tsf
)

target_link_libraries(msimeui PUBLIC
    d2d1.lib
    dxguid.lib
    d3d11.lib
    dcomp.lib
    dxgi.lib
    dwrite.lib
    dwmapi.lib
    imm32.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
    windowscodecs.lib
    shlwapi.lib
    # DeviceResources backdrop composition (Windows.UI.Composition through the WRL ABI).
    runtimeobject.lib
    CoreMessaging.lib
)

add_executable(msimeui-demo WIN32
    demos/msimeui-demo/DemoScene.cpp
    demos/msimeui-demo/DemoScene.h
    demos/msimeui-demo/main.cpp
    demos/msimeui-demo/msimeui-demo.rc
    demos/msimeui-demo/resource.h
)

target_link_libraries(msimeui-demo PRIVATE msimeui)
target_compile_definitions(msimeui-demo PRIVATE MSIMEUI_ASSET_DIR=L\"${CMAKE_CURRENT_SOURCE_DIR}/assets\")

add_executable(msimeui-emoji-panel WIN32
    demos/msimeui-emoji-panel/EmojiPanel.cpp
    demos/msimeui-emoji-panel/EmojiPanel.h
    demos/msimeui-emoji-panel/main.cpp
    demos/msimeui-emoji-panel/msimeui-emoji-panel.rc
    demos/msimeui-emoji-panel/resource.h
)

target_link_libraries(msimeui-emoji-panel PRIVATE msimeui)

add_executable(msimeui-keyboard-demo WIN32
    demos/msimeui-keyboard-demo/KeyboardPanel.cpp
    demos/msimeui-keyboard-demo/KeyboardPanel.h
    demos/msimeui-keyboard-demo/main.cpp
    demos/msimeui-keyboard-demo/msimeui-keyboard-demo.rc
    demos/msimeui-keyboard-demo/resource.h
)

target_link_libraries(msimeui-keyboard-demo PRIVATE msimeui)

add_executable(msimeui-handwriting-demo WIN32
    demos/msimeui-handwriting-demo/HandwritingPanel.cpp
    demos/msimeui-handwriting-demo/HandwritingPanel.h
    demos/msimeui-handwriting-demo/main.cpp
    demos/msimeui-handwriting-demo/msimeui-handwriting-demo.rc
    demos/msimeui-handwriting-demo/resource.h
)

target_link_libraries(msimeui-handwriting-demo PRIVATE msimeui windowsapp.lib)

# C++/WinRT needs C++20 coroutines. Under C++17 it falls back to
# <experimental/coroutine>, which recent MSVC STL versions reject outright.
target_compile_features(msimeui-handwriting-demo PRIVATE cxx_std_20)

if(MSVC)
    # include/msimeui/Controls.h is UTF-8 without a BOM, so every consumer of msimeui has to read
    # it as UTF-8 too; otherwise a CJK-locale build (code page 936) swallows lines and fails.
    target_compile_options(msimeui PUBLIC /utf-8)
    target_compile_options(msimeui PRIVATE /W4 /permissive-)
    target_compile_options(msimeui-demo PRIVATE /W4 /permissive-)
    target_compile_options(msimeui-emoji-panel PRIVATE /W4 /permissive-)
    target_compile_options(msimeui-keyboard-demo PRIVATE /W4 /permissive-)
    target_compile_options(msimeui-handwriting-demo PRIVATE /W4 /permissive-)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
