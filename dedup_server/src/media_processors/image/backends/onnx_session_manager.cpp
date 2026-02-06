#include "media_processors/image/backends/onnx_session_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>

namespace MediaDedup
{

#if defined(HAVE_ONNXRUNTIME)

    OnnxSessionWrapper::OnnxSessionWrapper(const std::string& model_path_param)
        : model_path(model_path_param)
    {
        Poco::Logger& logger = Poco::Logger::get("OnnxSessionManager");
        
        try
        {
            // Create ONNX Runtime environment
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MediaDedup");
            
            // Configure session options for optimal performance
            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(1);  // Single thread per session
            session_options.SetInterOpNumThreads(1);
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
            
            // Create session with model
            session = std::make_unique<Ort::Session>(*env, model_path.c_str(), session_options);
            
            logger.information("Created ONNX session for model: " + model_path);
        }
        catch (const std::exception& e)
        {
            logger.error("Failed to create ONNX session for " + model_path + ": " + std::string(e.what()));
            throw;
        }
    }

    OnnxSessionManager::OnnxSessionManager(std::shared_ptr<UnifiedObservableConfigManager> config)
        : config_(std::move(config))
    {
        if (config_)
        {
            // Load configuration
            cache_enabled_ = config_->getPropertyValue<bool>("media.image.quality.onnx.sessionCache.enabled", true);
            max_sessions_per_model_ = static_cast<size_t>(config_->getPropertyValue<int>("media.image.quality.onnx.sessionCache.maxSessions", 4));
            
            // Subscribe to configuration changes
            config_->subscribeToConfigChanges([this](const ConfigChangeEvent& event) {
                if (event.key.find("media.image.quality.onnx.sessionCache") != std::string::npos)
                {
                    onConfigChange();
                }
            });
        }
        
        Poco::Logger::get("OnnxSessionManager").information(
            "ONNX Session Manager initialized - Cache: " + std::string(cache_enabled_ ? "enabled" : "disabled") +
            ", Max sessions per model: " + std::to_string(max_sessions_per_model_)
        );
    }

    OnnxSessionManager::~OnnxSessionManager()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        size_t total_sessions = 0;
        for (const auto& pair : session_pools_)
        {
            total_sessions += pair.second.size();
        }
        
        Poco::Logger::get("OnnxSessionManager").information(
            "Destroying ONNX Session Manager - Releasing " + std::to_string(total_sessions) + " cached sessions"
        );
        
