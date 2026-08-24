#include "common.hpp"
#include "content.hpp"
#include "gateway_options.h"
#include "json.hpp"
#include "protocol.hpp"

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

static void put_le32(unsigned char* p, std::uint32_t value) {
    p[0] = static_cast<unsigned char>(value & 0xffU);
    p[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    p[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    p[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

static void test_json() {
    const std::string text = R"({"alias":"q2.demo","netConfig":{"iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false},"map":"q2dm1","game":"baseq2"})";
    auto parsed = q2dz::json::parse(text);
    assert(parsed);
    auto room = q2dz::json::extract_room_config(parsed.value());
    assert(room);
    assert(room.value().alias == "q2.demo");
    assert(!room.value().ordered);
    assert(room.value().map == "q2dm1");
    assert(q2dz::json::stringify(room.value().ice_servers).find("stun:example.invalid") != std::string::npos);
    assert(!q2dz::json::parse("{\"a\":1,\"a\":2}"));
    assert(!q2dz::json::parse("[1,]"));
    auto ordered = q2dz::json::parse(R"({"alias":"q2.demo","iceServers":[{}],"ordered":true})");
    assert(ordered && !q2dz::json::extract_room_config(ordered.value()));
}

static void test_protocol() {
    auto event = q2dz::parse_gateway_event("Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:27910 peer=42\n");
    assert(event);
    auto endpoint = q2dz::ready_endpoint(event.value());
    assert(endpoint && endpoint.value().port == 27910);
    auto non_loopback = q2dz::parse_gateway_event("Q2DZ/1 TRANSPORT_READY udp=0.0.0.0:27910");
    assert(non_loopback);
    assert(!q2dz::ready_endpoint(non_loopback.value()));
    assert(!q2dz::parse_gateway_event("Q2DZ/1 HEARTBEAT credential=oops"));
    const auto ping = q2dz::quake_oob_ping();
    assert(ping.size() == 9);
    const std::array<unsigned char, 8> ack = {0xffU,0xffU,0xffU,0xffU,'a','c','k','\n'};
    assert(q2dz::is_quake_oob_ack(std::as_bytes(std::span(ack))));
    assert(q2dz::sanitize_log_line("credential=secret") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("local-description-after-gathering=") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("v=0") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("c=IN IP4 192.0.2.1") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("a=fingerprint:sha-256 00:11") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(gateway_error_exit_code("Connection to peer was closed") == 20);
    assert(gateway_error_exit_code("alias failed") == 2);
    assert(gateway_receive_exit_code(-1, 42) == 20);
    assert(gateway_receive_exit_code(-1, 0) == 2);
    assert(gateway_receive_exit_code(0, 42) == 0);

    char program[] = "gateway";
    char bind[] = "--bind";
    char bind_endpoint[] = "127.0.0.1:27910";
    char signal[] = "--signal";
    char signal_url[] = "wss://net.js-dos.com:444";
    char ice_file[] = "--ice-file";
    char ice_path[] = "/tmp/ice.json";
    char alias[] = "--alias";
    char alias_value[] = "q2.test";
    char maximum[] = "--max-datagram";
    char accepted[] = "1400";
    char rejected[] = "1401";
    char* accepted_argv[] = {program, bind, bind_endpoint, signal, signal_url,
                             ice_file, ice_path, alias, alias_value, maximum, accepted};
    assert(parse_gateway_options(11, accepted_argv).max_datagram == 1400);
    accepted_argv[10] = rejected;
    bool rejected_1401 = false;
    try { (void)parse_gateway_options(11, accepted_argv); }
    catch (const std::invalid_argument&) { rejected_1401 = true; }
    assert(rejected_1401);
}

static void test_content() {
    const fs::path root = fs::temp_directory_path() / ("q2dz-content-test-" + q2dz::random_hex(8));
    fs::create_directories(root / "baseq2");
    const fs::path pak = root / "baseq2" / "pak0.pak";
    std::array<unsigned char, 12> header{};
    std::memcpy(header.data(), "PACK", 4);
    constexpr std::uint32_t data_offset = 12;
    constexpr std::uint32_t data_size = 4;
    constexpr std::uint32_t directory_offset = data_offset + data_size;
    constexpr std::uint32_t directory_size = 128;
    put_le32(header.data() + 4, directory_offset);
    put_le32(header.data() + 8, directory_size);
    std::array<unsigned char, 64> entry{};
    const char* name = "maps/q2dm1.bsp";
    std::memcpy(entry.data(), name, std::strlen(name));
    put_le32(entry.data() + 56, data_offset);
    put_le32(entry.data() + 60, data_size);
    std::array<unsigned char, 64> unsafe_entry{};
    const char* unsafe_name = "models/tank/../ctank/skin.pcx";
    std::memcpy(unsafe_entry.data(), unsafe_name, std::strlen(unsafe_name));
    put_le32(unsafe_entry.data() + 56, data_offset);
    put_le32(unsafe_entry.data() + 60, data_size);
    std::ofstream out(pak, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    out.write("TEST", 4);
    out.write(reinterpret_cast<const char*>(unsafe_entry.data()), static_cast<std::streamsize>(unsafe_entry.size()));
    out.write(reinterpret_cast<const char*>(entry.data()), static_cast<std::streamsize>(entry.size()));
    out.close();
    auto found = q2dz::find_map_content(root, "baseq2", "q2dm1");
    assert(found && found.value().found);
    auto missing = q2dz::find_map_content(root, "baseq2", "missing");
    assert(missing && !missing.value().found);
    const fs::path linked_root = root / "linked-q2";
    fs::create_directories(linked_root / "baseq2");
    fs::create_symlink(pak, linked_root / "baseq2" / "pak0.pak");
    auto linked = q2dz::find_map_content(linked_root, "baseq2", "q2dm1");
    assert(linked && linked.value().found);
    assert(!q2dz::find_map_content(root, "../bad", "q2dm1"));
    fs::remove_all(root);
}

static void test_child_environment_scrubs_credential_names() {
    ::setenv("Q2DZ_SYNTHETIC_CREDENTIAL", "must-not-reach-child", 1);
    ::setenv("Q2DZ_UNLISTED_VALUE", "must-not-reach-child", 1);
    ::setenv("HTTPS_PROXY", "http://user:opaque-password@proxy.invalid", 1);
    ::setenv("DOCKER_AUTH_CONFIG", "opaque-auth-payload", 1);
    ::setenv("DISPLAY", ":99", 1);
    const auto environment = q2dz::current_environment_with({{"Q2DZ_SESSION_ID", "session"}});
    bool display_found = false;
    bool session_found = false;
    for (const auto& item : environment) {
        assert(item.find("Q2DZ_SYNTHETIC_CREDENTIAL=") != 0);
        assert(item.find("Q2DZ_UNLISTED_VALUE=") != 0);
        assert(item.find("HTTPS_PROXY=") != 0);
        assert(item.find("DOCKER_AUTH_CONFIG=") != 0);
        display_found = display_found || item == "DISPLAY=:99";
        session_found = session_found || item == "Q2DZ_SESSION_ID=session";
    }
    assert(display_found && session_found);
    ::unsetenv("Q2DZ_SYNTHETIC_CREDENTIAL");
    ::unsetenv("Q2DZ_UNLISTED_VALUE");
    ::unsetenv("HTTPS_PROXY");
    ::unsetenv("DOCKER_AUTH_CONFIG");
    ::unsetenv("DISPLAY");
}

int main() {
    test_json();
    test_protocol();
    test_content();
    test_child_environment_scrubs_credential_names();
    std::cout << "q2dz unit tests: PASS\n";
    return 0;
}
