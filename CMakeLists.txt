# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# This multi-board application can exceed Windows' process command-line limit.
# Pass compiler/linker arguments through response files when using Ninja there.
if(CMAKE_HOST_WIN32)
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use Ninja response files" FORCE)
endif()

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.2")
project(xiaozhi)
