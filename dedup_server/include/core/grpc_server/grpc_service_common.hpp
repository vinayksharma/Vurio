#pragma once

// AR-5: shared dependency prelude for the per-service gRPC implementation
// headers split out of the former grpc_server_core.cpp god file. The service
// classes are header-defined (verbatim moves) so they stay constructible from
// tests; only grpc_server_core.cpp and tests include them, so the breadth of
// this prelude costs nothing in practice.
//
// AR-7: STATUS-CODE CONVENTION (deliberate, two tiers — the Swift client
// depends on both):
//   - Action RPCs (AdvancedActionResponse and friends): the transport status
//     is OK whenever the request was processed; the BUSINESS outcome travels
//     in success/success_count/error_message. Clients branch on resp.success.
//   - Media RPCs (MediaService): gRPC status codes ARE the flow control —
//     UNAVAILABLE = asset generating (retry via MediaReady push),
//     FAILED_PRECONDITION = permanently failed source (negative cache),
//     NOT_FOUND/INVALID_ARGUMENT for bad requests.
//   - UNAUTHENTICATED is reserved for the AR-4 auth processor on both tiers.
// Do not "normalize" one tier into the other — each is load-bearing for the
// client behavior described above.

#include <grpcpp/grpcpp.h>
#include <Poco/Logger.h>
#include <filesystem>
#include "dedup.grpc.pb.h"
#include "config/unified_observable_config.hpp"
#include "database/database_manager.hpp"
#include "orchestration/files_manager.hpp"
#include <nlohmann/json.hpp>
#include "database/scanned_files_ops.hpp"
#include "database/duplicate_groups_ops.hpp"
#include "database/day_groups_ops.hpp"
#include "database/user_settings_ops.hpp"
#include "database/ejected_files_ops.hpp"
#include "orchestration/thread_pool_manager.hpp"
#include <Poco/SHA1Engine.h>
#include <Poco/DigestEngine.h>
#include <Poco/DigestStream.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/UUIDGenerator.h>
#include <algorithm>
#include "orchestration/duplicate_finder.hpp"
#include "filesmanager/disk_cache.hpp"
#include "filesmanager/media_availability.hpp"
#include "orchestration/integrity_check_service.hpp"
#include "services/eject_service.hpp"
#include "media_processors/media_processor.hpp"
#include "media_processors/thumbnail_service.hpp"
#include "database/thumbnail_cache_ops.hpp"
#include "database/preview_cache_ops.hpp"
#include "orchestration/scheduler_service.hpp"
#include "utils/thumbnail_generator.hpp"
#include "media_processors/image/backends/transcoding_pipeline.hpp"
#include "core/media_ready_bus.hpp"
#include "core/media_generation_failures.hpp"
#include "database/processing_errors_ops.hpp"
#include "utils/temp_paths.hpp"
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <mutex>
#include <random>
#include <fstream>
#include <cstdlib>
