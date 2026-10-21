#include "app/ProjectTemplates.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {
	const char* kNameToken = "{{NAME}}";
	const char* kProjectDirToken = "{{PROJECT_DIR}}";
	const char* kMainSceneToken = "{{MAIN_SCENE}}";

	bool ReadFile(const fs::path& path, std::string& text)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return true;
	}

	bool WriteFile(const fs::path& path, const std::string& text, std::string& error)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		if (ec) {
			error = "cannot create " + path.parent_path().string() + ": " + ec.message();
			return false;
		}
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out) {
			error = "cannot write " + path.string();
			return false;
		}
		return true;
	}

	void Replace(std::string& text, const std::string& token, const std::string& value)
	{
		for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size())) {
			text.replace(at, token.size(), value);
		}
	}

	std::string Substitute(std::string text, const std::string& name, const std::string& projectDir, const std::string& mainScene)
	{
		Replace(text, kNameToken, name);
		Replace(text, kProjectDirToken, projectDir);
		Replace(text, kMainSceneToken, mainScene);
		return text;
	}

	// Forward-slash, repository-relative form of a path that lies inside root.
	std::string RelativeLogical(const fs::path& root, const fs::path& full)
	{
		return fs::relative(full, root).lexically_normal().generic_string();
	}

	bool ReadTemplateInfo(const fs::path& dir, ProjectTemplates::TemplateInfo& info)
	{
		std::string text;
		if (!ReadFile(dir / "template.json", text)) return false;
		const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
		if (!doc.is_object()) return false;
		info.id = dir.filename().string();
		info.name = doc.value("name", info.id);
		info.description = doc.value("description", std::string());
		return true;
	}
}

namespace ProjectTemplates {
	std::vector<TemplateInfo> List(const fs::path& root)
	{
		std::vector<TemplateInfo> result;
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::directory_iterator(root / "templates", ec)) {
			if (!entry.is_directory(ec)) continue;
			TemplateInfo info;
			if (ReadTemplateInfo(entry.path(), info)) result.push_back(info);
		}
		std::sort(result.begin(), result.end(), [](const TemplateInfo& a, const TemplateInfo& b) { return a.id < b.id; });
		return result;
	}

	bool ValidateName(const std::string& name, std::string& error)
	{
		if (name.empty()) { error = "the project name is empty"; return false; }
		if (name.size() > 48) { error = "the project name is longer than 48 characters"; return false; }
		if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) {
			error = "the project name must start with a letter";
			return false;
		}
		for (const char c : name) {
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
			if (!ok) {
				error = "the project name may only use letters, digits, '_' and '-': '" + name + "'";
				return false;
			}
		}
		return true;
	}

	bool Create(const CreateRequest& request, CreateResult& result, std::string& error)
	{
		if (!ValidateName(request.name, error)) return false;
		const fs::path root = fs::absolute(request.root).lexically_normal();
		std::error_code ec;
		if (!fs::is_directory(root / "templates", ec)) {
			error = "no templates folder in " + root.string() + " (new projects need a source checkout)";
			return false;
		}

		// Template lookup: the id must name a folder of templates/ (no path separators).
		if (request.templateId.empty() || request.templateId.find_first_of("/\\.") != std::string::npos) {
			error = "invalid template id '" + request.templateId + "'";
			return false;
		}
		const fs::path templateDir = root / "templates" / request.templateId;
		TemplateInfo info;
		if (!ReadTemplateInfo(templateDir, info)) {
			std::string known;
			for (const TemplateInfo& t : List(root)) known += (known.empty() ? "" : ", ") + t.id;
			error = "unknown template '" + request.templateId + "' (available: " + known + ")";
			return false;
		}

		// Destination: <root>/projects/<folder>, where the folder is the leaf of the requested path.
		fs::path projectDir = request.projectDir.empty() ? root / "projects" / request.name : request.projectDir;
		if (projectDir.is_relative()) projectDir = root / projectDir;
		projectDir = projectDir.lexically_normal();
		if (projectDir.parent_path() != (root / "projects").lexically_normal()) {
			error = "the project folder must be a direct child of projects/: " + projectDir.string();
			return false;
		}
		const std::string folder = projectDir.filename().string();
		if (!ValidateName(folder, error)) return false;
		if (fs::is_symlink(projectDir, ec)) {
			error = "refusing to write through a link: " + projectDir.string();
			return false;
		}

		const fs::path profile = root / "packaging" / (request.name + ".json");
		if (!request.overwrite) {
			if (fs::exists(projectDir, ec)) {
				error = "projects/" + folder + " already exists (choose another name or allow overwrite)";
				return false;
			}
			if (fs::exists(profile, ec)) {
				error = "packaging/" + request.name + ".json already exists (choose another name or allow overwrite)";
				return false;
			}
		}

		result.projectDir = projectDir;
		result.projectDescriptor = "projects/" + folder + "/project.json";
		result.mainScene = "projects/" + folder + "/scenes/main.json";
		result.profilePath = "packaging/" + request.name + ".json";
		result.profileName = request.name;
		const std::string projectRel = "projects/" + folder;

		// Project tree.
		const fs::path source = templateDir / "project";
		if (!fs::is_directory(source, ec)) {
			error = "template " + request.templateId + " has no project/ folder";
			return false;
		}
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source, ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const fs::path rel = fs::relative(entry.path(), source);
			std::string text;
			if (!ReadFile(entry.path(), text)) {
				error = "cannot read template file " + entry.path().string();
				return false;
			}
			if (!WriteFile(projectDir / rel, Substitute(text, request.name, projectRel, result.mainScene), error)) return false;
		}
		if (ec) {
			error = "cannot read template folder " + source.string() + ": " + ec.message();
			return false;
		}

		// Packaging profile from the template.
		std::string profileText;
		if (!ReadFile(templateDir / "packaging.json", profileText)) {
			error = "template " + request.templateId + " has no packaging.json";
			return false;
		}
		if (!WriteFile(profile, Substitute(profileText, request.name, projectRel, result.mainScene), error)) return false;
		return true;
	}

	std::string FindProfileForProject(const fs::path& root, const std::string& descriptor)
	{
		std::error_code ec;
		std::vector<fs::path> files;
		for (const fs::directory_entry& entry : fs::directory_iterator(root / "packaging", ec)) {
			if (entry.is_regular_file(ec) && entry.path().extension() == ".json") files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());
		for (const fs::path& file : files) {
			std::string text;
			if (!ReadFile(file, text)) continue;
			const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
			if (doc.is_object() && doc.value("project", std::string()) == descriptor) {
				return RelativeLogical(root, file);
			}
		}
		return {};
	}

	std::string ProfileNameFor(const std::string& profilePath)
	{
		return fs::path(profilePath).stem().string();
	}
}
