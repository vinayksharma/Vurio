#pragma once

// LOOK-5 (docs/todo_film_looks.md): a driver ties one number in a chain to
// a scene signal, so a recipe adapts to the photo it is applied to.
//
//   tool node:  "drivers": {"<numeric param id>" | "strength": DRIVER}
//   lane:       "drivers": {"strength": DRIVER}          (the merge strength)
//   DRIVER:     {"signal": "<signal id>", "points": [[signal, value], …]}
//
// 1–8 points, signal values strictly increasing; the value is interpolated
// linearly between points and held outside them.
//
// Drivers are RESOLVED, not rendered: `resolve` writes each driven value
// into the chain for one photo's signals (when a recipe is applied, or when
// the client asks). The engine, the canonical chain and the revision only
// ever see plain numbers, so a driven chain renders, caches and hashes like
// any other, and a value changed by hand simply stays as it was set. A
// driven lane strength of 0 makes the lane a no-op the compiler drops —
// the conditional lane.

#include "adjustments/scene_signals.hpp"
#include "adjustments/tool_registry.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace MediaDedup::Adjustments::Drivers
{
    constexpr const char *kKey = "drivers";
    constexpr const char *kStrength = "strength";
    constexpr int kMaxPoints = 8;

    // Every driver in the chain (nodes, lanes, lane steps) is well formed,
    // names a known signal and a numeric target of its tool, and keeps its
    // values within the target's limits. Unknown tools are skipped.
    bool validate(const nlohmann::json &chain, const ToolRegistry &registry, std::string &error);

    // One driver's value for these signals; false when the photo does not
    // have the signal (the target then keeps the value it has).
    bool evaluate(const nlohmann::json &driver, const SceneSignals &signals, double &out);

    // Write every driven value into `chain`. Returns how many were set.
    int resolve(nlohmann::json &chain, const SceneSignals &signals, const ToolRegistry &registry);

    bool hasAny(const nlohmann::json &chain);

    // The drivers alone, in chain order, numbers rounded: "" when there are
    // none. Appended to a recipe's identity so two recipes that differ only
    // in what they follow are different recipes.
    std::string canonical(const nlohmann::json &chain);
}
