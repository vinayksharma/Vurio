#pragma once

#include <string>
#include <any>
#include <type_traits>
#include <sstream>
#include <stdexcept>

namespace MediaDedup
{

    /**
     * @brief Type conversion error exception
     */
    class TypeConversionError : public std::runtime_error
    {
    public:
        explicit TypeConversionError(const std::string &message) : std::runtime_error(message) {}
    };

    /**
     * @brief Configuration type converter utility class
     *
     * Provides type-safe conversion between std::any and various C++ types,
     * with special support for string conversion and validation.
     */
    class ConfigTypeConverter
    {
    public:
        /**
         * @brief Convert std::any to string representation
         * @param value The value to convert
         * @return String representation of the value
         * @throws TypeConversionError if conversion fails
         */
        static std::string toString(const std::any &value);

        /**
         * @brief Convert string to specified type
         * @tparam T Target type
         * @param str String to convert
         * @return Converted value of type T
         * @throws TypeConversionError if conversion fails
         */
        template <typename T>
        static T fromString(const std::string &str);

        /**
         * @brief Convert std::any to specified type
         * @tparam T Target type
         * @param value The value to convert
         * @return Converted value of type T
         * @throws TypeConversionError if conversion fails
         */
        template <typename T>
        static T fromAny(const std::any &value);

        /**
         * @brief Convert value to std::any
         * @tparam T Source type
         * @param value The value to convert
         * @return std::any containing the value
         */
        template <typename T>
        static std::any toAny(const T &value);

        /**
         * @brief Check if std::any contains a specific type
         * @tparam T Type to check for
         * @param value The value to check
         * @return true if value contains type T
         */
        template <typename T>
        static bool isType(const std::any &value);

        /**
         * @brief Get the type name of std::any value
         * @param value The value to inspect
         * @return String representation of the type name
         */
        static std::string getTypeName(const std::any &value);

        /**
         * @brief Check if a string can be converted to a specific type
         * @tparam T Target type
         * @param str String to check
         * @return true if conversion is possible
         */
        template <typename T>
        static bool canConvertFromString(const std::string &str);

        /**
         * @brief Safely convert std::any to string with fallback
         * @param value The value to convert
         * @param fallback Fallback string if conversion fails
         * @return String representation or fallback
         */
        static std::string toStringSafe(const std::any &value, const std::string &fallback = "<unknown>");

        /**
         * @brief Safely convert string to type with fallback
         * @tparam T Target type
         * @param str String to convert
         * @param fallback Fallback value if conversion fails
         * @return Converted value or fallback
         */
        template <typename T>
        static T fromStringSafe(const std::string &str, const T &fallback);

        /**
         * @brief Compare two std::any values for equality
         * @param lhs Left-hand side value
         * @param rhs Right-hand side value
         * @return true if values are equal
         */
        static bool areEqual(const std::any &lhs, const std::any &rhs);

        /**
         * @brief Check if a value is empty (for string types) or zero (for numeric types)
         * @param value The value to check
         * @return true if value is considered empty
         */
        static bool isEmpty(const std::any &value);

    private:
        // Helper methods for specific type conversions
        static std::string convertToString(const std::any &value);
        static bool convertFromString(const std::string &str, bool &result);
        static bool convertFromString(const std::string &str, int &result);
        static bool convertFromString(const std::string &str, double &result);
        static bool convertFromString(const std::string &str, std::string &result);
        static bool convertFromString(const std::string &str, float &result);
        static bool convertFromString(const std::string &str, long &result);
        static bool convertFromString(const std::string &str, long long &result);
        static bool convertFromString(const std::string &str, unsigned int &result);
        static bool convertFromString(const std::string &str, unsigned long &result);
        static bool convertFromString(const std::string &str, unsigned long long &result);
    };

    // Template implementations
    template <typename T>
    T ConfigTypeConverter::fromString(const std::string &str)
    {
        T result;
        if (!convertFromString(str, result))
        {
            throw TypeConversionError("Cannot convert string '" + str + "' to " + typeid(T).name());
        }
        return result;
    }

    template <typename T>
    T ConfigTypeConverter::fromAny(const std::any &value)
    {
        try
        {
            return std::any_cast<T>(value);
        }
        catch (const std::bad_any_cast &)
        {
            throw TypeConversionError("Cannot convert std::any to " + std::string(typeid(T).name()));
        }
    }

    template <typename T>
    std::any ConfigTypeConverter::toAny(const T &value)
    {
        return std::any(value);
    }

    template <typename T>
    bool ConfigTypeConverter::isType(const std::any &value)
    {
        return value.type() == typeid(T);
    }

    template <typename T>
    bool ConfigTypeConverter::canConvertFromString(const std::string &str)
    {
        T result;
        return convertFromString(str, result);
    }

    template <typename T>
    T ConfigTypeConverter::fromStringSafe(const std::string &str, const T &fallback)
    {
        try
        {
            return fromString<T>(str);
        }
        catch (const TypeConversionError &)
        {
            return fallback;
        }
    }

} // namespace MediaDedup
