cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(scct_coop_net VERSION 0.1.0 LANGUAGES C CXX)

option(SCCT_BUILD_P2P_ONLY "Build only scct_p2p (for 64-bit Python host)" OFF)

# splintercell3.exe is 32-bit. Configure with: cmake -S coop-net -B coop-net/build -A Win32
if(NOT SCCT_BUILD_P2P_ONLY AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "scct-coop-net must be built as 32-bit (cmake -A Win32).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
set(JUICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjuice)
set(OPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus)
set(RNNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise)
set(MINIAUDIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

if(NOT EXISTS "${JUICE_DIR}/CMakeLists.txt")
    message(STATUS "Fetching libjuice via FetchContent...")
    FetchContent_Declare(
        libjuice
        GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
        GIT_TAG v1.5.0
    )
    FetchContent_Populate(libjuice)
    set(JUICE_DIR ${libjuice_SOURCE_DIR})
endif()

# Build libjuice static library (MPL-2.0) for P2P ICE/STUN/TURN
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_SERVER ON CACHE BOOL "" FORCE)
add_subdirectory(${JUICE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libjuice EXCLUDE_FROM_ALL)

if(NOT EXISTS "${OPUS_DIR}/CMakeLists.txt")
    message(STATUS "Fetching opus via FetchContent...")
    FetchContent_Declare(
        opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG v1.5.2
    )
    FetchContent_Populate(opus)
    set(OPUS_DIR ${opus_SOURCE_DIR})
endif()

# Build Opus static library (BSD-3-Clause)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
add_subdirectory(${OPUS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/opus EXCLUDE_FROM_ALL)

if(NOT EXISTS "${RNNOISE_DIR}/src/denoise.c")
    message(STATUS "Fetching rnnoise via FetchContent...")
    FetchContent_Declare(
        rnnoise
        GIT_REPOSITORY https://github.com/xiph/rnnoise.git
        GIT_TAG 70f1d256acd4b34a572f999a05c87bf00b67730d
    )
    FetchContent_Populate(rnnoise)
    set(RNNOISE_DIR ${rnnoise_SOURCE_DIR})
endif()

if(NOT EXISTS "${RNNOISE_DIR}/src/rnnoise_data.c")
    message(STATUS "Downloading pre-trained rnnoise model...")
    file(DOWNLOAD
        "https://media.xiph.org/rnnoise/models/rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz"
        "${RNNOISE_DIR}/rnnoise_data.tar.gz"
        EXPECTED_HASH SHA256=0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37
    )
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${RNNOISE_DIR}/rnnoise_data.tar.gz"
        WORKING_DIRECTORY "${RNNOISE_DIR}"
    )
endif()

if(NOT EXISTS "${MINIAUDIO_DIR}/miniaudio.h")
    message(STATUS "Fetching miniaudio via FetchContent...")
    FetchContent_Declare(
        miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 877ea4a7c2601c7b0f2a44e83dfac42e6470fea6
    )
    FetchContent_Populate(miniaudio)
    set(MINIAUDIO_DIR ${miniaudio_SOURCE_DIR})
endif()

# Build RNNoise static library (BSD-3-Clause)
add_library(scct_rnnoise STATIC
    ${RNNOISE_DIR}/src/celt_lpc.c
    ${RNNOISE_DIR}/src/denoise.c
    ${RNNOISE_DIR}/src/kiss_fft.c
    ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c
    ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/rnn.c
    ${RNNOISE_DIR}/src/rnnoise_data.c
    ${RNNOISE_DIR}/src/rnnoise_tables.c
)
target_include_directories(scct_rnnoise PUBLIC ${RNNOISE_DIR}/include PRIVATE ${RNNOISE_DIR}/src)
target_compile_definitions(scct_rnnoise PRIVATE RNNOISE_BUILD)

add_library(scct_minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde32.c
)
target_include_directories(scct_minhook PUBLIC
    ${MINHOOK_DIR}/include
    ${MINHOOK_DIR}/src
)

add_library(ScctCoopNet SHARED
    src/dllmain.cpp
    src/scda_gameplay.cpp
    src/scda_gameplay.h
    src/scda_hooks.cpp
    src/scda_hooks.h
    src/transport.cpp
    src/voice_audio.cpp
    src/voice_audio.h
    ScctCoopNet.def
)
target_include_directories(ScctCoopNet PRIVATE
    ${MINHOOK_DIR}/include
    ${MINIAUDIO_DIR}
    ${OPUS_DIR}/include
    ${RNNOISE_DIR}/include
)
target_link_libraries(ScctCoopNet PRIVATE scct_minhook opus scct_rnnoise ws2_32 bcrypt winmm winhttp)
set_target_properties(ScctCoopNet PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ScctCoopNet"
    SUFFIX ".dll"
)

add_library(scct_p2p SHARED
    src/p2p_ice.cpp
)
target_include_directories(scct_p2p PRIVATE ${JUICE_DIR}/include)
target_link_libraries(scct_p2p PRIVATE LibJuice::LibJuiceStatic ws2_32 bcrypt)
set_target_properties(scct_p2p PROPERTIES
    PREFIX ""
    OUTPUT_NAME "scct_p2p"
    SUFFIX ".dll"
)

add_library(DDRAW SHARED
    src/ddraw_proxy.cpp
    DDRAW.def
)
set_target_properties(DDRAW PROPERTIES
    PREFIX ""
    OUTPUT_NAME "DDRAW"
    SUFFIX ".dll"
)

add_executable(scct-coop-launch src/launcher.cpp)
set_target_properties(scct-coop-launch PROPERTIES OUTPUT_NAME "scct-coop-launch")

add_executable(scct-lan-relay src/relay.cpp)
target_link_libraries(scct-lan-relay PRIVATE ws2_32)
set_target_properties(scct-lan-relay PROPERTIES OUTPUT_NAME "scct-lan-relay")

add_executable(scct-relay-test src/relay_test.cpp)
target_link_libraries(scct-relay-test PRIVATE ws2_32)
set_target_properties(scct-relay-test PROPERTIES OUTPUT_NAME "scct-relay-test")

add_executable(scct-hook-path src/hook_path.cpp)
target_link_libraries(scct-hook-path PRIVATE ws2_32)
set_target_properties(scct-hook-path PROPERTIES OUTPUT_NAME "scct-hook-path")
if(MSVC)
    target_link_options(scct-coop-launch PRIVATE /ENTRY:wmainCRTStartup)
endif()

add_custom_command(TARGET ScctCoopNet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/scct-coop-net.ini
        $<TARGET_FILE_DIR:ScctCoopNet>/scct-coop-net.ini
    COMMENT "Copy default scct-coop-net.ini next to ScctCoopNet.dll"
)

if(MSVC)
    target_compile_options(ScctCoopNet PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct_p2p PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-coop-launch PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-lan-relay PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-relay-test PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-hook-path PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct_minhook PRIVATE /W3)
    target_compile_options(scct_rnnoise PRIVATE /W0)
    # The host exe has no Control Flow Guard. Keep the loader stub's indirect calls plain.
    target_link_options(ScctCoopNet PRIVATE /GUARD:NO)
    target_link_options(scct_p2p PRIVATE /GUARD:NO)
    target_link_options(scct-coop-launch PRIVATE /GUARD:NO)
endif()
