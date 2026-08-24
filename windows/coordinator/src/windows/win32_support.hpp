#pragma once
#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace q2dz::win {
class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE h) noexcept : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept { if(this!=&o) reset(o.release()); return *this; }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] explicit operator bool() const noexcept { return h_ && h_!=INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { auto h=h_; h_=nullptr; return h; }
    void reset(HANDLE h=nullptr) noexcept { if(*this) CloseHandle(h_); h_=h; }
private: HANDLE h_{};
};

struct ChildProcess { Handle process; Handle thread; DWORD pid{}; };
struct Pipe { Handle read; Handle write; };

[[nodiscard]] std::wstring last_error_message(DWORD code=GetLastError());
[[nodiscard]] std::wstring quote_arg(std::wstring_view arg);
[[nodiscard]] std::wstring command_line(const std::filesystem::path& exe, const std::vector<std::wstring>& args);
[[nodiscard]] Pipe make_pipe(bool read_inheritable, bool write_inheritable);
[[nodiscard]] Handle open_null(bool write);
[[nodiscard]] ChildProcess launch_suspended(
    const std::filesystem::path& exe,
    const std::vector<std::wstring>& args,
    const std::filesystem::path& cwd,
    HANDLE stdin_h,
    HANDLE stdout_h,
    HANDLE stderr_h,
    const std::vector<HANDLE>& inherited,
    DWORD extra_flags);
[[nodiscard]] std::filesystem::path default_session_root();
[[nodiscard]] std::optional<std::filesystem::path> locate_q2(const std::optional<std::filesystem::path>& explicit_path, const std::filesystem::path& coordinator);
[[nodiscard]] std::vector<unsigned char> read_all_stdin(std::size_t max_size);
void write_all(HANDLE h, const unsigned char* data, std::size_t size);
void secure_clear(std::vector<unsigned char>& data) noexcept;
[[nodiscard]] std::filesystem::path create_session_directory(const std::filesystem::path& root, Handle& lock);
[[nodiscard]] bool remove_tree_with_retry(const std::filesystem::path& path, DWORD total_ms);
void cleanup_stale_sessions(const std::filesystem::path& root);
} // namespace q2dz::win
#endif
