#include "common.hpp"
#include "alias_query_mailbox.hpp"
#include "gateway_options.h"
#include "json.hpp"
#include "multiroom_state.hpp"
#include "probe_options.h"
#include "protocol.hpp"

#include "humblenet_p2p.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kSignalServer = "wss://net.js-dos.com:444";
constexpr std::string_view kGameToken = "quake2";
constexpr std::string_view kGameSecret = "quake2-secret";
constexpr std::size_t kIceLimit = 64U * 1024U;
constexpr std::size_t kControlReadSize = 8192U;
constexpr std::size_t kMaxControlBytesPerTick = 64U * 1024U;
constexpr std::size_t kMaxControlCommandsPerTick = 32U;
constexpr std::size_t kMaxTransportDatagram = 1500U;
constexpr unsigned kMaxTransportPacketsPerLoop = 64U;
constexpr std::uint64_t kResolveTimeoutMs = 30'000U;
constexpr std::uint64_t kProbeIntervalMs = 1'000U;
constexpr std::uint64_t kProbeTimeoutMs = 2'000U;
constexpr unsigned kProbeFailures = 3U;

volatile std::sig_atomic_t running = 1;

void stop(int) { running = 0; }

bool loopback_source(const sockaddr_in& source) noexcept {
    return source.sin_family == AF_INET &&
           ntohl(source.sin_addr.s_addr) == INADDR_LOOPBACK;
}

std::runtime_error humblenet_error() {
    const char *message = humblenet_get_error();
    return std::runtime_error(message ? message : "unknown HumbleNet error");
}

q2dz::Result<std::string> extract_target_alias(const q2dz::json::Value& root) {
    const auto* alias = q2dz::json::find_first(
        root, {{"alias"}, {"roomAlias"}, {"room", "alias"}, {"net", "alias"}});
    if (alias == nullptr || alias->as_string() == nullptr) {
        return q2dz::Result<std::string>::fail("target JSON must contain an alias");
    }
    const std::string& value = *alias->as_string();
    const bool q2 = value.size() > 3U && value.starts_with("q2.");
    const bool q2ded = value.size() > 11U && value.starts_with("room.q2ded.");
    if (value.size() > 255U || (!q2 && !q2ded) ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        })) {
        return q2dz::Result<std::string>::fail("target JSON alias is unsafe");
    }
    if (const auto* ordered = q2dz::json::find_first(
            root, {{"ordered"}, {"netConfig", "ordered"}, {"room", "netConfig", "ordered"}});
        ordered != nullptr && (ordered->as_bool() == nullptr || *ordered->as_bool())) {
        return q2dz::Result<std::string>::fail("target JSON requires ordered=false");
    }
    return q2dz::Result<std::string>::ok(value);
}

q2dz::Result<q2dz::UniqueFd> bind_loopback_udp(q2dz::Endpoint& endpoint) {
    for (int attempt = 0; attempt != 8; ++attempt) {
        q2dz::UniqueFd socket_fd(
            ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!socket_fd) return q2dz::Result<q2dz::UniqueFd>::fail("UDP socket failed");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_fd.get(), reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0) {
            return q2dz::Result<q2dz::UniqueFd>::fail("UDP loopback bind failed");
        }
        socklen_t address_length = sizeof(address);
        if (::getsockname(socket_fd.get(), reinterpret_cast<sockaddr*>(&address),
                          &address_length) != 0) {
            return q2dz::Result<q2dz::UniqueFd>::fail("UDP endpoint lookup failed");
        }
        const auto port = ntohs(address.sin_port);
        if (address.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || port < 1024U) {
            continue;
        }
        endpoint = {"127.0.0.1", port};
        return q2dz::Result<q2dz::UniqueFd>::ok(std::move(socket_fd));
    }
    return q2dz::Result<q2dz::UniqueFd>::fail("kernel selected a privileged UDP port");
}

struct HumbleNetSession {
    bool initialized = false;

    ~HumbleNetSession() {
        if (initialized) humblenet_shutdown();
    }

