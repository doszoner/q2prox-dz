#include "q2dz/child_protocol.hpp"
#include "q2dz/session_safety.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures=0;
#define CHECK(x) do { if(!(x)){ std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #x "\n"; ++failures; } } while(0)

struct HubFixtureCase {
    std::string name;
    std::string parser;
    bool valid = false;
    std::string line;
    std::size_t repeat = 0;
    char fill = 'x';
};

std::string fixture_string(std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const auto key_pos = object.find(needle);
    if (key_pos == std::string_view::npos) return {};
    auto pos = key_pos + needle.size();
    if (pos >= object.size() || object[pos] != '"') return {};
    std::string out;
    for (++pos; pos < object.size(); ++pos) {
        const char c = object[pos];
        if (c == '"') return out;
        if (c != '\\') { out.push_back(c); continue; }
        if (++pos >= object.size()) return {};
        const char escaped = object[pos];
        if (escaped == 'n') out.push_back('\n');
        else if (escaped == 'r') out.push_back('\r');
        else if (escaped == 't') out.push_back('\t');
        else if (escaped == 'u') {
            if (pos + 4 >= object.size()) return {};
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
    return {};
}

std::vector<HubFixtureCase> load_hub_fixture() {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "../../../tests/contracts/hub-protocol-cases.json";
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
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
    CHECK(cases.size() == 48);
    return cases;
}

void protocol_tests() {
    auto ok=q2dz::parse_child_event("Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:27911 peer=42\r\n");
    CHECK(ok.event.has_value());
    CHECK(ok.event && ok.event->type==q2dz::ChildEventType::transport_ready);
    CHECK(ok.event && ok.event->fields.at("udp")=="127.0.0.1:27911");
    CHECK(q2dz::parse_loopback_endpoint("127.0.0.1:1024").has_value());
    CHECK(q2dz::parse_loopback_endpoint("127.0.0.1:65535").has_value());
    CHECK(!q2dz::parse_loopback_endpoint("0.0.0.0:27910"));
    CHECK(!q2dz::parse_loopback_endpoint("127.0.0.1:80"));
    CHECK(!q2dz::parse_loopback_endpoint("127.0.0.1:65536"));
    CHECK(!q2dz::parse_child_event("Q2DZ/1 TRANSPORT_READY udp=8.8.8.8:27910").event);
    CHECK(!q2dz::parse_child_event("Q2DZ/1 FATAL credential=leak").event);
    CHECK(!q2dz::parse_child_event("Q2DZ/1 FATAL stage=gateway").event);
    CHECK(q2dz::parse_child_event("Q2DZ/1 FATAL stage=gateway code=connect_failed").event);
    CHECK(!q2dz::parse_child_event("Q2DZ/1 FATAL stage=ice stage=two").event);
    auto closed=q2dz::parse_child_event("Q2DZ/1 ROOM_CLOSED reason=probe%20timeout");
    CHECK(closed.event && closed.event->fields.at("reason")=="probe timeout");
    const std::map<std::string, std::string, std::less<>> ready_fields = {
        {"id", "42"}, {"peer", "17"}, {"udp", "127.0.0.1:27910"}
    };
    const auto hub_ready = q2dz::format_child_event(q2dz::ChildEventType::transport_ready, ready_fields);
    CHECK(hub_ready == "Q2DZ/HUB/2 READY id=42 udp=127.0.0.1:27910 peer=17");
    CHECK(q2dz::parse_child_event(hub_ready).event.has_value());
    CHECK(q2dz::format_child_event(q2dz::ChildEventType::transport_ready,
                                   {{"id", "42"}, {"udp", "127.0.0.1:27910"}, {"credential", "secret"}}).empty());
    CHECK(q2dz::format_child_event(q2dz::ChildEventType::transport_ready,
                                   {{"aLiAs", "room.q2ded.server"}, {"id", "42"}, {"peer", "17"},
                                    {"udp", "127.0.0.1:27910"}}).empty());
}

void shared_hub_fixture_red() {
    for (const auto& item : load_hub_fixture()) {
        if (item.parser != "event") continue;
        const auto parsed = q2dz::parse_child_event(item.line);
        CHECK(parsed.event.has_value() == item.valid);
        if (parsed.event) {
            CHECK(q2dz::format_child_event(parsed.event->type, parsed.event->fields) == item.line);
            if (item.name == "failed") CHECK(parsed.event->type == q2dz::ChildEventType::target_failed);
        }
    }
    for (const auto& item : load_hub_fixture()) {
        if (item.parser != "event_mix") continue;
        q2dz::ChildEventParser parser;
        CHECK(parser.parse("Q2DZ/HUB/2 HEARTBEAT state=ok targets=1 ready=1").event.has_value());
        CHECK(!parser.parse(item.line).event);
    }
}

void hub_framer_boundary_tests() {
    const std::string head = "Q2DZ/HUB/2 ICE generation=7 json=[\"";
    const std::string tail = "\"]";
    const std::string line = head + std::string(q2dz::HubCommandFramer::max_line_bytes - head.size() - tail.size(), 'x') + tail;
    CHECK(line.size() == q2dz::HubCommandFramer::max_line_bytes);
    q2dz::HubCommandFramer framer;
    CHECK(framer.feed(line + '\n'));
    CHECK(framer.finish());
    const auto lines = framer.drain();
    CHECK(lines.size() == 1 && lines.front().view() == line);
    CHECK(q2dz::parse_hub_command(lines.front().view()).command.has_value());
    auto command = q2dz::parse_hub_command(lines.front().view());
    command.command->clear();
    CHECK(command.command->json.empty());

    q2dz::HubCommandFramer oversized;
    CHECK(!oversized.feed(line + "x"));
    CHECK(oversized.error() == q2dz::HubCommandFramer::Error::line_too_long);

    q2dz::HubCommandFramer cleared;
    CHECK(cleared.feed("Q2DZ/HUB/2 STOP\npartial"));
    cleared.clear();
    CHECK(cleared.queued_line_count() == 0 && cleared.pending_byte_count() == 0);

    q2dz::HubCommandFramer crlf;
    CHECK(crlf.feed("Q2DZ/ROOMS/2 ICE generation=1 json=[]\r\n"));
    const auto crlf_lines = crlf.drain();
    CHECK(crlf_lines.size() == 1 && crlf_lines.front().view() == "Q2DZ/ROOMS/2 ICE generation=1 json=[]");
}

void hub_command_move_wipes_source_tests() {
    q2dz::HubCommand short_source;
    short_source.room_id = "short-room";
    short_source.json = {'s', 'e', 'c'};
    q2dz::HubCommand short_destination(std::move(short_source));
    CHECK(short_source.room_id.empty() && short_source.json.empty());
    CHECK(short_destination.room_id == "short-room" && short_destination.json.size() == 3);

    q2dz::HubCommand heap_source;
    heap_source.room_id.assign(300, 'r');
    q2dz::HubCommand heap_destination;
    heap_destination = std::move(heap_source);
    CHECK(heap_source.room_id.empty());
    CHECK(heap_destination.room_id.size() == 300);
}

void short_payload_formatter_tests() {
    q2dz::HubCommand command;
    command.type = q2dz::HubCommandType::put;
    command.id_or_generation = 42;
    command.json = {'{', '"', 'x', '"', ':', '1', '}'};
    CHECK(q2dz::format_hub_command(command) == "Q2DZ/HUB/2 PUT id=42 json={\"x\":1}");
}

void lifecycle_room_command_tests() {
    q2dz::HubCommandParser rooms(q2dz::HubCommandChannel::rooms);
    CHECK(rooms.parse("Q2DZ/ROOMS/2 ICE generation=1 json=[]").command.has_value());
    const auto parsed = rooms.parse(
        "Q2DZ/ROOMS/2 PUT room=room-a json={\"version\":1,\"roomId\":\"room-a\","
        "\"alias\":\"q2.room-a\",\"ordered\":false,\"iceServers\":[],\"map\":\"q2dm1\","
        "\"game\":\"3zb2\"}");
    CHECK(parsed.command.has_value());
}

void hub_queue_capacity_tests() {
    q2dz::HubCommandFramer framer;
    CHECK(framer.feed("Q2DZ/HUB/2 ICE generation=7 json=[]\n"));
    for (std::uint64_t id = 1; id <= 256; ++id) {
        CHECK(framer.feed("Q2DZ/HUB/2 PUT id=" + std::to_string(id) + " json={}\n"));
    }
    for (std::uint64_t id = 1; id <= 256; ++id) {
        CHECK(framer.feed("Q2DZ/HUB/2 DROP id=" + std::to_string(id) + "\n"));
    }
    CHECK(framer.queued_line_count() == 513);
    CHECK(!framer.feed("Q2DZ/HUB/2 STOP\n"));
    CHECK(framer.error() == q2dz::HubCommandFramer::Error::queue_overflow);
    CHECK(framer.queued_line_count() == 0);
}

void shared_hub_fixture() {
    const auto cases = load_hub_fixture();
    q2dz::HubCommandParser rooms(q2dz::HubCommandChannel::rooms);
    q2dz::HubCommandParser hub(q2dz::HubCommandChannel::hub);
    for (const auto& item : cases) {
        if (item.parser == "rooms_command") {
            const auto parsed = rooms.parse(item.line);
            CHECK(parsed.command.has_value() == item.valid);
            if (parsed.command) CHECK(q2dz::format_rooms_command(*parsed.command) == item.line);
        } else if (item.parser == "hub_command") {
            const auto parsed = hub.parse(item.line);
            CHECK(parsed.command.has_value() == item.valid);
            if (parsed.command) CHECK(q2dz::format_hub_command(*parsed.command) == item.line);
        } else if (item.parser == "hub_sequence") {
            q2dz::HubCommandParser fresh(q2dz::HubCommandChannel::hub);
            CHECK(fresh.parse(item.line).command.has_value() == item.valid);
            CHECK(!q2dz::parse_hub_command(item.line).command.has_value());
        } else if (item.parser == "line") {
            q2dz::HubCommandFramer framer;
            const std::string input = item.line + std::string(item.repeat, item.fill);
            CHECK(!framer.feed(input));
            CHECK(framer.error() == q2dz::HubCommandFramer::Error::line_too_long);
        } else if (item.parser == "queue") {
            q2dz::HubCommandFramer framer;
            const auto input = item.line + '\n';
            std::size_t accepted = 0;
            for (std::size_t i = 0; i < item.repeat; ++i) {
                if (!framer.feed(input)) break;
                ++accepted;
            }
            CHECK(!framer.feed(""));
            CHECK(framer.error() == q2dz::HubCommandFramer::Error::queue_overflow);
            CHECK(accepted == q2dz::HubCommandFramer::max_queued_lines);
            CHECK(framer.queued_line_count() == 0);
        } else if (item.parser == "formatter_invalid_utf8") {
            q2dz::HubCommand command;
            command.type = q2dz::HubCommandType::put;
            command.id_or_generation = 42;
            command.json = {'{', '"', 'x', '"', ':', '"', 0xffU, '"', '}'};
            CHECK(q2dz::format_hub_command(command).empty());
        }
    }
}

void framer_tests() {
    constexpr auto line_too_long = "Q2DZ/1 FATAL stage=protocol code=line_too_long\n";
    constexpr auto queue_overflow = "Q2DZ/1 FATAL stage=protocol code=queue_overflow\n";

    q2dz::ChildEventFramer no_newline;
    CHECK(!no_newline.feed(std::string(100 * 1024, 'x')));
    CHECK(no_newline.error()==q2dz::ChildEventFramer::Error::line_too_long);
    CHECK(no_newline.pending_byte_count()<=q2dz::ChildEventFramer::max_line_bytes);
    auto no_newline_events=no_newline.drain();
    CHECK(no_newline_events.size()==1);
    CHECK(no_newline_events.front()==line_too_long);
    CHECK(!no_newline.feed("Q2DZ/1 HEARTBEAT\n"));
    CHECK(no_newline.drain().empty());

    q2dz::ChildEventFramer oversized;
    std::string oversized_line(q2dz::ChildEventFramer::max_line_bytes, 'x');
    oversized_line.push_back('\n');
    CHECK(!oversized.feed(oversized_line));
    CHECK(oversized.error()==q2dz::ChildEventFramer::Error::line_too_long);
    auto oversized_events=oversized.drain();
    CHECK(oversized_events.size()==1);
    CHECK(oversized_events.front()==line_too_long);
    CHECK(oversized.drain().empty());

    q2dz::ChildEventFramer legal;
    CHECK(legal.feed("Q2DZ/1 HEARTBEAT\nQ2DZ/1 HEARTBEAT\n"));
    CHECK(legal.queued_line_count()==2);
    CHECK(legal.queued_byte_count()==34);
    auto legal_events=legal.drain();
    CHECK(legal_events.size()==2);
    CHECK(legal_events.front()=="Q2DZ/1 HEARTBEAT\n");
    CHECK(legal_events.back()=="Q2DZ/1 HEARTBEAT\n");
    CHECK(legal.queued_line_count()==0);
    CHECK(legal.queued_byte_count()==0);

    q2dz::ChildEventFramer hub_ready;
    CHECK(hub_ready.feed("Q2DZ/HUB/2 READY id=1 udp=127.0.0.1:27911 peer=101\n"));
    const auto hub_ready_events = hub_ready.drain();
    CHECK(hub_ready_events.size() == 1);
    CHECK(q2dz::parse_child_event(hub_ready_events.front()).event.has_value());

    q2dz::ChildEventFramer ready_burst;
    std::string ready_lines;
    for (std::size_t i = 1; i <= 256; ++i) {
        ready_lines += "Q2DZ/HUB/2 READY id=" + std::to_string(i) +
                       " udp=127.0.0.1:" + std::to_string(27910U + i) +
                       " peer=" + std::to_string(100U + i) + "\n";
    }
    const std::string heartbeat = "Q2DZ/HUB/2 HEARTBEAT state=ok targets=256 ready=256\n";
    ready_lines += heartbeat;
    CHECK(ready_burst.feed(ready_lines));
    CHECK(ready_burst.queued_line_count() == 257);
    CHECK(ready_burst.queued_byte_count() < q2dz::ChildEventFramer::max_queued_bytes);
    const auto ready_burst_events = ready_burst.drain();
    CHECK(ready_burst_events.size() == 257);
    std::string drained_ready_lines;
    for (const auto& line : ready_burst_events) drained_ready_lines += line;
    CHECK(drained_ready_lines == ready_lines);
    CHECK(ready_burst_events.back() == heartbeat);

    q2dz::ChildEventFramer by_count;
    for (std::size_t i=0; i<q2dz::ChildEventFramer::max_queued_lines; ++i) {
        if (!by_count.feed("Q2DZ/1 HEARTBEAT\n")) break;
    }
    CHECK(by_count.error()==q2dz::ChildEventFramer::Error::queue_overflow);
    CHECK(by_count.queued_line_count()==q2dz::ChildEventFramer::max_queued_lines);
    CHECK(by_count.queued_byte_count()<=q2dz::ChildEventFramer::max_queued_bytes);
    auto count_events=by_count.drain();
    CHECK(count_events.size()==q2dz::ChildEventFramer::max_queued_lines);
    CHECK(count_events.back()==queue_overflow);
    CHECK(by_count.queued_byte_count()==0);

    q2dz::ChildEventFramer by_bytes;
    std::string large_line(q2dz::ChildEventFramer::max_line_bytes-1, 'x');
    large_line.push_back('\n');
    for (std::size_t i=0; i<q2dz::ChildEventFramer::max_queued_lines; ++i) {
        if (!by_bytes.feed(large_line)) break;
    }
    CHECK(by_bytes.error()==q2dz::ChildEventFramer::Error::queue_overflow);
    CHECK(by_bytes.queued_byte_count()<=q2dz::ChildEventFramer::max_queued_bytes);
    auto byte_events=by_bytes.drain();
    CHECK(byte_events.size()<q2dz::ChildEventFramer::max_queued_lines);
    CHECK(byte_events.back()==queue_overflow);
    CHECK(by_bytes.queued_byte_count()==0);
}

void session_safety_tests() {
    const auto root=std::filesystem::path("/tmp/q2dz-sessions");
    CHECK(q2dz::is_direct_session_child(root,root/"session-abc"));
    CHECK(!q2dz::is_direct_session_child(root,root));
    CHECK(!q2dz::is_direct_session_child(root,root/"nested"/"session-abc"));
    CHECK(!q2dz::is_direct_session_child(root,root/".."/"q2dz-sessions"/"session-abc"));
    CHECK(q2dz::is_expected_session_name(root/"session-abc"));
    CHECK(!q2dz::is_expected_session_name(root/"session"));
    CHECK(!q2dz::is_expected_session_name(root/"session_abc"));
    CHECK(q2dz::is_exact_session_marker("q2dz-session-v1\n"));
    CHECK(!q2dz::is_exact_session_marker("q2dz-session-v1"));
    CHECK(!q2dz::is_exact_session_marker("q2dz-session-v1\nextra"));
}

}

int main() {
    protocol_tests(); framer_tests(); shared_hub_fixture_red(); shared_hub_fixture(); hub_framer_boundary_tests(); hub_command_move_wipes_source_tests(); short_payload_formatter_tests(); lifecycle_room_command_tests(); hub_queue_capacity_tests(); session_safety_tests();
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "q2dz-core-tests: PASS\n";
    return EXIT_SUCCESS;
}
