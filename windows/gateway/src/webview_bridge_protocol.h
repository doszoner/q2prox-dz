#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct CadenceSnapshot {
    std::uint64_t timer_ticks;
    std::uint64_t timer_max_gap_ms;
    std::uint64_t wait_calls;
    std::uint64_t alias_poll_iterations;
    std::uint64_t probe_send_count;
    std::uint64_t probe_ack_count;
    std::uint64_t dc_to_native_datagrams;
    std::uint64_t dc_to_native_bytes;
    std::uint64_t dc_to_native_max_gap_ms;
    std::uint64_t native_to_dc_datagrams;
    std::uint64_t native_to_dc_bytes;
    std::uint64_t native_to_dc_max_gap_ms;
};

struct NativeCadence {
    std::uint64_t udp_from_q2_datagrams = 0;
    std::uint64_t udp_from_q2_bytes = 0;
    std::uint64_t udp_to_q2_datagrams = 0;
    std::uint64_t udp_to_q2_bytes = 0;
    std::uint64_t post_to_web_datagrams = 0;
    std::uint64_t web_to_native_datagrams = 0;
    std::uint64_t udp_to_q2_max_gap_ms = 0;
    std::uint64_t post_to_web_max_gap_ms = 0;
};

std::string encode_datagram_message(const std::vector<std::uint8_t> &datagram);
std::vector<std::uint8_t> decode_datagram_message(std::string_view message);
bool record_oversized_datagram(std::size_t size,
                               std::uint64_t &oversized_drops) noexcept;
void record_accepted_datagram(std::size_t size, std::size_t &maximum) noexcept;
bool record_oversized_transport_drop(std::string_view message,
                                     std::uint64_t &oversized_drops) noexcept;
std::optional<std::uint32_t> parse_transport_ready_peer(std::string_view message);
std::optional<std::string> parse_room_closed_reason(std::string_view message);
std::optional<std::string> parse_transport_failed_reason(std::string_view message);
std::optional<CadenceSnapshot> parse_cadence_message(std::string_view message);
void record_cadence_gap(std::uint64_t now, std::uint64_t &previous,
                        std::uint64_t &maximum) noexcept;
