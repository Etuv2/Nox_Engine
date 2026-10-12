#pragma once
// Transactional reload staging (docs/lua_api.md, "Reload"). A candidate world is built from a consistent,
// stamp-checked module set in a fresh VM with all host side effects unavailable; the caller swaps it in at a
// frame boundary only when staging succeeded.
#include "LuaWorld.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace NoxLua {

	// Identity of a file version: size and modification time (AssetIO::Stat in the engine).
	struct FileStamp {
		uint64_t size = 0;
		int64_t mtime = -1;
		bool operator==(const FileStamp& other) const { return size == other.size && mtime == other.mtime; }
		bool operator!=(const FileStamp& other) const { return !(*this == other); }
	};

	struct ModuleStamp {
		std::string path;
		FileStamp stamp;
	};

	// File access for project scripts. The engine uses AssetIO (AssetScriptFiles); tests inject memory files.
	class ScriptFiles {
	public:
		virtual ~ScriptFiles() = default;
		// Reads a logical file. Returns false with a reason; a file that changes during the read fails with
		// an error containing "changed while reading" so staging retries.
		virtual bool Read(const std::string& path, std::string& text, FileStamp& stamp, std::string& error) = 0;
		virtual bool Stat(const std::string& path, FileStamp& stamp) = 0;
	};

	class AssetScriptFiles : public ScriptFiles {
	public:
		bool Read(const std::string& path, std::string& text, FileStamp& stamp, std::string& error) override;
		bool Stat(const std::string& path, FileStamp& stamp) override;
	};

	// ScriptSource over ScriptFiles that records the stamp of every module it hands out.
	class StagedSource : public ScriptSource {
	public:
		explicit StagedSource(ScriptFiles& files) : m_files(files) {}
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override;
		const std::vector<ModuleStamp>& Stamps() const { return m_stamps; }
		bool SawChange() const { return m_sawChange; }
		// False (and the first changed path) when any staged module no longer matches its stamp.
		bool StillCurrent(std::string& changedPath);

	private:
		ScriptFiles& m_files;
		std::map<std::string, std::string> m_texts;
		std::vector<ModuleStamp> m_stamps;
		bool m_sawChange = false;
	};

	struct StageOutcome {
		bool ok = false;
		std::string reason;                    // when !ok
		int attempts = 0;
		std::unique_ptr<LuaWorld> world;       // the candidate (ok), or the failed candidate for diagnostics
		std::vector<ModuleStamp> stamps;       // module set of the candidate (ok)
	};

	// Builds a candidate world: loads the project, imports `snapshot` when given, runs the nox.on_configure hook
	// (with `documents` as the scene's game object and scene root), and verifies that no staged file changed
	// meanwhile (up to three attempts). The candidate owns its staged source. Authoring writes are refused in it.
	StageOutcome StageWorld(const RuntimeLimits& limits, ScriptFiles& files, const ProjectContent& content,
		SceneGraph& scene, const nlohmann::json& authoredEntities, const StateSnapshot* snapshot,
		const SceneDocuments* documents = nullptr);
}
