# PS Vita platform layer.
#
#   vita_compat  POSIX functions newlib does not provide (ioctl, getifaddrs, dlopen) and
#                the diagnostics that stand in for a debugger
#   vita_d3d8    Direct3D 8 interfaces implemented on vitaGL (replaces d3d8.dll / DXVK)
#   vita_device  GameEngine, mouse and keyboard devices; compiled into the game's
#                gameenginedevice library so they share its precompiled header
#   vita_main    entry point; compiled into the game executable
#
# vita_package(<target> <title>) turns a game executable into eboot.bin and a .vpk.

add_library(vita_compat STATIC
    compat/VitaDiagnostics.cpp
    compat/VitaHelper.cpp
    compat/VitaMachineTest.cpp
    compat/VitaPacing.cpp
    compat/VitaPosix.cpp
    compat/VitaSampler.cpp
    compat/VitaYuv.cpp
    compat/include/VitaDiagnostics.h
    compat/include/VitaHelper.h
    compat/include/VitaPacing.h
    compat/include/VitaYuv.h
    compat/include/dlfcn.h
    compat/include/ifaddrs.h
    compat/include/net/if.h
    compat/include/sys/ioctl.h
)
target_link_libraries(vita_compat PUBLIC SceNet_stub SceNetCtl_stub SceSysmem_stub SceKernelThreadMgr_stub SceLibKernel_stub ScePower_stub)
# Every throw goes through VitaDiagnostics.cpp, which writes it to the log.
target_link_options(vita_compat INTERFACE -Wl,--wrap=__cxa_throw)

add_library(vita_d3d8 STATIC
    d3d8/VitaD3D8.h
    d3d8/VitaD3D8Format.cpp
    d3d8/VitaD3D8Format.h
    d3d8/VitaD3D8Direct3D.cpp
    d3d8/VitaD3D8Device.cpp
    d3d8/VitaD3D8Device.h
    d3d8/VitaD3D8GL.h
    d3d8/VitaD3D8RenderThread.cpp
    d3d8/VitaD3D8RenderThread.h
    d3d8/VitaD3D8Resources.cpp
    d3d8/VitaD3D8Resources.h
    d3d8/VitaD3D8State.cpp
)

target_include_directories(vita_d3d8 PUBLIC d3d8)

# vitaGL: the build of tools/build-vitagl.sh in VITA_VITAGL_DIR, the SDK's package with point
# lights that end at their range as in Direct3D, or else the SDK's own, whose lights have no end.
set(VITA_VITAGL_DIR "${CMAKE_SOURCE_DIR}/build/vita-vitagl" CACHE PATH
    "vitaGL with ranges for point lights, see Platform/Vita/tools/build-vitagl.sh")
if(EXISTS "${VITA_VITAGL_DIR}/lib/libvitaGL.a")
    message(STATUS "PS Vita: vitaGL from ${VITA_VITAGL_DIR}")
    set(vita_vitagl_library ${VITA_VITAGL_DIR}/lib/libvitaGL.a)
    target_include_directories(vita_d3d8 BEFORE PUBLIC ${VITA_VITAGL_DIR}/include)
else()
    message(STATUS "PS Vita: the SDK's vitaGL, point lights have no range (see tools/build-vitagl.sh)")
    set(vita_vitagl_library vitaGL)
endif()

target_link_libraries(vita_d3d8 PUBLIC
    d3d8lib
    vita_compat
    ${vita_vitagl_library}
    vitashark
    SceShaccCgExt
    SceShaccCg_stub
    mathneon
    taihen_stub
    SceKernelDmacMgr_stub
    SceGxm_stub
    SceDisplay_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceSysmodule_stub
    SceLibKernel_stub
    m
)

add_library(vita_device INTERFACE)
target_sources(vita_device INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaGameEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaGameEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaInput.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaInput.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaKeyboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaKeyboard.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaMouse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaMouse.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaNoVideoPlayer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaSilentAudioManager.h
)
target_include_directories(vita_device INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Device)

