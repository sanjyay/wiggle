cmake_minimum_required(VERSION 3.25)

project(wiggle-native VERSION 0.1.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland)

add_library(wiggle-native SHARED src/main.cpp)
set_target_properties(wiggle-native PROPERTIES
  PREFIX ""
  OUTPUT_NAME "wiggle-native"
)

target_compile_features(wiggle-native PRIVATE cxx_std_23)
target_compile_options(wiggle-native PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
)
target_link_libraries(wiggle-native PRIVATE PkgConfig::HYPRLAND)

