#pragma once

#include "common.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace q2dz {

using TargetId = std::uint64_t;
using RemotePeerId = std::uint32_t;

struct UdpSource {
    std::uint32_t address = 0;
    std::uint16_t port = 0;

    friend bool operator==(const UdpSource&, const UdpSource&) = default;
};

enum class TargetState { resolving, probing, ready, closed, failed };

struct DatagramCounters {
    std::uint64_t udp_to_webrtc = 0;
    std::uint64_t webrtc_to_udp = 0;
    std::uint64_t oversized_udp_drops = 0;
    std::uint64_t oversized_transport_drops = 0;
    std::uint64_t unknown_peer_drops = 0;
    std::uint64_t second_udp_source_drops = 0;
    std::size_t max_udp_to_transport_datagram = 0;
    std::size_t max_transport_to_udp_datagram = 0;
};

struct Target {
    TargetId id = 0;
    std::uint16_t udp_port = 0;
    RemotePeerId route_peer = 0;
    RemotePeerId concrete_peer = 0;
    TargetState state = TargetState::resolving;
    std::optional<UdpSource> captured_source;
    std::string failure_code;
    DatagramCounters counters;
};

[[nodiscard]] bool probe_ack_is_outstanding(
    bool is_ack, std::uint64_t probe_sent_at) noexcept;
[[nodiscard]] bool probe_send_delivered(int sent, std::size_t expected) noexcept;

class MultiroomState {
public:
    static constexpr std::size_t max_targets = 256;

    [[nodiscard]] Result<void> put(TargetId id, std::uint16_t udp_port);
    [[nodiscard]] bool drop(TargetId id) noexcept;
    [[nodiscard]] bool bind_route_peer(TargetId id, RemotePeerId route_peer);
    [[nodiscard]] bool reserve_concrete_peer(TargetId id, RemotePeerId concrete_peer);
    [[nodiscard]] bool mark_probing(TargetId id) noexcept;
    [[nodiscard]] bool mark_ready(TargetId id) noexcept;
    [[nodiscard]] bool mark_closed(TargetId id, std::string reason);
    [[nodiscard]] bool mark_failed(TargetId id, std::string code);

    [[nodiscard]] Target* find(TargetId id) noexcept;
    [[nodiscard]] const Target* find(TargetId id) const noexcept;
    [[nodiscard]] std::optional<TargetId> target_for_peer(RemotePeerId peer) const;
    [[nodiscard]] std::vector<TargetId> target_ids() const;
    [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }

    [[nodiscard]] bool capture_source(TargetId id, UdpSource source);
    [[nodiscard]] bool source_allowed(TargetId id, UdpSource source);
    [[nodiscard]] std::optional<RemotePeerId> route_outbound(
        TargetId id, UdpSource source, std::size_t length, std::size_t maximum);
    [[nodiscard]] std::optional<TargetId> route_inbound(RemotePeerId peer,
                                                        std::size_t length);

    void record_udp_to_webrtc(TargetId id, std::size_t length) noexcept;
    void record_webrtc_to_udp(TargetId id, std::size_t length) noexcept;
    void record_oversized_udp(TargetId id) noexcept;
    void record_oversized_transport(TargetId id) noexcept;
    [[nodiscard]] const DatagramCounters& counters() const noexcept { return counters_; }

private:
    std::unordered_map<TargetId, Target> targets_;
    std::unordered_map<RemotePeerId, TargetId> route_peers_;
    std::unordered_map<RemotePeerId, TargetId> concrete_peers_;
    DatagramCounters counters_;
};

}  // namespace q2dz
