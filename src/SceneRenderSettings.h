#pragma once
#include <string>
#include "json.hpp"

struct RenderContext;

/*
* Scene-driven render settings ("render_settings" in a scene file), used by the night-street
* preset (docs/nfs2015/configuration.md lists every key).
*
* Every setting this module understands is first reset to its RenderContext default, then the
* scene's values are applied, so switching scenes never leaks a previous scene's look.
*
* Scenes without "render_settings" leave the context untouched (editor toggles persist), unless the
* previous scene had settings, in which case the defaults are restored.
*/
namespace SceneRenderSettings {
	// Returns true when the scene file contained a "render_settings" object.
	// "quality_tiers": { "<name>": { ...render_settings overrides... } } with "quality_tier" choosing
	// one (NOX_QUALITY_TIER overrides the scene's choice).
	bool ApplyFromSceneFile(RenderContext& ctx, const std::string& sceneFilePath, bool previousSceneHadSettings);
	void Apply(RenderContext& ctx, const nlohmann::json& settings, bool resetFirst = true);
	// The live values as a "render_settings" object (the saver writes this, not the loaded copy). Keys in
	// `existing` (the object already in the file, e.g. quality_tier) are kept and refreshed; other settings
	// are written only when they differ from the RenderContext defaults. Keys: render/RenderSettingsTable.h.
	nlohmann::json ToJson(const RenderContext& ctx, const nlohmann::json& existing);
	// Name of the tier applied by the last ApplyFromSceneFile ("" when none).
	const std::string& ActiveTier();
}
