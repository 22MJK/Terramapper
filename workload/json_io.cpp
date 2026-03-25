#include "workload/json_io.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

std::optional<DType> parse_dtype(const std::string& value) {
    if (value == "fp32") {
        return DType::FP32;
    }
    if (value == "fp64") {
        return DType::FP64;
    }
    if (value == "int32") {
        return DType::INT32;
    }
    if (value == "int64") {
        return DType::INT64;
    }
    return std::nullopt;
}

std::optional<DistKind> parse_dist_kind(const std::string& value) {
    if (value == "none") {
        return DistKind::NONE;
    }
    if (value == "replicated") {
        return DistKind::REPLICATED;
    }
    if (value == "block") {
        return DistKind::BLOCK;
    }
    if (value == "cyclic") {
        return DistKind::CYCLIC;
    }
    return std::nullopt;
}

std::optional<AccessKind> parse_access_kind(const std::string& value) {
    if (value == "dense") {
        return AccessKind::DENSE;
    }
    if (value == "sparse_csr") {
        return AccessKind::SPARSE_CSR;
    }
    if (value == "row-wise") {
        return AccessKind::ROW_WISE;
    }
    if (value == "col-wise") {
        return AccessKind::COL_WISE;
    }
    return std::nullopt;
}

bool parse_device_groups(const JsonArray& groups_array, std::vector<DeviceGroup>& groups, std::string& error) {
    groups.clear();
    groups.reserve(groups_array.size());
    std::unordered_set<std::string> seen;
    for (const auto& item : groups_array) {
        const auto* group_obj = item.as_object();
        if (!group_obj) {
            error = "Device group entry must be an object";
            return false;
        }
        const auto id = get_string(*group_obj, "id");
        if (!id || id->empty()) {
            error = "Device group missing 'id'";
            return false;
        }
        if (!seen.insert(*id).second) {
            error = "Duplicate device group id: " + *id;
            return false;
        }
        DeviceGroup group;
        group.id = *id;
        if (const auto* members_val = get(*group_obj, "members")) {
            if (const auto* members_str = members_val->as_string()) {
                if (*members_str == "all") {
                    group.members.push_back("all");
                } else {
                    error = "Device group 'members' string must be 'all'";
                    return false;
                }
            } else if (const auto* members_arr = members_val->as_array()) {
                for (const auto& member_item : *members_arr) {
                    const auto* member_str = member_item.as_string();
                    if (!member_str) {
                        error = "Device group members must be strings";
                        return false;
                    }
                    group.members.push_back(*member_str);
                }
            } else {
                error = "Device group 'members' must be array or 'all'";
                return false;
            }
        } else {
            error = "Device group missing 'members'";
            return false;
        }
        groups.push_back(std::move(group));
    }
    return true;
}

bool parse_tensors(const JsonArray& tensors_array, std::vector<Tensor>& tensors, std::string& error) {
    tensors.clear();
    tensors.reserve(tensors_array.size());
    std::unordered_set<std::string> seen_ids;
    for (const auto& item : tensors_array) {
        const auto* tensor_obj = item.as_object();
        if (!tensor_obj) {
            error = "Tensor entry must be an object";
            return false;
        }
        const auto id = get_string(*tensor_obj, "id");
        if (!id || id->empty()) {
            error = "Tensor entry missing 'id'";
            return false;
        }
        if (!seen_ids.insert(*id).second) {
            error = "Duplicate tensor id: " + *id;
            return false;
        }

        Tensor tensor;
        tensor.id = *id;
        tensor.name = get_string(*tensor_obj, "name").value_or(*id);

        if (const auto dtype_val = get_string(*tensor_obj, "dtype")) {
            const auto dtype = parse_dtype(*dtype_val);
            if (!dtype) {
                error = "Unsupported dtype: " + *dtype_val;
                return false;
            }
            tensor.dtype = *dtype;
        }

        if (const auto* shape_val = get(*tensor_obj, "shape"); shape_val && shape_val->as_array()) {
            for (const auto& dim_item : *shape_val->as_array()) {
                const auto* dim_num = dim_item.as_number();
                if (!dim_num || !std::isfinite(*dim_num)) {
                    error = "Tensor shape must be numeric";
                    return false;
                }
                double integral = 0.0;
                if (std::modf(*dim_num, &integral) != 0.0) {
                    error = "Tensor shape must be integer";
                    return false;
                }
                tensor.shape.push_back(static_cast<std::int64_t>(integral));
            }
        }

        if (const auto bytes_val = get_number(*tensor_obj, "bytes")) {
            if (*bytes_val < 0.0) {
                error = "Tensor bytes must be non-negative";
                return false;
            }
            tensor.bytes = static_cast<std::uint64_t>(*bytes_val);
        }

        if (const auto* dist_val = get(*tensor_obj, "distribution"); dist_val && dist_val->as_object()) {
            const auto* dist_obj = dist_val->as_object();
            if (const auto kind_val = get_string(*dist_obj, "kind")) {
                const auto kind = parse_dist_kind(*kind_val);
                if (!kind) {
                    error = "Unsupported distribution kind: " + *kind_val;
                    return false;
                }
                tensor.distribution.kind = *kind;
            }
            if (const auto axis_val = get_int(*dist_obj, "axis")) {
                tensor.distribution.axis = *axis_val;
            }
            if (const auto group_val = get_string(*dist_obj, "group")) {
                tensor.distribution.group = *group_val;
            }
        }

        if (const auto access_val = get_string(*tensor_obj, "access_pattern")) {
            const auto access = parse_access_kind(*access_val);
            if (!access) {
                error = "Unsupported access_pattern: " + *access_val;
                return false;
            }
            tensor.access_pattern = *access;
        }

        if (const auto* collect_val = get(*tensor_obj, "collective_hint"); collect_val && collect_val->as_object()) {
            const auto* collect_obj = collect_val->as_object();
            CollectiveHint hint;
            if (const auto type_val = get_string(*collect_obj, "type")) {
                hint.type = *type_val;
            }
            if (const auto op_val = get_string(*collect_obj, "op")) {
                hint.op = *op_val;
            }
            if (const auto group_val = get_string(*collect_obj, "group")) {
                hint.group = *group_val;
            }
            if (!hint.type.empty()) {
                tensor.collective = hint;
            }
        }

        if (const auto producer_val = get_int(*tensor_obj, "producer")) {
            tensor.producer_task = *producer_val;
        }

        tensors.push_back(std::move(tensor));
    }
    return true;
}

