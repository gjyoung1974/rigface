// A small JSON value, enough for the settings file.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rigface {

class Json {
public:
    enum class Type { Null, Bool, Int, Float, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(bool v) : type_(Type::Bool), num_(v) {}
    Json(int v) : type_(Type::Int), num_(v) {}
    Json(int64_t v) : type_(Type::Int), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Float), num_(v) {}
    Json(const char *v) : type_(Type::String), str_(v) {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}
    Json(Array v) : type_(Type::Array), arr_(std::move(v)) {}
    Json(Object v) : type_(Type::Object), obj_(std::move(v)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_number() const { return type_ == Type::Int || type_ == Type::Float; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    // Lenient readers: a value of the wrong type reads as the fallback.
    double num(double fallback = 0.0) const;
    int64_t integer(int64_t fallback = 0) const;
    bool boolean(bool fallback = false) const;      // Python truthiness
    std::string str(const std::string &fallback = "") const;

    /// Member of an object, created (and the value made an object) if missing.
    Json &operator[](const std::string &key);
    /// Member of an object, or null.
    const Json &get(const std::string &key) const;
    bool has(const std::string &key) const;
    Object &object();                               // makes the value an object
    const Object &object() const;

    /// Element of an array; the array is grown with nulls as needed.
    Json &operator[](size_t i);
    const Json &at(size_t i) const;
    size_t size() const;

    /// Parse a document; nullopt if it is not valid JSON.
    static std::optional<Json> parse(const std::string &text);
    std::string dump(int indent = 1) const;

private:
    void write(std::string &out, int indent, int depth) const;

    Type type_ = Type::Null;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

}  // namespace rigface
