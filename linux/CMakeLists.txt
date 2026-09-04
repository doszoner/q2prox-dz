cmake_minimum_required(VERSION 3.20)
project(q2prox_dz_linux LANGUAGES CXX)

option(Q2DZ_REPRODUCIBLE "Enable path-stable reproducible release flags" OFF)
option(Q2DZ_REQUIRE_WEBRTC_GATEWAY "Fail configure without the pinned WebRTC-NET runtime" OFF)

option(Q2DZ_ENABLE_SANITIZERS "Enable ASan/UBSan for tests" OFF)
option(Q2DZ_WERROR "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "q2prox-dz Linux build supports Linux only")
endif()


if(Q2DZ_REPRODUCIBLE)
  add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.")
  add_link_options("-Wl,--build-id=sha1")
endif()
add_library(q2dz_common STATIC
  src/common.cpp
  src/json.cpp
  src/protocol.cpp
  src/content.cpp
)
target_include_directories(q2dz_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(q2dz_common PRIVATE _GNU_SOURCE)

target_compile_options(q2dz_common PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  -Wformat=2 -Wshadow -Wnull-dereference -Wdouble-promotion
  -fstack-protector-strong -fno-plt -fno-common
)
if(Q2DZ_WERROR)
  target_compile_options(q2dz_common PRIVATE -Werror)
endif()

foreach(target q2dz-session q2dz-managed-gateway q2dz-content-check)
  add_executable(${target})
  target_link_libraries(${target} PRIVATE q2dz_common)
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    -Wformat=2 -Wshadow -Wnull-dereference -Wdouble-promotion
    -fstack-protector-strong -fno-plt -fno-common
  )
  if(Q2DZ_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  target_link_options(${target} PRIVATE
    -Wl,-z,relro,-z,now,-z,noexecstack
    -Wl,--as-needed
  )
endforeach()

target_sources(q2dz-session PRIVATE src/session_main.cpp)
target_sources(q2dz-managed-gateway PRIVATE src/managed_gateway_main.cpp)
target_sources(q2dz-content-check PRIVATE src/content_check_main.cpp)

set(Q2DZ_WEBRTC_NET_RELEASE_DIR "" CACHE PATH
    "Pinned WebRTC-NET release directory")
if(EXISTS "${Q2DZ_WEBRTC_NET_RELEASE_DIR}/humblenet_p2p.h" AND
   EXISTS "${Q2DZ_WEBRTC_NET_RELEASE_DIR}/libhumblenet.so" AND
   EXISTS "${Q2DZ_WEBRTC_NET_RELEASE_DIR}/libwebrtc.so")
  add_executable(q2dz-webrtc-gateway
    transport/q2_doszone_gateway.cpp
    transport/gateway_options.cpp
    transport/probe_options.cpp
    transport/multiroom_state.cpp)
  target_include_directories(q2dz-webrtc-gateway PRIVATE
    transport "${Q2DZ_WEBRTC_NET_RELEASE_DIR}")
  target_link_libraries(q2dz-webrtc-gateway PRIVATE
    q2dz_common "${Q2DZ_WEBRTC_NET_RELEASE_DIR}/libhumblenet.so" dl pthread)
  target_compile_options(q2dz-webrtc-gateway PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    -Wformat=2 -Wshadow -Wnull-dereference -Wdouble-promotion
    -fstack-protector-strong -fno-plt -fno-common)
  if(Q2DZ_WERROR)
    target_compile_options(q2dz-webrtc-gateway PRIVATE -Werror)
  endif()
  target_link_options(q2dz-webrtc-gateway PRIVATE
    -Wl,-z,relro,-z,now,-z,noexecstack -Wl,--as-needed)
  set_target_properties(q2dz-webrtc-gateway PROPERTIES
    BUILD_RPATH "${Q2DZ_WEBRTC_NET_RELEASE_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib")
  if(Q2DZ_ENABLE_SANITIZERS)
    target_compile_options(q2dz-webrtc-gateway PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(q2dz-webrtc-gateway PRIVATE
      -fsanitize=address,undefined)
  endif()
else()
  if(Q2DZ_REQUIRE_WEBRTC_GATEWAY)
    message(FATAL_ERROR "Pinned WebRTC-NET runtime is required for a product build: ${Q2DZ_WEBRTC_NET_RELEASE_DIR}")
  endif()
  message(STATUS "Pinned WebRTC-NET runtime not found; product transport target disabled")
endif()

if(Q2DZ_ENABLE_SANITIZERS)
  foreach(target q2dz_common q2dz-session q2dz-managed-gateway q2dz-content-check)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(q2dz-unit-tests tests/unit_tests.cpp
    transport/gateway_options.cpp transport/probe_options.cpp
    transport/multiroom_state.cpp)
  target_link_libraries(q2dz-unit-tests PRIVATE q2dz_common)
  target_include_directories(q2dz-unit-tests PRIVATE transport)
  target_compile_definitions(q2dz-unit-tests PRIVATE _GNU_SOURCE)
  target_compile_options(q2dz-unit-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -UNDEBUG)
  if(Q2DZ_WERROR)
    target_compile_options(q2dz-unit-tests PRIVATE -Werror)
  endif()
  if(Q2DZ_ENABLE_SANITIZERS)
    target_compile_options(q2dz-unit-tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(q2dz-unit-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME unit COMMAND q2dz-unit-tests)
  add_test(NAME fake-lifecycle
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_lifecycle.sh
            $<TARGET_FILE:q2dz-session>
            $<TARGET_FILE:q2dz-managed-gateway>)
  set_tests_properties(fake-lifecycle PROPERTIES TIMEOUT 90)
  add_test(NAME launcher
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_launcher.py -v)
  add_test(NAME live-snapshot-filter
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_live_snapshot.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/capture-live-snapshot.sh)
  set_tests_properties(live-snapshot-filter PROPERTIES TIMEOUT 20)
  if(TARGET q2dz-webrtc-gateway)
    add_test(NAME webrtc-gateway-contract
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify-legacy-contract.py
              --gateway $<TARGET_FILE:q2dz-webrtc-gateway>
              --adapter ${CMAKE_CURRENT_SOURCE_DIR}/config/legacy-gateway.json)
    add_test(NAME direct-gateway-hub
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_direct_gateway.py
              $<TARGET_FILE:q2dz-webrtc-gateway>)
    set_tests_properties(direct-gateway-hub PROPERTIES TIMEOUT 15)
    if(Q2DZ_ENABLE_SANITIZERS)
      set_tests_properties(direct-gateway-hub PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:strict_string_checks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
  endif()
endif()

set(Q2DZ_INSTALL_TARGETS q2dz-session q2dz-managed-gateway q2dz-content-check)
if(TARGET q2dz-webrtc-gateway)
  list(APPEND Q2DZ_INSTALL_TARGETS q2dz-webrtc-gateway)
  install(FILES "${Q2DZ_WEBRTC_NET_RELEASE_DIR}/libhumblenet.so"
          DESTINATION lib)
  install(FILES "${Q2DZ_WEBRTC_NET_RELEASE_DIR}/libwebrtc.so"
          DESTINATION lib/lib64)
endif()
install(TARGETS ${Q2DZ_INSTALL_TARGETS}
        RUNTIME DESTINATION bin)
install(PROGRAMS tools/q2dz-launch DESTINATION bin)
install(FILES config/legacy-gateway.json DESTINATION config)
install(FILES packaging/q2dz.desktop DESTINATION share/applications RENAME q2dz.desktop)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" DESTINATION .)
