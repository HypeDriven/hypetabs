#pragma once
#include <charconv>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace hype::wire {
constexpr uint32_t max_frame = 65536;
using Value = std::variant<std::string, int64_t, bool>;
using Object = std::map<std::string, Value, std::less<>>;
struct Error : std::runtime_error { Error() : std::runtime_error("Invalid protocol message") {} };
inline bool utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (c < 128) continue;
        uint32_t value; unsigned count; uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) { value = c & 31; count = 1; minimum = 128; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 15; count = 2; minimum = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; count = 3; minimum = 65536; }
        else return false;
        while (count--) {
            if (i == s.size()) return false;
            auto next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
class Parser {
    std::string_view data; size_t pos = 0;
    char take() { if (pos == data.size()) throw Error(); return data[pos++]; }
    void space() { while (pos < data.size() && (data[pos] == ' ' || data[pos] == '\r' || data[pos] == '\n' || data[pos] == '\t')) ++pos; }
    void expect(char c) { space(); if (take() != c) throw Error(); }
    uint32_t hex() {
        uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            char c = take(); unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else throw Error();
            result = result * 16 + digit;
        }
        return result;
    }
    static void append(std::string& out, uint32_t c) {
        if (c <= 127) out += static_cast<char>(c);
        else if (c <= 2047) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
        else if (c <= 65535) { out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
        else { out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
    }
    std::string string() {
        expect('"'); std::string out;
        for (;;) {
            char c = take();
            if (c == '"') break;
            if (static_cast<unsigned char>(c) < 32) throw Error();
            if (c != '\\') { out += c; continue; }
            switch (take()) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                uint32_t code = hex();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (take() != '\\' || take() != 'u') throw Error();
                    auto low = hex(); if (low < 0xdc00 || low > 0xdfff) throw Error();
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) throw Error();
                append(out, code); break;
            }
            default: throw Error();
            }
        }
        if (!utf8(out)) throw Error();
        return out;
    }
    Value value() {
        space(); if (pos == data.size()) throw Error();
        if (data[pos] == '"') return string();
        if (data.substr(pos, 4) == "true") { pos += 4; return true; }
        if (data.substr(pos, 5) == "false") { pos += 5; return false; }
        size_t start = pos;
        if (data[pos] == '-') ++pos;
        if (pos == data.size() || data[pos] < '0' || data[pos] > '9') throw Error();
        if (data[pos] == '0') ++pos;
        else while (pos < data.size() && data[pos] >= '0' && data[pos] <= '9') ++pos;
        int64_t number{};
        auto [end, error] = std::from_chars(data.data() + start, data.data() + pos, number);
        if (error != std::errc{} || end != data.data() + pos) throw Error();
        return number;
    }
public:
    explicit Parser(std::string_view input) : data(input) {}
    Object parse() {
        if (data.empty() || data.size() > max_frame) throw Error();
        Object result; expect('{'); space();
        if (pos < data.size() && data[pos] == '}') ++pos;
        else for (;;) {
            auto key = string(); if (key.empty() || key.size() > 32 || result.size() >= 16) throw Error();
            expect(':'); if (!result.emplace(std::move(key), value()).second) throw Error();
            space(); auto c = take(); if (c == '}') break; if (c != ',') throw Error();
        }
        space(); if (pos != data.size()) throw Error(); return result;
    }
};
inline std::string get_string(const Object& o, std::string_view key, size_t limit, bool empty = false) {
    auto it = o.find(key); if (it == o.end() || !std::holds_alternative<std::string>(it->second)) throw Error();
    const auto& s = std::get<std::string>(it->second);
    if (s.size() > limit || (!empty && s.empty()) || s.find('\0') != std::string::npos) throw Error(); return s;
}
inline int64_t get_int(const Object& o, std::string_view key, int64_t minimum, int64_t maximum) {
    auto it = o.find(key); if (it == o.end() || !std::holds_alternative<int64_t>(it->second)) throw Error();
    auto n = std::get<int64_t>(it->second); if (n < minimum || n > maximum) throw Error(); return n;
}
inline std::string quote(std::string_view value) {
    if (!utf8(value)) throw Error();
    constexpr char digits[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
}
