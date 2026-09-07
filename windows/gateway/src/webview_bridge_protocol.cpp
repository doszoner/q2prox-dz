#include "webview_bridge_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <stdexcept>

namespace {

constexpr std::size_t max_datagram = 1400;
constexpr std::string_view tagged_prefix = "{\"type\":\"udp\",\"targetId\":\"";
constexpr std::string_view tagged_data_prefix = "\",\"data\":\"";
constexpr std::string_view suffix = "\"}";
constexpr std::string_view ready_prefix =
    "{\"type\":\"state\",\"value\":\"transport-ready\",\"peer\":";
constexpr std::string_view room_closed_prefix =
    "{\"type\":\"state\",\"value\":\"room-closed\",\"reason\":\"";
constexpr std::string_view cadence_prefix = "{\"type\":\"cadence\",";
constexpr std::string_view tagged_drop_prefix = "{\"type\":\"drop\",\"targetId\":\"";
constexpr std::string_view tagged_drop_suffix = "\",\"reason\":\"oversized_transport\"}";
constexpr std::string_view unknown_peer_drop = "{\"type\":\"drop\",\"reason\":\"unknown_peer\"}";
constexpr std::string_view ice_prefix = "{\"type\":\"ice\",\"generation\":\"";
constexpr std::string_view ice_servers_separator = "\",\"iceServers\":";
constexpr char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::optional<std::uint64_t> positive_u64(std::string_view value) {
    if (value.empty() || (value.front() == '0' && value.size() > 1U)) return std::nullopt;
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result == 0) {
        return std::nullopt;
    }
    return result;
}

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

bool safe_reason(std::string_view value) {
    return !value.empty() && value.size() <= 64U && std::all_of(
        value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        });
}

struct ParsedStateFields {
    std::string type;
    std::string value;
    std::string reason;
    std::uint64_t target_id = 0;
    std::uint32_t peer = 0;
    bool has_type = false;
    bool has_value = false;
    bool has_reason = false;
    bool has_target_id = false;
    bool has_peer = false;
};

bool parse_state_fields(std::string_view message, ParsedStateFields& result) {
    if (message.size() < 2U || message.size() > 1024U || message.front() != '{' ||
        message.back() != '}') return false;
    auto body = message.substr(1U, message.size() - 2U);
    while (!body.empty()) {
        const auto comma = body.find(',');
        const auto part = body.substr(0, comma);
        const auto colon = part.find(':');
        if (colon == std::string_view::npos || colon < 3U) return false;
        const auto key = part.substr(0, colon);
        const auto raw = part.substr(colon + 1U);
        if (key.size() < 2U || key.front() != '"' || key.back() != '"' || raw.empty()) return false;
        const auto name = key.substr(1U, key.size() - 2U);
        if (name == "type") {
            if (result.has_type || raw.size() < 2U || raw.front() != '"' || raw.back() != '"') return false;
            result.type = std::string(raw.substr(1U, raw.size() - 2U));
            result.has_type = true;
        } else if (name == "value") {
            if (result.has_value || raw.size() < 2U || raw.front() != '"' || raw.back() != '"') return false;
            result.value = std::string(raw.substr(1U, raw.size() - 2U));
            result.has_value = true;
        } else if (name == "reason") {
            if (result.has_reason || raw.size() < 2U || raw.front() != '"' || raw.back() != '"') return false;
            result.reason = std::string(raw.substr(1U, raw.size() - 2U));
            result.has_reason = true;
        } else if (name == "targetId") {
            if (result.has_target_id || raw.size() < 2U || raw.front() != '"' || raw.back() != '"') return false;
            const auto value = positive_u64(raw.substr(1U, raw.size() - 2U));
            if (!value) return false;
            result.target_id = *value;
            result.has_target_id = true;
        } else if (name == "peer") {
            if (result.has_peer) return false;
            const auto value = positive_u64(raw);
            if (!value || *value >= 0x80000000U) return false;
            result.peer = static_cast<std::uint32_t>(*value);
            result.has_peer = true;
        } else {
            return false;
        }
        if (comma == std::string_view::npos) break;
        body.remove_prefix(comma + 1U);
        if (body.empty()) return false;
    }
    return true;
}

