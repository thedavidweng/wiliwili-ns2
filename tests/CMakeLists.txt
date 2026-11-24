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
