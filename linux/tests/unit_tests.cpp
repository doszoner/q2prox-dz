#include "common.hpp"
#include "content.hpp"
#include "gateway_options.h"
#include "multiroom_state.hpp"
#include "json.hpp"
#include "protocol.hpp"

#include <array>
#include <arpa/inet.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

struct HubFixtureCase {
    std::string name;
    std::string parser;
    bool valid = false;
    std::string line;
    std::size_t repeat = 0;
    char fill = 'x';
};

static std::string fixture_string(std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const auto key_pos = object.find(needle);
    assert(key_pos != std::string_view::npos);
    auto pos = key_pos + needle.size();
    assert(pos < object.size() && object[pos] == '"');
    std::string out;
    for (++pos; pos < object.size(); ++pos) {
        const char c = object[pos];
        if (c == '"') return out;
        if (c != '\\') { out.push_back(c); continue; }
        assert(++pos < object.size());
        const char escaped = object[pos];
        if (escaped == 'n') out.push_back('\n');
        else if (escaped == 'r') out.push_back('\r');
        else if (escaped == 't') out.push_back('\t');
        else if (escaped == 'u') {
            assert(pos + 4 < object.size());
            unsigned value = 0;
            for (std::size_t i = 1; i <= 4; ++i) {
                const char hex = object[pos + i];
                value = value * 16U + static_cast<unsigned>(hex >= '0' && hex <= '9' ? hex - '0' :
                    hex >= 'a' && hex <= 'f' ? hex - 'a' + 10 : hex - 'A' + 10);
            }
            out.push_back(static_cast<char>(value));
            pos += 4;
        } else {
            out.push_back(escaped);
        }
    }
    assert(false);
    return {};
}

static std::vector<HubFixtureCase> load_hub_fixture() {
    const auto source = fs::path(__FILE__).parent_path();
    const auto path = source / "../../tests/contracts/hub-protocol-cases.json";
    std::ifstream input(path, std::ios::binary);
    assert(input);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    std::vector<HubFixtureCase> cases;
    std::istringstream lines(text);
    std::string object;
    while (std::getline(lines, object)) {
        if (object.find("{\"name\":") == std::string::npos) continue;
        HubFixtureCase item;
        item.name = fixture_string(object, "name");
        item.parser = fixture_string(object, "parser");
        item.valid = object.find("\"valid\":true") != std::string_view::npos;
        if (object.find("\"line\":") != std::string_view::npos) item.line = fixture_string(object, "line");
        if (object.find("\"repeat\":") != std::string_view::npos) {
            const auto key = object.find("\"repeat\":") + 9;
            const auto end = object.find_first_not_of("0123456789", key);
            item.repeat = static_cast<std::size_t>(std::stoull(std::string(object.substr(key, end - key))));
        }
        if (object.find("\"fill\":") != std::string_view::npos) item.fill = fixture_string(object, "fill").at(0);
        cases.push_back(std::move(item));
    }
    assert(cases.size() == 48);
    return cases;
}

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
    auto dedicated = q2dz::json::parse(R"({"alias":"room.q2ded.example.server","iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false,"map":"q2dm1","game":"3zb2"})");
    assert(dedicated);
    auto dedicated_room = q2dz::json::extract_room_config(dedicated.value());
    assert(dedicated_room);
    assert(dedicated_room.value().alias == "room.q2ded.example.server");
    auto empty_dedicated = q2dz::json::parse(R"({"alias":"room.q2ded.","iceServers":[{}]})");
    assert(empty_dedicated && !q2dz::json::extract_room_config(empty_dedicated.value()));
    auto empty_q2 = q2dz::json::parse(R"({"alias":"q2.","iceServers":[{}]})");
    assert(empty_q2 && !q2dz::json::extract_room_config(empty_q2.value()));
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
    const std::map<std::string, std::string, std::less<>> ready_fields = {
        {"id", "42"}, {"peer", "17"}, {"udp", "127.0.0.1:27910"}
    };
    const auto hub_ready = q2dz::format_hub_event(q2dz::GatewayEventType::TransportReady, ready_fields);
    assert(hub_ready == "Q2DZ/HUB/2 READY id=42 udp=127.0.0.1:27910 peer=17");
    assert(q2dz::parse_gateway_event(hub_ready));
    assert(q2dz::format_hub_event(q2dz::GatewayEventType::TransportReady,
                                  {{"id", "42"}, {"udp", "127.0.0.1:27910"}, {"credential", "secret"}}).empty());
    assert(q2dz::format_hub_event(q2dz::GatewayEventType::TransportReady,
                                  {{"aLiAs", "room.q2ded.server"}, {"id", "42"}, {"peer", "17"},
                                   {"udp", "127.0.0.1:27910"}}).empty());
    assert(q2dz::format_gateway_event(q2dz::GatewayEventType::Heartbeat, {{"token", "secret"}}).empty());
    const auto ping = q2dz::quake_oob_ping();
    assert(ping.size() == 9);
    const std::array<unsigned char, 8> ack = {0xffU,0xffU,0xffU,0xffU,'a','c','k','\n'};
    assert(q2dz::is_quake_oob_ack(std::as_bytes(std::span(ack))));
    assert(q2dz::sanitize_log_line("credential=secret") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("local-description-after-gathering=") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("v=0") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("c=IN IP4 192.0.2.1") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("a=fingerprint:sha-256 00:11") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("My peer id is abc123") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("GATEWAY_READY alias=room.q2ded.example") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("Alias lookup for \"room.q2ded.example\" timed out") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("AliasError: unable to resolve \"room.q2ded.example\"") == "[q2dz] sensitive transport diagnostic suppressed");
    assert(q2dz::sanitize_log_line("alias failed") == "alias failed");
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

