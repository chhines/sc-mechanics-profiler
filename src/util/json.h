#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace smp::json {

class Value {
  public:
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Value() : storage_(nullptr) {}
    Value(std::nullptr_t) : storage_(nullptr) {}
    Value(bool value) : storage_(value) {}
    Value(double value) : storage_(value) {}
    Value(int value) : storage_(static_cast<double>(value)) {}
    Value(std::string value) : storage_(std::move(value)) {}
    Value(const char* value) : storage_(std::string(value)) {}
    Value(Array value) : storage_(std::move(value)) {}
    Value(Object value) : storage_(std::move(value)) {}

    [[nodiscard]] bool isNull() const;
    [[nodiscard]] bool isObject() const;
    [[nodiscard]] bool isArray() const;
    [[nodiscard]] bool isString() const;
    [[nodiscard]] bool isNumber() const;
    [[nodiscard]] bool isBool() const;
    [[nodiscard]] const Object& asObject() const;
    [[nodiscard]] Object& asObject();
    [[nodiscard]] const Array& asArray() const;
    [[nodiscard]] Array& asArray();
    [[nodiscard]] std::string asString(const std::string& fallback = {}) const;
    [[nodiscard]] double asNumber(double fallback = 0.0) const;
    [[nodiscard]] int asInt(int fallback = 0) const;
    [[nodiscard]] bool asBool(bool fallback = false) const;
    [[nodiscard]] const Value& operator[](const std::string& key) const;
    Value& operator[](const std::string& key);

  private:
    Storage storage_;
};

Value parse(const std::string& text);
Value parseFile(const std::filesystem::path& path);
std::string stringify(const Value& value, int indent = 2);
void writeFile(const std::filesystem::path& path, const Value& value);

} // namespace smp::json
