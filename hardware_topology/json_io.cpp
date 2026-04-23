#include "hardware_topology/json_io.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hardware_topology {
namespace {

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array };
    Type type{Type::Null};
    std::variant<std::nullptr_t, bool, double, std::string, JsonObject, JsonArray> value{nullptr};

    const JsonObject* as_object() const {
        return std::get_if<JsonObject>(&value);
    }
    const JsonArray* as_array() const {
        return std::get_if<JsonArray>(&value);
    }
    const std::string* as_string() const {
        return std::get_if<std::string>(&value);
    }
    const double* as_number() const {
        return std::get_if<double>(&value);
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string input) : input_(std::move(input)) {}

    bool parse(JsonValue& out, std::string& error) {
        skip_ws();
        if (!parse_value(out, error)) {
            return false;
        }
        skip_ws();
        if (pos_ != input_.size()) {
            error = "Trailing data after JSON";
            return false;
        }
        return true;
    }

private:
    bool parse_value(JsonValue& out, std::string& error) {
        skip_ws();
        if (pos_ >= input_.size()) {
            error = "Unexpected end of JSON";
            return false;
        }
        const char c = input_[pos_];
        if (c == '{') {
            return parse_object(out, error);
        }
        if (c == '[') {
            return parse_array(out, error);
        }
        if (c == '"') {
            std::string s;
            if (!parse_string(s, error)) {
                return false;
            }
            out.type = JsonValue::Type::String;
            out.value = std::move(s);
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            double num = 0.0;
            if (!parse_number(num, error)) {
                return false;
            }
            out.type = JsonValue::Type::Number;
            out.value = num;
            return true;
        }
        if (match_literal("true")) {
            out.type = JsonValue::Type::Bool;
            out.value = true;
            return true;
        }
        if (match_literal("false")) {
            out.type = JsonValue::Type::Bool;
            out.value = false;
            return true;
        }
        if (match_literal("null")) {
            out.type = JsonValue::Type::Null;
            out.value = nullptr;
            return true;
        }
        error = "Invalid JSON value";
        return false;
    }

    bool parse_object(JsonValue& out, std::string& error) {
        if (input_[pos_] != '{') {
            error = "Expected '{'";
            return false;
        }
        ++pos_;
        skip_ws();
        JsonObject obj;
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            out.type = JsonValue::Type::Object;
            out.value = std::move(obj);
            return true;
        }
        while (pos_ < input_.size()) {
            skip_ws();
            std::string key;
            if (!parse_string(key, error)) {
                return false;
            }
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                error = "Expected ':' after object key";
                return false;
            }
            ++pos_;
            JsonValue value;
            if (!parse_value(value, error)) {
                return false;
            }
            obj.emplace(std::move(key), std::move(value));
            skip_ws();
            if (pos_ >= input_.size()) {
                error = "Unexpected end of object";
                return false;
            }
            if (input_[pos_] == '}') {
                ++pos_;
                out.type = JsonValue::Type::Object;
                out.value = std::move(obj);
                return true;
            }
            if (input_[pos_] != ',') {
                error = "Expected ',' between object items";
                return false;
            }
            ++pos_;
        }
        error = "Unexpected end of object";
        return false;
    }

    bool parse_array(JsonValue& out, std::string& error) {
        if (input_[pos_] != '[') {
            error = "Expected '['";
            return false;
        }
        ++pos_;
        skip_ws();
        JsonArray arr;
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            out.type = JsonValue::Type::Array;
            out.value = std::move(arr);
            return true;
        }
        while (pos_ < input_.size()) {
            JsonValue value;
            if (!parse_value(value, error)) {
                return false;
            }
            arr.push_back(std::move(value));
            skip_ws();
            if (pos_ >= input_.size()) {
                error = "Unexpected end of array";
                return false;
            }
            if (input_[pos_] == ']') {
                ++pos_;
                out.type = JsonValue::Type::Array;
                out.value = std::move(arr);
                return true;
            }
            if (input_[pos_] != ',') {
                error = "Expected ',' between array items";
                return false;
            }
            ++pos_;
            skip_ws();
        }
        error = "Unexpected end of array";
        return false;
    }

    bool parse_string(std::string& out, std::string& error) {
        if (input_[pos_] != '"') {
            error = "Expected string";
            return false;
        }
        ++pos_;
        std::string result;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                out = std::move(result);
                return true;
            }
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    error = "Invalid escape sequence";
                    return false;
                }
                const char esc = input_[pos_++];
                switch (esc) {
                    case '"':
                        result.push_back('"');
                        break;
                    case '\\':
                        result.push_back('\\');
                        break;
                    case '/':
                        result.push_back('/');
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u':
                        if (pos_ + 3 >= input_.size()) {
                            error = "Invalid unicode escape";
                            return false;
                        }
                        // Skip \uXXXX (ASCII subset only).
                        pos_ += 4;
                        result.push_back('?');
                        break;
                    default:
                        error = "Invalid escape sequence";
                        return false;
                }
            } else {
                result.push_back(c);
            }
        }
        error = "Unterminated string";
        return false;
    }

    bool parse_number(double& out, std::string& error) {
        const char* start = input_.c_str() + pos_;
        char* end = nullptr;
        out = std::strtod(start, &end);
        if (end == start) {
            error = "Invalid number";
            return false;
        }
        pos_ = static_cast<size_t>(end - input_.c_str());
        return true;
    }

    bool match_literal(const char* literal) {
        const size_t len = std::strlen(literal);
        if (input_.compare(pos_, len, literal) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    std::string input_;
    size_t pos_{0};
};

const JsonValue* get(const JsonObject& obj, const std::string& key) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<std::string> get_string(const JsonObject& obj, const std::string& key) {
    const auto* value = get(obj, key);
    if (!value) {
        return std::nullopt;
    }
    if (const auto* s = value->as_string()) {
        return *s;
    }
    return std::nullopt;
}

std::optional<double> get_number(const JsonObject& obj, const std::string& key) {
    const auto* value = get(obj, key);
    if (!value) {
        return std::nullopt;
    }
    if (const auto* n = value->as_number()) {
        return *n;
    }
    return std::nullopt;
}

std::optional<int> get_int(const JsonObject& obj, const std::string& key) {
    const auto num = get_number(obj, key);
    if (!num.has_value()) {
        return std::nullopt;
    }
    if (*num < static_cast<double>(std::numeric_limits<int>::min()) ||
        *num > static_cast<double>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*num);
}

std::string canonical_device_type(std::string type) {
    // Normalize supported compute device kinds so downstream code can
    // recognize both existing GPUs and newly added CPUs consistently.
    for (char& ch : type) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (type == "GPU" || type == "CPU") {
        return type;
    }
    return type;
}

}  // namespace

