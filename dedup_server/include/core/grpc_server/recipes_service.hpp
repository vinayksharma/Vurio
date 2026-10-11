#pragma once

// CHAIN-3 (docs/todo_tool_chain.md §7.4): the gRPC face of the recipe
// store. Marshalling only. Action tier (AR-7): the transport status is OK
// when the request was processed and the outcome travels in `success`,
// except where the client branches on a code: INVALID_ARGUMENT for a
// refused chain, ALREADY_EXISTS for a taken name, NOT_FOUND for a missing
// recipe or photo.

#include "core/grpc_server/grpc_service_common.hpp"
#include "adjustments/recipe_store.hpp"

namespace MediaDedup {
namespace Core {

class RecipesServiceImpl final : public vurio::dedup::RecipesService::Service {
public:
    RecipesServiceImpl() = default;

    grpc::Status ListRecipes(grpc::ServerContext*, const vurio::dedup::Empty*,
                             vurio::dedup::RecipeList* reply) override {
        for (const auto& row : MediaDedup::Adjustments::RecipeStore::instance().list())
            fill(row, reply->add_recipes());
        return grpc::Status::OK;
    }

    grpc::Status SaveRecipe(grpc::ServerContext*, const vurio::dedup::SaveRecipeRequest* request,
                            vurio::dedup::RecipeResponse* reply) override {
        auto r = MediaDedup::Adjustments::RecipeStore::instance().save(
            request->name(), request->description(), request->chain_json(), request->from_file_path(),
            request->include_geometry());
        return finish(r, reply);
    }

    grpc::Status UpdateRecipe(grpc::ServerContext*, const vurio::dedup::UpdateRecipeRequest* request,
                              vurio::dedup::RecipeResponse* reply) override {
        auto r = MediaDedup::Adjustments::RecipeStore::instance().update(
            request->id(), request->name(), request->description(), request->set_chain(), request->chain_json());
        return finish(r, reply);
    }

    grpc::Status DeleteRecipe(grpc::ServerContext*, const vurio::dedup::RecipeIdRequest* request,
                              vurio::dedup::Empty*) override {
        if (!MediaDedup::Adjustments::RecipeStore::instance().get(request->id()))
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "no recipe with id " + request->id());
        MediaDedup::Adjustments::RecipeStore::instance().remove(request->id());
        return grpc::Status::OK;
    }

    grpc::Status ValidateChain(grpc::ServerContext*, const vurio::dedup::ValidateChainRequest* request,
                               vurio::dedup::ValidateChainResponse* reply) override {
        auto v = MediaDedup::Adjustments::RecipeStore::instance().validate(request->chain_json(), request->file_path());
        reply->set_ok(v.ok);
        reply->set_error_message(v.error);
        reply->set_node_count(v.node_count);
        return grpc::Status::OK;
    }

    grpc::Status ApplyRecipe(grpc::ServerContext*, const vurio::dedup::ApplyRecipeRequest* request,
                             vurio::dedup::SetAdjustmentsResponse* reply) override {
        using Mode = MediaDedup::Adjustments::RecipeStore::ApplyMode;
        using Outcome = MediaDedup::Adjustments::AdjustmentStore::Outcome;
        Mode mode = request->mode() == "append" ? Mode::Append : request->mode() == "insert" ? Mode::Insert : Mode::Replace;
        auto result = MediaDedup::Adjustments::RecipeStore::instance().apply(request->file_path(), request->recipe_id(),
                                                                             mode, request->index());
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

    grpc::Status ExportRecipes(grpc::ServerContext*, const vurio::dedup::RecipeIdsRequest* request,
                               vurio::dedup::RecipesFile* reply) override {
        std::vector<std::string> ids(request->ids().begin(), request->ids().end());
        reply->set_json(MediaDedup::Adjustments::RecipeStore::instance().exportJson(ids));
        return grpc::Status::OK;
    }

    grpc::Status ImportRecipes(grpc::ServerContext*, const vurio::dedup::ImportRecipesRequest* request,
                               vurio::dedup::ImportRecipesResult* reply) override {
        auto& store = MediaDedup::Adjustments::RecipeStore::instance();
        auto r = store.importJson(request->json(), MediaDedup::Adjustments::RecipeStore::policyFromString(request->conflict_policy()));
        reply->set_success(r.ok);
        reply->set_error_message(r.error);
        reply->set_imported(r.imported);
        reply->set_replaced(r.replaced);
        reply->set_skipped(r.skipped);
        for (const auto& o : r.outcomes) {
            auto* po = reply->add_outcomes();
            po->set_name(o.name);
            po->set_outcome(o.outcome);
            po->set_message(o.message);
        }
        return grpc::Status::OK;
    }

private:
    static void fill(const MediaDedup::RecipeRow& row, vurio::dedup::Recipe* out) {
        out->set_id(row.id);
        out->set_name(row.name);
        out->set_description(row.description);
        out->set_chain_json(row.chain);
        nlohmann::json chain;
        std::string error;
        out->set_node_count(MediaDedup::Adjustments::RecipeStore::parseChain(row.chain, chain, error) && chain.is_array()
                            ? static_cast<int>(chain.size()) : 0);
        out->set_includes_geometry(row.includes_geometry);
        out->set_source(row.source);
        out->set_created_at(row.created_at);
        out->set_updated_at(row.updated_at);
    }

    static grpc::Status finish(const MediaDedup::Adjustments::RecipeStore::Result& r, vurio::dedup::RecipeResponse* reply) {
        using Outcome = MediaDedup::Adjustments::RecipeStore::Outcome;
        reply->set_success(r.outcome == Outcome::Ok);
        reply->set_error_message(r.error);
        if (r.recipe) fill(*r.recipe, reply->mutable_recipe());
        switch (r.outcome) {
            case Outcome::Ok: return grpc::Status::OK;
            case Outcome::Invalid: return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, r.error);
            case Outcome::NotFound: return grpc::Status(grpc::StatusCode::NOT_FOUND, r.error);
            case Outcome::AlreadyExists: return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, r.error);
            case Outcome::Internal: break;
        }
        return grpc::Status(grpc::StatusCode::INTERNAL, r.error);
    }
};

} // namespace Core
} // namespace MediaDedup
