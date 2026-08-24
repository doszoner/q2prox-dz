#include "webview_bridge_protocol.h"
#include "web_message_dispatch.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
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
    CHECK(transport_drops == 1);

    for (const std::size_t size : {1, 32, 512, 1390, 1400}) {
        const auto input = datagram(size);
        const auto message = encode_datagram_message(input);
        CHECK(decode_datagram_message(message) == input);
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
    return failures ? 1 : 0;
}
