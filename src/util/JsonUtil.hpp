#pragma once
// Lenient JSON accessors: a missing key or a value of the wrong type reads as 0 / "" /
// false; handlers rely on that.

#include <string>

#include <nlohmann/json.hpp>

namespace tcng::util::jv {

using Json = nlohmann::json;

// j[key] if j is an object containing key, else null.
inline const Json& at(const Json& j, const char* key) {
    static const Json kNull;
    if (!j.is_object())
        return kNull;
    auto it = j.find(key);
    return it == j.end() ? kNull : *it;
}

inline double number(const Json& v) {
    return v.is_number() ? v.get<double>() : 0.0;
}
inline int integer(const Json& v) {
    if (v.is_number_integer())
        return static_cast<int>(v.get<long long>());
    return v.is_number() ? static_cast<int>(v.get<double>()) : 0;
}
inline std::string string(const Json& v) {
    return v.is_string() ? v.get<std::string>() : std::string();
}
inline bool boolean(const Json& v) {
    return v.is_boolean() && v.get<bool>();
}

inline double number(const Json& j, const char* key) { return number(at(j, key)); }
inline int integer(const Json& j, const char* key) { return integer(at(j, key)); }
inline std::string string(const Json& j, const char* key) { return string(at(j, key)); }
inline bool boolean(const Json& j, const char* key) { return boolean(at(j, key)); }

}  // namespace tcng::util::jv

namespace tcng {
namespace jv = util::jv;
}
