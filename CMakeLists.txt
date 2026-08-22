cmake_minimum_required(VERSION 3.15)
project(SmoothTalkerSAPI VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Which unicorn.dll goes with this build.  Both architectures are checked in
# under bin/unicorn/; unicorn is loaded at run time by absolute path rather
# than through the import table (see src/engine/unicorn_dyn.cpp), so this is
# only about which file gets copied and packaged.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ST_ARCH "x64")
else()
    set(ST_ARCH "x86")
endif()
message(STATUS "SmoothTalker: building ${ST_ARCH}")

set(ST_UNICORN_DLL "${CMAKE_CURRENT_SOURCE_DIR}/bin/unicorn/${ST_ARCH}/unicorn.dll")
set(ST_ENGINE_BIN  "${CMAKE_CURRENT_SOURCE_DIR}/bin/_smoothtalker_engine/engine.bin")

foreach(required "${ST_UNICORN_DLL}" "${ST_ENGINE_BIN}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "missing required file: ${required}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Shared code: paths, logging, settings, and the SmoothTalker ROM itself.
# ---------------------------------------------------------------------------
add_library(st_core STATIC
    src/common/st_paths.cpp
    src/common/st_log.cpp
    src/common/st_settings.cpp
    src/engine/unicorn_dyn.cpp
    src/engine/st_engine.cpp
)

target_include_directories(st_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicorn/include
)

target_link_libraries(st_core PUBLIC shell32 advapi32)

target_compile_definitions(st_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# ---------------------------------------------------------------------------
# The SAPI5 engine itself.  Same sources for x86 and x64: the ROM runs under
# Unicorn, which is pure computation, so there is no 32-bit-only dependency
# and therefore no need for the out-of-process bridge the BSTSpeech wrapper
# had to use.
# ---------------------------------------------------------------------------
add_library(SmoothTalkerSAPI SHARED
    src/sapi/sapi_main.cpp
    src/sapi/com.cpp
    src/sapi/registry.cpp
    src/sapi/ISpDataKeyImpl.cpp
    src/sapi/IEnumSpObjectTokensImpl.cpp
    src/sapi/ISpTTSEngineImpl.cpp
    src/sapi/voice_token.cpp
    src/sapi/smoothtalker_sapi.def
)

target_link_libraries(SmoothTalkerSAPI PRIVATE
    st_core
    ole32
    oleaut32
    advapi32
    shell32
)

# ---------------------------------------------------------------------------
# The configuration utility.
# ---------------------------------------------------------------------------
add_executable(SmoothTalkerConfig WIN32
    src/config/config_main.cpp
    src/config/config.rc
)

target_link_libraries(SmoothTalkerConfig PRIVATE
    st_core
    comctl32
    winmm
    shell32
)

# ---------------------------------------------------------------------------
# Command-line renderer.  Ships with the product because it is the fastest way
# to tell whether a problem is in the ROM or in SAPI, and it is what
# tools/compare_engines.py drives to check the C++ engine against the Python
# reference.
# ---------------------------------------------------------------------------
add_executable(st_render src/tools/st_render.cpp)
target_link_libraries(st_render PRIVATE st_core)

# End-to-end check of the SAPI5 surface.  Not installed -- it is a developer
# tool, and its --spvoice mode writes (and removes again) HKCU registry keys.
add_executable(st_sapi_test src/tools/st_sapi_test.cpp)
target_link_libraries(st_sapi_test PRIVATE st_core ole32 oleaut32 sapi)

# Reports the MSAA view of the configuration dialog -- what a screen reader
# actually sees.  Developer tool; not installed.
add_executable(st_a11y_dump src/tools/st_a11y_dump.cpp)
target_link_libraries(st_a11y_dump PRIVATE oleacc ole32 oleaut32 user32)

if(MSVC)
    foreach(target st_core SmoothTalkerSAPI SmoothTalkerConfig st_render st_sapi_test
                   st_a11y_dump)
        target_compile_options(${target} PRIVATE /W3 /MP)
        # Static CRT: a SAPI5 engine is loaded into whatever process wants to
        # speak, and requiring a particular redistributable to be present in
        # that process is a failure mode with no good diagnostic.
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

# Put unicorn.dll and the ROM image beside the binaries so the build tree is
# runnable without installing anything.
foreach(payload "${ST_UNICORN_DLL}" "${ST_ENGINE_BIN}")
    add_custom_command(TARGET SmoothTalkerSAPI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${payload}" "$<TARGET_FILE_DIR:SmoothTalkerSAPI>/"
        COMMENT "Staging ${payload}"
    )
endforeach()

install(TARGETS SmoothTalkerSAPI SmoothTalkerConfig st_render
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)
install(FILES "${ST_UNICORN_DLL}" "${ST_ENGINE_BIN}" DESTINATION .)
