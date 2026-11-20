# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Kid apps (offline launcher, clock, alarms, games) live outside the fork in
# the parent repo: components/kid_apps.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../components/kid_apps/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/kid_apps")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.6.0")
project(xiaozhi)
