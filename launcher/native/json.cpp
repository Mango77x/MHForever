#include "json.hpp"

#include <cmath>
#include <cstdio>

namespace mhf::launcher {

namespace {

const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

void escape(std::string &out, const std::string &s) {
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c)); // UTF-8 passes through
            }
        }
    }
    out.push_back('"');
}

void append_utf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    int depth = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool lit(std::string_view word) {
        if (s.substr(i, word.size()) != word) return false;
        i += word.size();
        return true;
    }
    std::optional<std::uint32_t> hex4() {
        if (i + 4 > s.size()) return std::nullopt;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    }
    std::optional<std::string> string() {
        if (i >= s.size() || s[i] != '"') return std::nullopt;
        ++i;
        std::string out;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) return std::nullopt;
            const char e = s[i++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto cp = hex4();
                if (!cp) return std::nullopt;
                if (*cp >= 0xD800 && *cp < 0xDC00 && lit("\\u")) {
                    auto lo = hex4();
                    if (!lo || *lo < 0xDC00 || *lo >= 0xE000) return std::nullopt;
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
                }
                append_utf8(out, *cp);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }
    std::optional<Json> value() {
        if (++depth > 64) return std::nullopt;
        ws();
        if (i >= s.size()) return std::nullopt;
        std::optional<Json> result;
        const char c = s[i];
        if (c == '{') {
            ++i;
            Json::Object o;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                result = Json(std::move(o));
            } else {
                while (true) {
                    ws();
                    auto key = string();
                    if (!key) return std::nullopt;
                    ws();
                    if (i >= s.size() || s[i++] != ':') return std::nullopt;
                    auto v = value();
                    if (!v) return std::nullopt;
                    o[*key] = std::move(*v);
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return std::nullopt;
                }
                result = Json(std::move(o));
            }
        } else if (c == '[') {
            ++i;
            Json::Array a;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                while (true) {
                    auto v = value();
                    if (!v) return std::nullopt;
                    a.push_back(std::move(*v));
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return std::nullopt;
                }
            }
            result = Json(std::move(a));
        } else if (c == '"') {
            auto str = string();
            if (!str) return std::nullopt;
            result = Json(std::move(*str));
        } else if (lit("true")) {
            result = Json(true);
        } else if (lit("false")) {
            result = Json(false);
        } else if (lit("null")) {
            result = Json(nullptr);
        } else {
            const std::size_t start = i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    s[i] == '-' || s[i] == '+')) {
                ++i;
            }
            if (i == start) return std::nullopt;
            const std::string num(s.substr(start, i - start));
            char *end = nullptr;
            const double d = std::strtod(num.c_str(), &end);
            if (end == num.c_str() || !std::isfinite(d)) return std::nullopt;
            result = Json(d);
        }
        --depth;
        return result;
    }
};

} // namespace

const Json::Array &Json::as_array() const { return is_array() ? std::get<Array>(v_) : kEmptyArray; }
const Json::Object &Json::as_object() const { return is_object() ? std::get<Object>(v_) : kEmptyObject; }

Json::Array &Json::array() {
    if (!is_array()) v_ = Array{};
    return std::get<Array>(v_);
}
Json::Object &Json::object() {
    if (!is_object()) v_ = Object{};
    return std::get<Object>(v_);
}

const Json &Json::operator[](std::string_view key) const {
    if (!is_object()) return kNull;
    const auto &o = std::get<Object>(v_);
    const auto it = o.find(std::string(key));
    return it == o.end() ? kNull : it->second;
}

bool Json::has(std::string_view key) const {
    return is_object() && std::get<Object>(v_).count(std::string(key)) != 0;
}

void Json::dump_to(std::string &out, bool pretty, int depth) const {
    const auto newline = [&](int d) {
        if (!pretty) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(d) * 2, ' ');
    };
    if (is_null()) {
        out += "null";
    } else if (is_bool()) {
        out += std::get<bool>(v_) ? "true" : "false";
    } else if (is_number()) {
        const double d = std::get<double>(v_);
        char buf[32];
        if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", d);
        else std::snprintf(buf, sizeof buf, "%.17g", d);
        out += buf;
    } else if (is_string()) {
        escape(out, std::get<std::string>(v_));
    } else if (is_array()) {
        const auto &a = std::get<Array>(v_);
        out.push_back('[');
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (k) out.push_back(',');
            newline(depth + 1);
            a[k].dump_to(out, pretty, depth + 1);
        }
        if (!a.empty()) newline(depth);
        out.push_back(']');
    } else {
        const auto &o = std::get<Object>(v_);
        out.push_back('{');
        bool first = true;
        for (const auto &[key, value] : o) {
            if (!first) out.push_back(',');
            first = false;
            newline(depth + 1);
            escape(out, key);
            out += pretty ? ": " : ":";
            value.dump_to(out, pretty, depth + 1);
        }
        if (!o.empty()) newline(depth);
        out.push_back('}');
    }
}

std::string Json::dump(bool pretty) const {
    std::string out;
    dump_to(out, pretty, 0);
    return out;
}

std::optional<Json> Json::parse(std::string_view text) {
    Parser p{text};
    auto v = p.value();
    if (!v) return std::nullopt;
    p.ws();
    if (p.i != text.size()) return std::nullopt;
    return v;
}

} // namespace mhf::launcher