bool load_from_json(const std::string& path, HardwareTopology& out, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) {
            *error = "Failed to open " + path;
        }
        return false;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    std::string content = oss.str();

    JsonParser parser(std::move(content));
    JsonValue root;
    std::string parse_error;
    if (!parser.parse(root, parse_error)) {
        if (error) {
            *error = parse_error;
        }
        return false;
    }

    const auto* root_obj = root.as_object();
    if (!root_obj) {
        if (error) {
            *error = "Root must be a JSON object";
        }
        return false;
    }

    if (const auto time_unit = get_string(*root_obj, "time_unit"); time_unit.has_value()) {
        out.set_time_unit(*time_unit);
    }

    const auto* devices_val = get(*root_obj, "devices");
    if (!devices_val || !devices_val->as_array()) {
        if (error) {
            *error = "Missing or invalid 'devices' array";
        }
        return false;
    }
    for (const auto& item : *devices_val->as_array()) {
        const auto* dev_obj = item.as_object();
        if (!dev_obj) {
            if (error) {
                *error = "Device entry must be an object";
            }
            return false;
        }
        const auto id = get_string(*dev_obj, "id");
        const auto name = get_string(*dev_obj, "name");
        const auto type = get_string(*dev_obj, "type");
        const auto peak = get_number(*dev_obj, "peak_gflops");
        const auto mem_bw = get_number(*dev_obj, "mem_bw_gbps");
        const auto max_concurrent = get_int(*dev_obj, "max_concurrent");
        if (!id || !name || !type || !peak || !mem_bw || !max_concurrent) {
            if (error) {
                *error = "Device entry missing required fields";
            }
            return false;
        }
        Device dev;
        dev.id = *id;
        dev.name = *name;
        // Preserve GPU behavior and recognize CPU as another valid compute type.
        dev.type = canonical_device_type(*type);
        dev.peak_gflops = *peak;
        dev.mem_bw_gbps = *mem_bw;
        dev.max_concurrent = *max_concurrent;
        out.add_device(std::move(dev));
    }

    const auto* links_val = get(*root_obj, "links");
    if (links_val && links_val->as_array()) {
        for (const auto& item : *links_val->as_array()) {
            const auto* link_obj = item.as_object();
            if (!link_obj) {
                if (error) {
                    *error = "Link entry must be an object";
                }
                return false;
            }
            const auto id = get_string(*link_obj, "id");
            const auto src = get_string(*link_obj, "src");
            const auto dst = get_string(*link_obj, "dst");
            const auto bw = get_number(*link_obj, "bw_gbps");
            const auto lat = get_number(*link_obj, "latency_ms");
            if (!id || !src || !dst || !bw || !lat) {
                if (error) {
                    *error = "Link entry missing required fields";
                }
                return false;
            }
            Link link;
            link.id = *id;
            link.src = *src;
            link.dst = *dst;
            link.bw_gbps = *bw;
            link.latency_ms = *lat;
            out.add_link(std::move(link));
        }
    } else if (links_val != nullptr) {
        if (error) {
            *error = "Invalid 'links' array";
        }
        return false;
    }

    return true;
}

}  // namespace hardware_topology
