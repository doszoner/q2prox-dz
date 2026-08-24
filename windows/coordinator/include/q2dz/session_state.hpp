#pragma once
#include <optional>
#include <string_view>

namespace q2dz {

enum class SessionState {
    idle,
    starting_gateway,
    waiting_transport,
    starting_q2,
    active,
    closing,
    closed,
    failed,
};

enum class SessionEvent {
    begin,
    gateway_started,
    transport_ready,
    q2_started,
    q2_exited,
    gateway_exited,
    room_closed,
    owner_stop,
    timeout,
    fatal_error,
    cleanup_complete,
    cleanup_failed,
};

enum class ShutdownCause {
    none,
    q2_exit,
    gateway_exit,
    room_close,
    owner_stop,
    timeout,
    fatal_error,
    cleanup_error,
};

struct TransitionResult {
    bool accepted{};
    SessionState previous{SessionState::idle};
    SessionState current{SessionState::idle};
    ShutdownCause cause{ShutdownCause::none};
    const char* error{};
};

class SessionStateMachine {
public:
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] ShutdownCause cause() const noexcept { return cause_; }
    [[nodiscard]] TransitionResult apply(SessionEvent event) noexcept;
private:
    SessionState state_{SessionState::idle};
    ShutdownCause cause_{ShutdownCause::none};
};

[[nodiscard]] std::string_view to_string(SessionState value) noexcept;
[[nodiscard]] std::string_view to_string(ShutdownCause value) noexcept;
[[nodiscard]] int exit_code_for(ShutdownCause value) noexcept;

} // namespace q2dz