    q2dz::Result<void> configure(std::string_view ice_json,
                                 std::string_view signal = kSignalServer) {
        std::string value(ice_json);
        const bool configured = humblenet_set_iceservers(value.c_str()) != 0;
        q2dz::secure_clear(value);
        if (!configured) {
            return q2dz::Result<void>::fail(q2dz::sanitize_log_line(humblenet_error().what()));
        }
        if (initialized) return q2dz::Result<void>::ok();
        if (!humblenet_init()) {
            return q2dz::Result<void>::fail(q2dz::sanitize_log_line(humblenet_error().what()));
        }
        initialized = true;
        std::string signal_value(signal);
        if (!humblenet_p2p_init(signal_value.c_str(), kGameToken.data(),
                                kGameSecret.data(), nullptr)) {
            q2dz::secure_clear(signal_value);
            return q2dz::Result<void>::fail(q2dz::sanitize_log_line(humblenet_error().what()));
        }
        q2dz::secure_clear(signal_value);
        return q2dz::Result<void>::ok();
    }
};

q2dz::Result<q2dz::UniqueFd> bind_legacy_udp(std::uint16_t port) {
    q2dz::UniqueFd socket_fd(
        ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!socket_fd) return q2dz::Result<q2dz::UniqueFd>::fail("UDP socket failed");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket_fd.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        return q2dz::Result<q2dz::UniqueFd>::fail(q2dz::errno_message("UDP bind"));
    }
    return q2dz::Result<q2dz::UniqueFd>::ok(std::move(socket_fd));
}

bool same_client(const sockaddr_in& left, const sockaddr_in& right) noexcept {
    return left.sin_family == right.sin_family &&
           left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
}

std::optional<int> inherited_fd_from_path(std::string_view path) noexcept {
    constexpr std::string_view prefix = "/proc/self/fd/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto number = path.substr(prefix.size());
    if (number.empty()) return std::nullopt;
    int fd = -1;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), fd);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || fd < 0) {
        return std::nullopt;
    }
    return fd;
}

