#include "LuaReload.h"

#include "../assets/AssetIO.h"

#include <memory>

namespace NoxLua {

	namespace {
		constexpr int kMaxStagingAttempts = 3;

		std::string FirstErrorText(const LuaWorld& world) {
			for (const Diagnostic& d : world.Diagnostics()) {
				if (d.severity != Diagnostic::Severity::Error) continue;
				std::string text = d.file.empty() ? d.message : d.file + ":" + std::to_string(d.line) + ": " + d.message;
				return d.source + ": " + text;
			}
			return "load failed";
		}
	}

	bool AssetScriptFiles::Read(const std::string& path, std::string& text, FileStamp& stamp, std::string& error) {
		const auto before = AssetIO::Stat(path);
		if (!before || before->isDirectory) {
			error = "no such file: " + path;
			return false;
		}
		const auto contents = AssetIO::ReadText(path, &error);
		if (!contents) return false;
		const auto after = AssetIO::Stat(path);
		if (!after || after->size != before->size || after->modifiedTime != before->modifiedTime) {
			error = path + " changed while reading";
			return false;
		}
		text = *contents;
		stamp.size = before->size;
		stamp.mtime = before->modifiedTime;
		return true;
	}

	bool AssetScriptFiles::Stat(const std::string& path, FileStamp& stamp) {
		const auto info = AssetIO::Stat(path);
		if (!info || info->isDirectory) return false;
		stamp.size = info->size;
		stamp.mtime = info->modifiedTime;
		return true;
	}

	bool StagedSource::ReadText(const std::string& logicalPath, std::string& text, std::string& error) {
		const auto cached = m_texts.find(logicalPath);
		if (cached != m_texts.end()) {
			text = cached->second;
			return true;
		}
		FileStamp stamp;
		std::string contents;
		if (!m_files.Read(logicalPath, contents, stamp, error)) {
			if (error.find("changed while reading") != std::string::npos) m_sawChange = true;
			return false;
		}
		m_texts[logicalPath] = contents;
		m_stamps.push_back(ModuleStamp{ logicalPath, stamp });
		text = std::move(contents);
		return true;
	}

	bool StagedSource::StillCurrent(std::string& changedPath) {
		for (const ModuleStamp& staged : m_stamps) {
			FileStamp now;
			if (!m_files.Stat(staged.path, now) || now != staged.stamp) {
				changedPath = staged.path;
				return false;
			}
		}
		return true;
	}

	StageOutcome StageWorld(const RuntimeLimits& limits, ScriptFiles& files, const ProjectContent& content,
		SceneGraph& scene, const nlohmann::json& authoredEntities, const StateSnapshot* snapshot,
		const SceneDocuments* documents) {
		StageOutcome outcome;
		for (int attempt = 1; attempt <= kMaxStagingAttempts; ++attempt) {
			outcome.attempts = attempt;
			auto source = std::make_unique<StagedSource>(files);
			StagedSource* staged = source.get();
			auto world = std::make_unique<LuaWorld>(limits, nullptr, std::move(source));
			if (documents) world->SetSceneDocuments(documents->game, documents->root);
			world->SetStaging(true);

			// Staging output is not published: count it and report once.
			auto discarded = std::make_shared<int>(0);
			world->GetRuntime().SetLogSink([discarded](const std::string&) { ++*discarded; });
			const bool loaded = world->Load(content, scene, authoredEntities);
			world->GetRuntime().SetLogSink(nullptr);
			if (*discarded > 0) {
				world->AddDiagnosticNote(Diagnostic::Severity::Warning, "reload",
					std::to_string(*discarded) + " line(s) printed while loading were discarded");
			}

			if (!loaded) {
				if (staged->SawChange() && attempt < kMaxStagingAttempts) continue;   // files moved: try again
				outcome.reason = FirstErrorText(*world);
				outcome.world = std::move(world);
				return outcome;
			}
			if (snapshot) {
				std::string reason;
				if (!world->ImportSnapshot(*snapshot, reason)) {
					outcome.reason = reason;
					outcome.world = std::move(world);
					return outcome;
				}
			}
			{
				std::string reason;
				if (!world->RunConfigureHook(reason)) {
					outcome.reason = reason;
					outcome.world = std::move(world);
					return outcome;
				}
			}
			std::string changed;
			if (!staged->StillCurrent(changed)) {
				// A module changed while it was being staged: discard the candidate and stage again.
				continue;
			}
			world->SetStaging(false);
			outcome.ok = true;
			outcome.stamps = staged->Stamps();
			outcome.world = std::move(world);
			return outcome;
		}
		outcome.ok = false;
		outcome.reason = "project files kept changing during staging (" + std::to_string(kMaxStagingAttempts) + " attempts)";
		return outcome;
	}
}
