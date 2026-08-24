#include "webview_bridge_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <stdexcept>

namespace {

constexpr std::size_t max_datagram = 1400;
constexpr std::string_view prefix = "{\"type\":\"udp\",\"data\":\"";
constexpr std::string_view suffix = "\"}";
constexpr std::string_view ready_prefix =
    "{\"type\":\"state\",\"value\":\"transport-ready\",\"peer\":";
constexpr std::string_view room_closed_prefix =
    "{\"type\":\"state\",\"value\":\"room-closed\",\"reason\":\"";
constexpr std::string_view failed_prefix =
    "{\"type\":\"state\",\"value\":\"failed\",\"reason\":\"";
constexpr std::string_view cadence_prefix = "{\"type\":\"cadence\",";
constexpr char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

void validate_size(std::size_t size) {
    if (size == 0 || size > max_datagram) {
        throw std::invalid_argument("datagram size must be 1..1400 bytes");
    }
}

std::string encode_base64(const std::vector<std::uint8_t> &input) {
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const std::uint32_t a = input[i];
        const std::uint32_t b = i + 1 < input.size() ? input[i + 1] : 0;
        const std::uint32_t c = i + 2 < input.size() ? input[i + 2] : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
        output.push_back(i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=');
        output.push_back(i + 2 < input.size() ? alphabet[value & 63] : '=');
    }
    return output;
}

std::vector<std::uint8_t> decode_base64(std::string_view input) {
    if (input.empty() || input.size() % 4 != 0) {
        throw std::invalid_argument("invalid base64 length");
    }

    std::vector<std::uint8_t> output;
    output.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const bool last = i + 4 == input.size();
        const bool pad2 = input[i + 2] == '=';
        const bool pad3 = input[i + 3] == '=';
        if ((!last && (pad2 || pad3)) || (pad2 && !pad3)) {
            throw std::invalid_argument("invalid base64 padding");
        }

        const int a = base64_value(input[i]);
        const int b = base64_value(input[i + 1]);
        const int c = pad2 ? 0 : base64_value(input[i + 2]);
        const int d = pad3 ? 0 : base64_value(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad2 && (b & 15) != 0) || (pad3 && !pad2 && (c & 3) != 0)) {
            throw std::invalid_argument("invalid base64 data");
        }

        const std::uint32_t value = (static_cast<std::uint32_t>(a) << 18) |
                                    (static_cast<std::uint32_t>(b) << 12) |
                                    (static_cast<std::uint32_t>(c) << 6) |
                                    static_cast<std::uint32_t>(d);
        output.push_back(static_cast<std::uint8_t>(value >> 16));
        if (!pad2) output.push_back(static_cast<std::uint8_t>(value >> 8));
        if (!pad3) output.push_back(static_cast<std::uint8_t>(value));
    }
    return output;
}

}  // namespace

std::string encode_datagram_message(const std::vector<std::uint8_t> &datagram) {
    validate_size(datagram.size());
    return std::string(prefix) + encode_base64(datagram) + std::string(suffix);
}

std::vector<std::uint8_t> decode_datagram_message(std::string_view message) {
    if (message.size() <= prefix.size() + suffix.size() ||
        message.substr(0, prefix.size()) != prefix ||
        message.substr(message.size() - suffix.size()) != suffix) {
        throw std::invalid_argument("invalid datagram message");
    }
    auto datagram = decode_base64(message.substr(
        prefix.size(), message.size() - prefix.size() - suffix.size()));
    validate_size(datagram.size());
    return datagram;
}

bool record_oversized_datagram(std::size_t size,
                               std::uint64_t &oversized_drops) noexcept {
    if (size <= max_datagram) return false;
    ++oversized_drops;
    return true;
}

void record_accepted_datagram(std::size_t size, std::size_t &maximum) noexcept {
    maximum = std::max(maximum, size);
}

bool record_oversized_transport_drop(std::string_view message,
                                     std::uint64_t &oversized_drops) noexcept {
    if (message != "{\"type\":\"drop\",\"reason\":\"oversized_transport\"}") {
        return false;
    }
    ++oversized_drops;
    return true;
}

std::optional<std::uint32_t> parse_transport_ready_peer(std::string_view message) {
    if (message.size()<=ready_prefix.size() || message.substr(0,ready_prefix.size())!=ready_prefix || message.back()!='}') return std::nullopt;
    const auto value=message.substr(ready_prefix.size(),message.size()-ready_prefix.size()-1);
    std::uint32_t peer=0;
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),peer);
    if(error!=std::errc{}||end!=value.data()+value.size()||peer==0)return std::nullopt;
    return peer;
}