static void test_shared_hub_fixture_red() {
    auto cases = load_hub_fixture();
    for (const auto& item : cases) {
        if (item.parser == "event") {
            const auto parsed = q2dz::parse_gateway_event(item.line);
            assert(static_cast<bool>(parsed) == item.valid);
            if (parsed) {
                assert(q2dz::format_hub_event(parsed.value().type, parsed.value().fields) == item.line);
                if (item.name == "failed") assert(parsed.value().type == q2dz::GatewayEventType::TargetFailed);
            }
        } else if (item.parser == "event_mix") {
            q2dz::GatewayEventParser parser;
            assert(parser.parse("Q2DZ/HUB/2 HEARTBEAT state=ok targets=1 ready=1"));
            assert(!parser.parse(item.line));
        }
    }
}

static void test_shared_hub_fixture() {
    const auto cases = load_hub_fixture();
    q2dz::HubCommandParser rooms(q2dz::HubCommandChannel::rooms);
    q2dz::HubCommandParser hub(q2dz::HubCommandChannel::hub);
    for (const auto& item : cases) {
        if (item.parser == "rooms_command") {
            const auto parsed = rooms.parse(item.line);
            assert(static_cast<bool>(parsed) == item.valid);
            if (parsed) assert(q2dz::format_rooms_command(parsed.value()) == item.line);
        } else if (item.parser == "hub_command") {
            const auto parsed = hub.parse(item.line);
            assert(static_cast<bool>(parsed) == item.valid);
            if (parsed) assert(q2dz::format_hub_command(parsed.value()) == item.line);
        } else if (item.parser == "hub_sequence") {
            q2dz::HubCommandParser fresh(q2dz::HubCommandChannel::hub);
            assert(static_cast<bool>(fresh.parse(item.line)) == item.valid);
            assert(!q2dz::parse_hub_command(item.line));
        } else if (item.parser == "line") {
            q2dz::HubCommandFramer framer;
            std::string input = item.line + std::string(item.repeat, item.fill);
            assert(!framer.feed(input));
            assert(framer.error() == q2dz::HubCommandFramer::Error::line_too_long);
        } else if (item.parser == "queue") {
            q2dz::HubCommandFramer framer;
            const auto input = item.line + '\n';
            std::size_t accepted = 0;
            for (std::size_t i = 0; i < item.repeat; ++i) {
                if (!framer.feed(input)) break;
                ++accepted;
            }
            assert(!framer.feed(""));
            assert(framer.error() == q2dz::HubCommandFramer::Error::queue_overflow);
            assert(accepted == q2dz::HubCommandFramer::max_queued_lines);
            assert(framer.queued_line_count() == 0);
        } else if (item.parser == "formatter_invalid_utf8") {
            q2dz::HubCommand command;
            command.type = q2dz::HubCommandType::put;
            command.id_or_generation = 42;
            command.json = {'{', '"', 'x', '"', ':', '"', 0xffU, '"', '}'};
            assert(q2dz::format_hub_command(command).empty());
        }
    }
}

