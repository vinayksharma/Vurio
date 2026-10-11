#pragma once

// ADJ-1 (docs/todo_image_adjustments.md §5.8): the gRPC face of the
// adjustment store. Marshalling only; every rule lives in
// Adjustments::AdjustmentStore. Status-code convention (AR-7): this is an
// action surface, but the media-style codes are used where the client
// branches on them — INVALID_ARGUMENT for a refused document (the tool's
// error text), NOT_FOUND for an unregistered file.

#include "core/grpc_server/grpc_service_common.hpp"
#include "adjustments/adjustment_document.hpp"
#include "adjustments/adjustment_store.hpp"
#include "adjustments/masks.hpp"
#include "adjustments/tool_registry.hpp"

namespace MediaDedup {
namespace Core {

class AdjustmentsServiceImpl final : public vurio::dedup::AdjustmentsService::Service {
public:
    AdjustmentsServiceImpl() = default;

    grpc::Status DescribeTools(grpc::ServerContext*, const vurio::dedup::Empty*,
                               vurio::dedup::ToolDescriptors* reply) override {
        for (const auto& d : MediaDedup::Adjustments::ToolRegistry::instance().descriptors())
            fillDescriptor(d, reply->add_tools());
        // CHAIN-4: the split itself is a tool in the catalogue; its lanes'
        // mask sources are described alongside.
        {
            MediaDedup::Adjustments::ToolDescriptor split;
            split.id = MediaDedup::Adjustments::AdjustmentDocument::kSplitTool;
            split.label = "Split";
            split.category = MediaDedup::Adjustments::ToolCategory::Split;
            split.icon = "arrow.triangle.branch";
            split.summary = "{lanes} lanes";
            fillDescriptor(split, reply->add_tools());
        }
        for (const auto& d : MediaDedup::Adjustments::MaskRegistry::instance().descriptors())
            fillDescriptor(d, reply->add_mask_sources());
        reply->set_document_version(MediaDedup::Adjustments::AdjustmentDocument::kVersion);
        for (const auto& mode : MediaDedup::Adjustments::RenderPlan::blendModes()) reply->add_blend_modes(mode);
        return grpc::Status::OK;
    }

    static void fillDescriptor(const MediaDedup::Adjustments::ToolDescriptor& d, vurio::dedup::ToolDescriptor* t) {
        using MediaDedup::Adjustments::ParamSpec;
        {
            t->set_id(d.id);
            t->set_label(d.label);
            t->set_version(d.version);
            t->set_category(MediaDedup::Adjustments::categoryName(d.category));
            t->set_icon(d.icon);
            t->set_lane_allowed(d.lane_allowed);
            t->set_resolution_dependent(d.resolution_dependent);
            t->set_summary(d.summary);
            t->set_active_at_defaults(d.active_at_defaults);
            for (const auto& p : d.params) {
                auto* ps = t->add_params();
                ps->set_id(p.id);
                ps->set_label(p.label);
                ps->set_kind(ParamSpec::kindName(p.kind));
                ps->set_min(p.min);
                ps->set_max(p.max);
                ps->set_step(p.step);
                ps->set_default_value(p.def);
                ps->set_default_option(p.default_option);
                for (const auto& o : p.options) ps->add_options(o);
            }
        }
    }

    grpc::Status GetAdjustments(grpc::ServerContext*, const vurio::dedup::AdjustmentsFileRequest* request,
                                vurio::dedup::AdjustmentDocumentResponse* reply) override {
        auto& store = MediaDedup::Adjustments::AdjustmentStore::instance();
        auto row = store.rowFor(request->file_path());
        if (!row) {
            reply->set_has_adjustments(false);
            return grpc::Status::OK;
        }
        reply->set_has_adjustments(true);
        reply->set_document_json(row->document);
        reply->set_render_revision(row->render_revision);
        reply->set_sidecar_spooled(row->sidecar_mtime == 0);
        reply->set_sidecar_path(row->sidecar_path);
        // Geometry needs the base dims; a direct row read is fine on this
        // one-per-selection call.
        MediaDedup::DisplayDims dims;
        if (auto scanned = MediaDedup::ScannedFilesOps::getByPath(*db(), request->file_path());
            scanned && scanned->exif_resolution) {
            auto base = MediaDedup::displayDimensionsFromExifResolution(*scanned->exif_resolution);
            dims = store.displayDimsFor(request->file_path(), base.width, base.height);
        }
        reply->set_display_width(dims.width);
        reply->set_display_height(dims.height);
        fillHistory(row->history, reply);
        return grpc::Status::OK;
    }

