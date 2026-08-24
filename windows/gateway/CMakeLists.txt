cmake_minimum_required(VERSION 3.20)
project(q2_doszone_webview2_gateway LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The WebView2 gateway builds only on Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(WEBVIEW2_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packages/webview2-sdk" CACHE PATH
    "Extracted Microsoft.Web.WebView2 NuGet package")
set(WEBRTC_NET_RELEASE_DIR "" CACHE PATH
    "Directory containing webrtcnet.mjs and webrtcnet.wasm")

foreach(required
        "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h"
        "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2Loader.dll"
        "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2Loader.dll.lib"
        "${WEBRTC_NET_RELEASE_DIR}/webrtcnet.mjs"
        "${WEBRTC_NET_RELEASE_DIR}/webrtcnet.wasm")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Required artifact is missing: ${required}")
    endif()
endforeach()

add_executable(q2-doszone-webview2-gateway
    src/main.cpp
    src/webview_bridge_protocol.cpp
    src/web_message_dispatch.hpp)
target_include_directories(q2-doszone-webview2-gateway PRIVATE
    "${WEBVIEW2_SDK_DIR}/build/native/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" Q2DZ_GATEWAY_SOURCE_ROOT)
target_compile_definitions(q2-doszone-webview2-gateway PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(q2-doszone-webview2-gateway PRIVATE
    /W4 /WX /permissive- /EHsc /sdl /GS /utf-8 /Brepro /experimental:deterministic
    "/pathmap:${Q2DZ_GATEWAY_SOURCE_ROOT}=C:/q2prox-dz/windows/gateway")
target_link_options(q2-doszone-webview2-gateway PRIVATE
    /Brepro /INCREMENTAL:NO /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA
    /NXCOMPAT /CETCOMPAT /guard:cf)
target_link_libraries(q2-doszone-webview2-gateway PRIVATE
    "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2Loader.dll.lib"
    ole32 shell32 user32 ws2_32)

add_custom_command(TARGET q2-doszone-webview2-gateway POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:q2-doszone-webview2-gateway>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2Loader.dll"
        "$<TARGET_FILE_DIR:q2-doszone-webview2-gateway>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/transport.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/transport_protocol.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/../coordinator/web/managed-readiness-probe.mjs"
        "${WEBRTC_NET_RELEASE_DIR}/webrtcnet.mjs"
        "${WEBRTC_NET_RELEASE_DIR}/webrtcnet.wasm"
        "$<TARGET_FILE_DIR:q2-doszone-webview2-gateway>/assets")

include(CTest)
if(BUILD_TESTING)
    add_executable(webview-bridge-protocol-test
        tests/webview_bridge_protocol_test.cpp
        src/webview_bridge_protocol.cpp
        src/web_message_dispatch.hpp)
    target_include_directories(webview-bridge-protocol-test PRIVATE src)
    target_compile_options(webview-bridge-protocol-test PRIVATE
        /W4 /WX /permissive- /EHsc /sdl /utf-8 /experimental:deterministic
        "/pathmap:${Q2DZ_GATEWAY_SOURCE_ROOT}=C:/q2prox-dz/windows/gateway")
    add_test(NAME webview-bridge-protocol COMMAND webview-bridge-protocol-test)
endif()
