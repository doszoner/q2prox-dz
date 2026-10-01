cmake_minimum_required(VERSION 3.24)
project(q2dz_session_coordinator LANGUAGES CXX)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(Q2DZ_BUILD_TESTS "Build unit and Windows lifecycle test binaries" ON)

add_library(q2dz_core STATIC
    src/child_protocol.cpp
    src/content_check.cpp
    src/session_safety.cpp
)
target_include_directories(q2dz_core PUBLIC include)
add_executable(q2dz-content-check src/content_check_main.cpp)
target_link_libraries(q2dz-content-check PRIVATE q2dz_core)
if(MSVC)
  target_compile_options(q2dz_core PRIVATE /W4 /WX /permissive- /sdl /GS /utf-8 /Brepro)
else()
  target_compile_options(q2dz_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(WIN32)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" Q2DZ_VERSION)
  string(STRIP "${Q2DZ_VERSION}" Q2DZ_VERSION)
  add_executable(q2dz-session
      src/windows/main.cpp
      src/windows/win32_support.cpp
  )
  target_include_directories(q2dz-session PRIVATE include src/windows)
  target_link_libraries(q2dz-session PRIVATE q2dz_core ole32 shell32 ws2_32)
  target_compile_definitions(q2dz-session PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
      Q2DZ_BUILD_VERSION="${Q2DZ_VERSION}")
  if(MSVC)
    target_compile_options(q2dz-session PRIVATE /W4 /WX /permissive- /sdl /GS /utf-8 /Brepro)
    target_link_options(q2dz-session PRIVATE /Brepro /INCREMENTAL:NO /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /guard:cf)
  endif()
endif()

if(Q2DZ_BUILD_TESTS)
  enable_testing()
  add_executable(q2dz-core-tests tests/core_tests.cpp)
  target_link_libraries(q2dz-core-tests PRIVATE q2dz_core)
  if(MSVC)
    target_compile_options(q2dz-core-tests PRIVATE /W4 /WX /permissive- /sdl /GS /utf-8 /Brepro)
    target_link_options(q2dz-core-tests PRIVATE /Brepro /INCREMENTAL:NO)
  else()
    target_compile_options(q2dz-core-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME q2dz-core-tests COMMAND q2dz-core-tests)
  add_executable(q2dz-content-check-tests tests/content_check_tests.cpp)
  target_link_libraries(q2dz-content-check-tests PRIVATE q2dz_core)
  add_test(NAME q2dz-content-check-tests COMMAND q2dz-content-check-tests)

  if(WIN32)
    add_executable(q2dz-fake-gateway tests/windows/fake_gateway.cpp)
    target_compile_definitions(q2dz-fake-gateway PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(q2dz-fake-gateway PRIVATE ws2_32)
    add_executable(q2dz-fake-q2 tests/windows/fake_q2.cpp)
    target_compile_definitions(q2dz-fake-q2 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(q2dz-fake-q2 PRIVATE ws2_32)
    foreach(t q2dz-fake-gateway q2dz-fake-q2)
      if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /permissive- /sdl /GS /utf-8 /Brepro)
        target_link_options(${t} PRIVATE /Brepro /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT)
      endif()
    endforeach()
  endif()
endif()

if(MSVC)
  foreach(t q2dz-content-check q2dz-content-check-tests)
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE /W4 /WX /permissive- /sdl /GS /utf-8 /Brepro)
      target_link_options(${t} PRIVATE /Brepro /INCREMENTAL:NO /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /guard:cf)
    endif()
  endforeach()
endif()
