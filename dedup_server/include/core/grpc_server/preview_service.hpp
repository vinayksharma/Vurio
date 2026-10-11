#pragma once

// CHAIN-1 (docs/todo_tool_chain.md §5.4): the gRPC face of the live preview.
// Marshalling only; the session manager renders. Status codes follow the
// media tier (AR-7): NOT_FOUND when the file cannot be decoded for preview,
// INVALID_ARGUMENT for a document the engine refuses.

#include "core/grpc_server/grpc_service_common.hpp"
#include "adjustments/preview_session.hpp"

namespace MediaDedup {
namespace Core {

class PreviewServiceImpl final : public vurio::dedup::PreviewService::Service {
public:
    PreviewServiceImpl() = default;

    grpc::Status RenderPreview(grpc::ServerContext*, const vurio::dedup::RenderPreviewRequest* request,
                               vurio::dedup::PreviewFrame* reply) override {
        using Outcome = MediaDedup::Adjustments::PreviewSessionManager::Outcome;
        MediaDedup::Adjustments::PreviewRequest req;
        req.document_json = request->document_json();
        req.max_side = request->max_side();
        req.quality = request->quality();
        req.truncate_at_node_id = request->truncate_at_node_id();
        req.before = request->before();
        auto response = MediaDedup::Adjustments::PreviewSessionManager::instance().render(request->file_path(), req);
        reply->set_seq(request->seq());
        switch (response.outcome) {
            case Outcome::Ok: break;
            case Outcome::Invalid: return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, response.error);
            case Outcome::NotFound: return grpc::Status(grpc::StatusCode::NOT_FOUND, response.error);
            case Outcome::Internal: return grpc::Status(grpc::StatusCode::INTERNAL, response.error);
        }
        const auto& r = response.result;
        reply->set_jpeg(r.jpeg.data(), r.jpeg.size());
        reply->set_width(r.width);
        reply->set_height(r.height);
        reply->set_render_ms(r.render_ms);
        reply->set_base_width(static_cast<int>(std::lround(r.base_full.width)));
        reply->set_base_height(static_cast<int>(std::lround(r.base_full.height)));
        reply->set_display_width(static_cast<int>(std::lround(r.display_full.width)));
        reply->set_display_height(static_cast<int>(std::lround(r.display_full.height)));
        return grpc::Status::OK;
    }

    grpc::Status ReleasePreview(grpc::ServerContext*, const vurio::dedup::AdjustmentsFileRequest* request,
                                vurio::dedup::Empty*) override {
        MediaDedup::Adjustments::PreviewSessionManager::instance().release(request->file_path());
        return grpc::Status::OK;
    }
};

} // namespace Core
} // namespace MediaDedup
