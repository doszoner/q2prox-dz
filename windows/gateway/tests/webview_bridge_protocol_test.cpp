#include "webview_bridge_protocol.h"
#include "web_message_dispatch.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
int failures=0;
#define CHECK(x) do { if(!(x)){ std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #x "\n"; ++failures; } } while(0)

std::vector<std::uint8_t> datagram(std::size_t size) {
    std::vector<std::uint8_t> value(size);
    for (std::size_t i = 0; i < size; ++i) {
        value[i] = static_cast<std::uint8_t>((i * 31 + size) & 0xff);
    }
    return value;
}

template <typename Function>
bool throws_invalid_argument(Function function) {
    try {
        function();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

void checks_owned_web_message_is_released_when_callback_throws() {
    int delete_count = 0;
    bool callback_threw = false;
    wchar_t *raw = new wchar_t[1]{L'x'};
    try {
        with_owned_web_message(raw,
            [&delete_count](wchar_t *value) noexcept {
                ++delete_count;
                delete[] value;
            },
            [](wchar_t *) { throw std::runtime_error("callback failed"); });
    } catch (const std::runtime_error &) {
        callback_threw = true;
    }
    CHECK(callback_threw);
    CHECK(delete_count == 1);
}

}  // namespace

int main() {
    checks_owned_web_message_is_released_when_callback_throws();
    const std::string cadence_message =
        "{\"type\":\"cadence\",\"timerTicks\":1,\"timerMaxGapMs\":2,"
        "\"waitCalls\":3,\"aliasPollIterations\":4,\"probeSendCount\":5,"
        "\"probeAckCount\":6,\"dcToNativeDatagrams\":7,\"dcToNativeBytes\":8,"
        "\"dcToNativeMaxGapMs\":9,\"nativeToDcDatagrams\":10,"
        "\"nativeToDcBytes\":11,\"nativeToDcMaxGapMs\":12}";
    const auto cadence = parse_cadence_message(cadence_message);
    CHECK(cadence && cadence->timer_ticks == 1 && cadence->timer_max_gap_ms == 2 &&
          cadence->wait_calls == 3 && cadence->alias_poll_iterations == 4 &&
          cadence->probe_send_count == 5 && cadence->probe_ack_count == 6 &&
          cadence->dc_to_native_datagrams == 7 && cadence->dc_to_native_bytes == 8 &&
          cadence->dc_to_native_max_gap_ms == 9 &&
          cadence->native_to_dc_datagrams == 10 && cadence->native_to_dc_bytes == 11 &&
          cadence->native_to_dc_max_gap_ms == 12);
    CHECK(!parse_cadence_message(cadence_message.substr(
        0, cadence_message.rfind(",\"nativeToDcMaxGapMs\"")) + "}"));
    CHECK(!parse_cadence_message(cadence_message.substr(0, cadence_message.size() - 1) +
        ",\"extra\":0}"));
    const auto with_timer_ticks = [&cadence_message](std::string_view value) {
        auto result = cadence_message;
        const auto begin = result.find("\"timerTicks\":1");
        result.replace(begin, std::string_view{"\"timerTicks\":1"}.size(),
                       "\"timerTicks\":" + std::string(value));
        return result;
    };
    CHECK(!parse_cadence_message(with_timer_ticks("1,\"timerTicks\":1")));
    CHECK(!parse_cadence_message(with_timer_ticks("\"1\"")));
    CHECK(!parse_cadence_message(with_timer_ticks("-1")));
    CHECK(!parse_cadence_message(with_timer_ticks("1.0")));
    CHECK(!parse_cadence_message(with_timer_ticks("01")));
    CHECK(!parse_cadence_message(with_timer_ticks("9007199254740992")));
    CHECK(!parse_cadence_message(with_timer_ticks("18446744073709551616")));
    CHECK(!parse_cadence_message(cadence_message.substr(0, cadence_message.size() - 1) +
        ",\"token\":\"credential-shaped\"}"));

    std::uint64_t last_udp_to_q2 = 0;
    std::uint64_t last_post_to_web = 0;
    std::uint64_t max_udp_to_q2 = 0;
    std::uint64_t max_post_to_web = 0;
    record_cadence_gap(100, last_udp_to_q2, max_udp_to_q2);
    record_cadence_gap(250, last_udp_to_q2, max_udp_to_q2);
    record_cadence_gap(300, last_post_to_web, max_post_to_web);
    record_cadence_gap(500, last_post_to_web, max_post_to_web);
    CHECK(max_udp_to_q2 == 150 && max_post_to_web == 200);
    CHECK(last_udp_to_q2 == 250 && last_post_to_web == 500);
    std::size_t max_datagram = 0;
    std::uint64_t oversized_drops = 0;
    CHECK(record_oversized_datagram(1401, oversized_drops));
    CHECK(max_datagram == 0 && oversized_drops == 1);
    CHECK(!record_oversized_datagram(1400, oversized_drops));
    CHECK(max_datagram == 0 && oversized_drops == 1);
    record_accepted_datagram(1400, max_datagram);
    CHECK(max_datagram == 1400 && oversized_drops == 1);
    record_accepted_datagram(1399, max_datagram);
    CHECK(max_datagram == 1400 && oversized_drops == 1);

    std::uint64_t transport_drops = 0;
    CHECK(record_oversized_transport_drop(
        "{\"type\":\"drop\",\"reason\":\"oversized_transport\"}", transport_drops));
    CHECK(transport_drops == 1);
    CHECK(!record_oversized_transport_drop(
        "{\"type\":\"drop\",\"reason\":\"oversized_transport\",\"extra\":1}",
        transport_drops));
    CHECK(!record_oversized_transport_drop(
        "{\"type\":\"drop\",\"reason\":\"transport_not_ready\"}", transport_drops));
    CHECK(record_oversized_transport_drop(
        "{\"type\":\"drop\",\"targetId\":\"42\",\"reason\":\"oversized_transport\"}",
        transport_drops));
    CHECK(!record_oversized_transport_drop(
        "{\"type\":\"drop\",\"targetId\":\"42\",\"credential\":\"x\",\"reason\":\"oversized_transport\"}",
        transport_drops));
    std::uint64_t unknown_peer_drops = 0;
    CHECK(record_unknown_peer_drop(
        "{\"type\":\"drop\",\"reason\":\"unknown_peer\"}", unknown_peer_drops));
    CHECK(!record_unknown_peer_drop(
        "{\"type\":\"drop\",\"reason\":\"unknown_peer\",\"credential\":\"x\"}",
        unknown_peer_drops));
    CHECK(unknown_peer_drops == 1);
    CHECK(transport_drops == 2);

    const auto large_ice_generation = parse_ice_generation_message(
        "{\"type\":\"ice\",\"generation\":\"9007199254740993\",\"iceServers\":[]}");
    CHECK(large_ice_generation && *large_ice_generation == UINT64_C(9007199254740993));
    const auto maximum_ice_generation = parse_ice_generation_message(
        "{\"type\":\"ice\",\"generation\":\"18446744073709551615\",\"iceServers\":[]}");
    CHECK(maximum_ice_generation && *maximum_ice_generation == UINT64_C(18446744073709551615));
    CHECK(!parse_ice_generation_message(
        "{\"type\":\"ice\",\"generation\":9007199254740993,\"iceServers\":[]}"));
    CHECK(!parse_ice_generation_message(
        "{\"type\":\"ice\",\"generation\":\"01\",\"iceServers\":[]}"));
    CHECK(!parse_ice_generation_message(
        "{\"type\":\"ice\",\"generation\":\"18446744073709551616\",\"iceServers\":[]}"));
    CHECK(!parse_ice_generation_message(
        "{\"type\":\"ice\",\"generation\":\"1\",\"iceServers\":[],\"credential\":\"x\"}"));

    for (const std::size_t size : {1, 32, 512, 1390, 1400}) {
        const auto input = datagram(size);
        const auto message = encode_datagram_message(input);
        CHECK(decode_datagram_message(message) == input);
    }

    const auto tagged = encode_datagram_message(42, datagram(32));
    const auto tagged_decoded = decode_tagged_datagram_message(tagged);
    CHECK(tagged_decoded.target_id == 42 && tagged_decoded.datagram == datagram(32));
    CHECK(decode_tagged_datagram_message(encode_datagram_message(42, datagram(1400))).datagram.size() == 1400);
    CHECK(throws_invalid_argument([] {
        encode_datagram_message(42, std::vector<std::uint8_t>(1401));
    }));
    CHECK(throws_invalid_argument([] {
        encode_datagram_message(0, std::vector<std::uint8_t>(1));
    }));
    CHECK(throws_invalid_argument([] {
        decode_tagged_datagram_message(
            "{\"type\":\"udp\",\"targetId\":\"0\",\"data\":\"AQ==\"}");
    }));
    CHECK(throws_invalid_argument([] {
        decode_tagged_datagram_message(
            "{\"type\":\"udp\",\"targetId\":\"1\",\"data\":\"AQ==\",\"credential\":\"x\"}");
    }));
    CHECK(throws_invalid_argument([] {
        decode_tagged_datagram_message(
            "{\"type\":\"udp\",\"targetId\":1,\"data\":\"AQ==\"}");
    }));
    CHECK(throws_invalid_argument([] {
        decode_datagram_message("{\"type\":\"udp\",\"data\":\"AQ==\"}");
    }));

    CHECK(parse_transport_ready_message(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"targetId\":\"42\",\"peer\":77}")
          .value().target_id == 42);
    CHECK(!parse_transport_ready_message(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"targetId\":\"42\",\"peer\":77,\"token\":\"x\"}"));
    CHECK(!parse_transport_ready_message(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"targetId\":\"42\",\"peer\":2147483648}"));
    CHECK(!parse_transport_ready_message(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"targetId\":42,\"peer\":77}"));
    CHECK(!parse_transport_ready_message(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"targetId\":\"042\",\"peer\":77}"));
    CHECK(!parse_target_state_message(
        "{\"type\":\"state\",\"value\":\"failed\",\"targetId\":\"42\",\"reason\":\"bad reason\"}"));
    CHECK(parse_target_state_message(
        "{\"type\":\"state\",\"value\":\"room-closed\",\"targetId\":\"42\",\"reason\":\"probe_timeout\"}")
          .value().target_id == 42);
    const auto maximum_target_message = encode_datagram_message(
        UINT64_C(18446744073709551615), datagram(1));
    CHECK(maximum_target_message.find("\"targetId\":\"18446744073709551615\"") !=
          std::string::npos);
    CHECK(decode_tagged_datagram_message(maximum_target_message).target_id ==
          UINT64_C(18446744073709551615));
    for (std::uint64_t target = 1; target <= 40; ++target) {
        const auto message = encode_datagram_message(target, datagram(1));
        const auto decoded = decode_tagged_datagram_message(message);
        CHECK(decoded.target_id == target);
    }

    CHECK(throws_invalid_argument([] { encode_datagram_message({}); }));
    CHECK(throws_invalid_argument([] {
        encode_datagram_message(std::vector<std::uint8_t>(1401));
    }));
    CHECK(throws_invalid_argument([] {
        decode_datagram_message("{\"type\":\"udp\",\"data\":\"" +
                                std::string(1868, 'A') + "\"}");
    }));
    CHECK(throws_invalid_argument([] { decode_datagram_message("{}"); }));
    CHECK(throws_invalid_argument([] {
        decode_datagram_message("{\"type\":\"udp\",\"data\":\"%%%\"}");
    }));

    CHECK(parse_transport_ready_peer(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"peer\":42}") == 42u);
    CHECK(!parse_transport_ready_peer(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"peer\":0}"));
    CHECK(!parse_transport_ready_peer(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"peer\":2147483648}"));
    CHECK(!parse_transport_ready_peer(
        "{\"type\":\"state\",\"value\":\"transport-ready\",\"peer\":42,\"token\":\"leak\"}"));
    CHECK(parse_room_closed_reason(
        "{\"type\":\"state\",\"value\":\"room-closed\",\"reason\":\"probe_timeout\"}") == "probe_timeout");
    CHECK(!parse_room_closed_reason(
        "{\"type\":\"state\",\"value\":\"room-closed\",\"reason\":\"bad reason\"}"));
    CHECK(parse_transport_failed_reason(
        "{\"type\":\"state\",\"value\":\"failed\",\"reason\":\"alias_not_unique\"}") ==
        "alias_not_unique");
    CHECK(parse_transport_failed_reason(
        "{\"reason\":\"invalid_start_message\",\"value\":\"failed\",\"type\":\"state\"}") ==
        "invalid_start_message");
    CHECK(!parse_transport_failed_reason(
        "{\"type\":\"state\",\"value\":\"failed\",\"reason\":\"secret leaked\"}"));
    CHECK(!parse_transport_failed_reason(
        "{\"type\":\"state\",\"value\":\"failed\",\"reason\":\"safe\",\"credential\":\"x\"}"));

    std::map<std::uint64_t, bool> active_targets;
    for (std::uint64_t target = 1; target <= 40; ++target) active_targets[target] = true;
    std::map<std::uint64_t, std::vector<std::uint8_t>> routes;
    for (std::uint64_t round = 0; round < 3; ++round) {
        if (round == 1) active_targets.erase(17);
        for (std::uint64_t offset = 0; offset < 40; ++offset) {
            const auto target = 1 + ((round * 13 + offset) % 40);
            if (!active_targets.contains(target)) continue;
            const auto message = encode_datagram_message(target,
                {static_cast<std::uint8_t>(round), static_cast<std::uint8_t>(target)});
            const auto tagged_message = decode_tagged_datagram_message(message);
            const auto destination = active_targets.find(tagged_message.target_id);
            CHECK(destination != active_targets.end());
            if (destination != active_targets.end()) {
                routes[tagged_message.target_id].insert(routes[tagged_message.target_id].end(),
                                                        tagged_message.datagram.begin(),
                                                        tagged_message.datagram.end());
            }
        }
    }
    CHECK(routes.size() == 40);
    for (std::uint64_t target = 1; target <= 40; ++target) {
        if (target == 17) {
            CHECK(routes[target].size() == 2);
            continue;
        }
        const auto &route = routes[target];
        CHECK(route.size() == 6);
        for (std::size_t round = 0; round < 3; ++round) {
            CHECK(route[round * 2] == round && route[round * 2 + 1] == target);
        }
    }
    return failures ? 1 : 0;
}