        session_pools_.clear();
    }

    std::unique_ptr<OnnxSessionWrapper> OnnxSessionManager::createSession(const std::string& model_path)
    {
        Poco::Logger& logger = Poco::Logger::get("OnnxSessionManager");
        
        try
        {
            logger.debug("Creating new ONNX session for model: " + model_path);
            return std::make_unique<OnnxSessionWrapper>(model_path);
        }
        catch (const std::exception& e)
        {
            logger.error("Failed to create ONNX session: " + std::string(e.what()));
            throw std::runtime_error("Failed to create ONNX session for " + model_path + ": " + e.what());
        }
    }

    OnnxSessionLease OnnxSessionManager::borrowSession(const std::string& model_path)
    {
        Poco::Logger& logger = Poco::Logger::get("OnnxSessionManager");
        
        // If caching is disabled, always create a new session
        if (!cache_enabled_)
        {
            logger.trace("Session caching disabled, creating new session for: " + model_path);
            auto session = createSession(model_path);
            OnnxSessionWrapper* raw_ptr = session.release();
            
            // Return lease with deleter instead of pool return
            return OnnxSessionLease(raw_ptr, [](OnnxSessionWrapper* s) {
                delete s;  // Direct deletion when cache disabled
            });
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Try to get existing session from pool
        auto& pool = session_pools_[model_path];
        
        if (!pool.empty())
        {
            // Reuse existing session from pool
            auto session = std::move(pool.front());
            pool.pop_front();
            
            logger.debug("Borrowed cached ONNX session for: " + model_path + 
                        " (pool size: " + std::to_string(pool.size()) + ")");
            
            OnnxSessionWrapper* raw_ptr = session.release();
            return OnnxSessionLease(raw_ptr, [this](OnnxSessionWrapper* s) {
                returnSession(s);
            });
        }
        
        // No available session, create new one
        // Note: This is outside the pool limit - we create as needed and pool returns later
        logger.debug("No cached session available, creating new ONNX session for: " + model_path);
        auto session = createSession(model_path);
        
        OnnxSessionWrapper* raw_ptr = session.release();
        return OnnxSessionLease(raw_ptr, [this](OnnxSessionWrapper* s) {
            returnSession(s);
        });
    }

    void OnnxSessionManager::returnSession(OnnxSessionWrapper* session)
    {
        if (!session) return;
        
        Poco::Logger& logger = Poco::Logger::get("OnnxSessionManager");
        
        // If caching is disabled, just delete the session
        if (!cache_enabled_)
        {
            logger.trace("Session caching disabled, deleting session for: " + session->model_path);
            delete session;
            return;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto& pool = session_pools_[session->model_path];
        
        // Check if pool is at capacity
        if (pool.size() >= max_sessions_per_model_)
        {
            logger.debug("Session pool at capacity for " + session->model_path + 
                        ", deleting returned session (pool size: " + std::to_string(pool.size()) + ")");
            delete session;
            return;
        }
        
        // Return session to pool for reuse
        logger.debug("Returned ONNX session to pool for: " + session->model_path + 
                    " (pool size: " + std::to_string(pool.size() + 1) + ")");
        pool.push_back(std::unique_ptr<OnnxSessionWrapper>(session));
    }

    void OnnxSessionManager::clearCache()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        size_t total_sessions = 0;
        for (const auto& pair : session_pools_)
        {
            total_sessions += pair.second.size();
        }
        
        Poco::Logger::get("OnnxSessionManager").information(
            "Clearing ONNX session cache - Releasing " + std::to_string(total_sessions) + " sessions"
        );
        
        session_pools_.clear();
    }

    OnnxSessionManager::PoolStats OnnxSessionManager::getStats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        PoolStats stats;
        stats.cache_enabled = cache_enabled_;
        stats.max_sessions_per_model = max_sessions_per_model_;
        
        for (const auto& pair : session_pools_)
        {
            stats.total_sessions += pair.second.size();
            stats.available_sessions += pair.second.size();
        }
        
        // Note: borrowed_sessions would require tracking, not implemented for simplicity
        stats.borrowed_sessions = 0;
        
        return stats;
    }

    void OnnxSessionManager::onConfigChange()
    {
        if (!config_) return;
        
        bool new_cache_enabled = config_->getPropertyValue<bool>("media.image.quality.onnx.sessionCache.enabled", true);
        size_t new_max_sessions = static_cast<size_t>(config_->getPropertyValue<int>("media.image.quality.onnx.sessionCache.maxSessions", 4));
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (cache_enabled_ != new_cache_enabled)
        {
            cache_enabled_ = new_cache_enabled;
            Poco::Logger::get("OnnxSessionManager").information(
                "ONNX session cache " + std::string(cache_enabled_ ? "enabled" : "disabled")
            );
            
            // If cache disabled, clear all sessions
            if (!cache_enabled_)
            {
                session_pools_.clear();
                Poco::Logger::get("OnnxSessionManager").information("Cleared all cached sessions (cache disabled)");
            }
        }
        
        if (max_sessions_per_model_ != new_max_sessions)
        {
            max_sessions_per_model_ = new_max_sessions;
            Poco::Logger::get("OnnxSessionManager").information(
                "Updated max sessions per model: " + std::to_string(max_sessions_per_model_)
            );
            
            // Trim pools that exceed new limit
            for (auto& pair : session_pools_)
            {
                while (pair.second.size() > max_sessions_per_model_)
                {
                    pair.second.pop_back();
                }
            }
        }
    }

#endif // HAVE_ONNXRUNTIME

} // namespace MediaDedup
