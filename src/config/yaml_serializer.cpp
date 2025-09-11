#include "config/yaml_serializer.hpp"
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace MediaDedup
{

    YamlConfigSerializer::YamlConfigSerializer(const std::string &file_path)
        : file_path_(file_path)
    {
    }

    std::unordered_map<std::string, std::any> YamlConfigSerializer::parseYamlFile() const
    {
        std::unordered_map<std::string, std::any> properties;

        try
        {
            YAML::Node config = YAML::LoadFile(file_path_);

            for (const auto &item : config)
            {
                std::string key = item.first.Scalar();
                auto value = yamlNodeToAny(item.second);
                properties[key] = value;
            }
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error("YAML parsing failed: " + std::string(e.what()));
        }

        return properties;
    }

    bool YamlConfigSerializer::serializeToYamlFile(const std::unordered_map<std::string, std::any> &properties) const
    {
        try
        {
            YAML::Node config;

            for (const auto &item : properties)
            {
                config[item.first] = anyToYamlNode(item.second);
            }

            std::ofstream file(file_path_);
            if (!file.is_open())
            {
                return false;
            }

            file << config;
            file.close();

            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    std::any YamlConfigSerializer::yamlNodeToAny(const YAML::Node &node)
    {
        if (node.IsScalar())
        {
            if (node.IsNull())
            {
                return std::any{};
            }
            else if (node.IsScalar())
            {
                std::string str_value = node.Scalar();
                return convertStringToType(str_value);
            }
        }
        else if (node.IsSequence())
        {
            std::vector<std::string> vec;
            for (const auto &item : node)
            {
                vec.push_back(item.Scalar());
            }
            return std::any{vec};
        }
        else if (node.IsMap())
        {
            // For now, convert maps to strings. Could be enhanced to support nested structures
            return std::any{node.Scalar()};
        }

        return std::any{};
    }

    YAML::Node YamlConfigSerializer::anyToYamlNode(const std::any &value)
    {
        try
        {
            if (value.type() == typeid(std::string))
            {
                return YAML::Node(std::any_cast<std::string>(value));
            }
            else if (value.type() == typeid(int))
            {
                return YAML::Node(std::any_cast<int>(value));
            }
            else if (value.type() == typeid(double))
            {
                return YAML::Node(std::any_cast<double>(value));
            }
            else if (value.type() == typeid(bool))
            {
                return YAML::Node(std::any_cast<bool>(value));
            }
            else if (value.type() == typeid(std::vector<std::string>))
            {
                YAML::Node node;
                auto vec = std::any_cast<std::vector<std::string>>(value);
                for (const auto &item : vec)
                {
                    node.push_back(item);
                }
                return node;
            }
        }
        catch (const std::bad_any_cast &)
        {
            // Fall through to return null
        }

        return YAML::Node();
    }

    std::any YamlConfigSerializer::convertStringToType(const std::string &str_value)
    {
        try
        {
            // Try int first
            return std::any{std::stoi(str_value)};
        }
        catch (...)
        {
            try
            {
                // Try double
                return std::any{std::stod(str_value)};
            }
            catch (...)
            {
                try
                {
                    // Try bool
                    std::string lower_value = str_value;
                    std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
                    bool bool_value = (lower_value == "true" || lower_value == "1" || lower_value == "yes");
                    return std::any{bool_value};
                }
                catch (...)
                {
                    // Default to string
                    return std::any{str_value};
                }
            }
        }
    }

} // namespace MediaDedup
