# Host checks. They read the repository and the pinned configuration, so they
# only make sense on the build host, never in a cross build.
if (CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "WILIWILI_BUILD_TESTS checks the pinned build configuration; "
            "configure a desktop build to run it")
endif ()

add_executable(wiliwili_config_test config_test.cpp)
set_target_properties(wiliwili_config_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
target_include_directories(wiliwili_config_test PRIVATE ${BOREALIS_LIBRARY}/include)
target_compile_definitions(wiliwili_config_test PRIVATE
        WILIWILI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME config_pins COMMAND wiliwili_config_test)

add_executable(wiliwili_boot_test
        boot_diagnostics_test.cpp
        ${PROJECT_SOURCE_DIR}/wiliwili/source/utils/boot_diagnostics.cpp)
set_target_properties(wiliwili_boot_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
target_include_directories(wiliwili_boot_test PRIVATE ${PROJECT_SOURCE_DIR}/wiliwili/include)
target_compile_definitions(wiliwili_boot_test PRIVATE
        WILIWILI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME boot_diagnostics COMMAND wiliwili_boot_test)

# The Switch 2 input, IME and font decisions (#4). The pure headers have no
# <switch.h>, so the host can compile and run them; everything that talks to the
# hardware is verified on a console through G1.
add_executable(wiliwili_switch2_input_test
        switch2_input_test.cpp
        ${PROJECT_SOURCE_DIR}/wiliwili/source/utils/platform_api.cpp)
set_target_properties(wiliwili_switch2_input_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
# switch2_cjk.hpp includes utils/platform_api.h for the sensitive-input flag,
# so this target needs both include roots and the source root for read_source.
target_include_directories(wiliwili_switch2_input_test PRIVATE
        ${PROJECT_SOURCE_DIR}/platform/switch2
        ${PROJECT_SOURCE_DIR}/wiliwili/include)
target_compile_definitions(wiliwili_switch2_input_test PRIVATE
        WILIWILI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME switch2_input COMMAND wiliwili_switch2_input_test)

# Audits a Switch 2 build tree (NRO header, RomFS contents, linker map). The
# script skips itself when the cross toolchain or the artifacts are absent, so
# this stays green on a desktop-only checkout.
add_test(NAME switch2_artifact_audit
        COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/switch2/audit-nro.sh
                ${PROJECT_SOURCE_DIR}/build/switch2-release)
set_tests_properties(switch2_artifact_audit PROPERTIES SKIP_RETURN_CODE 77)

# Console lifecycle: display and framebuffer policy, refresh decoding,
# presentation pacing, the pre-playback probe and the teardown order (#5). The
# pure logic lives in headers, so this target only needs the platform include
# directory and no console toolchain.
add_executable(wiliwili_switch2_lifecycle_test
        switch2_lifecycle_test.cpp
        ${PROJECT_SOURCE_DIR}/wiliwili/source/utils/boot_diagnostics.cpp)
set_target_properties(wiliwili_switch2_lifecycle_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
target_include_directories(wiliwili_switch2_lifecycle_test PRIVATE
        ${PROJECT_SOURCE_DIR}/platform/switch2
        ${PROJECT_SOURCE_DIR}/wiliwili/include)
target_compile_definitions(wiliwili_switch2_lifecycle_test PRIVATE
        WILIWILI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME switch2_lifecycle COMMAND wiliwili_switch2_lifecycle_test)
