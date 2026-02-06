#include "config/config_property.hpp"
#include <algorithm>
#include <stdexcept>

namespace MediaDedup
{

    ConfigProperty::ConfigProperty(const std::string &key, const std::any &default_value,
                                   const std::string &description)
        : key_(key), value_(default_value), default_value_(default_value),
          description_(description), modified_(false)
    {
    }

    std::string ConfigProperty::getValueAsString() const
    {
        try
        {
            // Handle special case for vector<string>
            if (value_.type() == typeid(std::vector<std::string>))
            {
                auto vec = std::any_cast<std::vector<std::string>>(value_);
                std::string result = "[";
                for (size_t i = 0; i < vec.size(); ++i)
                {
                    if (i > 0)
                        result += ", ";
                    result += vec[i];
                }
                result += "]";
                return result;
            }

            // Use ConfigTypeConverter for standard types
            return ConfigTypeConverter::toString(value_);
        }
        catch (const TypeConversionError &)
        {
            return "";
        }
    }

    bool ConfigProperty::setValueFromString(const std::string &value)
    {
        try
        {
            // Use ConfigTypeConverter based on the default value type
            if (default_value_.type() == typeid(std::string))
            {
                return setValue(ConfigTypeConverter::fromString<std::string>(value));
            }
            else if (default_value_.type() == typeid(int))
            {
                return setValue(ConfigTypeConverter::fromString<int>(value));
            }
            else if (default_value_.type() == typeid(double))
            {
                return setValue(ConfigTypeConverter::fromString<double>(value));
            }
            else if (default_value_.type() == typeid(bool))
            {
                return setValue(ConfigTypeConverter::fromString<bool>(value));
            }
            else if (default_value_.type() == typeid(float))
            {
                return setValue(ConfigTypeConverter::fromString<float>(value));
            }
            else if (default_value_.type() == typeid(long))
            {
                return setValue(ConfigTypeConverter::fromString<long>(value));
            }
            else if (default_value_.type() == typeid(long long))
            {
                return setValue(ConfigTypeConverter::fromString<long long>(value));
            }
            else if (default_value_.type() == typeid(unsigned int))
            {
                return setValue(ConfigTypeConverter::fromString<unsigned int>(value));
            }
            else if (default_value_.type() == typeid(unsigned long))
            {
                return setValue(ConfigTypeConverter::fromString<unsigned long>(value));
            }
            else if (default_value_.type() == typeid(unsigned long long))
            {
                return setValue(ConfigTypeConverter::fromString<unsigned long long>(value));
            }
        }
        catch (const TypeConversionError &)
        {
            return false;
        }
        return false;
    }

    bool ConfigProperty::setValue(const std::any &new_value)
    {
        return setValueInternal(new_value);
    }

    bool ConfigProperty::setValueFromFile(const std::any &new_value)
    {
        return setValueInternal(new_value);
    }

    void ConfigProperty::resetToDefault()
    {
        value_ = default_value_;
        modified_ = true;
    }

    bool ConfigProperty::setValueInternal(const std::any &new_value)
    {
        value_ = new_value;
        modified_ = true;
        return true;
    }

} // namespace MediaDedup