static void test_hub_framer_boundary() {
    const std::string head = "Q2DZ/HUB/2 ICE generation=7 json=[\"";
    const std::string tail = "\"]";
    const std::string line = head + std::string(q2dz::HubCommandFramer::max_line_bytes - head.size() - tail.size(), 'x') + tail;
    assert(line.size() == q2dz::HubCommandFramer::max_line_bytes);
    q2dz::HubCommandFramer framer;
    assert(framer.feed(line + '\n'));
    assert(framer.finish());
    const auto lines = framer.drain();
    assert(lines.size() == 1 && lines.front().view() == line);
    assert(q2dz::parse_hub_command(lines.front().view()));
    auto command = q2dz::parse_hub_command(lines.front().view());
    command.value().clear();
    assert(command.value().json.empty());

    q2dz::HubCommandFramer oversized;
    assert(!oversized.feed(line + "x"));
    assert(oversized.error() == q2dz::HubCommandFramer::Error::line_too_long);

    q2dz::HubCommandFramer cleared;
    assert(cleared.feed("Q2DZ/HUB/2 STOP\npartial"));
    cleared.clear();
    assert(cleared.queued_line_count() == 0 && cleared.pending_byte_count() == 0);
}

static void test_hub_command_move_wipes_source() {
    q2dz::HubCommand short_source;
    short_source.room_id = "short-room";
    short_source.json = {'s', 'e', 'c'};
    q2dz::HubCommand short_destination(std::move(short_source));
    assert(short_source.room_id.empty() && short_source.json.empty());
    assert(short_destination.room_id == "short-room" && short_destination.json.size() == 3);

    q2dz::HubCommand heap_source;
    heap_source.room_id.assign(300, 'r');
    q2dz::HubCommand heap_destination;
    heap_destination = std::move(heap_source);
    assert(heap_source.room_id.empty());
    assert(heap_destination.room_id.size() == 300);
}

static void test_short_payload_formatter() {
    q2dz::HubCommand command;
    command.type = q2dz::HubCommandType::put;
    command.id_or_generation = 42;
    command.json = {'{', '"', 'x', '"', ':', '1', '}'};
    assert(q2dz::format_hub_command(command) == "Q2DZ/HUB/2 PUT id=42 json={\"x\":1}");
}

static void test_hub_queue_capacity() {
    q2dz::HubCommandFramer framer;
    assert(framer.feed("Q2DZ/HUB/2 ICE generation=7 json=[]\n"));
    for (std::uint64_t id = 1; id <= 256; ++id) {
        assert(framer.feed("Q2DZ/HUB/2 PUT id=" + std::to_string(id) + " json={}\n"));
    }
    for (std::uint64_t id = 1; id <= 256; ++id) {
        assert(framer.feed("Q2DZ/HUB/2 DROP id=" + std::to_string(id) + "\n"));
    }
    assert(framer.queued_line_count() == 513);
    assert(!framer.feed("Q2DZ/HUB/2 STOP\n"));
    assert(framer.error() == q2dz::HubCommandFramer::Error::queue_overflow);
    assert(framer.queued_line_count() == 0);

    q2dz::HubCommandFramer bounded;
    for (std::uint64_t id = 1; id <= 40; ++id) {
        assert(bounded.feed("Q2DZ/HUB/2 DROP id=" + std::to_string(id) + "\n"));
    }
    const auto first_batch = bounded.drain(7);
    assert(first_batch.size() == 7);
    assert(bounded.queued_line_count() == 33);
    assert(bounded.drain(33).size() == 33);
    assert(bounded.queued_line_count() == 0);
}

static void test_probe_ack_routing() {
    assert(!q2dz::probe_ack_is_outstanding(false, 0));
    assert(!q2dz::probe_ack_is_outstanding(false, 17));
    assert(!q2dz::probe_ack_is_outstanding(true, 0));
    assert(q2dz::probe_ack_is_outstanding(true, 17));
}

