#include "ImsJson.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include <cctype>
#include <cstring>
#include <cstdlib>

struct ImsJson::Value
{
    Type type = Type::Null;
    bool boolValue = false;
    bool isInteger = false;
    uint64_t uintValue = 0;
    double numberValue = 0.0;
    std::string str;
    std::vector<ValuePtr> arrayValue;
    std::map<std::string, ValuePtr> objectValue;
};

namespace {

void AppendUtf8(std::string& out, uint32_t codePoint)
{
    if (codePoint <= 0x7F) {
        out += static_cast<char>(codePoint);
    }
    else if (codePoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

using VPtr = ImsJson::ValuePtr;

VPtr MakeValue(ImsJson::Type type)
{
    auto value = std::make_shared<ImsJson::Value>();
    value->type = type;
    return value;
}

class JsonParser
{
public:
    JsonParser(const char* data, size_t size) : m_data(data), m_size(size) {}

    bool Parse(VPtr& out)
    {
        SkipSpace();
        if (!ParseValue(out)) {
            return false;
        }
        SkipSpace();
        return true;
    }

private:
    void SkipSpace()
    {
        while (m_pos < m_size) {
            const char ch = m_data[m_pos];
            if ((ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n')) {
                ++m_pos;
            }
            else {
                break;
            }
        }
    }

    bool ParseValue(VPtr& out)
    {
        if (m_pos >= m_size) {
            return false;
        }
        const char ch = m_data[m_pos];
        switch (ch) {
        case '{':
            return ParseObject(out);
        case '[':
            return ParseArray(out);
        case '"': {
            auto value = MakeValue(ImsJson::Type::String);
            if (!ParseString(value->str)) {
                return false;
            }
            out = value;
            return true;
        }
        case 't':
            if (MatchLiteral("true")) {
                auto value = MakeValue(ImsJson::Type::Bool);
                value->boolValue = true;
                out = value;
                return true;
            }
            return false;
        case 'f':
            if (MatchLiteral("false")) {
                out = MakeValue(ImsJson::Type::Bool);
                return true;
            }
            return false;
        case 'n':
            if (MatchLiteral("null")) {
                out = MakeValue(ImsJson::Type::Null);
                return true;
            }
            return false;
        default:
            return ParseNumber(out);
        }
    }

    bool MatchLiteral(const char* literal)
    {
        const size_t length = strlen(literal);
        if ((m_pos + length) > m_size) {
            return false;
        }
        if (memcmp(m_data + m_pos, literal, length) != 0) {
            return false;
        }
        m_pos += length;
        return true;
    }

    bool ParseNumber(VPtr& out)
    {
        const size_t start = m_pos;
        if ((m_pos < m_size) && ((m_data[m_pos] == '-') || (m_data[m_pos] == '+'))) {
            ++m_pos;
        }
        bool anyDigit = false;
        while ((m_pos < m_size) && (std::isdigit(static_cast<unsigned char>(m_data[m_pos])) != 0)) {
            ++m_pos;
            anyDigit = true;
        }
        bool isDouble = false;
        if ((m_pos < m_size) && (m_data[m_pos] == '.')) {
            isDouble = true;
            ++m_pos;
            while ((m_pos < m_size) && (std::isdigit(static_cast<unsigned char>(m_data[m_pos])) != 0)) {
                ++m_pos;
            }
        }
        if ((m_pos < m_size) && ((m_data[m_pos] == 'e') || (m_data[m_pos] == 'E'))) {
            isDouble = true;
            ++m_pos;
            if ((m_pos < m_size) && ((m_data[m_pos] == '-') || (m_data[m_pos] == '+'))) {
                ++m_pos;
            }
            while ((m_pos < m_size) && (std::isdigit(static_cast<unsigned char>(m_data[m_pos])) != 0)) {
                ++m_pos;
            }
        }
        if (!anyDigit) {
            return false;
        }

        auto value = MakeValue(ImsJson::Type::Number);
        const std::string token(m_data + start, m_pos - start);
        value->numberValue = std::strtod(token.c_str(), nullptr);
        if (!isDouble) {
            errno = 0;
            value->uintValue = std::strtoull(token.c_str(), nullptr, 10);
            value->isInteger = true;
        }
        out = value;
        return true;
    }

    bool ParseHex4(uint32_t& out)
    {
        if ((m_pos + 4) > m_size) {
            return false;
        }
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char ch = m_data[m_pos + i];
            value <<= 4;
            if ((ch >= '0') && (ch <= '9')) {
                value |= static_cast<uint32_t>(ch - '0');
            }
            else if ((ch >= 'a') && (ch <= 'f')) {
                value |= static_cast<uint32_t>(ch - 'a' + 10);
            }
            else if ((ch >= 'A') && (ch <= 'F')) {
                value |= static_cast<uint32_t>(ch - 'A' + 10);
            }
            else {
                return false;
            }
        }
        m_pos += 4;
        out = value;
        return true;
    }

    bool ParseString(std::string& out)
    {
        if ((m_pos >= m_size) || (m_data[m_pos] != '"')) {
            return false;
        }
        ++m_pos;
        out.clear();
        while (m_pos < m_size) {
            const char ch = m_data[m_pos++];
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (m_pos >= m_size) {
                return false;
            }
            const char esc = m_data[m_pos++];
            switch (esc) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                uint32_t codePoint = 0;
                if (!ParseHex4(codePoint)) {
                    return false;
                }
                if ((codePoint >= 0xD800) && (codePoint <= 0xDBFF) && ((m_pos + 1) < m_size) &&
                    (m_data[m_pos] == '\\') && (m_data[m_pos + 1] == 'u')) {
                    const size_t saved = m_pos;
                    m_pos += 2;
                    uint32_t low = 0;
                    if (ParseHex4(low) && (low >= 0xDC00) && (low <= 0xDFFF)) {
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else {
                        m_pos = saved;
                    }
                }
                AppendUtf8(out, codePoint);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseObject(VPtr& out)
    {
        ++m_pos; // '{'
        auto value = MakeValue(ImsJson::Type::Object);
        SkipSpace();
        if ((m_pos < m_size) && (m_data[m_pos] == '}')) {
            ++m_pos;
            out = value;
            return true;
        }
        while (m_pos < m_size) {
            SkipSpace();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipSpace();
            if ((m_pos >= m_size) || (m_data[m_pos] != ':')) {
                return false;
            }
            ++m_pos;
            SkipSpace();
            VPtr member;
            if (!ParseValue(member)) {
                return false;
            }
            value->objectValue[key] = member;
            SkipSpace();
            if ((m_pos < m_size) && (m_data[m_pos] == ',')) {
                ++m_pos;
                continue;
            }
            if ((m_pos < m_size) && (m_data[m_pos] == '}')) {
                ++m_pos;
                out = value;
                return true;
            }
            return false;
        }
        return false;
    }

    bool ParseArray(VPtr& out)
    {
        ++m_pos; // '['
        auto value = MakeValue(ImsJson::Type::Array);
        SkipSpace();
        if ((m_pos < m_size) && (m_data[m_pos] == ']')) {
            ++m_pos;
            out = value;
            return true;
        }
        while (m_pos < m_size) {
            SkipSpace();
            VPtr element;
            if (!ParseValue(element)) {
                return false;
            }
            value->arrayValue.push_back(element);
            SkipSpace();
            if ((m_pos < m_size) && (m_data[m_pos] == ',')) {
                ++m_pos;
                continue;
            }
            if ((m_pos < m_size) && (m_data[m_pos] == ']')) {
                ++m_pos;
                out = value;
                return true;
            }
            return false;
        }
        return false;
    }

    const char* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
};

const VPtr& NullValue()
{
    static const VPtr instance;
    return instance;
}

} // namespace

ImsJson::ImsJson(ValuePtr value) : m_value(value)
{
}

bool ImsJson::Parse(const std::string& text, ImsJson& out)
{
    if (text.empty()) {
        return false;
    }
    VPtr root;
    JsonParser parser(text.data(), text.size());
    if (!parser.Parse(root)) {
        out = ImsJson();
        return false;
    }
    out = ImsJson(root);
    return true;
}

ImsJson::Type ImsJson::GetType() const
{
    return (m_value != nullptr) ? m_value->type : Type::Null;
}

bool ImsJson::IsNull() const
{
    return (m_value == nullptr) || (m_value->type == Type::Null);
}

bool ImsJson::IsObject() const
{
    return GetType() == Type::Object;
}

bool ImsJson::IsArray() const
{
    return GetType() == Type::Array;
}

bool ImsJson::IsString() const
{
    return GetType() == Type::String;
}

bool ImsJson::IsNumber() const
{
    return GetType() == Type::Number;
}

ImsJson ImsJson::operator[](const std::string& key) const
{
    if ((m_value == nullptr) || (m_value->type != Type::Object)) {
        return ImsJson(NullValue());
    }
    auto iter = m_value->objectValue.find(key);
    if (iter == m_value->objectValue.end()) {
        return ImsJson(NullValue());
    }
    return ImsJson(iter->second);
}

bool ImsJson::Has(const std::string& key) const
{
    if ((m_value == nullptr) || (m_value->type != Type::Object)) {
        return false;
    }
    return m_value->objectValue.find(key) != m_value->objectValue.end();
}

size_t ImsJson::Size() const
{
    if ((m_value == nullptr) || (m_value->type != Type::Array)) {
        return 0;
    }
    return m_value->arrayValue.size();
}

ImsJson ImsJson::At(size_t index) const
{
    if ((m_value == nullptr) || (m_value->type != Type::Array) || (index >= m_value->arrayValue.size())) {
        return ImsJson(NullValue());
    }
    return ImsJson(m_value->arrayValue[index]);
}

bool ImsJson::AsBool(bool defaultValue) const
{
    if ((m_value == nullptr) || (m_value->type != Type::Bool)) {
        return defaultValue;
    }
    return m_value->boolValue;
}

uint64_t ImsJson::AsUInt64(uint64_t defaultValue) const
{
    if ((m_value == nullptr) || (m_value->type != Type::Number)) {
        return defaultValue;
    }
    if (m_value->isInteger) {
        return m_value->uintValue;
    }
    if (m_value->numberValue >= 0) {
        return static_cast<uint64_t>(m_value->numberValue);
    }
    return defaultValue;
}

int ImsJson::AsInt(int defaultValue) const
{
    if ((m_value == nullptr) || (m_value->type != Type::Number)) {
        return defaultValue;
    }
    return static_cast<int>(m_value->numberValue);
}

std::string ImsJson::AsStringOr(const std::string& defaultValue) const
{
    if ((m_value == nullptr) || (m_value->type != Type::String)) {
        return defaultValue;
    }
    return m_value->str;
}

std::string ImsJsonEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 8);
    result += '"';
    for (const char ch : value) {
        switch (ch) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if ((static_cast<unsigned char>(ch) < 0x20) != false) {
                static const char* hex = "0123456789abcdef";
                result += "\\u00";
                result += hex[(static_cast<unsigned char>(ch) >> 4) & 0x0F];
                result += hex[static_cast<unsigned char>(ch) & 0x0F];
            }
            else {
                result += ch;
            }
            break;
        }
    }
    result += '"';
    return result;
}

#endif // CXIM_IMS_CORE_SDK_VERSION
