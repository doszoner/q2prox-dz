#pragma once

#include "common.hpp"

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace q2dz::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

struct Value {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Storage storage;

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;
    [[nodiscard]] const bool* as_bool() const noexcept;
    [[nodiscard]] const std::string* as_string() const noexcept;
    [[nodiscard]] const Array* as_array() const noexcept;
    [[nodiscard]] const Object* as_object() const noexcept;
};

Result<Value> parse(std::string_view input, std::size_t max_depth = 64);
std::string stringify(const Value& value);
const Value* find_path(const Value& root, std::initializer_list<std::string_view> path);
const Value* find_first(const Value& root,
                        std::initializer_list<std::initializer_list<std::string_view>> paths);

struct RoomConfig {
    std::string alias;
    Value ice_servers;
    bool ordered = false;
    std::string signaling_url;
    std::string map;
    std::string game;
};

Result<RoomConfig> extract_room_config(const Value& root);

} // namespace q2dz::json
