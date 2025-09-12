#include "config/type_converter.hpp"
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <limits>

namespace MediaDedup
{

    std::string ConfigTypeConverter::toString(const std::any &value)
    {
        if (value.type() == typeid(void))
        {
            return "<void>";
        }

        return convertToString(value);
    }

    std::string ConfigTypeConverter::convertToString(const std::any &value)
    {
        if (value.type() == typeid(std::string))
        {
            return std::any_cast<std::string>(value);
        }
        else if (value.type() == typeid(int))
        {
            return std::to_string(std::any_cast<int>(value));
        }
        else if (value.type() == typeid(double))
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6) << std::any_cast<double>(value);
            return oss.str();
        }
        else if (value.type() == typeid(bool))
        {
            return std::any_cast<bool>(value) ? "true" : "false";
        }
        else if (value.type() == typeid(float))
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6) << std::any_cast<float>(value);
            return oss.str();
        }
        else if (value.type() == typeid(long))
        {
            return std::to_string(std::any_cast<long>(value));
        }
        else if (value.type() == typeid(long long))
        {
            return std::to_string(std::any_cast<long long>(value));
        }
        else if (value.type() == typeid(unsigned int))
        {
            return std::to_string(std::any_cast<unsigned int>(value));
        }
        else if (value.type() == typeid(unsigned long))
        {
            return std::to_string(std::any_cast<unsigned long>(value));
        }
        else if (value.type() == typeid(unsigned long long))
        {
            return std::to_string(std::any_cast<unsigned long long>(value));
        }
        else
        {
            return "<" + std::string(value.type().name()) + ">";
        }
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, bool &result)
    {
        std::string lower_str = str;
        std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);

        if (lower_str == "true" || lower_str == "1" || lower_str == "yes" || lower_str == "on")
        {
            result = true;
            return true;
        }
        else if (lower_str == "false" || lower_str == "0" || lower_str == "no" || lower_str == "off")
        {
            result = false;
            return true;
        }

        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, int &result)
    {
        try
        {
            size_t pos;
            int value = std::stoi(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, double &result)
    {
        try
        {
            size_t pos;
            double value = std::stod(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, std::string &result)
    {
        result = str;
        return true;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, float &result)
    {
        try
        {
            size_t pos;
            float value = std::stof(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, long &result)
    {
        try
        {
            size_t pos;
            long value = std::stol(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, long long &result)
    {
        try
        {
            size_t pos;
            long long value = std::stoll(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, unsigned int &result)
    {
        try
        {
            size_t pos;
            unsigned long value = std::stoul(str, &pos);
            if (pos == str.length() && value <= std::numeric_limits<unsigned int>::max())
            {
                result = static_cast<unsigned int>(value);
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, unsigned long &result)
    {
        try
        {
            size_t pos;
            unsigned long value = std::stoul(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    bool ConfigTypeConverter::convertFromString(const std::string &str, unsigned long long &result)
    {
        try
        {
            size_t pos;
            unsigned long long value = std::stoull(str, &pos);
            if (pos == str.length())
            {
                result = value;
                return true;
            }
        }
        catch (const std::exception &)
        {
            // Conversion failed
        }
        return false;
    }

    std::string ConfigTypeConverter::getTypeName(const std::any &value)
    {
        if (value.type() == typeid(void))
        {
            return "void";
        }
        else if (value.type() == typeid(std::string))
        {
            return "string";
        }
        else if (value.type() == typeid(int))
        {
            return "int";
        }
        else if (value.type() == typeid(double))
        {
            return "double";
        }
        else if (value.type() == typeid(bool))
        {
            return "bool";
        }
        else if (value.type() == typeid(float))
        {
            return "float";
        }
        else if (value.type() == typeid(long))
        {
            return "long";
        }
        else if (value.type() == typeid(long long))
        {
            return "long long";
        }
        else if (value.type() == typeid(unsigned int))
        {
            return "unsigned int";
        }
        else if (value.type() == typeid(unsigned long))
        {
            return "unsigned long";
        }
        else if (value.type() == typeid(unsigned long long))
        {
            return "unsigned long long";
        }
        else
        {
            return std::string(value.type().name());
        }
    }

    std::string ConfigTypeConverter::toStringSafe(const std::any &value, const std::string &fallback)
    {
        try
        {
            return toString(value);
        }
        catch (const TypeConversionError &)
        {
            return fallback;
        }
    }

    bool ConfigTypeConverter::areEqual(const std::any &lhs, const std::any &rhs)
    {
        if (lhs.type() != rhs.type())
        {
            return false;
        }

        if (lhs.type() == typeid(std::string))
        {
            return std::any_cast<std::string>(lhs) == std::any_cast<std::string>(rhs);
        }
        else if (lhs.type() == typeid(int))
        {
            return std::any_cast<int>(lhs) == std::any_cast<int>(rhs);
        }
        else if (lhs.type() == typeid(double))
        {
            return std::any_cast<double>(lhs) == std::any_cast<double>(rhs);
        }
        else if (lhs.type() == typeid(bool))
        {
            return std::any_cast<bool>(lhs) == std::any_cast<bool>(rhs);
        }
        else if (lhs.type() == typeid(float))
        {
            return std::any_cast<float>(lhs) == std::any_cast<float>(rhs);
        }
        else if (lhs.type() == typeid(long))
        {
            return std::any_cast<long>(lhs) == std::any_cast<long>(rhs);
        }
        else if (lhs.type() == typeid(long long))
        {
            return std::any_cast<long long>(lhs) == std::any_cast<long long>(rhs);
        }
        else if (lhs.type() == typeid(unsigned int))
        {
            return std::any_cast<unsigned int>(lhs) == std::any_cast<unsigned int>(rhs);
        }
        else if (lhs.type() == typeid(unsigned long))
        {
            return std::any_cast<unsigned long>(lhs) == std::any_cast<unsigned long>(rhs);
        }
        else if (lhs.type() == typeid(unsigned long long))
        {
            return std::any_cast<unsigned long long>(lhs) == std::any_cast<unsigned long long>(rhs);
        }

        return false;
    }

    bool ConfigTypeConverter::isEmpty(const std::any &value)
    {
        if (value.type() == typeid(std::string))
        {
            return std::any_cast<std::string>(value).empty();
        }
        else if (value.type() == typeid(int))
        {
            return std::any_cast<int>(value) == 0;
        }
        else if (value.type() == typeid(double))
        {
            return std::any_cast<double>(value) == 0.0;
        }
        else if (value.type() == typeid(bool))
        {
            return !std::any_cast<bool>(value);
        }
        else if (value.type() == typeid(float))
        {
            return std::any_cast<float>(value) == 0.0f;
        }
        else if (value.type() == typeid(long))
        {
            return std::any_cast<long>(value) == 0L;
        }
        else if (value.type() == typeid(long long))
        {
            return std::any_cast<long long>(value) == 0LL;
        }
        else if (value.type() == typeid(unsigned int))
        {
            return std::any_cast<unsigned int>(value) == 0U;
        }
        else if (value.type() == typeid(unsigned long))
        {
            return std::any_cast<unsigned long>(value) == 0UL;
        }
        else if (value.type() == typeid(unsigned long long))
        {
            return std::any_cast<unsigned long long>(value) == 0ULL;
        }

        return false;
    }

} // namespace MediaDedup
