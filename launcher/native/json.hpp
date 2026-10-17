#pragma once

// Minimal JSON value for the launcher's settings file and its bridge with the UI (no third-party dependency).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mhf::launcher {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v_(b) {}
    Json(int i) : v_(static_cast<double>(i)) {}
    Json(std::int64_t i) : v_(static_cast<double>(i)) {}
    Json(double d) : v_(d) {}
    Json(const char *s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(v_); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(v_); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(v_); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<Array>(v_); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(v_); }

    [[nodiscard]] bool as_bool(bool fallback = false) const { return is_bool() ? std::get<bool>(v_) : fallback; }
    [[nodiscard]] double as_number(double fallback = 0) const { return is_number() ? std::get<double>(v_) : fallback; }
    [[nodiscard]] std::string as_string(std::string fallback = {}) const {
        return is_string() ? std::get<std::string>(v_) : fallback;
    }
    [[nodiscard]] const Array &as_array() const;
    [[nodiscard]] const Object &as_object() const;
    Array &array();   // converts to an empty array if needed
    Object &object(); // converts to an empty object if needed

    // Object member access; a missing member reads as null.
    [[nodiscard]] const Json &operator[](std::string_view key) const;
    Json &operator[](const std::string &key) { return object()[key]; }
    [[nodiscard]] bool has(std::string_view key) const;

    [[nodiscard]] std::string dump(bool pretty = false) const;
    // Returns nothing on malformed input.
    static std::optional<Json> parse(std::string_view text);

private:
    void dump_to(std::string &out, bool pretty, int depth) const;
    std::variant<std::monostate, bool, double, std::string, Array, Object> v_;
};

} // namespace mhf::launcher