std::optional<std::string> parse_room_closed_reason(std::string_view message) {
    if (message.size()<=room_closed_prefix.size()+suffix.size() || message.substr(0,room_closed_prefix.size())!=room_closed_prefix || message.substr(message.size()-suffix.size())!=suffix) return std::nullopt;
    const auto value=message.substr(room_closed_prefix.size(),message.size()-room_closed_prefix.size()-suffix.size());
    if(value.empty()||value.size()>64)return std::nullopt;
    for(const unsigned char c:value)if(!(std::isalnum(c)||c=='_'||c=='-'))return std::nullopt;
    return std::string(value);
}

std::optional<std::string> parse_transport_failed_reason(std::string_view message) {
    constexpr std::string_view type_field = "\"type\":\"state\"";
    constexpr std::string_view value_field = "\"value\":\"failed\"";
    constexpr std::string_view reason_field = "\"reason\":\"";
    if (message.size() < failed_prefix.size()+suffix.size() || message.front()!='{' || message.back()!='}' ||
        message.find(type_field)==std::string_view::npos ||
        message.find(value_field)==std::string_view::npos) return std::nullopt;
    const auto begin=message.find(reason_field);
    if(begin==std::string_view::npos)return std::nullopt;
    const auto start=begin+reason_field.size();
    const auto end=message.find('"',start);
    if(end==std::string_view::npos)return std::nullopt;
    const auto value=message.substr(start,end-start);
    if(value.empty()||value.size()>64)return std::nullopt;
    for(const unsigned char c:value)if(!(std::isalnum(c)||c=='_'||c=='-'))return std::nullopt;
    return std::string(value);
}

std::optional<CadenceSnapshot> parse_cadence_message(std::string_view message) {
    constexpr std::uint64_t maximum = 9007199254740991ULL;
    constexpr std::array fields{
        std::pair{std::string_view{"\"timerTicks\":"}, &CadenceSnapshot::timer_ticks},
        std::pair{std::string_view{"\"timerMaxGapMs\":"}, &CadenceSnapshot::timer_max_gap_ms},
        std::pair{std::string_view{"\"waitCalls\":"}, &CadenceSnapshot::wait_calls},
        std::pair{std::string_view{"\"aliasPollIterations\":"}, &CadenceSnapshot::alias_poll_iterations},
        std::pair{std::string_view{"\"probeSendCount\":"}, &CadenceSnapshot::probe_send_count},
        std::pair{std::string_view{"\"probeAckCount\":"}, &CadenceSnapshot::probe_ack_count},
        std::pair{std::string_view{"\"dcToNativeDatagrams\":"}, &CadenceSnapshot::dc_to_native_datagrams},
        std::pair{std::string_view{"\"dcToNativeBytes\":"}, &CadenceSnapshot::dc_to_native_bytes},
        std::pair{std::string_view{"\"dcToNativeMaxGapMs\":"}, &CadenceSnapshot::dc_to_native_max_gap_ms},
        std::pair{std::string_view{"\"nativeToDcDatagrams\":"}, &CadenceSnapshot::native_to_dc_datagrams},
        std::pair{std::string_view{"\"nativeToDcBytes\":"}, &CadenceSnapshot::native_to_dc_bytes},
        std::pair{std::string_view{"\"nativeToDcMaxGapMs\":"}, &CadenceSnapshot::native_to_dc_max_gap_ms},
    };
    if (message.size() > 512 ||
        message.substr(0, cadence_prefix.size()) != cadence_prefix) return std::nullopt;
    message.remove_prefix(cadence_prefix.size());
    CadenceSnapshot result{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto &[name, member] = fields[index];
        if (message.substr(0, name.size()) != name) return std::nullopt;
        message.remove_prefix(name.size());
        const char delimiter = index + 1 == fields.size() ? '}' : ',';
        const auto end = message.find(delimiter);
        if (end == std::string_view::npos ||
            (end > 1 && message.front() == '0')) return std::nullopt;
        std::uint64_t value = 0;
        const auto [parsed, error] = std::from_chars(
            message.data(), message.data() + end, value);
        if (error != std::errc{} || parsed != message.data() + end || value > maximum) {
            return std::nullopt;
        }
        result.*member = value;
        message.remove_prefix(end + 1);
    }
    return message.empty() ? std::optional{result} : std::nullopt;
}

void record_cadence_gap(std::uint64_t now, std::uint64_t &previous,
                        std::uint64_t &maximum) noexcept {
    if (previous != 0 && now >= previous) maximum = std::max(maximum, now - previous);
    previous = now;
}
