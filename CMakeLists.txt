#
# This file is part of the AzerothCore Project. See AUTHORS file for
# Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#

set(MOD_ANTICHEAT_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR})

# AzerothCore discovers src/ recursively. A direct configure builds only the
# portable engine and tests, without downloading the core or dependencies.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(mod_anticheat_tests LANGUAGES CXX)
  enable_testing()
  find_package(Threads REQUIRED)
  option(ANTICHEAT_ASAN "AddressSanitizer" OFF)
  option(ANTICHEAT_UBSAN "UndefinedBehaviorSanitizer" OFF)
  option(ANTICHEAT_TSAN "ThreadSanitizer (separate build)" OFF)
  if(ANTICHEAT_TSAN AND (ANTICHEAT_ASAN OR ANTICHEAT_UBSAN))
    message(FATAL_ERROR "Use a separate build directory for ThreadSanitizer")
  endif()

  add_library(anticheat_engine STATIC
    src/core/AnticheatConfig.cpp
    src/detectors/MovementDetectors.cpp
    src/scoring/ViolationScorer.cpp
    src/enforcement/EnforcementManager.cpp
    src/persistence/ReportQueue.cpp
    src/persistence/ReportSql.cpp)
  target_include_directories(anticheat_engine PUBLIC src)
  target_compile_features(anticheat_engine PUBLIC cxx_std_17)
  if(MSVC)
    target_compile_options(anticheat_engine PUBLIC /W4 /WX /permissive-)
    if(ANTICHEAT_ASAN)
      target_compile_options(anticheat_engine PUBLIC /fsanitize=address)
    endif()
    if(ANTICHEAT_UBSAN OR ANTICHEAT_TSAN)
      message(FATAL_ERROR "UBSan/TSan require GCC or Clang on Linux")
    endif()
  else()
    target_compile_options(anticheat_engine PUBLIC -Wall -Wextra -Wpedantic -Werror)
    foreach(sanitizer ASAN UBSAN TSAN)
      if(ANTICHEAT_${sanitizer})
        if(sanitizer STREQUAL "ASAN")
          set(flag address)
        elseif(sanitizer STREQUAL "UBSAN")
          set(flag undefined)
        else()
          set(flag thread)
        endif()
        target_compile_options(anticheat_engine PUBLIC -fsanitize=${flag} -fno-omit-frame-pointer)
        target_link_options(anticheat_engine PUBLIC -fsanitize=${flag})
      endif()
    endforeach()
  endif()

  add_executable(anticheat_tests tests/TestMain.cpp tests/MovementTests.cpp tests/PolicyTests.cpp
    tests/RepositoryTests.cpp tests/ConfigTests.cpp tests/SequenceTests.cpp)
  target_link_libraries(anticheat_tests PRIVATE anticheat_engine Threads::Threads)
  target_compile_definitions(anticheat_tests PRIVATE ANTICHEAT_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/conf/mod_anticheat.conf.dist")
  # Exercise the same C++20 atomic shared_ptr publication used by the core adapter.
  target_compile_features(anticheat_tests PRIVATE cxx_std_20)
  foreach(suite movement policy repository config sequences)
    add_test(NAME anticheat_${suite} COMMAND anticheat_tests ${suite})
  endforeach()
  add_executable(anticheat_benchmark tests/Benchmark.cpp)
  target_link_libraries(anticheat_benchmark PRIVATE anticheat_engine)
endif()
