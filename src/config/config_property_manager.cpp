#include "config/config_property_manager.hpp"
#include "config/unified_observable_config.hpp"

namespace MediaDedup
{

    // ConfigPropertyManager implementation
    // Most functionality is implemented inline in the header file as template methods

    void ConfigPropertyManager::resetToDefaults()
    {
        std::lock_guard<std::mutex> lock(properties_mutex_);
        for (auto &[_, property] : properties_)
        {
            property->resetToDefault();
        }
    }

    void ConfigPropertyManager::setGlobalChangeCallback(ConfigChangeCallback callback)
    {
        std::lock_guard<std::mutex> lock(properties_mutex_);
        for (auto &[_, property] : properties_)
        {
            property->setChangeCallback(callback);
        }
    }

} // namespace MediaDedup
