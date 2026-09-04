#include "multiroom_state.hpp"

#include <algorithm>

namespace q2dz {
namespace {

Target* lookup(std::unordered_map<TargetId, Target>& targets, TargetId id) noexcept {
    const auto it = targets.find(id);
    return it == targets.end() ? nullptr : &it->second;
}

void count_max(std::size_t& maximum, std::size_t length) noexcept {
    maximum = std::max(maximum, length);
}

}  // namespace

Result<RemotePeerId> select_exact_alias_match(
    std::string_view alias,
    const std::vector<std::pair<std::string, RemotePeerId>>& matches) {
    std::optional<RemotePeerId> peer;
    for (const auto& [matched_alias, matched_peer] : matches) {
        if (matched_alias != alias) continue;
        if (peer) return Result<RemotePeerId>::fail("alias_ambiguous");
        peer = matched_peer;
    }
    if (!peer || *peer == 0) return Result<RemotePeerId>::fail("alias_not_found");
    return Result<RemotePeerId>::ok(*peer);
}

bool probe_ack_is_outstanding(bool is_ack, std::uint64_t probe_sent_at) noexcept {
    return is_ack && probe_sent_at != 0;
}

ResolutionAction resolution_action(
    std::uint64_t now, std::uint64_t resolve_deadline,
    bool alias_query_pending, bool peer_ready) noexcept {
    if (resolve_deadline != 0 && now >= resolve_deadline) {
        return ResolutionAction::timeout;
    }
    if (alias_query_pending) return ResolutionAction::wait_for_alias_callback;
    if (!peer_ready) return ResolutionAction::wait_for_peer;
    return ResolutionAction::query_alias;
}

Result<void> MultiroomState::put(TargetId id, std::uint16_t udp_port) {
    if (id == 0) return Result<void>::fail("target id must be nonzero");
    if (udp_port < 1024U) return Result<void>::fail("target UDP port is privileged");
    if (const auto existing = targets_.find(id); existing != targets_.end()) {
        if (existing->second.udp_port == udp_port) return Result<void>::ok();
        return Result<void>::fail("target id is already bound to another endpoint");
    }
    if (targets_.size() >= max_targets) return Result<void>::fail("target capacity exceeded");
    const auto endpoint_taken = std::any_of(targets_.begin(), targets_.end(),
                                            [udp_port](const auto& item) {
                                                return item.second.udp_port == udp_port;
                                            });
    if (endpoint_taken) return Result<void>::fail("target UDP endpoint is already in use");
    targets_.emplace(id, Target{id, udp_port, 0, TargetState::resolving, std::nullopt, {}, {}});
    return Result<void>::ok();
}

bool MultiroomState::drop(TargetId id) noexcept {
    const auto it = targets_.find(id);
    if (it == targets_.end()) return false;
    if (it->second.remote_peer != 0) peers_.erase(it->second.remote_peer);
    targets_.erase(it);
    return true;
}

bool MultiroomState::bind_peer(TargetId id, RemotePeerId peer) {
    Target* target = lookup(targets_, id);
    if (target == nullptr || peer == 0 || target->state == TargetState::failed ||
        target->state == TargetState::closed) return false;
    if (target->remote_peer == peer) return true;
    if (target->remote_peer != 0) return false;
    const auto existing = peers_.find(peer);
    if (existing != peers_.end() && existing->second != id) {
        target->state = TargetState::failed;
        target->failure_code = "duplicate_remote_peer";
        return false;
    }
    target->remote_peer = peer;
    peers_[peer] = id;
    target->state = TargetState::probing;
    target->failure_code.clear();
    return true;
}

bool MultiroomState::mark_probing(TargetId id) noexcept {
    Target* target = lookup(targets_, id);
    if (target == nullptr || target->state == TargetState::failed ||
        target->state == TargetState::closed) {
        return false;
    }
    target->state = TargetState::probing;
    return true;
}

bool MultiroomState::mark_ready(TargetId id) noexcept {
    Target* target = lookup(targets_, id);
    if (target == nullptr || target->remote_peer == 0 ||
        target->state == TargetState::failed || target->state == TargetState::closed) {
        return false;
    }
    target->state = TargetState::ready;
    return true;
}

bool MultiroomState::mark_closed(TargetId id, std::string reason) {
    Target* target = lookup(targets_, id);
    if (target == nullptr || target->state == TargetState::closed ||
        target->state == TargetState::failed) return false;
    if (target->remote_peer != 0) peers_.erase(target->remote_peer);
    target->remote_peer = 0;
    target->state = TargetState::closed;
    target->failure_code = std::move(reason);
    return true;
}

bool MultiroomState::mark_failed(TargetId id, std::string code) {
    Target* target = lookup(targets_, id);
    if (target == nullptr || target->state == TargetState::closed ||
        target->state == TargetState::failed) return false;
    if (target->remote_peer != 0) peers_.erase(target->remote_peer);
    target->remote_peer = 0;
    target->state = TargetState::failed;
    target->failure_code = std::move(code);
    return true;
}

Target* MultiroomState::find(TargetId id) noexcept { return lookup(targets_, id); }

const Target* MultiroomState::find(TargetId id) const noexcept {
    const auto it = targets_.find(id);
    return it == targets_.end() ? nullptr : &it->second;
}

std::optional<TargetId> MultiroomState::target_for_peer(RemotePeerId peer) const {
    const auto it = peers_.find(peer);
    if (it == peers_.end() || !targets_.contains(it->second)) return std::nullopt;
    return it->second;
}

std::vector<TargetId> MultiroomState::target_ids() const {
    std::vector<TargetId> ids;
    ids.reserve(targets_.size());
    for (const auto& [id, unused] : targets_) {
        static_cast<void>(unused);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool MultiroomState::capture_source(TargetId id, UdpSource source) {
    Target* target = lookup(targets_, id);
    if (target == nullptr || source.port == 0 || target->state == TargetState::failed ||
        target->state == TargetState::closed) return false;
    if (!target->captured_source) {
        target->captured_source = source;
        return true;
    }
    if (*target->captured_source == source) return true;
    ++target->counters.second_udp_source_drops;
    ++counters_.second_udp_source_drops;
    return false;
}

bool MultiroomState::source_allowed(TargetId id, UdpSource source) {
    Target* target = lookup(targets_, id);
    if (target == nullptr || target->state == TargetState::failed ||
        target->state == TargetState::closed) return false;
    if (!target->captured_source) return capture_source(id, source);
    if (*target->captured_source == source) return true;
    ++target->counters.second_udp_source_drops;
    ++counters_.second_udp_source_drops;
    return false;
}

std::optional<RemotePeerId> MultiroomState::route_outbound(TargetId id, UdpSource source,
                                                            std::size_t length,
                                                            std::size_t maximum) {
    Target* target = lookup(targets_, id);
    if (target == nullptr || target->remote_peer == 0 ||
        target->state == TargetState::failed || target->state == TargetState::closed) {
        return std::nullopt;
    }
    if (length > maximum) {
        record_oversized_udp(id);
        return std::nullopt;
    }
    if (!source_allowed(id, source)) return std::nullopt;
    record_udp_to_webrtc(id, length);
    return target->remote_peer;
}

std::optional<TargetId> MultiroomState::route_inbound(RemotePeerId peer, std::size_t length) {
    const auto id = target_for_peer(peer);
    if (!id) {
        ++counters_.unknown_peer_drops;
        return std::nullopt;
    }
    const auto target = find(*id);
    if (target == nullptr || target->state == TargetState::failed ||
        target->state == TargetState::closed) {
        ++counters_.unknown_peer_drops;
        return std::nullopt;
    }
    static_cast<void>(length);
    return id;
}

void MultiroomState::record_udp_to_webrtc(TargetId id, std::size_t length) noexcept {
    Target* target = lookup(targets_, id);
    if (target == nullptr) return;
    ++target->counters.udp_to_webrtc;
    ++counters_.udp_to_webrtc;
    count_max(target->counters.max_udp_to_transport_datagram, length);
    count_max(counters_.max_udp_to_transport_datagram, length);
}

void MultiroomState::record_webrtc_to_udp(TargetId id, std::size_t length) noexcept {
    Target* target = lookup(targets_, id);
    if (target == nullptr) return;
    ++target->counters.webrtc_to_udp;
    ++counters_.webrtc_to_udp;
    count_max(target->counters.max_transport_to_udp_datagram, length);
    count_max(counters_.max_transport_to_udp_datagram, length);
}

void MultiroomState::record_oversized_udp(TargetId id) noexcept {
    Target* target = lookup(targets_, id);
    if (target != nullptr) ++target->counters.oversized_udp_drops;
    ++counters_.oversized_udp_drops;
}

void MultiroomState::record_oversized_transport(TargetId id) noexcept {
    Target* target = lookup(targets_, id);
    if (target != nullptr) ++target->counters.oversized_transport_drops;
    ++counters_.oversized_transport_drops;
}

}  // namespace q2dz