static void test_multiroom_state_virtual_routing() {
    q2dz::MultiroomState state;
    assert(state.put(11, 27911));
    assert(state.put(12, 27912));
    assert(state.put(13, 27913));
    assert(!state.put(14, 27912));
    assert(state.size() == 3);
    assert(state.find(11) != nullptr);
    assert(state.find(12) != nullptr);
    assert(state.find(11)->udp_port != state.find(12)->udp_port);

    constexpr q2dz::RemotePeerId route_a = 0x80000001U;
    constexpr q2dz::RemotePeerId route_b = 0x80000002U;
    assert(state.bind_route_peer(11, route_a));
    assert(!state.bind_route_peer(12, route_b));
    assert(state.find(11)->route_peer == route_a);
    assert(state.find(11)->concrete_peer == 0);
    assert(state.find(12)->route_peer == 0);
    assert(state.route_inbound(route_a, 64).value() == 11);
    assert(state.route_outbound(
               11, q2dz::UdpSource{htonl(INADDR_LOOPBACK), htons(31011)}, 64, 1400)
               .value() == route_a);

    assert(state.reserve_concrete_peer(11, 101));
    assert(state.bind_route_peer(12, route_b));
    assert(state.reserve_concrete_peer(12, 102));
    assert(!state.bind_route_peer(13, 103));
    assert(state.find(13)->route_peer == 0);
    assert(!state.bind_route_peer(13, route_a));
    assert(state.bind_route_peer(13, 0x80000003U));
    assert(!state.reserve_concrete_peer(13, 101));
    assert(state.find(13)->failure_code == "duplicate_remote_peer");
    assert(state.find(11)->concrete_peer == 101);
    assert(state.target_for_peer(route_a).value() == 11);
    assert(state.target_for_peer(route_b).value() == 12);

    const q2dz::UdpSource first{htonl(INADDR_LOOPBACK), htons(31011)};
    const q2dz::UdpSource second{htonl(INADDR_LOOPBACK), htons(31012)};
    assert(state.capture_source(11, first));
    assert(state.capture_source(12, second));
    assert(state.source_allowed(11, first));
    assert(state.source_allowed(12, second));
    assert(!state.source_allowed(11, second));
    assert(!state.source_allowed(12, first));
    assert(state.find(11)->counters.second_udp_source_drops == 1);
    assert(state.find(12)->counters.second_udp_source_drops == 1);

    assert(state.route_inbound(route_a, 64).value() == 11);
    assert(state.route_inbound(999, 64) == std::nullopt);
    assert(state.counters().unknown_peer_drops == 1);
    state.record_udp_to_webrtc(11, 64);
    state.record_webrtc_to_udp(11, 48);
    state.record_udp_to_webrtc(11, 64);
    assert(state.find(11)->counters.udp_to_webrtc == 3);
    assert(state.find(11)->counters.webrtc_to_udp == 1);
    assert(state.find(11)->counters.max_udp_to_transport_datagram == 64);
    assert(state.find(11)->counters.max_transport_to_udp_datagram == 48);

    assert(state.drop(11));
    assert(state.find(11) == nullptr);
    assert(state.find(12) != nullptr);
    assert(state.find(13) != nullptr);
    assert(state.target_for_peer(route_a) == std::nullopt);
    assert(state.route_inbound(route_a, 64) == std::nullopt);
    assert(state.find(12)->counters.udp_to_webrtc == 0);
    assert(!state.drop(11));
    assert(!state.bind_route_peer(999, 0x80000004U));
}

static void test_gateway_resolution_deadline_contract() {
    const auto source_path = fs::path(__FILE__).parent_path() /
        "../transport/q2_doszone_gateway.cpp";
    std::ifstream input(source_path, std::ios::binary);
    assert(input);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    assert(source.find("runtime.resolve_deadline = 0;") != std::string::npos);
    assert(source.find("kResolveTimeoutMs = 30'000U") != std::string::npos);
    const auto bind = source.find("state_.bind_route_peer(id, route_peer)");
    assert(bind != std::string::npos);
    const auto deadline = source.find("runtime->second.resolve_deadline =", bind);
    assert(deadline != std::string::npos);
    assert(source.find("q2dz::monotonic_millis() + kResolveTimeoutMs", deadline) !=
           std::string::npos);
    assert(source.find(
               "state_target->route_peer != 0 && runtime->second.resolve_deadline != 0") !=
           std::string::npos);
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
    test_shared_hub_fixture_red();
    test_shared_hub_fixture();
    test_hub_framer_boundary();
    test_hub_command_move_wipes_source();
    test_short_payload_formatter();
    test_hub_queue_capacity();
    test_probe_ack_routing();
    test_multiroom_state_virtual_routing();
    test_gateway_resolution_deadline_contract();
    test_content();
    test_child_environment_scrubs_credential_names();
    std::cout << "q2dz unit tests: PASS\n";
    return 0;
}