std::optional<TargetStateMessage> parse_state_message(std::string_view message) {
    ParsedStateFields fields;
    if (!parse_state_fields(message, fields) || !fields.has_type || fields.type != "state" ||
        !fields.has_value || !fields.has_target_id) return std::nullopt;
    if (fields.value == "transport-ready") {
        if (!fields.has_peer || fields.has_reason) return std::nullopt;
    } else if (fields.value == "room-closed" || fields.value == "failed") {
        if (!fields.has_reason || fields.has_peer || !safe_reason(fields.reason)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return TargetStateMessage{fields.target_id, fields.value, fields.reason, fields.peer};
}

}  // namespace

std::string encode_datagram_message(const std::vector<std::uint8_t> &datagram) {
    return encode_datagram_message(1, datagram);
}

std::string encode_datagram_message(std::uint64_t target_id,
                                    const std::vector<std::uint8_t> &datagram) {
    if (!positive_u64(std::to_string(target_id))) {
        throw std::invalid_argument("target id must be nonzero");
    }
    validate_size(datagram.size());
    return std::string(tagged_prefix) + std::to_string(target_id) +
           std::string(tagged_data_prefix) + encode_base64(datagram) + std::string(suffix);
}

std::vector<std::uint8_t> decode_datagram_message(std::string_view message) {
    return decode_tagged_datagram_message(message).datagram;
}

TaggedDatagram decode_tagged_datagram_message(std::string_view message) {
    if (message.size() <= tagged_prefix.size() + tagged_data_prefix.size() + suffix.size() ||
        message.substr(0, tagged_prefix.size()) != tagged_prefix ||
        message.substr(message.size() - suffix.size()) != suffix) {
        throw std::invalid_argument("invalid tagged datagram message");
    }
    const auto separator = message.find(tagged_data_prefix, tagged_prefix.size());
    if (separator == std::string_view::npos) {
        throw std::invalid_argument("invalid tagged datagram message");
    }
    const auto target = positive_u64(message.substr(tagged_prefix.size(),
                                                   separator - tagged_prefix.size()));
    if (!target) throw std::invalid_argument("invalid target id");
    const auto encoded = message.substr(
        separator + tagged_data_prefix.size(),
        message.size() - separator - tagged_data_prefix.size() - suffix.size());
    auto datagram = decode_base64(encoded);
    validate_size(datagram.size());
    return TaggedDatagram{*target, std::move(datagram)};
}

std::optional<std::uint64_t> parse_ice_generation_message(std::string_view message) {
    if (message.size() <= ice_prefix.size() + ice_servers_separator.size() + 2U ||
        message.substr(0, ice_prefix.size()) != ice_prefix || message.back() != '}') {
        return std::nullopt;
    }
    const auto separator = message.find(ice_servers_separator, ice_prefix.size());
    if (separator == std::string_view::npos) return std::nullopt;
    const auto generation = positive_u64(message.substr(
        ice_prefix.size(), separator - ice_prefix.size()));
    if (!generation) return std::nullopt;
    const auto servers = message.substr(
        separator + ice_servers_separator.size(),
        message.size() - separator - ice_servers_separator.size() - 1U);
    if (servers.size() < 2U || servers.front() != '[' || servers.back() != ']') {
        return std::nullopt;
    }
    return generation;
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
    if (message == "{\"type\":\"drop\",\"reason\":\"oversized_transport\"}") {
        ++oversized_drops;
        return true;
    }
    if (message.size() <= tagged_drop_prefix.size() + tagged_drop_suffix.size() ||
        message.substr(0, tagged_drop_prefix.size()) != tagged_drop_prefix ||
        message.substr(message.size() - tagged_drop_suffix.size()) != tagged_drop_suffix ||
        !positive_u64(message.substr(tagged_drop_prefix.size(),
                                     message.size() - tagged_drop_prefix.size() -
                                     tagged_drop_suffix.size()))) return false;
    ++oversized_drops;
    return true;
}

bool record_unknown_peer_drop(std::string_view message,
                              std::uint64_t &unknown_peer_drops) noexcept {
    if (message != unknown_peer_drop) return false;
    ++unknown_peer_drops;
    return true;
}

std::optional<std::uint32_t> parse_transport_ready_peer(std::string_view message) {
    if (const auto parsed = parse_transport_ready_message(message)) return parsed->peer;
    if (message.size()<=ready_prefix.size() || message.substr(0,ready_prefix.size())!=ready_prefix || message.back()!='}') return std::nullopt;
    const auto value=message.substr(ready_prefix.size(),message.size()-ready_prefix.size()-1);
    std::uint32_t peer=0;
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),peer);
    if(error!=std::errc{}||end!=value.data()+value.size()||peer==0||peer>=0x80000000U)return std::nullopt;
    return peer;
}

std::optional<TransportReadyMessage> parse_transport_ready_message(std::string_view message) {
    const auto parsed = parse_state_message(message);
    if (!parsed || parsed->value != "transport-ready") return std::nullopt;
    return TransportReadyMessage{parsed->target_id, parsed->peer};
}

std::optional<TargetStateMessage> parse_target_state_message(std::string_view message) {
    return parse_state_message(message);
}

std::optional<std::string> parse_room_closed_reason(std::string_view message) {
    if (const auto parsed = parse_target_state_message(message);
        parsed && parsed->value == "room-closed") return parsed->reason;
    if (message.size()<=room_closed_prefix.size()+suffix.size() || message.substr(0,room_closed_prefix.size())!=room_closed_prefix || message.substr(message.size()-suffix.size())!=suffix) return std::nullopt;
    const auto value=message.substr(room_closed_prefix.size(),message.size()-room_closed_prefix.size()-suffix.size());
    if(value.empty()||value.size()>64)return std::nullopt;
    for(const unsigned char c:value)if(!(std::isalnum(c)||c=='_'||c=='-'))return std::nullopt;
    return std::string(value);
}

std::optional<std::string> parse_transport_failed_reason(std::string_view message) {
    if (const auto parsed = parse_target_state_message(message);
        parsed && parsed->value == "failed") return parsed->reason;
    ParsedStateFields fields;
    if (!parse_state_fields(message, fields) || !fields.has_type ||
        fields.type != "state" || !fields.has_value || fields.value != "failed" ||
        fields.has_target_id || fields.has_peer || !fields.has_reason ||
        !safe_reason(fields.reason)) return std::nullopt;
    return fields.reason;
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
