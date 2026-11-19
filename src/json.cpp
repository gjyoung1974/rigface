#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rigface {

namespace {

const Json NULL_JSON;
const Json::Object EMPTY_OBJECT;

void append_utf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

void write_string(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char c : s) {
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
                out += char(c);
            }
        }
    }
    out += '"';
}

struct Parser {
    const std::string &s;
    size_t i = 0;
    bool ok = true;

    void skip()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    bool eat(const char *word)
    {
        size_t n = 0;
        while (word[n])
            ++n;
        if (s.compare(i, n, word) != 0)
            return false;
        i += n;
        return true;
    }

    Json fail()
    {
        ok = false;
        return Json();
    }

    bool hex4(uint32_t &v)
    {
        if (i + 4 > s.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool string(std::string &out)
    {
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size())
                return false;
            switch (s[i++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                    uint32_t lo;
                    i += 2;
                    if (!hex4(lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    Json value(int depth = 0)
    {
        skip();
        if (i >= s.size() || depth > 64)
            return fail();
        char c = s[i];
        if (c == '{') {
            ++i;
            Json::Object obj;
            skip();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return Json(std::move(obj));
            }
            while (ok) {
                skip();
                std::string key;
                if (!string(key))
                    return fail();
                skip();
                if (i >= s.size() || s[i++] != ':')
                    return fail();
                obj[key] = value(depth + 1);
                skip();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    return Json(std::move(obj));
                }
                return fail();
            }
            return fail();
        }
        if (c == '[') {
            ++i;
            Json::Array arr;
            skip();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return Json(std::move(arr));
            }
            while (ok) {
                arr.push_back(value(depth + 1));
                skip();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    return Json(std::move(arr));
                }
                return fail();
            }
            return fail();
        }
        if (c == '"') {
            std::string str;
            if (!string(str))
                return fail();
            return Json(std::move(str));
        }
        if (eat("true")) return Json(true);
        if (eat("false")) return Json(false);
        if (eat("null")) return Json();

        size_t start = i;
        bool is_float = false;
        if (i < s.size() && s[i] == '-')
            ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e'
                                || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            if (s[i] == '.' || s[i] == 'e' || s[i] == 'E')
                is_float = true;
            ++i;
        }
        if (i == start)
            return fail();
        std::string text = s.substr(start, i - start);
        char *end = nullptr;
        double v = std::strtod(text.c_str(), &end);
        if (!end || *end)
            return fail();
        return is_float ? Json(v) : Json(static_cast<int64_t>(v));
    }
};

}  // namespace

double Json::num(double fallback) const
{
    return is_number() || type_ == Type::Bool ? num_ : fallback;
}

int64_t Json::integer(int64_t fallback) const
{
    return is_number() || type_ == Type::Bool ? static_cast<int64_t>(num_) : fallback;
}

bool Json::boolean(bool fallback) const
{
    switch (type_) {
    case Type::Bool:
    case Type::Int:
    case Type::Float: return num_ != 0.0;
    case Type::String: return !str_.empty();
    case Type::Array: return !arr_.empty();
    case Type::Object: return !obj_.empty();
    case Type::Null: break;
    }
    return fallback;
}

std::string Json::str(const std::string &fallback) const
{
    return type_ == Type::String ? str_ : fallback;
}

Json::Object &Json::object()
{
    if (type_ != Type::Object) {
        *this = Json(Object());
    }
    return obj_;
}

const Json::Object &Json::object() const
{
    return type_ == Type::Object ? obj_ : EMPTY_OBJECT;
}

Json &Json::operator[](const std::string &key)
{
    return object()[key];
}

const Json &Json::get(const std::string &key) const
{
    if (type_ == Type::Object) {
        auto it = obj_.find(key);
        if (it != obj_.end())
            return it->second;
    }
    return NULL_JSON;
}

bool Json::has(const std::string &key) const
{
    return type_ == Type::Object && obj_.count(key);
}

Json &Json::operator[](size_t i)
{
    if (type_ != Type::Array)
        *this = Json(Array());
    if (arr_.size() <= i)
        arr_.resize(i + 1);
    return arr_[i];
}

const Json &Json::at(size_t i) const
{
    return type_ == Type::Array && i < arr_.size() ? arr_[i] : NULL_JSON;
}

size_t Json::size() const
{
    return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0;
}

std::optional<Json> Json::parse(const std::string &text)
{
    Parser p{text};
    Json v = p.value();
    p.skip();
    if (!p.ok || p.i != text.size())
        return std::nullopt;
    return v;
}

void Json::write(std::string &out, int indent, int depth) const
{
    auto newline = [&](int d) {
        out += '\n';
        out.append(size_t(indent) * d, ' ');
    };
    char buf[40];
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += num_ != 0.0 ? "true" : "false"; break;
    case Type::Int:
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(num_));
        out += buf;
        break;
    case Type::Float:
        if (!std::isfinite(num_)) {
            out += "null";
        } else {
            std::snprintf(buf, sizeof buf, "%.17g", num_);
            for (int prec = 1; prec < 17; ++prec) {        // shortest text that reads back the same
                char s[40];
                std::snprintf(s, sizeof s, "%.*g", prec, num_);
                if (std::strtod(s, nullptr) == num_) {
                    std::snprintf(buf, sizeof buf, "%s", s);
                    break;
                }
            }
            std::string s = buf;
            if (s.find_first_of(".eE") == std::string::npos)
                s += ".0";                                  // keep it a float
            out += s;
        }
        break;
    case Type::String: write_string(out, str_); break;
    case Type::Array:
        if (arr_.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i)
                out += ',';
            newline(depth + 1);
            arr_[i].write(out, indent, depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    case Type::Object: {
        if (obj_.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        bool first = true;
        for (const auto &kv : obj_) {
            if (!first)
                out += ',';
            first = false;
            newline(depth + 1);
            write_string(out, kv.first);
            out += ": ";
            kv.second.write(out, indent, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
    }
}

std::string Json::dump(int indent) const
{
    std::string out;
    write(out, indent, 0);
    return out;
}

}  // namespace rigface