bool parse_tasks(const JsonArray& tasks_array, std::vector<Task>& tasks, std::string& error) {
    tasks.clear();
    tasks.reserve(tasks_array.size());
    std::unordered_set<int> seen_ids;
    std::unordered_set<std::string> seen_names;
    for (const auto& item : tasks_array) {
        const auto* task_obj = item.as_object();
        if (!task_obj) {
            error = "Task entry must be an object";
            return false;
        }
        const auto id = get_int(*task_obj, "id");
        const auto name = get_string(*task_obj, "name");
        const auto op = get_string(*task_obj, "op");
        if (!id || !name || !op) {
            error = "Task entry missing required fields";
            return false;
        }
        if (!seen_ids.insert(*id).second) {
            error = "Duplicate task id: " + std::to_string(*id);
            return false;
        }
        if (!seen_names.insert(*name).second) {
            error = "Duplicate task name: " + *name;
            return false;
        }

        Task task;
        task.id = *id;
        task.name = *name;
        task.op = *op;
        task.compute_flops = get_number(*task_obj, "compute_flops").value_or(0.0);

        if (const auto* inputs_val = get(*task_obj, "inputs"); inputs_val && inputs_val->as_array()) {
            for (const auto& input_item : *inputs_val->as_array()) {
                const auto* input_obj = input_item.as_object();
                if (!input_obj) {
                    error = "Task input entry must be an object";
                    return false;
                }
                const auto tensor_id = get_string(*input_obj, "tensor");
                if (!tensor_id) {
                    error = "Task input missing 'tensor'";
                    return false;
                }
                TensorUse use;
                use.tensor_id = *tensor_id;
                if (const auto access_val = get_string(*input_obj, "access")) {
                    const auto access = parse_access_kind(*access_val);
                    if (!access) {
                        error = "Unsupported access: " + *access_val;
                        return false;
                    }
                    use.access = *access;
                }
                task.inputs.push_back(std::move(use));
            }
        }

        if (const auto* outputs_val = get(*task_obj, "outputs"); outputs_val && outputs_val->as_array()) {
            for (const auto& output_item : *outputs_val->as_array()) {
                if (const auto* output_str = output_item.as_string()) {
                    task.outputs.push_back(*output_str);
                    continue;
                }
                const auto* output_obj = output_item.as_object();
                if (!output_obj) {
                    error = "Task output entry must be a string or object";
                    return false;
                }
                const auto tensor_id = get_string(*output_obj, "tensor");
                if (!tensor_id) {
                    error = "Task output missing 'tensor'";
                    return false;
                }
                task.outputs.push_back(*tensor_id);
            }
        }

        if (const auto* hint_val = get(*task_obj, "placement_hint"); hint_val && hint_val->as_object()) {
            if (const auto group_val = get_string(*hint_val->as_object(), "group")) {
                task.placement_group = *group_val;
            }
        }

        tasks.push_back(std::move(task));
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

    std::vector<DeviceGroup> groups;
    if (const auto* groups_val = get(*root_obj, "device_groups"); groups_val && groups_val->as_array()) {
        if (!parse_device_groups(*groups_val->as_array(), groups, parse_error)) {
            if (error) {
                *error = parse_error;
            }
            return false;
        }
    }

    const auto* tensors_val = get(*root_obj, "tensors");
    if (!tensors_val || !tensors_val->as_array()) {
        if (error) {
            *error = "Missing or invalid 'tensors' array";
        }
        return false;
    }
    std::vector<Tensor> tensors;
    if (!parse_tensors(*tensors_val->as_array(), tensors, parse_error)) {
        if (error) {
            *error = parse_error;
        }
        return false;
    }

    const auto* tasks_val = get(*root_obj, "tasks");
    if (!tasks_val || !tasks_val->as_array()) {
        if (error) {
            *error = "Missing or invalid 'tasks' array";
        }
        return false;
    }
    std::vector<Task> tasks;
    if (!parse_tasks(*tasks_val->as_array(), tasks, parse_error)) {
        if (error) {
            *error = parse_error;
        }
        return false;
    }

    if (get(*root_obj, "edges") != nullptr) {
        if (error) {
            *error = "Edges are not supported; use tensor producers and task inputs instead";
        }
        return false;
    }

    out = Workload(name, std::move(tasks), std::move(tensors), std::move(groups));
    return true;
}

}  // namespace workload
