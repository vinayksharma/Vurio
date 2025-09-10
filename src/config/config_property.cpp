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
            if (value_.type() == typeid(std::string))
            {
                return std::any_cast<std::string>(value_);
            }
            else if (value_.type() == typeid(int))
            {
                return std::to_string(std::any_cast<int>(value_));
            }
            else if (value_.type() == typeid(double))
            {
                return std::to_string(std::any_cast<double>(value_));
            }
            else if (value_.type() == typeid(bool))
            {
                return std::any_cast<bool>(value_) ? "true" : "false";
            }
            else if (value_.type() == typeid(std::vector<std::string>))
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
        }
        catch (const std::bad_any_cast &)
        {
            // Fall through to return empty string
        }
        return "";
    }

    bool ConfigProperty::setValueFromString(const std::string &value)
    {
        try
        {
            // Try to determine the type and convert
            if (default_value_.type() == typeid(std::string))
            {
                return setValue(value);
            }
            else if (default_value_.type() == typeid(int))
            {
                return setValue(std::stoi(value));
            }
            else if (default_value_.type() == typeid(double))
            {
                return setValue(std::stod(value));
            }
            else if (default_value_.type() == typeid(bool))
            {
                std::string lower_value = value;
                std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
                bool bool_value = (lower_value == "true" || lower_value == "1" || lower_value == "yes");
                return setValue(bool_value);
            }
        }
        catch (const std::exception &)
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
