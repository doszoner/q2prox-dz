#include "common.hpp"
#include "json.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <vector>
namespace {

using q2dz::UniqueFd;
namespace fs = std::filesystem;
constexpr std::size_t max_datagram = 1400;

std::atomic<bool> stop_requested{false};
void on_signal(int) { stop_requested.store(true); }

struct AdapterConfig {
    std::vector<std::string> argv;
    std::string working_directory;
    int probe_timeout_ms = 45000;
    int probe_interval_ms = 500;
    int room_close_probe_idle_ms = 15000;
    int room_close_failures = 3;
    std::size_t max_datagram = ::max_datagram;
};

q2dz::Result<AdapterConfig> parse_adapter_config(const q2dz::json::Value& root) {
    AdapterConfig out;
    const auto* object = root.as_object();
    if (!object) return q2dz::Result<AdapterConfig>::fail("adapter config must be an object");
    const auto argv_it = object->find("argv");
    if (argv_it == object->end() || !argv_it->second.as_array() || argv_it->second.as_array()->empty()) {
        return q2dz::Result<AdapterConfig>::fail("adapter config requires non-empty argv array");
    }
    for (const auto& value : *argv_it->second.as_array()) {
        if (!value.as_string() || value.as_string()->empty() || value.as_string()->size() > 4096) {
            return q2dz::Result<AdapterConfig>::fail("adapter argv must contain non-empty strings");
        }
        const auto& item = *value.as_string();
        if (item.find("{ice_json}") != std::string::npos ||
            item.find("{credential}") != std::string::npos ||
            item.find("{token}") != std::string::npos ||
            item.find("{secret}") != std::string::npos) {
            return q2dz::Result<AdapterConfig>::fail("adapter config attempts to place sensitive data in argv");
        }
        out.argv.push_back(item);
    }
    auto integer = [&](std::string_view key, int& target, int min, int max) -> q2dz::Result<void> {
        const auto it = object->find(key);
        if (it == object->end()) return q2dz::Result<void>::ok();
        const auto* number = std::get_if<double>(&it->second.storage);
        if (!number || *number < static_cast<double>(min) || *number > static_cast<double>(max) || std::floor(*number) != *number) {
            return q2dz::Result<void>::fail(std::string(key) + " is outside the accepted range");
        }
        target = static_cast<int>(*number);
        return q2dz::Result<void>::ok();
    };
    if (const auto it = object->find("workingDirectory"); it != object->end()) {
        if (!it->second.as_string()) return q2dz::Result<AdapterConfig>::fail("workingDirectory must be string");
        out.working_directory = *it->second.as_string();
    }
    if (auto r = integer("probeTimeoutMs", out.probe_timeout_ms, 1000, 180000); !r) return q2dz::Result<AdapterConfig>::fail(r.error().message);
    if (auto r = integer("probeIntervalMs", out.probe_interval_ms, 50, 5000); !r) return q2dz::Result<AdapterConfig>::fail(r.error().message);
    if (auto r = integer("roomCloseProbeIdleMs", out.room_close_probe_idle_ms, 5000, 300000); !r) return q2dz::Result<AdapterConfig>::fail(r.error().message);
    if (auto r = integer("roomCloseFailures", out.room_close_failures, 1, 10); !r) return q2dz::Result<AdapterConfig>::fail(r.error().message);
    int configured_max_datagram = static_cast<int>(out.max_datagram);
    if (auto r = integer("maxDatagram", configured_max_datagram, 1400, 1400); !r) return q2dz::Result<AdapterConfig>::fail(r.error().message);
    out.max_datagram = static_cast<std::size_t>(configured_max_datagram);
    return q2dz::Result<AdapterConfig>::ok(std::move(out));
}

q2dz::Result<UniqueFd> bind_loopback_udp(std::uint16_t requested_port, q2dz::Endpoint& endpoint) {
    UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd) return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("socket"));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(requested_port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("bind loopback UDP"));
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("getsockname"));
    }
    endpoint = {"127.0.0.1", ntohs(address.sin_port)};
    if (endpoint.port < 1024) return q2dz::Result<UniqueFd>::fail("kernel selected privileged UDP port");
    return q2dz::Result<UniqueFd>::ok(std::move(fd));
}

struct ReservedPort {
    UniqueFd socket;
    q2dz::Endpoint endpoint;
};

q2dz::Result<ReservedPort> reserve_internal_port() {
    q2dz::Endpoint endpoint;
    auto fd = bind_loopback_udp(0, endpoint);
    if (!fd) return q2dz::Result<ReservedPort>::fail(fd.error().message);
    return q2dz::Result<ReservedPort>::ok(ReservedPort{std::move(fd.value()), std::move(endpoint)});
}

