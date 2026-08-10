#include "util/json.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace smp::json {
namespace {

const Value nullValue{};

class Parser {
  public:
    explicit Parser(const std::string& source) : source_(source) {}

    Value run() {
        skipWhitespace();
        auto value = parseValue();
        skipWhitespace();
        if (position_ != source_.size()) {
            fail("unexpected trailing content");
        }
        return value;
    }

  private:
    Value parseValue() {
        if (position_ >= source_.size())
            fail("expected a value");
        switch (source_[position_]) {
        case '{':
            return parseObject();
        case '[':
            return parseArray();
        case '"':
            return Value(parseString());
        case 't':
            consumeLiteral("true");
            return Value(true);
        case 'f':
            consumeLiteral("false");
            return Value(false);
        case 'n':
            consumeLiteral("null");
            return Value(nullptr);
        default:
            if (source_[position_] == '-' || std::isdigit(static_cast<unsigned char>(source_[position_]))) {
                return Value(parseNumber());
            }
            fail("invalid value");
        }
    }

    Value parseObject() {
        ++position_;
        Value::Object object;
        skipWhitespace();
        if (consume('}'))
            return Value(std::move(object));
        for (;;) {
            skipWhitespace();
            if (position_ >= source_.size() || source_[position_] != '"')
                fail("expected object key");
            auto key = parseString();
            skipWhitespace();
            if (!consume(':'))
                fail("expected ':'");
            skipWhitespace();
            object.emplace(std::move(key), parseValue());
            skipWhitespace();
            if (consume('}'))
                break;
            if (!consume(','))
                fail("expected ',' or '}'");
        }
        return Value(std::move(object));
    }

    Value parseArray() {
        ++position_;
        Value::Array array;
        skipWhitespace();
        if (consume(']'))
            return Value(std::move(array));
        for (;;) {
            skipWhitespace();
            array.push_back(parseValue());
            skipWhitespace();
            if (consume(']'))
                break;
            if (!consume(','))
                fail("expected ',' or ']'");
        }
        return Value(std::move(array));
    }

    std::string parseString() {
        ++position_;
        std::string result;
        while (position_ < source_.size()) {
            const char ch = source_[position_++];
            if (ch == '"')
                return result;
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (position_ >= source_.size())
                fail("unterminated escape");
            switch (source_[position_++]) {
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
            case 'u': {
                if (position_ + 4 > source_.size())
                    fail("invalid unicode escape");
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = source_[position_++];
                    value <<= 4;
                    if (digit >= '0' && digit <= '9')
                        value += static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f')
                        value += static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F')
                        value += static_cast<unsigned>(digit - 'A' + 10);
                    else
                        fail("invalid unicode escape");
                }
                if (value <= 0x7F)
                    result.push_back(static_cast<char>(value));
                else if (value <= 0x7FF) {
                    result.push_back(static_cast<char>(0xC0 | (value >> 6)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                } else {
                    result.push_back(static_cast<char>(0xE0 | (value >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
                break;
            }
            default:
                fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    double parseNumber() {
        const auto start = position_;
        if (source_[position_] == '-')
            ++position_;
        while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_])))
            ++position_;
        if (position_ < source_.size() && source_[position_] == '.') {
            ++position_;
            while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_])))
                ++position_;
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-'))
                ++position_;
            while (position_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[position_])))
                ++position_;
        }
        try {
            return std::stod(source_.substr(start, position_ - start));
        } catch (...) {
            fail("invalid number");
        }
    }

    void consumeLiteral(const char* literal) {
        while (*literal) {
            if (position_ >= source_.size() || source_[position_++] != *literal++)
                fail("invalid literal");
        }
    }

    bool consume(char expected) {
        if (position_ < source_.size() && source_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(position_) + ": " + message);
    }

    const std::string& source_;
    std::size_t position_{};
};

