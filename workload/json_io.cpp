#include "workload/json_io.h"

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

namespace workload {
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

bool parse_tasks(const JsonArray& tasks_array,
                 std::vector<WorkloadStage>& stages,
                 std::string& error) {
    stages.clear();
    stages.reserve(tasks_array.size());
    for (const auto& item : tasks_array) {
        const auto* task_obj = item.as_object();
        if (!task_obj) {
            error = "Task entry must be an object";
            return false;
        }
        const auto name = get_string(*task_obj, "name");
        const auto compute = get_number(*task_obj, "compute_flops");
        const auto memory = get_number(*task_obj, "memory_gb");
        if (!name || !compute || !memory) {
            error = "Task entry missing required fields";
            return false;
        }
        WorkloadStage stage;
        stage.name = *name;
        stage.compute_flops = *compute;
        stage.memory_gb = *memory;

        if (const auto* deps_val = get(*task_obj, "dependencies"); deps_val && deps_val->as_array()) {
            for (const auto& dep_item : *deps_val->as_array()) {
                const auto* dep_name = dep_item.as_string();
                if (!dep_name) {
                    error = "Dependency entry must be a string";
                    return false;
                }
                stage.dependencies.push_back(*dep_name);
            }
        }

        stages.push_back(std::move(stage));
    }
    return true;
}

bool parse_edges(const JsonArray& edges_array, std::vector<WorkloadEdge>& edges, std::string& error) {
    edges.clear();
    edges.reserve(edges_array.size());
    for (const auto& item : edges_array) {
        const auto* edge_obj = item.as_object();
        if (!edge_obj) {
            error = "Edge entry must be an object";
            return false;
        }
        const auto src = get_string(*edge_obj, "src");
        const auto dst = get_string(*edge_obj, "dst");
        const auto tensor = get_number(*edge_obj, "tensor_size_mb");
        if (!src || !dst || !tensor) {
            error = "Edge entry missing required fields";
            return false;
        }
        WorkloadEdge edge;
        edge.src = *src;
        edge.dst = *dst;
        edge.tensor_size_mb = *tensor;
        edges.push_back(std::move(edge));
    }
    return true;
}

}  // namespace

bool load_from_json(const std::string& path, Workload& out, std::string* error) {
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

    const auto name = get_string(*root_obj, "name").value_or("workload");
    const auto* tasks_val = get(*root_obj, "tasks");
    if (!tasks_val || !tasks_val->as_array()) {
        if (error) {
            *error = "Missing or invalid 'tasks' array";
        }
        return false;
    }
    std::vector<WorkloadStage> stages;
    if (!parse_tasks(*tasks_val->as_array(), stages, parse_error)) {
        if (error) {
            *error = parse_error;
        }
        return false;
    }

    std::vector<WorkloadEdge> edges;
    if (const auto* edges_val = get(*root_obj, "edges"); edges_val && edges_val->as_array()) {
        if (!parse_edges(*edges_val->as_array(), edges, parse_error)) {
            if (error) {
                *error = parse_error;
            }
            return false;
        }
    } else if (get(*root_obj, "edges") != nullptr) {
        if (error) {
            *error = "Invalid 'edges' array";
        }
        return false;
    }

    out = Workload(name, std::move(stages), std::move(edges));
    return true;
}

}  // namespace workload