if(VITA_AUDIO)
    # miniaudio, a single header: its mixer, resource manager and decoders (WAV, MP3). A
    # release is pinned so that every build is made of the same code; the engine's own
    # cmake/miniaudio.cmake takes whatever the master branch is on the day.
    include(FetchContent)
    FetchContent_Declare(
        vita_miniaudio
        URL "https://github.com/mackron/miniaudio/archive/refs/tags/0.11.23.tar.gz"
        URL_HASH SHA256=1062155cc5882b55c48cb37f57a4dc783669e83ae0838535c62b206eeb1587a6
    )
    FetchContent_GetProperties(vita_miniaudio)
    if(NOT vita_miniaudio_POPULATED)
        FetchContent_Populate(vita_miniaudio)
    endif()

    # That release writes to freed memory. What is compiled is a corrected copy.
    include(${CMAKE_CURRENT_SOURCE_DIR}/FixMiniaudio.cmake)
    set(vita_miniaudio_include_dir ${CMAKE_CURRENT_BINARY_DIR}/miniaudio)
    vita_fix_miniaudio(${vita_miniaudio_SOURCE_DIR}/miniaudio.h ${vita_miniaudio_include_dir}/miniaudio.h)

    # The engine's miniaudio manager is built here and not through SAGE_USE_MINIAUDIO,
    # which would also pull in the parts of it that need FFmpeg.
    target_sources(vita_device INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaAudioLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaAudioLoader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaAudioManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaAudioManager.h
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/MiniAudioDevice/MiniAudioManager.cpp
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/MiniAudioDevice/MiniAudioStream.cpp
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include/MiniAudioDevice/MiniAudioManager.h
        ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include/MiniAudioDevice/MiniAudioStream.h
    )
    target_include_directories(vita_device SYSTEM INTERFACE ${vita_miniaudio_include_dir})
    target_compile_definitions(vita_device INTERFACE
        VITA_AUDIO
        # No audio devices of miniaudio's own: the engine mixes without one and
        # Device/VitaAudioManager.cpp feeds the console's output.
        MA_ENABLE_ONLY_SPECIFIC_BACKENDS
        MA_ENABLE_NULL
        MA_NO_RUNTIME_LINKING
        MA_NO_ENCODING
        MA_NO_GENERATION
        MA_NO_FLAC
    )
    target_link_libraries(vita_device INTERFACE SceAudio_stub pthread)

    # Movies: the game's Bink files, decoded by an FFmpeg that tools/build-ffmpeg.sh builds
    # into VITA_FFMPEG_DIR. Without it the movies are skipped (Device/VitaNoVideoPlayer.h).
    # The sound of a movie goes through the miniaudio manager, hence only with VITA_AUDIO.
    set(VITA_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/build/vita-ffmpeg" CACHE PATH
        "FFmpeg with the Bink decoders for the PS Vita, see Platform/Vita/tools/build-ffmpeg.sh")
    if(EXISTS "${VITA_FFMPEG_DIR}/lib/libavcodec.a")
        message(STATUS "PS Vita: movies are decoded with the FFmpeg in ${VITA_FFMPEG_DIR}")
        target_sources(vita_device INTERFACE
            ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Device/VitaFFmpegVideoPlayer.cpp
        )
        target_include_directories(vita_device SYSTEM INTERFACE ${VITA_FFMPEG_DIR}/include)
        target_compile_definitions(vita_device INTERFACE VITA_MOVIES)
        target_link_libraries(vita_device INTERFACE
            ${VITA_FFMPEG_DIR}/lib/libavformat.a
            ${VITA_FFMPEG_DIR}/lib/libavcodec.a
            ${VITA_FFMPEG_DIR}/lib/libswscale.a
            ${VITA_FFMPEG_DIR}/lib/libavutil.a
        )
    else()
        message(STATUS "PS Vita: no FFmpeg in ${VITA_FFMPEG_DIR}, movies are skipped (see tools/build-ffmpeg.sh)")
    endif()
endif()
target_link_libraries(vita_device INTERFACE
    vita_d3d8
    SceCtrl_stub
    SceTouch_stub
)

add_library(vita_main INTERFACE)
target_sources(vita_main INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Main/VitaMain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Main/VitaStubs.cpp
)
target_compile_definitions(vita_main INTERFACE
    VITA_NEWLIB_HEAP_MB=${VITA_NEWLIB_HEAP_MB}
)
# The device sources are part of the game's gameenginedevice library; linking the
# vita_device interface here would compile them into the executable a second time.
target_include_directories(vita_main INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Device)
target_link_libraries(vita_main INTERFACE
    ScePower_stub
    SceAppUtil_stub
    SceSysmodule_stub
    SceNet_stub
    SceNetCtl_stub
    SceIofilemgr_stub
    pthread
)

function(vita_package target title)
    # vita_create_self() looks for the ELF under the target's name in the current binary directory.
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${target}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )

    vita_create_self(${target}.self ${target} UNSAFE)
    # ATTRIBUTE2=12 requests the extended application memory budget.
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
    vita_create_vpk(${target}.vpk ${VITA_TITLE_ID} ${target}.self
        VERSION ${VITA_APP_VERSION}
        NAME "${title}"
        FILE ${CMAKE_SOURCE_DIR}/Platform/Vita/sce_sys/icon0.png sce_sys/icon0.png
        FILE ${CMAKE_SOURCE_DIR}/Platform/Vita/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        FILE ${CMAKE_SOURCE_DIR}/Platform/Vita/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE ${CMAKE_SOURCE_DIR}/Platform/Vita/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
        # Fonts for the FreeType text renderer (Liberation, metric compatible with the fonts
        # the game asks for). The entry point points the engine at app0:fonts.
        FILE ${CMAKE_SOURCE_DIR}/assets/fonts/arial.ttf fonts/arial.ttf
        FILE ${CMAKE_SOURCE_DIR}/assets/fonts/arialbold.ttf fonts/arialbold.ttf
        FILE ${CMAKE_SOURCE_DIR}/assets/fonts/couriernew.ttf fonts/couriernew.ttf
        FILE ${CMAKE_SOURCE_DIR}/assets/fonts/timesnewroman.ttf fonts/timesnewroman.ttf
        FILE ${CMAKE_SOURCE_DIR}/assets/fonts/LICENSE.liberation fonts/LICENSE.liberation
    )
endfunction()