std::string escape(const std::string& input) {
    std::ostringstream out;
    for (const unsigned char ch : input) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

void appendValue(std::ostringstream& out, const Value& value, int indent, int depth) {
    const auto padding = [&](int extra = 0) {
        return std::string(static_cast<std::size_t>((depth + extra) * indent), ' ');
    };
    if (value.isNull()) {
        out << "null";
        return;
    }
    if (value.isObject()) {
        const auto& object = value.asObject();
        out << '{';
        if (!object.empty()) {
            bool first = true;
            for (const auto& [key, item] : object) {
                out << (first ? "\n" : ",\n") << padding(1) << '"' << escape(key) << "\": ";
                appendValue(out, item, indent, depth + 1);
                first = false;
            }
            out << '\n' << padding();
        }
        out << '}';
    } else if (value.isArray()) {
        const auto& array = value.asArray();
        out << '[';
        if (!array.empty()) {
            for (std::size_t i = 0; i < array.size(); ++i) {
                out << (i == 0 ? "\n" : ",\n") << padding(1);
                appendValue(out, array[i], indent, depth + 1);
            }
            out << '\n' << padding();
        }
        out << ']';
    } else if (value.isString()) {
        out << '"' << escape(value.asString()) << '"';
    } else if (value.isBool()) {
        out << (value.asBool() ? "true" : "false");
    } else {
        const double number = value.asNumber();
        if (std::isfinite(number) && std::floor(number) == number)
            out << std::defaultfloat << std::setprecision(15) << number;
        else
            out << std::defaultfloat << std::setprecision(15) << number;
    }
}

} // namespace

bool Value::isNull() const {
    return std::holds_alternative<std::nullptr_t>(storage_);
}
bool Value::isObject() const {
    return std::holds_alternative<Object>(storage_);
}
bool Value::isArray() const {
    return std::holds_alternative<Array>(storage_);
}
bool Value::isString() const {
    return std::holds_alternative<std::string>(storage_);
}
bool Value::isNumber() const {
    return std::holds_alternative<double>(storage_);
}
bool Value::isBool() const {
    return std::holds_alternative<bool>(storage_);
}
const Value::Object& Value::asObject() const {
    static const Object empty;
    if (const auto* value = std::get_if<Object>(&storage_))
        return *value;
    return empty;
}
Value::Object& Value::asObject() {
    if (!isObject())
        storage_ = Object{};
    return std::get<Object>(storage_);
}
const Value::Array& Value::asArray() const {
    static const Array empty;
    if (const auto* value = std::get_if<Array>(&storage_))
        return *value;
    return empty;
}
Value::Array& Value::asArray() {
    if (!isArray())
        storage_ = Array{};
    return std::get<Array>(storage_);
}
std::string Value::asString(const std::string& fallback) const {
    if (const auto* value = std::get_if<std::string>(&storage_))
        return *value;
    return fallback;
}
double Value::asNumber(double fallback) const {
    if (const auto* value = std::get_if<double>(&storage_))
        return *value;
    return fallback;
}
int Value::asInt(int fallback) const {
    return static_cast<int>(asNumber(static_cast<double>(fallback)));
}
bool Value::asBool(bool fallback) const {
    if (const auto* value = std::get_if<bool>(&storage_))
        return *value;
    return fallback;
}
const Value& Value::operator[](const std::string& key) const {
    if (const auto* object = std::get_if<Object>(&storage_)) {
        if (const auto found = object->find(key); found != object->end())
            return found->second;
    }
    return nullValue;
}
Value& Value::operator[](const std::string& key) {
    return asObject()[key];
}

Value parse(const std::string& text) {
    return Parser(text).run();
}

Value parseFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Unable to open JSON file: " + path.string());
    std::ostringstream content;
    content << stream.rdbuf();
    return parse(content.str());
}

std::string stringify(const Value& value, int indent) {
    std::ostringstream out;
    appendValue(out, value, indent, 0);
    out << '\n';
    return out.str();
}

void writeFile(const std::filesystem::path& path, const Value& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("Unable to write JSON file: " + path.string());
    stream << stringify(value);
}

} // namespace smp::json