    grpc::Status SetAdjustments(grpc::ServerContext*, const vurio::dedup::SetAdjustmentsRequest* request,
                                vurio::dedup::SetAdjustmentsResponse* reply) override {
        using Outcome = MediaDedup::Adjustments::AdjustmentStore::Outcome;
        auto result = MediaDedup::Adjustments::AdjustmentStore::instance().setDocument(
            request->file_path(), request->document_json(), request->step_label());
        reply->set_success(result.outcome == Outcome::Ok);
        reply->set_error_message(result.error);
        reply->set_render_revision(result.render_revision);
        reply->set_display_width(result.display_dims.width);
        reply->set_display_height(result.display_dims.height);
        reply->set_sidecar_spooled(result.sidecar_spooled);
        reply->set_sidecar_path(result.sidecar_path);
        reply->set_history_index(result.history_index);
        for (const auto& step : result.history_steps) {
            auto* h = reply->add_history_steps();
            h->set_label(step.first);
            h->set_at(step.second);
        }
        return statusOf(result);
    }

    grpc::Status StepHistory(grpc::ServerContext*, const vurio::dedup::StepHistoryRequest* request,
                             vurio::dedup::AdjustmentDocumentResponse* reply) override {
        using Outcome = MediaDedup::Adjustments::AdjustmentStore::Outcome;
        auto result = MediaDedup::Adjustments::AdjustmentStore::instance().stepHistory(
            request->file_path(), request->delta(), request->use_index(), request->index());
        if (result.outcome != Outcome::Ok) return statusOf(result);
        reply->set_has_adjustments(!result.document_json.empty());
        reply->set_document_json(result.document_json);
        reply->set_render_revision(result.render_revision);
        reply->set_display_width(result.display_dims.width);
        reply->set_display_height(result.display_dims.height);
        reply->set_sidecar_spooled(result.sidecar_spooled);
        reply->set_sidecar_path(result.sidecar_path);
        reply->set_history_index(result.history_index);
        for (const auto& step : result.history_steps) {
            auto* h = reply->add_history_steps();
            h->set_label(step.first);
            h->set_at(step.second);
        }
        return grpc::Status::OK;
    }

    grpc::Status GetEditsRoot(grpc::ServerContext*, const vurio::dedup::EditsRootRequest* request,
                              vurio::dedup::EditsRootResponse* reply) override {
        auto& store = MediaDedup::Adjustments::AdjustmentStore::instance();
        auto layout = store.layoutFor(request->location_path());
        reply->set_edits_root(layout.editsRoot());
        reply->set_is_default(layout.isDefaultRoot());
        auto summary = store.summarize(request->location_path());
        reply->set_sidecar_count(summary.sidecars);
        reply->set_spooled_count(summary.spooled);
        return grpc::Status::OK;
    }

    grpc::Status SetEditsRoot(grpc::ServerContext*, const vurio::dedup::SetEditsRootRequest* request,
                              vurio::dedup::SetEditsRootResponse* reply) override {
        auto result = MediaDedup::Adjustments::AdjustmentStore::instance().setEditsRoot(
            request->location_path(), request->edits_root());
        reply->set_success(result.ok);
        reply->set_error_message(result.error);
        reply->set_moved(result.moved);
        reply->set_failed(result.failed);
        reply->set_conflicting_location(result.conflicting_location);
        if (!result.ok && !result.conflicting_location.empty())
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, result.error);
        return grpc::Status::OK;
    }

private:
    static std::shared_ptr<MediaDedup::DatabaseManager> db() { return MediaDedup::Adjustments::AdjustmentStore::instance().database(); }

    static grpc::Status statusOf(const MediaDedup::Adjustments::AdjustmentStore::CommitResult& result) {
        using Outcome = MediaDedup::Adjustments::AdjustmentStore::Outcome;
        switch (result.outcome) {
            case Outcome::Ok: return grpc::Status::OK;
            case Outcome::Invalid: return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, result.error);
            case Outcome::NotFound: return grpc::Status(grpc::StatusCode::NOT_FOUND, result.error);
            case Outcome::OutOfRange: return grpc::Status(grpc::StatusCode::OUT_OF_RANGE, result.error);
            case Outcome::Disabled: return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, result.error);
            case Outcome::Internal: break;
        }
        return grpc::Status(grpc::StatusCode::INTERNAL, result.error);
    }

    // The row's history column → the reply's index and step list.
    static void fillHistory(const std::string& history_json, vurio::dedup::AdjustmentDocumentResponse* reply) {
        if (history_json.empty()) { reply->set_history_index(-1); return; }
        MediaDedup::Adjustments::AdjustmentDocument doc;
        try {
            nlohmann::json h = nlohmann::json::parse(history_json);
            if (h.is_object()) doc.history = h;
        } catch (...) {}
        reply->set_history_index(doc.historyIndex());
        for (const auto& step : doc.historySteps()) {
            auto* s = reply->add_history_steps();
            s->set_label(step.first);
            s->set_at(step.second);
        }
    }
};

} // namespace Core
} // namespace MediaDedup