q2dz::Result<UniqueFd> create_ice_memfd(std::string_view ice_json) {
    const int raw = ::memfd_create("q2dz-ice", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (raw < 0) return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("memfd_create"));
    UniqueFd fd(raw);
    if (auto result = q2dz::write_all(fd.get(), ice_json); !result) return q2dz::Result<UniqueFd>::fail(result.error().message);
    if (::lseek(fd.get(), 0, SEEK_SET) < 0) return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("lseek memfd"));
    if (::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
        return q2dz::Result<UniqueFd>::fail(q2dz::errno_message("seal ICE memfd"));
    }
    if (auto result = q2dz::set_cloexec(fd.get(), false); !result) return q2dz::Result<UniqueFd>::fail(result.error().message);
    return q2dz::Result<UniqueFd>::ok(std::move(fd));
}

std::string replace_all(std::string value, std::string_view needle, std::string_view replacement) {
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

q2dz::Result<std::vector<std::string>> expand_args(const AdapterConfig& config,
                                                    const q2dz::json::RoomConfig& room,
                                                    std::uint16_t internal_port,
                                                    int ice_fd) {
    if (room.signaling_url.find('@') != std::string::npos || room.signaling_url.find('?') != std::string::npos || room.signaling_url.find('#') != std::string::npos) {
        return q2dz::Result<std::vector<std::string>>::fail("signaling URL with userinfo/query/fragment may not be placed in argv");
    }
    std::vector<std::string> out;
    for (auto item : config.argv) {
        item = replace_all(std::move(item), "{alias}", room.alias);
        item = replace_all(std::move(item), "{ice_memfd}", "/proc/self/fd/" + std::to_string(ice_fd));
        item = replace_all(std::move(item), "{internal_port}", std::to_string(internal_port));
        item = replace_all(std::move(item), "{internal_endpoint}", "127.0.0.1:" + std::to_string(internal_port));
        if (item.find('{') != std::string::npos || item.find('}') != std::string::npos) {
            return q2dz::Result<std::vector<std::string>>::fail("unknown placeholder in adapter argv");
        }
        out.push_back(std::move(item));
    }
    return q2dz::Result<std::vector<std::string>>::ok(std::move(out));
}

struct Child {
    pid_t pid = -1;
    UniqueFd diagnostics;
};

q2dz::Result<Child> spawn_legacy(const fs::path& executable,
                                 const std::vector<std::string>& arguments,
                                 const AdapterConfig& config) {
    int diagnostics_pipe[2]{};
    if (::pipe2(diagnostics_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        return q2dz::Result<Child>::fail(q2dz::errno_message("pipe2 legacy diagnostics"));
    }
    UniqueFd diagnostics_read(diagnostics_pipe[0]);
    UniqueFd diagnostics_write(diagnostics_pipe[1]);
    const pid_t pid = ::fork();
    if (pid < 0) return q2dz::Result<Child>::fail(q2dz::errno_message("fork legacy gateway"));
    if (pid == 0) {
        (void)::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        (void)::setpgid(0, ::getpgrp());
        if (!config.working_directory.empty() && ::chdir(config.working_directory.c_str()) != 0) _exit(126);
        UniqueFd devnull(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        if (!devnull || ::dup2(devnull.get(), STDIN_FILENO) < 0 ||
            ::dup2(diagnostics_write.get(), STDOUT_FILENO) < 0 ||
            ::dup2(diagnostics_write.get(), STDERR_FILENO) < 0) _exit(126);
        std::vector<std::string> storage;
        storage.push_back(executable.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        _exit(127);
    }
    diagnostics_write.reset();
    return q2dz::Result<Child>::ok(Child{pid, std::move(diagnostics_read)});
}

void drain_diagnostics(int fd, std::string& buffer) {
    std::array<char, 4096> temp{};
    while (true) {
        const ssize_t n = ::read(fd, temp.data(), temp.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        if (n == 0) break;
        buffer.append(temp.data(), static_cast<std::size_t>(n));
        while (true) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            std::cerr << "[legacy] " << q2dz::sanitize_log_line(std::string_view(buffer).substr(0, newline)) << "\n";
            buffer.erase(0, newline + 1);
        }
        if (buffer.size() > 64U * 1024U) buffer.erase(0, buffer.size() - 4096U);
    }
}

void terminate_and_reap(Child& child) {
    if (child.pid <= 0) return;
    (void)::kill(child.pid, SIGTERM);
    int status = 0;
    const auto deadline = q2dz::monotonic_millis() + 250U;
    while (q2dz::monotonic_millis() < deadline) {
        const pid_t waited = ::waitpid(child.pid, &status, WNOHANG);
        if (waited == child.pid || (waited < 0 && errno == ECHILD)) {
            child.pid = -1;
            return;
        }
        if (waited < 0 && errno != EINTR) break;
        ::usleep(10000);
    }
    (void)::kill(child.pid, SIGKILL);
    while (true) {
        const pid_t waited = ::waitpid(child.pid, &status, 0);
        if (waited == child.pid || (waited < 0 && errno == ECHILD)) break;
        if (waited < 0 && errno != EINTR) break;
    }
    child.pid = -1;
}

bool loopback_source(const sockaddr_in& source) { return source.sin_family == AF_INET && ntohl(source.sin_addr.s_addr) == INADDR_LOOPBACK; }

int emit_fatal(std::string stage, std::string code, int exit_code) {
    std::cout << q2dz::format_gateway_event(q2dz::GatewayEventType::Fatal, {{"stage", std::move(stage)}, {"code", std::move(code)}}) << std::flush;
    return exit_code;
}

int emit_room_closed(std::string reason) {
    std::cout << q2dz::format_gateway_event(q2dz::GatewayEventType::RoomClosed, {{"reason", std::move(reason)}}) << std::flush;
    return 20;
}

void emit_child_exit_diagnostic(std::string_view phase, int status) {
    if (WIFEXITED(status)) {
        std::cerr << "legacy child exited " << phase << " code=" << WEXITSTATUS(status) << "\n";
    } else if (WIFSIGNALED(status)) {
        std::cerr << "legacy child exited " << phase << " signal=" << WTERMSIG(status) << "\n";
    } else {
        std::cerr << "legacy child exited " << phase << " status=unknown\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string legacy_path;
    std::string adapter_path;
    std::string bind_value = "127.0.0.1:0";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (arg == "--legacy") { if (!take(legacy_path)) return 64; }
        else if (arg == "--adapter") { if (!take(adapter_path)) return 64; }
        else if (arg == "--bind") { if (!take(bind_value)) return 64; }
        else if (arg == "--managed") { }
        else if (arg == "--runtime-dir") { std::string ignored; if (!take(ignored)) return 64; }
        else if (arg == "--help") {
            std::cout << "Usage: q2dz-managed-gateway --legacy PATH --adapter FILE --bind 127.0.0.1:0\n"
                         "Room configuration is accepted only on stdin.\n";
            return 0;
        } else { std::cerr << "unknown option: " << arg << "\n"; return 64; }
    }
    if (legacy_path.empty() || adapter_path.empty()) { std::cerr << "--legacy and --adapter are required\n"; return 64; }
    if (bind_value != "127.0.0.1:0") return emit_fatal("bind", "loopback-ephemeral-required", 65);
    ::umask(0077);
    if (auto hardened = q2dz::disable_core_dumps_and_ptrace(); !hardened) {
        std::cerr << hardened.error().message << "\n";
        return emit_fatal("hardening", "core-dump-disable-failed", 70);
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    auto room_json_result = q2dz::read_all_fd(STDIN_FILENO, 1024U * 1024U);
    if (!room_json_result) return emit_fatal("input", "room-config-read-failed", 65);
    std::string room_json = std::move(room_json_result.value());
    auto parsed_room = q2dz::json::parse(room_json);
    if (!parsed_room) { q2dz::secure_clear(room_json); return emit_fatal("input", "room-config-invalid", 65); }
    auto room = q2dz::json::extract_room_config(parsed_room.value());
    if (!room) { q2dz::secure_clear(room_json); return emit_fatal("input", "room-config-incomplete", 65); }

    auto adapter_text = q2dz::read_file(adapter_path, 256U * 1024U);
    if (!adapter_text) { q2dz::secure_clear(room_json); return emit_fatal("adapter", "config-read-failed", 66); }
    auto adapter_json = q2dz::json::parse(adapter_text.value());
    if (!adapter_json) { q2dz::secure_clear(room_json); return emit_fatal("adapter", "config-invalid", 66); }
    auto adapter = parse_adapter_config(adapter_json.value());
    if (!adapter) { q2dz::secure_clear(room_json); return emit_fatal("adapter", "config-contract-invalid", 66); }

    std::string ice_json = q2dz::json::stringify(room.value().ice_servers);
    auto ice_memfd = create_ice_memfd(ice_json);
    q2dz::secure_clear(ice_json);
    if (!ice_memfd) { q2dz::secure_clear(room_json); return emit_fatal("ice", "memfd-failed", 70); }

    q2dz::Endpoint public_endpoint;
    auto public_socket = bind_loopback_udp(0, public_endpoint);
    if (!public_socket) { q2dz::secure_clear(room_json); return emit_fatal("bind", "public-udp-failed", 70); }
    auto legacy_executable = q2dz::find_executable(legacy_path);
    if (!legacy_executable) { q2dz::secure_clear(room_json); return emit_fatal("legacy", "executable-not-found", 69); }
    UniqueFd proxy_socket(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!proxy_socket) {
        q2dz::secure_clear(room_json);
        return emit_fatal("proxy", "socket-failed", 70);
    }
    const auto ping = q2dz::quake_oob_ping();
    std::array<std::byte, 65536> datagram{};
    std::string diagnostic_buffer;
    bool ready = false;
    std::optional<Child> child;
    std::uint16_t internal_port = 0;
    constexpr int max_start_attempts = 3;
    bool child_exited_before_ready = false;
    for (int attempt = 1; attempt <= max_start_attempts && !stop_requested.load() && !ready; ++attempt) {
        auto reservation = reserve_internal_port();
        if (!reservation) { q2dz::secure_clear(room_json); return emit_fatal("bind", "internal-port-failed", 70); }
        internal_port = reservation.value().endpoint.port;
        auto args = expand_args(adapter.value(), room.value(), internal_port, ice_memfd.value().get());
        if (!args) { q2dz::secure_clear(room_json); return emit_fatal("adapter", "argument-expansion-failed", 66); }

        // Keep the reservation until the last operation before the known
        // legacy child is spawned; a child bind race is handled by retry.
        reservation.value().socket.reset();
        auto spawned = spawn_legacy(*legacy_executable, args.value(), adapter.value());
        if (!spawned) { q2dz::secure_clear(room_json); return emit_fatal("legacy", "spawn-failed", 70); }
        child.emplace(std::move(spawned.value()));

        sockaddr_in internal_address{};
        internal_address.sin_family = AF_INET;
        internal_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        internal_address.sin_port = htons(internal_port);
        if (::connect(proxy_socket.get(), reinterpret_cast<sockaddr*>(&internal_address), sizeof(internal_address)) != 0) {
            q2dz::secure_clear(room_json);
            terminate_and_reap(*child);
            return emit_fatal("proxy", "connect-failed", 70);
        }

        const std::uint64_t deadline = q2dz::monotonic_millis() + static_cast<std::uint64_t>(adapter.value().probe_timeout_ms);
        std::uint64_t next_probe = 0;
        child_exited_before_ready = false;
        while (!stop_requested.load() && q2dz::monotonic_millis() < deadline) {
            const auto now = q2dz::monotonic_millis();
            if (now >= next_probe) {
                (void)::send(proxy_socket.get(), ping.data(), ping.size(), 0);
                next_probe = now + static_cast<std::uint64_t>(adapter.value().probe_interval_ms);
            }
            pollfd fds[2] = {{proxy_socket.get(), POLLIN, 0}, {child->diagnostics.get(), POLLIN, 0}};
            const int rc = ::poll(fds, 2, 100);
            if (rc < 0 && errno != EINTR) break;
            if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) drain_diagnostics(child->diagnostics.get(), diagnostic_buffer);
            if ((fds[0].revents & POLLIN) != 0) {
                const ssize_t n = ::recv(proxy_socket.get(), datagram.data(), datagram.size(), 0);
                if (n > 0 && q2dz::is_quake_oob_ack(std::span(datagram.data(), static_cast<std::size_t>(n)))) { ready = true; break; }
            }
            int status = 0;
            if (::waitpid(child->pid, &status, WNOHANG) == child->pid) {
                drain_diagnostics(child->diagnostics.get(), diagnostic_buffer);
                emit_child_exit_diagnostic("before readiness", status);
                child.reset();
                child_exited_before_ready = true;
                break;
            }
        }
        if (!ready && !child_exited_before_ready && child) {
            int status = 0;
            if (::waitpid(child->pid, &status, WNOHANG) == child->pid) {
                drain_diagnostics(child->diagnostics.get(), diagnostic_buffer);
                emit_child_exit_diagnostic("before readiness", status);
                child.reset();
                child_exited_before_ready = true;
            }
        }
        if (ready) break;
        if (child_exited_before_ready) continue;
        if (child) {
            terminate_and_reap(*child);
            child.reset();
        }
        q2dz::secure_clear(room_json);
        return emit_fatal("probe", stop_requested.load() ? "stopped" : "q2-oob-timeout", 72);
    }
    q2dz::secure_clear(room_json);
    if (!ready || !child) return emit_fatal("legacy", "exited-before-ready", 71);

    std::cout << q2dz::format_gateway_event(q2dz::GatewayEventType::TransportReady,
                                             {{"udp", public_endpoint.to_string()}, {"peer", "resolved"}}) << std::flush;

    ice_memfd.value().reset();

    bool have_client = false;
    sockaddr_in client_address{};
    socklen_t client_length = sizeof(client_address);
    std::uint64_t last_remote = q2dz::monotonic_millis();
    std::uint64_t probe_sent_at = 0;
    int probe_failures = 0;
    std::size_t max_udp_to_transport_datagram = 0;
    std::size_t max_transport_to_udp_datagram = 0;
    std::uint64_t oversized_udp_drops = 0;
    std::uint64_t oversized_transport_drops = 0;
    while (!stop_requested.load()) {
        pollfd fds[3] = {
            {public_socket.value().get(), POLLIN, 0},
            {proxy_socket.get(), POLLIN, 0},
            {child->diagnostics.get(), POLLIN, 0}
        };
        const int rc = ::poll(fds, 3, 250);
        if (rc < 0 && errno != EINTR) return emit_fatal("proxy", "poll-failed", 70);
        if ((fds[2].revents & POLLIN) != 0) drain_diagnostics(child->diagnostics.get(), diagnostic_buffer);
        if ((fds[0].revents & POLLIN) != 0) {
            sockaddr_in source{};
            socklen_t source_length = sizeof(source);
            const ssize_t n = ::recvfrom(public_socket.value().get(), datagram.data(), datagram.size(), MSG_TRUNC,
                                         reinterpret_cast<sockaddr*>(&source), &source_length);
            if (n > 0) {
                const auto size = static_cast<std::size_t>(n);
                if (size > adapter.value().max_datagram) { ++oversized_udp_drops; continue; }
                if (!loopback_source(source)) continue;
                if (!have_client) { client_address = source; client_length = source_length; have_client = true; }
                else if (source.sin_addr.s_addr != client_address.sin_addr.s_addr || source.sin_port != client_address.sin_port) continue;
                if (::send(proxy_socket.get(), datagram.data(), size, 0) != static_cast<ssize_t>(size)) return emit_fatal("udp", "forward-to-webrtc-failed", 70);
                max_udp_to_transport_datagram = std::max(max_udp_to_transport_datagram, size);
            }
        }
        if ((fds[1].revents & POLLIN) != 0) {
            const ssize_t n = ::recv(proxy_socket.get(), datagram.data(), datagram.size(), MSG_TRUNC);
            if (n > 0) {
                const auto size = static_cast<std::size_t>(n);
                if (size > adapter.value().max_datagram) { ++oversized_transport_drops; continue; }
                max_transport_to_udp_datagram = std::max(max_transport_to_udp_datagram, size);
                last_remote = q2dz::monotonic_millis();
                if (probe_sent_at != 0 && q2dz::is_quake_oob_ack(std::span(datagram.data(), size))) {
                    probe_sent_at = 0;
                    probe_failures = 0;
                } else if (have_client) {
                    if (::sendto(public_socket.value().get(), datagram.data(), size, 0,
                                 reinterpret_cast<sockaddr*>(&client_address), client_length) != static_cast<ssize_t>(size)) {
                        return emit_fatal("udp", "forward-to-q2-failed", 70);
                    }
                }
            }
        }
        int status = 0;
        if (::waitpid(child->pid, &status, WNOHANG) == child->pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 20) return emit_room_closed("transport-closed");
            return emit_fatal("legacy", "transport-exited", 71);
        }
        const auto now = q2dz::monotonic_millis();
        if (probe_sent_at != 0 && now - probe_sent_at >= static_cast<std::uint64_t>(adapter.value().probe_interval_ms * 2)) {
            probe_sent_at = 0;
            ++probe_failures;
            if (probe_failures >= adapter.value().room_close_failures) {
                terminate_and_reap(*child);
                return emit_room_closed("oob-probe-failed");
            }
        }
        if (probe_sent_at == 0 && now - last_remote >= static_cast<std::uint64_t>(adapter.value().room_close_probe_idle_ms)) {
            (void)::send(proxy_socket.get(), ping.data(), ping.size(), 0);
            probe_sent_at = now;
            last_remote = now;
        }
    }
    terminate_and_reap(*child);
    std::cerr << "GATEWAY_STOP max_udp_to_transport_datagram=" << max_udp_to_transport_datagram
              << " max_transport_to_udp_datagram=" << max_transport_to_udp_datagram
              << " oversized_udp_drops=" << oversized_udp_drops
              << " oversized_transport_drops=" << oversized_transport_drops << "\n";
    return 0;
}
