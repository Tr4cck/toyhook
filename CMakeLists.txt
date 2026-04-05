cmake_minimum_required(VERSION 3.10)
project(toyhook C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- arch: ARM64 low-level primitives ---
add_library(toyhook_arch_arm64 STATIC
    arch/arm64/asm.c
    arch/arm64/emit.c
    arch/arm64/dispatch.c
)
target_include_directories(toyhook_arch_arm64 PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(toyhook_arch_arm64 log)

# --- utils ---
add_library(toyhook_utils STATIC
    utils/elf.c
    utils/mem.c
)
target_include_directories(toyhook_utils PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(toyhook_utils PRIVATE -g -O0)
target_link_libraries(toyhook_utils log)

# --- PLT/GOT hook ---
add_library(toyhook_plt_hook STATIC backend/plt_hook.c)
target_include_directories(toyhook_plt_hook PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(toyhook_plt_hook PRIVATE -g -O0)
target_link_libraries(toyhook_plt_hook log)

# --- inline hook ---
add_library(toyhook_inline_hook STATIC backend/inline_hook.c)
target_include_directories(toyhook_inline_hook PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(toyhook_inline_hook PRIVATE -g -O0)
target_link_libraries(toyhook_inline_hook toyhook_arch_arm64 log)

# --- unified API ---
add_library(toyhook_api STATIC core/toyhook.c)
target_include_directories(toyhook_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(toyhook_api PRIVATE -g -O0)
target_link_libraries(toyhook_api toyhook_inline_hook toyhook_plt_hook toyhook_arch_arm64 toyhook_utils log dl)

# --- injector ---
add_executable(toyhook injector/injector.c)
target_include_directories(toyhook PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(toyhook PRIVATE -g -O0)
target_link_libraries(toyhook toyhook_utils dl log)

# --- payload ---
add_library(toyhook_payload SHARED payload/payload.c)
target_include_directories(toyhook_payload PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(toyhook_payload PRIVATE -g -O0)
target_link_libraries(toyhook_payload toyhook_api toyhook_arch_arm64 log dl)

# --- host unit tests (build with: cmake -DBUILD_TESTS=ON ..) ---
option(BUILD_TESTS "Build host unit tests" OFF)
if(BUILD_TESTS)
    add_executable(test_inline_hook
        tests/test_inline_hook.c
        arch/arm64/asm.c
        arch/arm64/emit.c
        utils/mem.c
    )
    target_include_directories(test_inline_hook PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_options(test_inline_hook PRIVATE -g -O0)

    add_executable(test_toyhook
        tests/test_toyhook.c
    )
    target_include_directories(test_toyhook PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_options(test_toyhook PRIVATE -g -O0)
    target_link_libraries(test_toyhook dl)

    add_executable(test_plt_hook
        tests/test_plt_hook.c
    )
    target_include_directories(test_plt_hook PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_options(test_plt_hook PRIVATE -g -O0)
endif()
