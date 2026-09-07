// Minimal native WebRTC-NET peer for the retained shared-gateway harness.
// It accepts ICE only on stdin and answers only Quake II OOB ping/status.

#include "humblenet_p2p.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kDefaultSignal = "wss://net.js-dos.com:444";
constexpr std::string_view kGameToken = "quake2";
constexpr std::string_view kGameSecret = "quake2-secret";
constexpr std::size_t kMaxIceBytes = 64U * 1024U;
constexpr std::size_t kMaxDatagram = 64U * 1024U;
constexpr std::uint8_t kChannel = 0;
constexpr auto kAliasReadyTimeout = std::chrono::seconds(30);

volatile std::sig_atomic_t running = 1;

void stop(int) { running = 0; }

void secure_clear(std::string& value) noexcept {
    volatile char* data = value.empty() ? nullptr : value.data();
    for (std::size_t index = 0; index < value.size(); ++index) data[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

bool valid_alias(std::string_view alias) noexcept {
    if (alias.size() < 5U || alias.size() > 128U) return false;
    if (!alias.starts_with("q2.") && !alias.starts_with("room.q2ded.")) return false;
    return std::all_of(alias.begin(), alias.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '.' || value == '-' || value == '_';
    });
}

bool valid_signal(std::string_view signal) noexcept {
    if (signal.size() < 8U || signal.size() > 256U || signal.find_first_of(" \t\r\n") != std::string_view::npos) {
        return false;
    }
    return signal.starts_with("ws://") || signal.starts_with("wss://");
}

bool looks_like_nonempty_ice_array(std::string_view value) noexcept {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1U]))) --end;
    if (end - begin < 3U || value[begin] != '[' || value[end - 1U] != ']') return false;
    for (std::size_t index = begin + 1U; index + 1U < end; ++index) {
        if (!std::isspace(static_cast<unsigned char>(value[index]))) return true;
    }
    return false;
}

bool read_ice(std::string& value) {
    value.reserve(kMaxIceBytes);
    char current = '\0';
    while (std::cin.get(current)) {
        if (current == '\n') break;
        if (value.size() >= kMaxIceBytes || current == '\r' || current == '\0') return false;
        value.push_back(current);
    }
    if (value.empty() || !looks_like_nonempty_ice_array(value)) return false;
    return true;
}

struct Options {
    std::vector<std::string> aliases;
    std::string signal = std::string(kDefaultSignal);
    bool diagnostic_two_alias = false;
};

void emit_record(std::string_view line) noexcept {
    ::flockfile(stdout);
    (void)std::fwrite(line.data(), 1U, line.size(), stdout);
    (void)std::fflush(stdout);
    ::funlockfile(stdout);
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--alias") {
            if (index + 1 >= argc) return false;
            if (options.aliases.size() >= 2U) return false;
            options.aliases.emplace_back(argv[++index]);
        } else if (argument == "--signal") {
            if (index + 1 >= argc) return false;
            options.signal = argv[++index];
        } else if (argument == "--diagnostic-two-alias") {
            options.diagnostic_two_alias = true;
        } else if (argument == "--help") {
            emit_record("Usage: q2dz-controlled-peer --alias q2.server01 [--signal URL]\n"
                        "       q2dz-controlled-peer --diagnostic-two-alias --alias q2.server01"
                        " --alias q2.server02 [--signal URL]\n");
            return false;
        } else {
            return false;
        }
    }
    const bool alias_count_valid = options.diagnostic_two_alias ? options.aliases.size() == 2U
                                                                : options.aliases.size() == 1U;
    const bool aliases_distinct = options.aliases.size() < 2U || options.aliases[0] != options.aliases[1];
    return alias_count_valid && aliases_distinct &&
           std::all_of(options.aliases.begin(), options.aliases.end(),
                       [](const std::string& alias) { return valid_alias(alias); }) &&
           valid_signal(options.signal);
}

void emit_fatal(std::string_view code) {
    std::string line = "{\"event\":\"fatal\",\"code\":\"";
    line += code;
    line += "\"}\n";
    emit_record(line);
}

void emit_ready(std::string_view alias, PeerId peer) {
    std::string line = "{\"event\":\"ready\",\"alias\":\"";
    line += alias;
    line += "\",\"peer\":";
    line += std::to_string(peer);
    line += "}\n";
    emit_record(line);
}

void emit_request(const Options& options, std::string_view kind, PeerId peer, int length) {
    std::string line = "{\"event\":\"request\",";
    if (options.aliases.size() == 1U) {
        line += "\"alias\":\"";
        line += options.aliases.front();
        line += "\"";
    } else {
        line += "\"aliases\":[";
        for (std::size_t index = 0; index != options.aliases.size(); ++index) {
            if (index != 0U) line += ',';
            line += "\"";
            line += options.aliases[index];
            line += "\"";
        }
        line += "]";
    }
    line += ",\"kind\":\"";
    line += kind;
    line += "\",\"peer\":";
    line += std::to_string(peer);
    line += ",\"bytes\":";
    line += std::to_string(length);
    line += "}\n";
    emit_record(line);
}

