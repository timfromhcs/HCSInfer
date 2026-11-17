#include "hcs/api/json_schema.hpp"

#include <string>

namespace hcs::api {
namespace {

using json = nlohmann::json;

bool type_matches(const std::string& t, const json& v) {
    if (t == "object")
        return v.is_object();
    if (t == "array")
        return v.is_array();
    if (t == "string")
        return v.is_string();
    if (t == "boolean")
        return v.is_boolean();
    if (t == "null")
        return v.is_null();
    if (t == "integer")
        return v.is_number_integer();
    if (t == "number")
        return v.is_number();
    return false;
}

Result<void> fail(const std::string& path, const std::string& msg) {
    return make_error(ErrorCode::InvalidArgument, (path.empty() ? std::string("$") : path) + ": " + msg);
}

Result<void> check(const json& schema, const json& v, const std::string& path, int depth) {
    if (depth > 32)
        return fail(path, "schema nesting too deep");
    if (!schema.is_object())
        return {};
    if (schema.contains("pattern"))
        return fail(path, "schema uses unsupported keyword 'pattern'");

    if (auto it = schema.find("type"); it != schema.end()) {
        bool ok = false;
        if (it->is_string())
            ok = type_matches(it->get<std::string>(), v);
        else if (it->is_array())
            for (const auto& t : *it) ok = ok || (t.is_string() && type_matches(t.get<std::string>(), v));
        if (!ok)
            return fail(path, "expected type " + it->dump());
    }
    if (auto it = schema.find("const"); it != schema.end() && *it != v)
        return fail(path, "must equal " + it->dump());
    if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
        bool found = false;
        for (const auto& e : *it) found = found || e == v;
        if (!found)
            return fail(path, "must be one of " + it->dump());
    }
    if (auto it = schema.find("oneOf"); it != schema.end() && it->is_array()) {
        int n = 0;
        for (const auto& s : *it) n += check(s, v, path, depth + 1).ok() ? 1 : 0;
        if (n != 1)
            return fail(path, "must match exactly one alternative");
    }
    if (auto it = schema.find("anyOf"); it != schema.end() && it->is_array()) {
        bool any = false;
        for (const auto& s : *it) any = any || check(s, v, path, depth + 1).ok();
        if (!any)
            return fail(path, "must match at least one alternative");
    }
    if (v.is_number()) {
        const double d = v.get<double>();
        if (auto it = schema.find("minimum"); it != schema.end() && it->is_number() && d < it->get<double>())
            return fail(path, "must be >= " + it->dump());
        if (auto it = schema.find("maximum"); it != schema.end() && it->is_number() && d > it->get<double>())
            return fail(path, "must be <= " + it->dump());
        if (auto it = schema.find("exclusiveMinimum");
            it != schema.end() && it->is_number() && d <= it->get<double>())
            return fail(path, "must be > " + it->dump());
        if (auto it = schema.find("exclusiveMaximum");
            it != schema.end() && it->is_number() && d >= it->get<double>())
            return fail(path, "must be < " + it->dump());
    }
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        // length in bytes; schemas use it as a bound on memory, not on characters.
        if (auto it = schema.find("minLength");
            it != schema.end() && it->is_number_unsigned() && s.size() < it->get<std::size_t>())
            return fail(path, "string too short");
        if (auto it = schema.find("maxLength");
            it != schema.end() && it->is_number_unsigned() && s.size() > it->get<std::size_t>())
            return fail(path, "string too long");
    }
    if (v.is_array()) {
        if (auto it = schema.find("minItems");
            it != schema.end() && it->is_number_unsigned() && v.size() < it->get<std::size_t>())
            return fail(path, "too few items");
        if (auto it = schema.find("maxItems");
            it != schema.end() && it->is_number_unsigned() && v.size() > it->get<std::size_t>())
            return fail(path, "too many items");
        if (auto it = schema.find("items"); it != schema.end() && it->is_object()) {
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (auto r = check(*it, v[i], path + "/" + std::to_string(i), depth + 1); !r)
                    return r;
            }
        }
    }
    if (v.is_object()) {
        if (auto it = schema.find("required"); it != schema.end() && it->is_array()) {
            for (const auto& k : *it) {
                if (k.is_string() && !v.contains(k.get<std::string>()))
                    return fail(path, "missing required property '" + k.get<std::string>() + "'");
            }
        }
        const json* props = nullptr;
        if (auto it = schema.find("properties"); it != schema.end() && it->is_object())
            props = &*it;
        bool allow_extra = true;
        if (auto it = schema.find("additionalProperties"); it != schema.end() && it->is_boolean())
            allow_extra = it->get<bool>();
        for (auto it = v.begin(); it != v.end(); ++it) {
            const json* sub = nullptr;
            if (props) {
                if (auto p = props->find(it.key()); p != props->end())
                    sub = &*p;
            }
            if (sub) {
                if (auto r = check(*sub, it.value(), path + "/" + it.key(), depth + 1); !r)
                    return r;
            } else if (!allow_extra) {
                return fail(path, "unexpected property '" + it.key() + "'");
            }
        }
    }
    return {};
}

}  // namespace

Result<void> validate_json(const nlohmann::json& schema, const nlohmann::json& value) {
    return check(schema, value, "", 0);
}

}  // namespace hcs::api
