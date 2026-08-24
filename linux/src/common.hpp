#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace q2dz {

struct Error {
    std::string message;
};

template <typename T>
class Result {
public:
    static Result ok(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }
    static Result fail(std::string message) {
        return Result(std::in_place_index<1>, Error{std::move(message)});
    }
    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }
    T& value() { return std::get<0>(storage_); }
    const T& value() const { return std::get<0>(storage_); }
    const Error& error() const { return std::get<1>(storage_); }
private:
    template <std::size_t Index, typename U>
    explicit Result(std::in_place_index_t<Index>, U&& value)
        : storage_(std::in_place_index<Index>, std::forward<U>(value)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    static Result ok() { return Result(true, {}); }
    static Result fail(std::string message) { return Result(false, Error{std::move(message)}); }
    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const { return error_; }
private:
    Result(bool ok, Error error) : ok_(ok), error_(std::move(error)) {}
    bool ok_;
    Error error_;
};

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd();
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept;
    void reset(int fd = -1) noexcept;
private:
    int fd_ = -1;
};

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
    [[nodiscard]] std::string to_string() const;
};

Result<Endpoint> parse_loopback_endpoint(std::string_view value);
Result<UniqueFd> make_pipe_read(int pipefd[2], bool nonblocking = false);
Result<void> set_nonblocking(int fd);
Result<void> set_cloexec(int fd, bool enabled);
Result<void> write_all(int fd, std::span<const std::byte> data);
Result<void> write_all(int fd, std::string_view data);
Result<std::string> read_all_fd(int fd, std::size_t limit);
Result<std::string> read_file(const std::filesystem::path& path, std::size_t limit);
Result<void> write_private_file(const std::filesystem::path& path, std::string_view data);
std::string random_hex(std::size_t bytes);
std::string errno_message(std::string_view operation);
std::optional<std::filesystem::path> find_executable(std::string_view name);
std::string sanitize_log_line(std::string_view line);
bool contains_sensitive_key(std::string_view text);
void secure_clear(std::string& value) noexcept;
Result<void> disable_core_dumps_and_ptrace();
std::uint64_t monotonic_millis();
std::optional<std::uint64_t> proc_start_time(pid_t pid);
bool process_instance_alive(pid_t pid, std::uint64_t start_time);
std::string process_comm(pid_t pid);
bool process_has_session_marker(pid_t pid, uid_t uid, std::string_view session_id);
Result<void> remove_session_dir_safely(const std::filesystem::path& base,
                                       const std::filesystem::path& session_dir,
                                       std::string_view session_id);
Result<std::filesystem::path> canonical_owned_directory(const std::filesystem::path& path);
std::vector<std::string> current_environment_with(const std::vector<std::pair<std::string,std::string>>& overrides);

} // namespace q2dz
