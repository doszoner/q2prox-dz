#include "q2dz/session_state.hpp"

namespace q2dz {
namespace {
ShutdownCause cause_for(SessionEvent event) noexcept {
    switch (event) {
    case SessionEvent::q2_exited: return ShutdownCause::q2_exit;
    case SessionEvent::gateway_exited: return ShutdownCause::gateway_exit;
    case SessionEvent::room_closed: return ShutdownCause::room_close;
    case SessionEvent::owner_stop: return ShutdownCause::owner_stop;
    case SessionEvent::timeout: return ShutdownCause::timeout;
    case SessionEvent::fatal_error: return ShutdownCause::fatal_error;
    case SessionEvent::cleanup_failed: return ShutdownCause::cleanup_error;
    default: return ShutdownCause::none;
    }
}

bool is_running(SessionState s) noexcept {
    return s == SessionState::starting_gateway || s == SessionState::waiting_transport ||
           s == SessionState::starting_q2 || s == SessionState::active;
}
} // namespace

TransitionResult SessionStateMachine::apply(SessionEvent event) noexcept {
    TransitionResult r{false, state_, state_, cause_, "invalid session transition"};
    auto accept = [&](SessionState next) {
        state_ = next;
        r.accepted = true;
        r.current = next;
        r.cause = cause_;
        r.error = nullptr;
    };

    if (is_running(state_)) {
        const auto c = cause_for(event);
        if (c != ShutdownCause::none) {
            if (cause_ == ShutdownCause::none) cause_ = c;
            accept(SessionState::closing);
            r.cause = cause_;
            return r;
        }
    }

    switch (state_) {
    case SessionState::idle:
        if (event == SessionEvent::begin) accept(SessionState::starting_gateway);
        break;
    case SessionState::starting_gateway:
        if (event == SessionEvent::gateway_started) accept(SessionState::waiting_transport);
        break;
    case SessionState::waiting_transport:
        if (event == SessionEvent::transport_ready) accept(SessionState::starting_q2);
        break;
    case SessionState::starting_q2:
        if (event == SessionEvent::q2_started) accept(SessionState::active);
        break;
    case SessionState::active:
        break;
    case SessionState::closing:
        if (event == SessionEvent::cleanup_complete) accept(SessionState::closed);
        else if (event == SessionEvent::cleanup_failed) {
            cause_ = ShutdownCause::cleanup_error;
            accept(SessionState::failed);
        }
        break;
    case SessionState::closed:
    case SessionState::failed:
        break;
    }
    return r;
}

std::string_view to_string(SessionState v) noexcept {
    switch (v) {
    case SessionState::idle: return "Idle";
    case SessionState::starting_gateway: return "StartingGateway";
    case SessionState::waiting_transport: return "WaitingTransport";
    case SessionState::starting_q2: return "StartingQ2";
    case SessionState::active: return "Active";
    case SessionState::closing: return "Closing";
    case SessionState::closed: return "Closed";
    case SessionState::failed: return "Failed";
    }
    return "Unknown";
}

std::string_view to_string(ShutdownCause v) noexcept {
    switch (v) {
    case ShutdownCause::none: return "none";
    case ShutdownCause::q2_exit: return "q2_exit";
    case ShutdownCause::gateway_exit: return "gateway_exit";
    case ShutdownCause::room_close: return "room_close";
    case ShutdownCause::owner_stop: return "owner_stop";
    case ShutdownCause::timeout: return "timeout";
    case ShutdownCause::fatal_error: return "fatal_error";
    case ShutdownCause::cleanup_error: return "cleanup_error";
    }
    return "unknown";
}

int exit_code_for(ShutdownCause v) noexcept {
    switch (v) {
    case ShutdownCause::none:
    case ShutdownCause::q2_exit:
    case ShutdownCause::owner_stop: return 0;
    case ShutdownCause::room_close: return 10;
    case ShutdownCause::gateway_exit: return 11;
    case ShutdownCause::timeout: return 12;
    case ShutdownCause::fatal_error: return 13;
    case ShutdownCause::cleanup_error: return 14;
    }
    return 13;
}
} // namespace q2dz
