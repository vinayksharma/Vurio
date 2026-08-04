#include "media_processors/image/backends/onnx_session_manager.hpp"
#include "config/unified_observable_config.hpp"
#include <Poco/Logger.h>
#include <filesystem>
#include <unordered_map>

namespace MediaDedup
{

#if defined(HAVE_ONNXRUNTIME)

    Ort::Env &OnnxSessionWrapper::sharedEnv()
    {
        // DF-4: ONE environment for the whole process. An Ort::Env owns
        // thread pools and telemetry state — one per pooled session was
        // pure duplication.
        static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "MediaDedup");
        return env;
    }

    OnnxSessionWrapper::OnnxSessionWrapper(const std::string& model_path_param,
                                           bool try_coreml,
                                           const std::string& coreml_cache_dir)
        : model_path(model_path_param)
    {
        Poco::Logger& logger = Poco::Logger::get("OnnxSessionManager");

        auto buildOptions = [&](bool with_coreml)
        {
            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(1); // Single thread per session
            session_options.SetInterOpNumThreads(1);
            // DF-4: was ORT_ENABLE_BASIC — ALL adds the extended/layout fusions
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (with_coreml)
            {
                std::unordered_map<std::string, std::string> coreml_options{
                    {"MLComputeUnits", "MLComputeUnitsAll"}, // ANE/GPU/CPU, CoreML picks
                    {"ModelFormat", "MLProgram"},
                };
                if (!coreml_cache_dir.empty())
                {
                    std::error_code ec;
                    std::filesystem::create_directories(coreml_cache_dir, ec);
                    if (!ec)
                        coreml_options["ModelCacheDirectory"] = coreml_cache_dir;
                }
                session_options.AppendExecutionProvider("CoreML", coreml_options);
            }
            return session_options;
        };

        if (try_coreml)
        {
            // DF-4: CoreML runs compatible subgraphs on ANE/GPU and falls back
            // per-node to CPU on its own; a hard failure here (EP not compiled
            // in, unsupported OS) downgrades the whole session to CPU-only.
            try
            {
                auto session_options = buildOptions(true);
                session = std::make_unique<Ort::Session>(sharedEnv(), model_path.c_str(), session_options);
                coreml_active = true;
                logger.information("Created ONNX session (CoreML EP) for model: " + model_path);
            }
            catch (const std::exception& e)
            {
                logger.warning("CoreML EP unavailable, falling back to CPU: " + std::string(e.what()));
            }
        }

        if (!session)
        {
            try
            {
                auto session_options = buildOptions(false);
                session = std::make_unique<Ort::Session>(sharedEnv(), model_path.c_str(), session_options);
                logger.information("Created ONNX session (CPU) for model: " + model_path);
            }
            catch (const std::exception& e)
            {
                logger.error("Failed to create ONNX session for " + model_path + ": " + std::string(e.what()));
                throw;
            }
        }

        introspect();
    }

    void OnnxSessionWrapper::introspect()
    {
        // DF-4: resolve the model's I/O layout once; inference only reads it.
        Ort::AllocatorWithDefaultOptions allocator;
        io.num_inputs = session->GetInputCount();
        io.input_names.reserve(io.num_inputs);
        io.input_dims.reserve(io.num_inputs);
        io.input_types.reserve(io.num_inputs);

        for (size_t i = 0; i < io.num_inputs; ++i)
        {
            auto name_ptr = session->GetInputNameAllocated(i, allocator);
            io.input_names.emplace_back(name_ptr.get());
            auto tensor_info = session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
            io.input_dims.push_back(tensor_info.GetShape());
            io.input_types.push_back(tensor_info.GetElementType());
        }

        // Image input: 4D tensor, NCHW (?,3,?,?) or NHWC (?,?,?,3); -1 = dynamic.
        for (size_t i = 0; i < io.num_inputs; ++i)
        {
            const auto &dims = io.input_dims[i];
            if (dims.size() != 4)
                continue;
            if (dims[1] == 3 || dims[1] == -1)
            {
                io.image_input_index = i;
                io.found_image_input = true;
                break;
            }
            if (dims[3] == 3 || dims[3] == -1)
            {
                io.image_input_index = i;
                io.found_image_input = true;
                io.image_input_nhwc = true;
                break;
            }
        }

        if (session->GetOutputCount() > 0)
        {
            auto out_ptr = session->GetOutputNameAllocated(0, allocator);
            io.output_name = out_ptr.get();
        }

        // Built LAST: input_names is complete, so the c_str() views are stable
        // (the wrapper is non-movable).
        io.input_name_ptrs.reserve(io.num_inputs);
        for (const auto &n : io.input_names)
            io.input_name_ptrs.push_back(n.c_str());
    }

    OnnxSessionManager::OnnxSessionManager(std::shared_ptr<UnifiedObservableConfigManager> config)
        : config_(std::move(config))
    {
        if (config_)
        {
            // Load configuration
            cache_enabled_ = config_->getPropertyValue<bool>("media.image.onnx.sessionCache.enabled", true);
            max_sessions_per_model_ = static_cast<size_t>(config_->getPropertyValue<int>("media.image.onnx.sessionCache.maxSessions", 4));
            // DF-4: default OFF in code so tests/headless runs stay CPU;
            // production config.yaml turns it on.
            coreml_enabled_ = config_->getPropertyValue<bool>("media.image.onnx.coreml.enabled", false);
            coreml_cache_dir_ = config_->getPropertyValue<std::string>("media.image.onnx.coreml.cacheDir", "cache/coreml");

            // Subscribe to configuration changes
            config_->subscribeToConfigChanges([this](const ConfigChangeEvent& event) {
                if (event.key.find("media.image.onnx.sessionCache") != std::string::npos)
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
            return std::make_unique<OnnxSessionWrapper>(model_path, coreml_enabled_, coreml_cache_dir_);
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
        
        bool new_cache_enabled = config_->getPropertyValue<bool>("media.image.onnx.sessionCache.enabled", true);
        size_t new_max_sessions = static_cast<size_t>(config_->getPropertyValue<int>("media.image.onnx.sessionCache.maxSessions", 4));
        
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