std::string server_status(const Options& options) {
    const std::string& alias = options.aliases.front();
    std::string server = alias.substr(alias.size() - 2U);
    return std::string("\xff\xff\xff\xffprint\n") +
           "\\hostname\\server" + server + "\\game\\3zb2\\gamedir\\3zb2" +
           "\\mapname\\q2dm1\\levelname\\q2dm1\\protocol\\35" +
           "\\maxclients\\16\\sv_maxclients\\16\\clients\\1\n" +
           "0 50 \"Player1\"\n";
}

std::string_view oob_command(const std::array<unsigned char, kMaxDatagram>& packet,
                             std::size_t length) {
    if (length < 4U || !std::all_of(packet.begin(), packet.begin() + 4,
                                     [](unsigned char value) { return value == 0xffU; })) {
        return {};
    }
    std::size_t end = 4U;
    while (end < length && packet[end] != '\n') ++end;
    while (end > 4U && (packet[end - 1U] == '\r' || packet[end - 1U] == '\0')) --end;
    return std::string_view(reinterpret_cast<const char*>(packet.data() + 4U), end - 4U);
}

bool aliases_registered(const Options& options) {
    return std::all_of(options.aliases.begin(), options.aliases.end(), [](const std::string& alias) {
        return humblenet_p2p_is_alias_registered(alias.c_str()) != 0;
    });
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        emit_fatal("invalid_options");
        return 64;
    }

    std::string ice;
    if (!read_ice(ice)) {
        emit_fatal("invalid_ice_input");
        return 65;
    }
    if (!humblenet_set_iceservers(ice.c_str())) {
        secure_clear(ice);
        emit_fatal("ice_invalid");
        return 65;
    }
    secure_clear(ice);
    if (!humblenet_init()) {
        emit_fatal("runtime_init_failed");
        return 70;
    }
    if (!humblenet_p2p_init(options.signal.c_str(), kGameToken.data(), kGameSecret.data(), nullptr)) {
        emit_fatal("signaling_init_failed");
        humblenet_shutdown();
        return 69;
    }

    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const std::string status = server_status(options);
    const auto alias_ready_deadline = std::chrono::steady_clock::now() + kAliasReadyTimeout;
    PeerId own_peer = 0;
    bool registration_requested = false;
    bool ready_emitted = false;
    std::uint64_t request_count = 0;
    int exit_code = 0;

    while (running != 0) {
        (void)humblenet_p2p_wait(100);
        own_peer = humblenet_p2p_get_my_peer_id();
        if (own_peer != 0 && !registration_requested) {
            for (const auto& alias : options.aliases) {
                if (!humblenet_p2p_register_alias(alias.c_str())) {
                    emit_fatal("alias_registration_failed");
                    exit_code = 1;
                    break;
                }
            }
            if (exit_code != 0) break;
            registration_requested = true;
        }
        if (registration_requested && !ready_emitted) {
            if (aliases_registered(options)) {
                own_peer = humblenet_p2p_get_my_peer_id();
                if (own_peer != 0) {
                    ready_emitted = true;
                    for (const auto& alias : options.aliases) emit_ready(alias, own_peer);
                }
            }
        }
        if (!ready_emitted) {
            if (std::chrono::steady_clock::now() >= alias_ready_deadline) {
                emit_fatal("alias_registration_timeout");
                exit_code = 1;
                break;
            }
            continue;
        }

        for (unsigned packet = 0; packet != 64U; ++packet) {
            std::array<unsigned char, kMaxDatagram> buffer{};
            PeerId from_peer = 0;
            const int length = humblenet_p2p_recvfrom(buffer.data(),
                                                      static_cast<std::uint32_t>(buffer.size()),
                                                      &from_peer, kChannel);
            if (length <= 0) break;
            const auto size = static_cast<std::size_t>(length);
            const auto command = oob_command(buffer, size);
            if (command == "ping") {
                emit_request(options, "ping", from_peer, length);
                std::string response(4U, static_cast<char>(0xff));
                response += "ack\n";
                (void)humblenet_p2p_sendto(response.data(), static_cast<std::uint32_t>(response.size()),
                                            from_peer, SEND_RELIABLE, kChannel);
                ++request_count;
            } else if (command == "status") {
                emit_request(options, "status", from_peer, length);
                (void)humblenet_p2p_sendto(status.data(), static_cast<std::uint32_t>(status.size()),
                                            from_peer, SEND_RELIABLE, kChannel);
                ++request_count;
            }
        }
    }

    humblenet_shutdown();
    std::string stopped = "{\"event\":\"stopped\",\"alias\":\"";
    stopped += options.aliases.front();
    stopped += "\",\"requests\":";
    stopped += std::to_string(request_count);
    stopped += "}\n";
    emit_record(stopped);
    return exit_code;
}
