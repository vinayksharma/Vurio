#include "config/config_change_event.hpp"
#include <sstream>
#include <iomanip>

namespace MediaDedup
{

    ConfigChangeEvent::ConfigChangeEvent(const std::string &k, const std::any &old_val, const std::any &new_val,
                                         const std::string &src, bool file_update)
        : key(k), old_value(old_val), new_value(new_val), source(src),
          timestamp(std::chrono::system_clock::now()), is_file_update(file_update)
    {
    }

    std::string ConfigChangeEvent::toString() const
    {
        std::ostringstream oss;

        // Format timestamp
        auto time_t = std::chrono::system_clock::to_time_t(timestamp);
        auto tm = *std::localtime(&time_t);

        oss << std::put_time(&tm, "%H:%M:%S") << " [CONFIG] " << key << " changed from ";

        // Handle old value
        if (old_value.type() == typeid(std::string))
        {
            oss << "'" << std::any_cast<std::string>(old_value) << "'";
        }
        else if (old_value.type() == typeid(int))
        {
            oss << std::any_cast<int>(old_value);
        }
        else if (old_value.type() == typeid(double))
        {
            oss << std::any_cast<double>(old_value);
        }
        else if (old_value.type() == typeid(bool))
        {
            oss << (std::any_cast<bool>(old_value) ? "true" : "false");
        }
        else
        {
            oss << "<unknown>";
        }

        oss << " to ";

        // Handle new value
        if (new_value.type() == typeid(std::string))
        {
            oss << "'" << std::any_cast<std::string>(new_value) << "'";
        }
        else if (new_value.type() == typeid(int))
        {
            oss << std::any_cast<int>(new_value);
        }
        else if (new_value.type() == typeid(double))
        {
            oss << std::any_cast<double>(new_value);
        }
        else if (new_value.type() == typeid(bool))
        {
            oss << (std::any_cast<bool>(new_value) ? "true" : "false");
        }
        else
        {
            oss << "<unknown>";
        }

        oss << " (source: " << source << ", file_update: " << (is_file_update ? "yes" : "no") << ")";

        return oss.str();
    }

    bool ConfigChangeEvent::hasValueChanged() const
    {
        // Simple comparison based on type and value
        if (old_value.type() != new_value.type())
        {
            return true;
        }

        try
        {
            if (old_value.type() == typeid(std::string))
            {
                return std::any_cast<std::string>(old_value) != std::any_cast<std::string>(new_value);
            }
            else if (old_value.type() == typeid(int))
            {
                return std::any_cast<int>(old_value) != std::any_cast<int>(new_value);
            }
            else if (old_value.type() == typeid(double))
            {
                return std::any_cast<double>(old_value) != std::any_cast<double>(new_value);
            }
            else if (old_value.type() == typeid(bool))
            {
                return std::any_cast<bool>(old_value) != std::any_cast<bool>(new_value);
            }
        }
        catch (...)
        {
            // If any_cast fails, assume values are different
            return true;
        }

        // For other types, assume they are different
        return true;
    }

    std::chrono::milliseconds ConfigChangeEvent::getAge() const
    {
        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - timestamp);
        return duration;
    }

} // namespace MediaDedup
