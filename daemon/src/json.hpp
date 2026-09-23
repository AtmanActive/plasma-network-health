// SPDX-License-Identifier: MIT
// Minimal dependency-free JSON reader/writer.
//
// Scope is deliberately small: this daemon only ever exchanges flat config and
// state documents with the plasmoid, so there is no need to pull in a library.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace nh {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>; // insertion ordered, small documents only

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : m_type(Type::Bool), m_bool(v) {}
    Json(double v) : m_type(Type::Number), m_number(v) {}
    Json(int v) : m_type(Type::Number), m_number(v) {}
    Json(long long v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    Json(unsigned long long v) : m_type(Type::Number), m_number(static_cast<double>(v)) {}
    Json(const char *v) : m_type(Type::String), m_string(v ? v : "") {}
    Json(std::string v) : m_type(Type::String), m_string(std::move(v)) {}

    static Json object() { Json j; j.m_type = Type::Object; return j; }
    static Json array() { Json j; j.m_type = Type::Array; return j; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool def = false) const
    {
        if (m_type == Type::Bool) {
            return m_bool;
        }
        if (m_type == Type::Number) {
            return m_number != 0.0;
        }
        return def;
    }

    double toDouble(double def = 0.0) const { return m_type == Type::Number ? m_number : def; }

    long long toInt(long long def = 0) const
    {
        if (m_type != Type::Number || !std::isfinite(m_number)) {
            return def;
        }
        return static_cast<long long>(m_number < 0 ? m_number - 0.5 : m_number + 0.5);
    }

    std::string toString(const std::string &def = std::string()) const
    {
        return m_type == Type::String ? m_string : def;
    }

    size_t size() const
    {
        if (m_type == Type::Array) {
            return m_array.size();
        }
        if (m_type == Type::Object) {
            return m_object.size();
        }
        return 0;
    }

    const Json &at(size_t index) const
    {
        static const Json nullValue;
        return (m_type == Type::Array && index < m_array.size()) ? m_array[index] : nullValue;
    }

    const Json &operator[](const std::string &key) const
    {
        static const Json nullValue;
        if (m_type == Type::Object) {
            for (const Member &member : m_object) {
                if (member.first == key) {
                    return member.second;
                }
            }
        }
        return nullValue;
    }

    bool contains(const std::string &key) const { return !(*this)[key].isNull(); }

    Json &set(const std::string &key, Json value)
    {
        if (m_type != Type::Object) {
            m_type = Type::Object;
            m_object.clear();
        }
        for (Member &member : m_object) {
            if (member.first == key) {
                member.second = std::move(value);
                return *this;
            }
        }
        m_object.emplace_back(key, std::move(value));
        return *this;
    }

    Json &push(Json value)
    {
        if (m_type != Type::Array) {
            m_type = Type::Array;
            m_array.clear();
        }
        m_array.push_back(std::move(value));
        return *this;
    }

    const Array &items() const { return m_array; }
    const Object &members() const { return m_object; }

    // -- serialisation ----------------------------------------------------

    /// \param asciiOnly escape every non-ASCII byte as \uXXXX. Used for the
    /// payload the widget decodes from base64, so that no UTF-8 handling is
    /// involved anywhere along that path.
    std::string dump(int indent = -1, bool asciiOnly = false) const
    {
        std::string out;
        out.reserve(256);
        write(out, indent, 0, asciiOnly);
        return out;
    }

    // -- parsing ----------------------------------------------------------

    static Json parse(const std::string &text, std::string *error = nullptr)
    {
        Parser parser(text);
        Json value;
        if (!parser.parseValue(value)) {
            if (error) {
                *error = parser.error;
            }
            return Json();
        }
        parser.skipWhitespace();
        if (parser.pos != text.size()) {
            if (error) {
                *error = "trailing data after JSON document";
            }
            return Json();
        }
        if (error) {
            error->clear();
        }
        return value;
    }

private:
    static void writeEscapedCodePoint(std::string &out, unsigned int codePoint)
    {
        char buf[16];
        if (codePoint < 0x10000) {
            std::snprintf(buf, sizeof(buf), "\\u%04x", codePoint);
            out += buf;
            return;
        }
        const unsigned int value = codePoint - 0x10000;
        std::snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", 0xD800 + (value >> 10), 0xDC00 + (value & 0x3FF));
        out += buf;
    }

    static void writeString(std::string &out, const std::string &value, bool asciiOnly = false)
    {
        out.push_back('"');
        for (size_t i = 0; i < value.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (asciiOnly && c >= 0x80) {
                // Decode one UTF-8 sequence and emit it as an escape.
                unsigned int codePoint = 0;
                size_t extra = 0;
                if ((c & 0xE0) == 0xC0) {
                    codePoint = c & 0x1Fu;
                    extra = 1;
                } else if ((c & 0xF0) == 0xE0) {
                    codePoint = c & 0x0Fu;
                    extra = 2;
                } else if ((c & 0xF8) == 0xF0) {
                    codePoint = c & 0x07u;
                    extra = 3;
                } else {
                    out += "\\ufffd"; // stray continuation byte
                    continue;
                }
                if (i + extra >= value.size()) {
                    out += "\\ufffd";
                    break;
                }
                bool valid = true;
                for (size_t k = 1; k <= extra; ++k) {
                    const unsigned char continuation = static_cast<unsigned char>(value[i + k]);
                    if ((continuation & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                    codePoint = (codePoint << 6) | (continuation & 0x3Fu);
                }
                if (!valid) {
                    out += "\\ufffd";
                    continue;
                }
                i += extra;
                writeEscapedCodePoint(out, codePoint);
                continue;
            }
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    writeEscapedCodePoint(out, c);
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }
        }
        out.push_back('"');
    }

    static void writeNumber(std::string &out, double value)
    {
        if (!std::isfinite(value)) {
            out += "null";
            return;
        }
        char buf[40];
        // Integral values are far more readable (and byte-exact) as integers.
        if (value == std::floor(value) && std::fabs(value) < 9.0e15) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        } else {
            std::snprintf(buf, sizeof(buf), "%.10g", value);
        }
        out += buf;
    }

    void write(std::string &out, int indent, int depth, bool asciiOnly) const
    {
        const bool pretty = indent >= 0;
        const std::string pad = pretty ? std::string(size_t(indent) * size_t(depth + 1), ' ') : std::string();
        const std::string padEnd = pretty ? std::string(size_t(indent) * size_t(depth), ' ') : std::string();

        switch (m_type) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += m_bool ? "true" : "false";
            break;
        case Type::Number:
            writeNumber(out, m_number);
            break;
        case Type::String:
            writeString(out, m_string, asciiOnly);
            break;
        case Type::Array:
            if (m_array.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < m_array.size(); ++i) {
                if (i) {
                    out.push_back(',');
                }
                if (pretty) {
                    out.push_back('\n');
                    out += pad;
                }
                m_array[i].write(out, indent, depth + 1, asciiOnly);
            }
            if (pretty) {
                out.push_back('\n');
                out += padEnd;
            }
            out.push_back(']');
            break;
        case Type::Object:
            if (m_object.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < m_object.size(); ++i) {
                if (i) {
                    out.push_back(',');
                }
                if (pretty) {
                    out.push_back('\n');
                    out += pad;
                }
                writeString(out, m_object[i].first, asciiOnly);
                out.push_back(':');
                if (pretty) {
                    out.push_back(' ');
                }
                m_object[i].second.write(out, indent, depth + 1, asciiOnly);
            }
            if (pretty) {
                out.push_back('\n');
                out += padEnd;
            }
            out.push_back('}');
            break;
        }
    }

    struct Parser {
        explicit Parser(const std::string &t) : text(t) {}

        const std::string &text;
        size_t pos = 0;
        int depth = 0;
        std::string error;

        void skipWhitespace()
        {
            while (pos < text.size()) {
                const char c = text[pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ++pos;
                } else {
                    break;
                }
            }
        }

        bool fail(const char *message)
        {
            if (error.empty()) {
                error = std::string(message) + " at offset " + std::to_string(pos);
            }
            return false;
        }

        bool literal(const char *word)
        {
            const size_t len = std::char_traits<char>::length(word);
            if (text.compare(pos, len, word) != 0) {
                return fail("invalid literal");
            }
            pos += len;
            return true;
        }

        bool parseValue(Json &out)
        {
            if (++depth > 64) {
                --depth;
                return fail("nesting too deep");
            }
            const bool ok = parseValueInner(out);
            --depth;
            return ok;
        }

        bool parseValueInner(Json &out)
        {
            skipWhitespace();
            if (pos >= text.size()) {
                return fail("unexpected end of input");
            }
            switch (text[pos]) {
            case 'n':
                if (!literal("null")) {
                    return false;
                }
                out = Json();
                return true;
            case 't':
                if (!literal("true")) {
                    return false;
                }
                out = Json(true);
                return true;
            case 'f':
                if (!literal("false")) {
                    return false;
                }
                out = Json(false);
                return true;
            case '"': {
                std::string value;
                if (!parseString(value)) {
                    return false;
                }
                out = Json(std::move(value));
                return true;
            }
            case '[':
                return parseArray(out);
            case '{':
                return parseObject(out);
            default:
                return parseNumber(out);
            }
        }

        bool parseNumber(Json &out)
        {
            const size_t start = pos;
            if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
                ++pos;
            }
            while (pos < text.size()) {
                const char c = text[pos];
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                    ++pos;
                } else {
                    break;
                }
            }
            if (pos == start) {
                return fail("unexpected character");
            }
            try {
                size_t consumed = 0;
                const std::string token = text.substr(start, pos - start);
                const double value = std::stod(token, &consumed);
                if (consumed != token.size()) {
                    return fail("malformed number");
                }
                out = Json(value);
            } catch (...) {
                return fail("malformed number");
            }
            return true;
        }

        static void appendUtf8(std::string &out, unsigned int cp)
        {
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

        bool parseHex4(unsigned int &out)
        {
            if (pos + 4 > text.size()) {
                return fail("truncated \\u escape");
            }
            out = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = text[pos++];
                out <<= 4;
                if (c >= '0' && c <= '9') {
                    out |= unsigned(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    out |= unsigned(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    out |= unsigned(c - 'A' + 10);
                } else {
                    return fail("invalid hex digit");
                }
            }
            return true;
        }

        bool parseString(std::string &out)
        {
            if (pos >= text.size() || text[pos] != '"') {
                return fail("expected string");
            }
            ++pos;
            out.clear();
            while (true) {
                if (pos >= text.size()) {
                    return fail("unterminated string");
                }
                const char c = text[pos++];
                if (c == '"') {
                    return true;
                }
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (pos >= text.size()) {
                    return fail("unterminated escape");
                }
                const char esc = text[pos++];
                switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!parseHex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
                        pos += 2;
                        unsigned int low = 0;
                        if (!parseHex4(low)) {
                            return false;
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            appendUtf8(out, cp);
                            cp = low;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
                }
            }
        }

        bool parseArray(Json &out)
        {
            ++pos; // '['
            out = Json::array();
            skipWhitespace();
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Json value;
                if (!parseValue(value)) {
                    return false;
                }
                out.push(std::move(value));
                skipWhitespace();
                if (pos >= text.size()) {
                    return fail("unterminated array");
                }
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == ']') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }

        bool parseObject(Json &out)
        {
            ++pos; // '{'
            out = Json::object();
            skipWhitespace();
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                skipWhitespace();
                std::string key;
                if (!parseString(key)) {
                    return false;
                }
                skipWhitespace();
                if (pos >= text.size() || text[pos] != ':') {
                    return fail("expected ':'");
                }
                ++pos;
                Json value;
                if (!parseValue(value)) {
                    return false;
                }
                out.set(key, std::move(value));
                skipWhitespace();
                if (pos >= text.size()) {
                    return fail("unterminated object");
                }
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == '}') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
    };

    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    Array m_array;
    Object m_object;
};

} // namespace nh
