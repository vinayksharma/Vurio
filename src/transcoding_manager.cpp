#include "core/transcoding_manager.hpp"
#include "core/file_utils.hpp"
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <chrono>

// Raw file extensions that need transcoding
const std::vector<std::string> TranscodingManager::raw_extensions_ = {
    "cr2", "nef", "arw", "dng", "raf", "rw2", "orf", "pef", "srw", "kdc", "dcr", 
    "mos", "mrw", "raw", "bay", "3fr", "fff", "mef", "iiq", "rwz", "nrw", "rwl", 
    "r3d", "dcm", "dicom"
};

TranscodingManager& TranscodingManager::getInstance()
{
    static TranscodingManager instance;
    return instance;
}

void TranscodingManager::initialize(const std::string& cache_dir, int max_threads)
{
    cache_dir_ = cache_dir;
    max_threads_ = max_threads;
    db_manager_ = &DatabaseManager::getInstance();
    
    // Create cache directory if it doesn't exist
    std::filesystem::create_directories(cache_dir_);
    
    Logger::info("TranscodingManager initialized with cache dir: " + cache_dir_ + 
                 ", max threads: " + std::to_string(max_threads_));
}

void TranscodingManager::startTranscoding()
{
    if (running_.load())
    {
        Logger::warn("Transcoding is already running");
        return;
    }
    
    running_.store(true);
    cancelled_.store(false);
    
    // Start transcoding threads
    for (int i = 0; i < max_threads_; ++i)
    {
        transcoding_threads_.emplace_back(&TranscodingManager::transcodingThreadFunction, this);
    }
    
    Logger::info("Started " + std::to_string(max_threads_) + " transcoding threads");
}

void TranscodingManager::stopTranscoding()
{
    if (!running_.load())
    {
        return;
    }
    
    Logger::info("Stopping transcoding threads");
    
    cancelled_.store(true);
    queue_cv_.notify_all();
    
    // Wait for all threads to complete
    for (auto& thread : transcoding_threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    
    transcoding_threads_.clear();
    running_.store(false);
    
    Logger::info("Transcoding threads stopped");
}

bool TranscodingManager::isRawFile(const std::string& file_path)
{
    std::string extension = MediaProcessor::getFileExtension(file_path);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    return std::find(raw_extensions_.begin(), raw_extensions_.end(), extension) != raw_extensions_.end();
}

void TranscodingManager::queueForTranscoding(const std::string& file_path)
{
    if (!running_.load())
    {
        Logger::warn("Transcoding not running, cannot queue file: " + file_path);
        return;
    }
    
    // Check if already transcoded
    std::string existing_transcoded = getTranscodedFilePath(file_path);
    if (!existing_transcoded.empty())
    {
        Logger::debug("File already transcoded: " + file_path);
        return;
    }
    
    // Add to database queue
    DBOpResult result = db_manager_->insertTranscodingFile(file_path);
    if (!result.success)
    {
        Logger::error("Failed to queue file for transcoding: " + file_path + " - " + result.error_message);
        return;
    }
    
    // Add to thread queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        transcoding_queue_.push(file_path);
        queued_count_.fetch_add(1);
    }
    
    queue_cv_.notify_one();
    
    Logger::debug("Queued file for transcoding: " + file_path);
}

std::string TranscodingManager::getTranscodedFilePath(const std::string& source_file_path)
{
    return db_manager_->getTranscodedFilePath(source_file_path);
}

bool TranscodingManager::isTranscodingRunning() const
{
    return running_.load();
}

std::pair<size_t, size_t> TranscodingManager::getTranscodingStats() const
{
    return {queued_count_.load(), completed_count_.load()};
}

void TranscodingManager::shutdown()
{
    stopTranscoding();
    Logger::info("TranscodingManager shutdown complete");
}

void TranscodingManager::transcodingThreadFunction()
{
    Logger::debug("Transcoding thread started");
    
    while (running_.load() && !cancelled_.load())
    {
        std::string file_path;
        
        // Get next file from queue
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { 
                return !transcoding_queue_.empty() || !running_.load() || cancelled_.load(); 
            });
            
            if (!running_.load() || cancelled_.load())
            {
                break;
            }
            
            if (transcoding_queue_.empty())
            {
                continue;
            }
            
            file_path = transcoding_queue_.front();
            transcoding_queue_.pop();
        }
        
        Logger::info("Starting transcoding: " + file_path);
        
        try
        {
            std::string transcoded_path = transcodeFile(file_path);
            
            if (!transcoded_path.empty())
            {
                // Update database with transcoded file path
                DBOpResult result = db_manager_->updateTranscodedFilePath(file_path, transcoded_path);
                if (result.success)
                {
                    completed_count_.fetch_add(1);
                    Logger::info("Transcoding completed: " + file_path + " -> " + transcoded_path);
                }
                else
                {
                    failed_count_.fetch_add(1);
                    Logger::error("Failed to update transcoded file path in database: " + file_path + " - " + result.error_message);
                }
            }
            else
            {
                failed_count_.fetch_add(1);
                Logger::error("Transcoding failed: " + file_path);
            }
        }
        catch (const std::exception& e)
        {
            failed_count_.fetch_add(1);
            Logger::error("Exception during transcoding: " + file_path + " - " + std::string(e.what()));
        }
    }
    
    Logger::debug("Transcoding thread stopped");
}

std::string TranscodingManager::transcodeFile(const std::string& source_file_path)
{
    std::string cache_filename = generateCacheFilename(source_file_path);
    std::string output_path = std::filesystem::path(cache_dir_) / cache_filename;
    
    // Check if already exists
    if (std::filesystem::exists(output_path))
    {
        Logger::debug("Transcoded file already exists: " + output_path);
        return output_path;
    }
    
    // Use FFmpeg to transcode raw file to JPEG
    std::string ffmpeg_cmd = "ffmpeg -i \"" + source_file_path + "\" -y -q:v 2 \"" + output_path + "\" 2>/dev/null";
    
    Logger::debug("Executing FFmpeg command: " + ffmpeg_cmd);
    
    int result = std::system(ffmpeg_cmd.c_str());
    
    if (result == 0 && std::filesystem::exists(output_path))
    {
        Logger::info("FFmpeg transcoding successful: " + source_file_path + " -> " + output_path);
        return output_path;
    }
    else
    {
        Logger::error("FFmpeg transcoding failed for: " + source_file_path + " (exit code: " + std::to_string(result) + ")");
        
        // Clean up failed output file if it exists
        if (std::filesystem::exists(output_path))
        {
            std::filesystem::remove(output_path);
        }
        
        return "";
    }
}

std::string TranscodingManager::generateCacheFilename(const std::string& source_file_path)
{
    // Generate a unique filename based on source file path hash
    std::string hash = MediaProcessor::generateHash(std::vector<uint8_t>(source_file_path.begin(), source_file_path.end()));
    
    // Get original extension and convert to jpg
    std::string original_ext = MediaProcessor::getFileExtension(source_file_path);
    std::transform(original_ext.begin(), original_ext.end(), original_ext.begin(), ::tolower);
    
    // Use first 16 characters of hash + original extension + .jpg
    return hash.substr(0, 16) + "_" + original_ext + ".jpg";
} 