q2dz::Result<std::string> read_legacy_ice(std::string_view path) {
    const auto inherited_fd = inherited_fd_from_path(path);
    if (!inherited_fd) return q2dz::read_file(std::string(path), kIceLimit);

    struct stat metadata{};
    if (::fstat(*inherited_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        return q2dz::Result<std::string>::fail("inherited ICE fd is not a regular file");
    }
    constexpr int required_seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    const int seals = ::fcntl(*inherited_fd, F_GET_SEALS);
    if (seals < 0 || (seals & required_seals) != required_seals) {
        return q2dz::Result<std::string>::fail("inherited ICE fd is not a sealed memfd");
    }

    q2dz::UniqueFd duplicate(::dup(*inherited_fd));
    if (!duplicate || ::lseek(duplicate.get(), 0, SEEK_SET) < 0) {
        return q2dz::Result<std::string>::fail("inherited ICE fd cannot be read");
    }
    return q2dz::read_all_fd(duplicate.get(), kIceLimit);
}

struct TargetRuntime {
    q2dz::UniqueFd socket;
    std::string alias;
    std::uint64_t resolve_deadline = 0;
    std::uint64_t resolution_token = 0;
    std::uint64_t next_probe = 0;
    std::uint64_t probe_sent_at = 0;
    unsigned probe_failures = 0;
    bool alias_query_pending = false;
};

class Gateway {
public:
    explicit Gateway(GatewayOptions options)
        : options_(std::move(options)),
          udp_buffer_(static_cast<std::size_t>(options_.max_datagram) + 1U),
          transport_buffer_(kMaxTransportDatagram) {}

    ~Gateway() {
        for (auto& [unused, target] : targets_) {
            static_cast<void>(unused);
            q2dz::secure_clear(target.alias);
        }
    }

    int run() {
        ::umask(0077);
        (void)::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
        (void)::setpgid(0, 0);
        if (auto hardened = q2dz::disable_core_dumps_and_ptrace(); !hardened) {
            throw std::runtime_error("gateway hardening failed");
        }
        if (auto nonblocking = q2dz::set_nonblocking(STDIN_FILENO); !nonblocking) {
            throw std::runtime_error("control pipe setup failed");
        }
        event_output_.reset(::dup(STDOUT_FILENO));
        if (!event_output_) throw std::runtime_error("event pipe setup failed");
        q2dz::UniqueFd silent_output(
            ::open("/dev/null", O_WRONLY | O_CLOEXEC));
        if (!silent_output || ::dup2(silent_output.get(), STDOUT_FILENO) < 0) {
            throw std::runtime_error("diagnostic output setup failed");
        }

        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGHUP, stop);
        std::signal(SIGPIPE, SIG_IGN);
        while (running && !stop_requested_) {
            const auto poll_data = wait_for_io();
            if (poll_data.control_ready || framer_.queued_line_count() != 0) process_control();
            if (stop_requested_ || !running) break;
            if (network_.initialized) {
                // One runtime-thread pump services every active peer.
                (void)humblenet_p2p_wait(0);
                process_alias_query_results();
                process_transport();
                process_resolutions();
                process_probes();
            }
            process_udp(poll_data);
            emit_heartbeat_if_due();
        }
        return 0;
    }

private:
    struct PollData {
        bool control_ready = false;
        std::vector<std::pair<q2dz::TargetId, short>> target_events;
    };

    PollData wait_for_io() {
        std::vector<pollfd> descriptors;
        descriptors.reserve(targets_.size() + 1U);
        descriptors.push_back({STDIN_FILENO, POLLIN | POLLHUP | POLLERR, 0});

        std::vector<q2dz::TargetId> descriptor_targets;
        descriptor_targets.reserve(targets_.size());
        for (const auto id : state_.target_ids()) {
            const auto it = targets_.find(id);
            if (it == targets_.end() || !it->second.socket) continue;
            descriptors.push_back({it->second.socket.get(), POLLIN | POLLERR | POLLHUP, 0});
            descriptor_targets.push_back(id);
        }

        const auto now = q2dz::monotonic_millis();
        std::uint64_t wake_at = now + 100U;
        if (framer_.queued_line_count() != 0) wake_at = now;
        if (next_heartbeat_ != 0) wake_at = std::min(wake_at, next_heartbeat_);
        for (const auto id : descriptor_targets) {
            const auto runtime = targets_.find(id);
            if (runtime == targets_.end()) continue;
            if (runtime->second.next_probe != 0) {
                wake_at = std::min(wake_at, runtime->second.next_probe);
            }
            if (runtime->second.probe_sent_at != 0) {
                wake_at = std::min(wake_at,
                                   runtime->second.probe_sent_at + kProbeTimeoutMs);
            }
            if (runtime->second.resolve_deadline != 0) {
                wake_at = std::min(wake_at, runtime->second.resolve_deadline);
            }
        }
        const auto timeout_ms = wake_at <= now
            ? 0U
            : std::min<std::uint64_t>(wake_at - now, 1000U);
        const int result = ::poll(descriptors.data(), descriptors.size(),
                                  static_cast<int>(timeout_ms));
        if (result < 0 && errno != EINTR) throw std::runtime_error("control poll failed");

        PollData data;
        data.control_ready = (descriptors[0].revents &
                              (POLLIN | POLLHUP | POLLERR)) != 0;
        for (std::size_t index = 1; index < descriptors.size(); ++index) {
            if (descriptors[index].revents != 0) {
                data.target_events.emplace_back(descriptor_targets[index - 1U],
                                                descriptors[index].revents);
            }
        }
        return data;
    }

    void process_control() {
        std::array<char, kControlReadSize> buffer{};
        std::size_t bytes_read = 0;
        std::size_t commands_processed = 0;
        while (!stop_requested_ && running &&
               bytes_read < kMaxControlBytesPerTick &&
               commands_processed < kMaxControlCommandsPerTick) {
            commands_processed += process_queued_commands(
                kMaxControlCommandsPerTick - commands_processed);
            if (stop_requested_ || !running ||
                commands_processed >= kMaxControlCommandsPerTick) return;
            const auto read_size = std::min(
                buffer.size(), kMaxControlBytesPerTick - bytes_read);
            const ssize_t count = ::read(STDIN_FILENO, buffer.data(), read_size);
            if (count > 0) {
                bytes_read += static_cast<std::size_t>(count);
                if (!framer_.feed(std::string_view(buffer.data(),
                                                   static_cast<std::size_t>(count)))) {
                    throw std::runtime_error("invalid HUB control framing");
                }
                commands_processed += process_queued_commands(
                    kMaxControlCommandsPerTick - commands_processed);
                continue;
            }
            if (count == 0) {
                if (!framer_.finish()) throw std::runtime_error("incomplete HUB control framing");
                commands_processed += process_queued_commands(
                    kMaxControlCommandsPerTick - commands_processed);
                control_eof_ = true;
                stop_requested_ = true;
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            throw std::runtime_error("HUB control read failed");
        }
    }

    std::size_t process_queued_commands(std::size_t maximum) {
        std::size_t processed = 0;
        for (auto lines = framer_.drain(maximum); !lines.empty(); lines.pop_front()) {
            auto parsed = parser_.parse(lines.front().view());
            if (!parsed) throw std::runtime_error("invalid HUB command");
            handle_command(std::move(parsed.value()));
            ++processed;
            if (stop_requested_) break;
        }
        return processed;
    }

    void handle_command(q2dz::HubCommand command) {
        switch (command.type) {
            case q2dz::HubCommandType::ice:
                apply_ice(command);
                break;
            case q2dz::HubCommandType::put:
                put_target(command.id_or_generation, command.json);
                break;
            case q2dz::HubCommandType::drop:
                drop_target(command.id_or_generation);
                break;
            case q2dz::HubCommandType::stop:
                stop_requested_ = true;
                break;
        }
    }

    void apply_ice(const q2dz::HubCommand& command) {
        if (command.id_or_generation <= ice_generation_) return;
        std::string ice(reinterpret_cast<const char*>(command.json.data()),
                        command.json.size());
        const auto parsed = q2dz::json::parse(ice);
        if (!parsed || !parsed.value().is_array() || parsed.value().as_array()->empty() ||
            ice.size() > kIceLimit) {
            q2dz::secure_clear(ice);
            throw std::runtime_error("invalid ICE configuration");
        }
        const auto configured = network_.configure(ice);
        q2dz::secure_clear(ice);
        if (!configured) throw std::runtime_error(configured.error().message);
        ice_generation_ = command.id_or_generation;
    }

    void put_target(q2dz::TargetId id, const std::vector<unsigned char>& payload) {
        if (state_.find(id) != nullptr) return;
        std::string json(reinterpret_cast<const char*>(payload.data()), payload.size());
        auto parsed = q2dz::json::parse(json);
        if (!parsed) {
            q2dz::secure_clear(json);
            emit_failed(id, "validate", "invalid_target");
            return;
        }
        auto alias = extract_target_alias(parsed.value());
        if (!alias) {
            q2dz::secure_clear(json);
            emit_failed(id, "validate", "invalid_target");
            return;
        }
        q2dz::secure_clear(json);

        q2dz::Endpoint endpoint;
        auto socket_fd = bind_loopback_udp(endpoint);
        if (!socket_fd) {
            emit_failed(id, "bind", "udp_socket_failed");
            return;
        }
        if (auto added = state_.put(id, endpoint.port); !added) {
            emit_failed(id, "bind", "udp_endpoint_conflict");
            return;
        }

        TargetRuntime runtime;
        runtime.socket = std::move(socket_fd.value());
        runtime.alias = std::move(alias.value());
        runtime.resolve_deadline = q2dz::monotonic_millis() + kResolveTimeoutMs;
        runtime.resolution_token = next_resolution_token_++;
        if (runtime.resolution_token == 0) runtime.resolution_token = next_resolution_token_++;
        targets_.emplace(id, std::move(runtime));
    }

    void drop_target(q2dz::TargetId id) {
        const auto state_target = state_.find(id);
        if (state_target == nullptr) return;
        if (state_target->remote_peer != 0 && network_.initialized) {
            (void)humblenet_p2p_disconnect(state_target->remote_peer);
        }
        static_cast<void>(state_.drop(id));
        erase_runtime(id);
    }

    void erase_runtime(q2dz::TargetId id) {
        const auto it = targets_.find(id);
        if (it == targets_.end()) return;
        q2dz::secure_clear(it->second.alias);
        it->second.socket.reset();
        targets_.erase(it);
    }

    void emit_failed(q2dz::TargetId id, std::string_view stage,
                     std::string_view code) {
        emit(q2dz::GatewayEventType::TargetFailed,
             {{"id", std::to_string(id)}, {"stage", std::string(stage)},
              {"code", std::string(code)}});
    }

    void fail_target(q2dz::TargetId id, std::string_view stage,
                     std::string_view code) {
        const auto target = state_.find(id);
        if (target == nullptr) return;
        if (target->remote_peer != 0 && network_.initialized) {
            (void)humblenet_p2p_disconnect(target->remote_peer);
        }
        static_cast<void>(state_.mark_failed(id, std::string(code)));
        emit_failed(id, stage, code);
        erase_runtime(id);
    }

    void close_target(q2dz::TargetId id, std::string_view reason) {
        const auto target = state_.find(id);
        if (target == nullptr) return;
        if (target->remote_peer != 0 && network_.initialized) {
            (void)humblenet_p2p_disconnect(target->remote_peer);
        }
        static_cast<void>(state_.mark_closed(id, std::string(reason)));
        emit(q2dz::GatewayEventType::RoomClosed,
             {{"id", std::to_string(id)}, {"reason", std::string(reason)}});
        erase_runtime(id);
    }

    void process_udp(const PollData& poll_data) {
        for (const auto& [id, events] : poll_data.target_events) {
            if ((events & POLLIN) == 0) {
                if ((events & (POLLERR | POLLHUP)) != 0) close_target(id, "udp_socket_closed");
                continue;
            }
            const auto runtime = targets_.find(id);
            const auto target = state_.find(id);
            if (runtime == targets_.end() || target == nullptr ||
                target->state == q2dz::TargetState::failed ||
                target->state == q2dz::TargetState::closed) {
                continue;
            }
            for (unsigned packet = 0; packet != kMaxTransportPacketsPerLoop; ++packet) {
                sockaddr_in source{};
                iovec iov{udp_buffer_.data(), udp_buffer_.size()};
                msghdr message{};
                message.msg_name = &source;
                message.msg_namelen = sizeof(source);
                message.msg_iov = &iov;
                message.msg_iovlen = 1;
                const ssize_t length = ::recvmsg(runtime->second.socket.get(), &message,
                                                 MSG_DONTWAIT | MSG_TRUNC);
                if (length == 0) break;
                if (length < 0) {
                    if (errno == EINTR) { --packet; continue; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    close_target(id, "udp_read_failed");
                    break;
                }
                const auto size = static_cast<std::size_t>(length);
                if (!loopback_source(source)) continue;
                if (size > static_cast<std::size_t>(options_.max_datagram)) {
                    state_.record_oversized_udp(id);
                    continue;
                }
                const q2dz::UdpSource udp_source{source.sin_addr.s_addr, source.sin_port};
                if (!state_.source_allowed(id, udp_source)) continue;
                const auto current = state_.find(id);
                if (current == nullptr || current->remote_peer == 0) continue;
                const int sent = humblenet_p2p_sendto(
                    udp_buffer_.data(), static_cast<std::uint32_t>(size),
                    current->remote_peer, SEND_RELIABLE, 0);
                if (sent < 0) {
                    close_target(id, "transport_send_failed");
                    break;
                }
                if (sent == static_cast<int>(size)) state_.record_udp_to_webrtc(id, size);
            }
        }
    }

    void process_transport() {
        for (unsigned packet = 0; packet != kMaxTransportPacketsPerLoop; ++packet) {
            PeerId from_peer = 0;
            const int length = humblenet_p2p_recvfrom(
                transport_buffer_.data(), static_cast<std::uint32_t>(transport_buffer_.size()),
                &from_peer, 0);
            if (length == 0) break;
            if (length < 0) {
                const auto id = state_.target_for_peer(from_peer);
                if (id) close_target(*id, "transport_closed");
                else if (from_peer != 0) {
                    (void)state_.route_inbound(from_peer, 0);
                } else {
                    throw std::runtime_error("transport receive failed");
                }
                continue;
            }
            const auto size = static_cast<std::size_t>(length);
            const auto id = state_.target_for_peer(from_peer);
            if (!id) {
                (void)state_.route_inbound(from_peer, size);
                continue;
            }
            if (size > static_cast<std::size_t>(options_.max_datagram)) {
                state_.record_oversized_transport(*id);
                continue;
            }
            const auto routed = state_.route_inbound(from_peer, size);
            if (!routed) continue;
            const auto target = state_.find(*routed);
            const auto runtime = targets_.find(*routed);
            if (target == nullptr || runtime == targets_.end()) continue;
            const bool ack = q2dz::is_quake_oob_ack(
                std::as_bytes(std::span(transport_buffer_.data(), size)));
            auto& probe = runtime->second;
            if (q2dz::probe_ack_is_outstanding(ack, probe.probe_sent_at)) {
                probe.probe_sent_at = 0;
                probe.probe_failures = 0;
                if (target->state == q2dz::TargetState::probing && state_.mark_ready(*routed)) {
                    probe.resolve_deadline = 0;
                    emit(q2dz::GatewayEventType::TransportReady,
                         {{"id", std::to_string(*routed)},
                          {"udp", "127.0.0.1:" + std::to_string(target->udp_port)},
                          {"peer", std::to_string(target->remote_peer)}});
                }
                continue;
            }
            if (target->state != q2dz::TargetState::ready || !target->captured_source) continue;
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_addr.s_addr = target->captured_source->address;
            destination.sin_port = target->captured_source->port;
            const auto sent = ::sendto(runtime->second.socket.get(), transport_buffer_.data(),
                                       size, MSG_NOSIGNAL,
                                       reinterpret_cast<sockaddr*>(&destination),
                                       sizeof(destination));
            if (sent != static_cast<ssize_t>(size)) {
                close_target(*routed, "udp_write_failed");
                continue;
            }
            state_.record_webrtc_to_udp(*routed, size);
        }
    }

    void process_alias_query_results() {
        auto batch = alias_query_mailbox_->drain();
        if (batch.overflowed) {
            for (const auto id : state_.target_ids()) {
                const auto target = state_.find(id);
                const auto runtime = targets_.find(id);
                if (target == nullptr || runtime == targets_.end() ||
                    target->state != q2dz::TargetState::resolving ||
                    !runtime->second.alias_query_pending) continue;
                fail_target(id, "resolve", "alias_query_overflow");
            }
        }
        for (const auto& result : batch.results) {
            finish_alias_query(result.target_id, result.resolution_token,
                               result.alias, result.matches);
        }
    }

    void process_resolutions() {
        const auto now = q2dz::monotonic_millis();
        const auto ids = state_.target_ids();
        for (const auto id : ids) {
            const auto state_target = state_.find(id);
            const auto runtime = targets_.find(id);
            if (state_target == nullptr || runtime == targets_.end() ||
                state_target->state != q2dz::TargetState::resolving) continue;
            const auto action = q2dz::resolution_action(
                now, runtime->second.resolve_deadline,
                runtime->second.alias_query_pending,
                humblenet_p2p_get_my_peer_id() != 0);
            switch (action) {
                case q2dz::ResolutionAction::timeout:
                    fail_target(id, "resolve", "alias_timeout");
                    break;
                case q2dz::ResolutionAction::wait_for_alias_callback:
                case q2dz::ResolutionAction::wait_for_peer:
                    break;
                case q2dz::ResolutionAction::query_alias:
                    start_alias_query(id, runtime->second);
                    break;
            }
        }
    }

    void start_alias_query(q2dz::TargetId id, TargetRuntime& runtime) {
        const auto token = runtime.resolution_token;
        const std::string alias = runtime.alias;
        const std::string query = "=" + alias;
        const auto mailbox = alias_query_mailbox_;
        runtime.alias_query_pending = true;
        const bool queued = humblenet_p2p_alias_query(
            query.c_str(),
            [mailbox, id, token, alias](
                std::vector<std::pair<std::string, PeerId>> matches) mutable {
                static_cast<void>(mailbox->push(q2dz::AliasQueryResult{
                    id, token, std::move(alias), std::move(matches)}));
            });
        if (!queued) runtime.alias_query_pending = false;
    }

    void finish_alias_query(
        q2dz::TargetId id, std::uint64_t token, std::string_view alias,
        const std::vector<std::pair<std::string, PeerId>>& matches) {
        const auto runtime = targets_.find(id);
        const auto target = state_.find(id);
        if (runtime == targets_.end() || target == nullptr ||
            runtime->second.resolution_token != token || runtime->second.alias != alias ||
            !runtime->second.alias_query_pending ||
            target->state != q2dz::TargetState::resolving) return;
        runtime->second.alias_query_pending = false;

        const auto resolved = q2dz::select_exact_alias_match(alias, matches);
        if (!resolved) {
            fail_target(id, "resolve", resolved.error().message);
            return;
        }
        if (!state_.bind_peer(id, resolved.value())) {
            const auto current = state_.find(id);
            const bool duplicate = current != nullptr &&
                current->failure_code == "duplicate_remote_peer";
            fail_target(id, "resolve", duplicate ? "duplicate_remote_peer" : "peer_bind_failed");
            return;
        }
        runtime->second.resolve_deadline = q2dz::monotonic_millis() + kResolveTimeoutMs;
        runtime->second.next_probe = 0;
    }

    void process_probes() {
        const auto now = q2dz::monotonic_millis();
        const auto ping = q2dz::quake_oob_ping();
        for (const auto id : state_.target_ids()) {
            const auto target = state_.find(id);
            const auto runtime = targets_.find(id);
            if (target == nullptr || runtime == targets_.end() || target->remote_peer == 0 ||
                (target->state != q2dz::TargetState::probing &&
                 target->state != q2dz::TargetState::ready)) continue;
            auto& probe = runtime->second;
            if (target->state == q2dz::TargetState::probing &&
                probe.resolve_deadline != 0 && now >= probe.resolve_deadline) {
                close_target(id, "probe_timeout");
                continue;
            }
            if (probe.probe_sent_at != 0 && now >= probe.probe_sent_at + kProbeTimeoutMs) {
                probe.probe_sent_at = 0;
                ++probe.probe_failures;
                if (probe.probe_failures >= kProbeFailures) {
                    close_target(id, "probe_timeout");
                    continue;
                }
            }
            if (probe.probe_sent_at != 0 || (probe.next_probe != 0 && now < probe.next_probe)) continue;
            const int sent = humblenet_p2p_sendto(
                ping.data(), static_cast<std::uint32_t>(ping.size()), target->remote_peer,
                SEND_RELIABLE, 0);
            probe.next_probe = now + kProbeIntervalMs;
            if (sent < 0) {
                close_target(id, "transport_closed");
            } else if (sent == static_cast<int>(ping.size())) {
                probe.probe_sent_at = now;
            }
        }
    }

    void emit_heartbeat_if_due() {
        const auto now = q2dz::monotonic_millis();
        if (next_heartbeat_ != 0 && now < next_heartbeat_) return;
        std::size_t ready = 0;
        for (const auto id : state_.target_ids()) {
            const auto target = state_.find(id);
            if (target != nullptr && target->state == q2dz::TargetState::ready) ++ready;
        }
        emit(q2dz::GatewayEventType::Heartbeat,
             {{"state", "ok"}, {"targets", std::to_string(state_.size())},
              {"ready", std::to_string(ready)}});
        next_heartbeat_ = now + 1'000U;
    }

    void emit(q2dz::GatewayEventType type,
              const std::map<std::string, std::string, std::less<>>& fields) {
        const std::string line = q2dz::format_hub_event(type, fields);
        if (line.empty()) throw std::runtime_error("gateway event formatting failed");
        std::string framed = line + '\n';
        if (auto written = q2dz::write_all(event_output_.get(), framed); !written) {
            throw std::runtime_error("gateway event write failed");
        }
    }

    GatewayOptions options_;
    q2dz::UniqueFd event_output_;
    std::shared_ptr<q2dz::AliasQueryMailbox> alias_query_mailbox_ =
        std::make_shared<q2dz::AliasQueryMailbox>();
    HumbleNetSession network_;
    q2dz::MultiroomState state_;
    std::unordered_map<q2dz::TargetId, TargetRuntime> targets_;
    q2dz::HubCommandParser parser_{q2dz::HubCommandChannel::hub};
    q2dz::HubCommandFramer framer_;
    std::vector<unsigned char> udp_buffer_;
    std::vector<unsigned char> transport_buffer_;
    std::uint64_t ice_generation_ = 0;
    std::uint64_t next_heartbeat_ = 0;
    std::uint64_t next_resolution_token_ = 1;
    bool control_eof_ = false;
    bool stop_requested_ = false;
};

int run_legacy_gateway(const GatewayOptions& options) {
    ::umask(0077);
    (void)::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
    (void)::setpgid(0, 0);
    if (auto hardened = q2dz::disable_core_dumps_and_ptrace(); !hardened) {
        throw std::runtime_error("gateway hardening failed");
    }

    auto ice_file = read_legacy_ice(options.ice_file);
    if (!ice_file || !valid_ice_json(ice_file.value())) {
        throw std::runtime_error("invalid ICE file");
    }
    HumbleNetSession network;
    const auto configured = network.configure(ice_file.value(), options.signal);
    q2dz::secure_clear(ice_file.value());
    if (!configured) throw std::runtime_error("legacy transport initialization failed");

    auto socket_result = bind_legacy_udp(options.port);
    if (!socket_result) throw std::runtime_error(socket_result.error().message);
    q2dz::UniqueFd socket_fd = std::move(socket_result.value());

    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGHUP, stop);
    std::signal(SIGPIPE, SIG_IGN);
    std::cout.setf(std::ios::unitbuf);

    PeerId own_peer = 0;
    PeerId remote_peer = 0;
    sockaddr_in client{};
    bool have_client = false;
    std::uint64_t udp_to_webrtc = 0;
    std::uint64_t webrtc_to_udp = 0;
    std::size_t max_udp_to_transport_datagram = 0;
    std::size_t max_transport_to_udp_datagram = 0;
    std::uint64_t oversized_udp_drops = 0;
    std::uint64_t oversized_transport_drops = 0;
    std::vector<unsigned char> buffer(static_cast<std::size_t>(options.max_datagram));

    while (running) {
        (void)humblenet_p2p_wait(1);
        if (own_peer == 0) {
            own_peer = humblenet_p2p_get_my_peer_id();
            if (own_peer != 0) {
                remote_peer = humblenet_p2p_virtual_peer_for_alias(options.alias.c_str());
                if (remote_peer == 0) throw std::runtime_error("alias failed");
                std::cout << "GATEWAY_READY bind=127.0.0.1:" << options.port
                          << " own_peer_id=" << own_peer << " alias="
                          << options.alias << '\n';
            }
        }

        sockaddr_in source{};
        iovec iov{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_name = &source;
        message.msg_namelen = sizeof(source);
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        const ssize_t udp_length = ::recvmsg(
            socket_fd.get(), &message, MSG_DONTWAIT | MSG_TRUNC);
        if (udp_length > 0) {
            const auto size = static_cast<std::size_t>(udp_length);
            if (size > static_cast<std::size_t>(options.max_datagram)) {
                ++oversized_udp_drops;
            } else if (have_client && !same_client(client, source)) {
                std::cout << "DROP second_udp_source\n";
            } else if (remote_peer != 0) {
                if (!have_client) {
                    client = source;
                    have_client = true;
                    std::cout << "UDP_CLIENT_CAPTURED\n";
                }
                const int sent = humblenet_p2p_sendto(
                    buffer.data(), static_cast<std::uint32_t>(size),
                    remote_peer, SEND_RELIABLE, 0);
                if (sent < 0) throw humblenet_error();
                if (sent == static_cast<int>(size)) {
                    ++udp_to_webrtc;
                    max_udp_to_transport_datagram = std::max(
                        max_udp_to_transport_datagram, size);
                }
            }
        } else if (udp_length < 0 && errno != EINTR &&
                   errno != EAGAIN && errno != EWOULDBLOCK) {
            throw std::runtime_error(q2dz::errno_message("UDP receive"));
        }

        PeerId from_peer = 0;
        const int web_length = humblenet_p2p_recvfrom(
            buffer.data(), static_cast<std::uint32_t>(buffer.size()), &from_peer, 0);
        const int receive_exit = gateway_receive_exit_code(web_length, from_peer);
        if (receive_exit == 20) {
            std::cerr << "GATEWAY_ROOM_CLOSED peer-closed\n";
            return receive_exit;
        }
        if (receive_exit != 0) throw humblenet_error();
        if (web_length > options.max_datagram) {
            ++oversized_transport_drops;
        } else if (web_length > 0 && have_client && from_peer != 0) {
            const auto size = static_cast<std::size_t>(web_length);
            const ssize_t sent = ::sendto(
                socket_fd.get(), buffer.data(), size, MSG_NOSIGNAL,
                reinterpret_cast<const sockaddr*>(&client), sizeof(client));
            if (sent != static_cast<ssize_t>(size)) {
                throw std::runtime_error(q2dz::errno_message("UDP send"));
            }
            ++webrtc_to_udp;
            max_transport_to_udp_datagram = std::max(
                max_transport_to_udp_datagram, size);
        }
    }

    std::cout << "GATEWAY_STOP udp_to_webrtc=" << udp_to_webrtc
              << " webrtc_to_udp=" << webrtc_to_udp
              << " max_udp_to_transport_datagram=" << max_udp_to_transport_datagram
              << " max_transport_to_udp_datagram=" << max_transport_to_udp_datagram
              << " oversized_udp_drops=" << oversized_udp_drops
              << " oversized_transport_drops=" << oversized_transport_drops << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: q2dz-webrtc-gateway [--max-datagram 1400]\n"
                         "Reads persistent Q2DZ/HUB/2 commands from stdin.\n"
                         "Legacy compatibility contract: --bind 127.0.0.1:PORT "
                         "--signal wss://net.js-dos.com:444 --ice-file PATH "
                         "--alias q2.ID --max-datagram 1400\n";
            return 0;
        }
        const GatewayOptions options = parse_gateway_options(argc, argv);
        if (options.legacy_arguments) return run_legacy_gateway(options);
        return Gateway(options).run();
    } catch (const std::exception& error) {
        const std::string safe = q2dz::sanitize_log_line(error.what());
        std::cerr << "GATEWAY_ERROR " << safe << '\n';
        return gateway_error_exit_code(error.what());
    }
}
