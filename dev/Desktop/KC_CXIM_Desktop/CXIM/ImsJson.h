#ifndef CXIM_IMS_JSON_H_
#define CXIM_IMS_JSON_H_

#include "CXIMAppConfig.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

/** @class ImsJson
* @brief 轻量 JSON 解析器（仅用于 IMS 接口对接，支持对象/数组/字符串/整数/小数/布尔/null）
* @note  子节点以 shared_ptr 持有，operator[] / At 返回值包装，引用安全且拷贝廉价。
*/
class ImsJson
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    struct Value;
    using ValuePtr = std::shared_ptr<Value>;

    ImsJson() = default;
    explicit ImsJson(ValuePtr value);

    /** 解析一段 JSON 文本，成功返回 true */
    static bool Parse(const std::string& text, ImsJson& out);

    Type GetType() const;
    bool IsNull() const;
    bool IsObject() const;
    bool IsArray() const;
    bool IsString() const;
    bool IsNumber() const;

    /** 对象成员访问，不存在返回 Null 节点 */
    ImsJson operator[](const std::string& key) const;
    bool Has(const std::string& key) const;

    /** 数组访问，越界返回 Null 节点 */
    size_t Size() const;
    ImsJson At(size_t index) const;

    /** 标量取值（类型不符时返回默认值） */
    bool AsBool(bool defaultValue = false) const;
    uint64_t AsUInt64(uint64_t defaultValue = 0) const;
    int AsInt(int defaultValue = 0) const;
    std::string AsStringOr(const std::string& defaultValue) const;

private:
    ValuePtr m_value;
};

/** 把任意 UTF-8 字符串编码成 JSON 字符串字面量（含首尾引号） */
std::string ImsJsonEscape(const std::string& value);

#endif // CXIM_IMS_CORE_SDK_VERSION

#endif // CXIM_IMS_JSON_H_
