cmake_minimum_required(VERSION 3.20)
project(scct_coop_net VERSION 0.1.0 LANGUAGES C CXX)

# splintercell3.exe is 32-bit. Configure with: cmake -S coop-net -B coop-net/build -A Win32
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "scct-coop-net must be built as 32-bit (cmake -A Win32).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)

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
    ScctCoopNet.def
)
target_include_directories(ScctCoopNet PRIVATE ${MINHOOK_DIR}/include)
target_link_libraries(ScctCoopNet PRIVATE scct_minhook ws2_32 bcrypt winmm winhttp)
set_target_properties(ScctCoopNet PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ScctCoopNet"
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
    target_compile_options(scct-coop-launch PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-lan-relay PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-relay-test PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct-hook-path PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(scct_minhook PRIVATE /W3)
    # The host exe has no Control Flow Guard. Keep the loader stub's indirect calls plain.
    target_link_options(ScctCoopNet PRIVATE /GUARD:NO)
    target_link_options(scct-coop-launch PRIVATE /GUARD:NO)
endif()
