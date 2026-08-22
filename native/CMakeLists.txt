cmake_minimum_required(VERSION 3.25)

project(wiggle-native VERSION 0.1.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland)

add_library(wiggle-native SHARED
  src/CursorTracker.cpp
  src/ShakeDetector.cpp
  src/main.cpp
)
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

include(CTest)
if(BUILD_TESTING)
  add_executable(cursor-tracker-test
    tests/CursorTrackerTest.cpp
    src/CursorTracker.cpp
  )
  target_compile_features(cursor-tracker-test PRIVATE cxx_std_23)
  target_compile_options(cursor-tracker-test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
  )
  target_include_directories(cursor-tracker-test PRIVATE src)
  target_link_libraries(cursor-tracker-test PRIVATE PkgConfig::HYPRLAND)
  add_test(NAME cursor-tracker-bounded-history COMMAND cursor-tracker-test)

  add_executable(shake-detector-test
    tests/ShakeDetectorTest.cpp
    src/ShakeDetector.cpp
  )
  target_compile_features(shake-detector-test PRIVATE cxx_std_23)
  target_compile_options(shake-detector-test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
  )
  target_include_directories(shake-detector-test PRIVATE src)
  target_link_libraries(shake-detector-test PRIVATE PkgConfig::HYPRLAND)
  add_test(NAME shake-detector-behavior COMMAND shake-detector-test)
endif()
