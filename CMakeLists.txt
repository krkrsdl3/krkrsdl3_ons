cmake_minimum_required(VERSION 3.28)

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
project(krkrsdl3_ons C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_KRKRSDL3_LINUX=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_definitions(_KRKRSDL3_WINDOWS=1)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# 公共功能宏
add_compile_definitions(_KRKRSDL3_USE_FFMPEG=1)
add_compile_definitions(_KRKRSDL3_USE_SDL3=1)
add_compile_definitions(_KRKRSDL3_GL=1)

# ----------------------------------------------------------------------------
file(GLOB_RECURSE SRC_TJS2_LIST CONFIGURE_DEPENDS "Core/tjs2/*.cpp" "Core/tjs2/*.c")
file(GLOB_RECURSE SRC_CORE_MAIN_LIST CONFIGURE_DEPENDS
    "Core/core/archive/*.cpp" "Core/core/archive/*.c"
    "Core/core/main/*.cpp" "Core/core/main/*.c"
    "Core/core/media/*.cpp" "Core/core/media/*.c"
    "Core/core/msg/*.cpp" "Core/core/msg/*.c"
    "Core/core/script/*.cpp" "Core/core/script/*.c"
    "Core/core/utils/*.cpp" "Core/core/utils/*.c")
set(SRC_CORE_RENDER_LIST
    Core/core/render/backend/GLRenderBackend.cpp
    Core/core/render/backend/SWRenderBackend.cpp
    Core/core/render/gl/blend_function.cpp
    Core/core/render/gl/tvpgl.cpp
    Core/core/render/ComplexRect.cpp
    Core/core/render/DrawDevice.cpp
    Core/core/render/LayerBitmap.cpp
    Core/core/render/LayerManager.cpp
    Core/core/render/LayerTreeOwner.cpp
    Core/core/render/RenderManager.cpp
    Core/core/render/TVPColor.cpp
    Core/core/render/TVPTrans.cpp
    Core/core/render/TVPCompositor.cpp)
set(SRC_ENVIRON_LIST
    Core/environ/Platform.cpp
    Core/environ/sdl3/sdl3_dialog.cpp
    Core/environ/sdl3/sdl3_utils.cpp
    Core/environ/sdl3/sdl3_app.cpp
    Core/environ/sdl3/sdl3_entry.cpp
    Core/environ/sdl3/sdl3_file.cpp
    Core/environ/sdl3/sdl3_input.cpp
    Core/environ/sdl3/sdl3_thread.cpp
    Core/environ/sdl3/sdl3_audio.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SRC_ENVIRON_LIST Core/environ/linux/linux_core.cpp)
    list(APPEND SRC_ENVIRON_LIST Core/environ/unix/unix_file.cpp)
    list(APPEND SRC_ENVIRON_LIST Core/environ/unix/unix_sysinfo.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND SRC_ENVIRON_LIST Core/environ/windows/windows_core.cpp)
endif()
set(SRC_PLUGINS_LIST
    Core/plugins/TVPPlugin.cpp
    Core/plugins/ncbind/ncbind.cpp
    #图片扩展
    Core/plugins/Kirikiroid2/LoadJPEG.cpp
    Core/plugins/Kirikiroid2/LoadPNG.cpp
    Core/plugins/Kirikiroid2/LoadWEBP.cpp
    #音频扩展
    Core/plugins/wuffmpeg.cpp
    Core/plugins/wuopus.cpp
    Core/plugins/wuvorbis.cpp
    #onscripter 自有代码（指令转发架构，docs/06）
    onscripter/onscripter.cpp
    onscripter/ons_archive.cpp
    onscripter/ons_audio.cpp
onscripter/ons_movie.cpp
    onscripter/ons_events.cpp
    onscripter/ons_screen.cpp
    onscripter/ons_vm.cpp
    # 编码转换表（上游 GPL-2.0 代码的适配副本：源已转码 UTF-8、缓冲放大，
    # 见 onscripter/coding2utf16.h 说明）
    onscripter/coding2utf16.cpp
    onscripter/gbk2utf16.cpp
    onscripter/sjis2utf16.cpp
    )

set(KRKR_SRC ${SRC_TJS2_LIST} ${SRC_CORE_MAIN_LIST} ${SRC_CORE_RENDER_LIST} ${SRC_ENVIRON_LIST} ${SRC_PLUGINS_LIST})
add_executable(${PROJECT_NAME} ${KRKR_SRC})

target_include_directories(${PROJECT_NAME} PUBLIC
    Core/core/archive Core/core/main Core/core/media/font Core/core/media/image
    Core/core/media/movie Core/core/media/sound Core/core/msg Core/core/render
    Core/core/script Core/core/utils Core/core/utils/math Core/environ Core/plugins Core/tjs2
    onscripter)

find_package(glad CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE glad::glad)
find_package(FFMPEG REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
find_package(PNG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE PNG::PNG)
find_package(JPEG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE JPEG::JPEG)
find_package(libjpeg-turbo CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>)
find_package(WebP CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE WebP::webp WebP::webpdecoder WebP::webpdemux)
find_package(Vorbis CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Vorbis::vorbis Vorbis::vorbisfile Vorbis::vorbisenc)
find_package(Opus CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Opus::opus)
find_package(OpusFile CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpusFile::opusfile)
find_package(Freetype REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Freetype::Freetype)
# NBZ/bzip2（ONS 封包压缩）
find_package(BZip2 REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE BZip2::BZip2)
find_package(plutovg CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE plutovg::plutovg)
find_package(oniguruma CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE oniguruma::onig)
find_package(glm CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE glm::glm-header-only)
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
find_package(SDL3_ttf CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3_ttf::SDL3_ttf)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/Res
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/Res
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/demo
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/demo)
