cmake_minimum_required(VERSION 3.20)
project(Bathos VERSION 1.1.0 LANGUAGES C CXX)
# Bathos: a picture in, its depth map out. Part of Atelier.
#   Bathos      the window (SDL2, OpenGL 3, Dear ImGui)
#   bathos-cli  the command line (Chimera's Filter > Bathos runs it)
# Both use src/depth.cpp: Depth Anything V2 through ONNX Runtime's C API (onnxruntime.dll is loaded at run time).
# stb, ONNX Runtime's header and Dear ImGui are fetched here (or taken from external/); SDL2 by build.bat / apt.
#   Windows: build.bat    Linux: ./build.sh (libsdl2-dev)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # the C++ runtime built in: no VC++ redistributable needed
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(STB_DIR ${CMAKE_SOURCE_DIR}/external/stb)
foreach(f stb_image.h stb_image_write.h)
    if(NOT EXISTS ${STB_DIR}/${f})
        file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/master/${f} ${STB_DIR}/${f})
    endif()
endforeach()
set(ORT_DIR ${CMAKE_SOURCE_DIR}/external/onnxruntime)
if(NOT EXISTS ${ORT_DIR}/onnxruntime_c_api.h)
    file(DOWNLOAD https://raw.githubusercontent.com/microsoft/onnxruntime/v1.20.1/include/onnxruntime/core/session/onnxruntime_c_api.h ${ORT_DIR}/onnxruntime_c_api.h)
endif()

# the depth core, shared by both programs
add_library(bathos_core STATIC src/depth.cpp src/face.cpp src/stb.cpp)
target_include_directories(bathos_core PUBLIC ${STB_DIR} ${ORT_DIR} src)
if(MSVC)
    target_compile_options(bathos_core PRIVATE /O2 /fp:fast /utf-8 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(bathos_core PUBLIC dl pthread)
endif()

add_executable(bathos-cli src/cli.cpp)
target_link_libraries(bathos-cli PRIVATE bathos_core)
if(MSVC)
    target_compile_options(bathos-cli PRIVATE /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
endif()

include(FetchContent)
if(EXISTS ${CMAKE_SOURCE_DIR}/external/imgui-1.90.4/imgui.h)
    set(FETCHCONTENT_SOURCE_DIR_IMGUI ${CMAKE_SOURCE_DIR}/external/imgui-1.90.4)
endif()
FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.4.tar.gz)
FetchContent_MakeAvailable(imgui)
if(WIN32 AND NOT SDL2_DIR AND EXISTS ${CMAKE_SOURCE_DIR}/external/SDL2-2.30.9/cmake)
    set(SDL2_DIR ${CMAKE_SOURCE_DIR}/external/SDL2-2.30.9/cmake)
endif()
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL2::SDL2 OpenGL::GL)

set(GUI_SOURCES src/gui.cpp)
if(WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/src/app.rc)
    list(APPEND GUI_SOURCES src/app.rc)
endif()
add_executable(Bathos WIN32 ${GUI_SOURCES})
target_link_libraries(Bathos PRIVATE bathos_core imgui)
target_compile_definitions(Bathos PRIVATE BATHOS_VERSION="${PROJECT_VERSION}")
if(TARGET SDL2::SDL2main)
    target_link_libraries(Bathos PRIVATE SDL2::SDL2main)
endif()
if(MSVC)
    target_compile_options(Bathos PRIVATE /W3 /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS)
endif()

# assets, SDL2 and ONNX Runtime next to the exes
add_custom_command(TARGET Bathos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Bathos>/assets)
# the small face models ship with Bathos (the depth model, ~99 MB, is fetched once)
add_custom_command(TARGET bathos-cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/models $<TARGET_FILE_DIR:bathos-cli>/models)
if(WIN32)
    add_custom_command(TARGET Bathos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:Bathos>)
    if(EXISTS ${ORT_DIR}/onnxruntime.dll)
        add_custom_command(TARGET bathos-cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DIR}/onnxruntime.dll $<TARGET_FILE_DIR:bathos-cli>)
    endif()
endif()
