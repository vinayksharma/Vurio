#pragma once

#include <string>
#include <any>
#include <vector>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace MediaDedup
{

    /**
     * @brief YAML configuration serializer for configuration properties
     *
     * Handles serialization and deserialization of configuration properties to/from YAML files.
     * Provides type conversion between YAML nodes and std::any values.
     */
    class YamlConfigSerializer
    {
    public:
        /**
         * @brief Constructor
         * @param file_path Path to the YAML configuration file
         */
        explicit YamlConfigSerializer(const std::string &file_path);

        /**
         * @brief Parse YAML file and return properties as key-value pairs
         * @return Map of property keys to their values
         * @throws std::exception if file parsing fails
         */
        std::unordered_map<std::string, std::any> parseYamlFile() const;

        /**
         * @brief Serialize properties to YAML file
         * @param properties Map of property keys to their values
         * @return true if serialization succeeded, false otherwise
         */
        bool serializeToYamlFile(const std::unordered_map<std::string, std::any> &properties) const;

        /**
         * @brief Convert YAML node to std::any value
         * @param node YAML node to convert
         * @return std::any containing the converted value
         */
        static std::any yamlNodeToAny(const YAML::Node &node);

        /**
         * @brief Convert std::any value to YAML node
         * @param value std::any value to convert
         * @return YAML node containing the converted value
         */
        static YAML::Node anyToYamlNode(const std::any &value);

        /**
         * @brief Get the file path
         * @return Path to the YAML configuration file
         */
        const std::string &getFilePath() const { return file_path_; }

        /**
         * @brief Set the file path
         * @param file_path New path to the YAML configuration file
         */
        void setFilePath(const std::string &file_path) { file_path_ = file_path; }

    private:
        std::string file_path_;

        /**
         * @brief Try to convert string to appropriate type (int, double, bool, string)
         * @param str_value String value to convert
         * @return std::any containing the converted value
         */
        static std::any convertStringToType(const std::string &str_value);
    };

} // namespace MediaDedup
