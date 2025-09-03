#include "core/server.hpp"
#include <Poco/Exception.h>
#include <iostream>

/**
 * @brief Main entry point for the Media Deduplication Server
 * 
 * This function creates and runs the MediaDedupServer application.
 * It handles any uncaught exceptions and provides proper error reporting.
 * 
 * @return Exit code (0 for success, non-zero for failure)
 */
int main(int argc, char* argv[]) {
    try {
        // Create and run the server application
        MediaDedup::MediaDedupServer app;
        
        // Run the application with command line arguments
        return app.run(argc, argv);
    }
    catch (const Poco::Exception& e) {
        // Handle Poco-specific exceptions
        std::cerr << "Poco Exception: " << e.displayText() << std::endl;
        std::cerr << "Code: " << e.code() << std::endl;
        std::cerr << "What: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e) {
        // Handle standard C++ exceptions
        std::cerr << "Standard Exception: " << e.what() << std::endl;
        return 1;
    }
    catch (...) {
        // Handle any other exceptions
        std::cerr << "Unknown Exception occurred" << std::endl;
        return 1;
    }
}
