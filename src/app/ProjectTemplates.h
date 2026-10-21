#pragma once
// New-project templates (docs/engine_overview.md "Making a new project", docs/packaging.md "Creating a new game").
//
// A template is templates/<id>/ with template.json (name, description), packaging.json (the packaging profile
// template) and project/ (the project tree: project.json, input.json, scripts/, scenes/, prefabs/, README.md).
// Creating a project copies project/ into projects/<name>/ and writes packaging/<name>.json, substituting
// {{NAME}}, {{PROJECT_DIR}} and {{MAIN_SCENE}}. The editor (File > New Project) and the command line
// (Nox_Engine.exe --new-project) both use this module. It works on the source checkout only.
#include <filesystem>
#include <string>
#include <vector>

namespace ProjectTemplates {
	struct TemplateInfo {
		std::string id;           // folder name under templates/
		std::string name;         // display name
		std::string description;  // one or two sentences for the picker
	};

	struct CreateRequest {
		std::string name;                 // project name: letters, digits, '_' and '-', starts with a letter
		std::string templateId;
		std::filesystem::path root;       // source checkout (ProjectBootstrap::Info().sourceRoot)
		std::filesystem::path projectDir; // empty = <root>/projects/<name>; otherwise must be <root>/projects/<folder>
		bool overwrite = false;           // replace files that already exist (nothing is deleted)
	};

	struct CreateResult {
		std::filesystem::path projectDir;   // absolute
		std::string projectDescriptor;      // "projects/<folder>/project.json"
		std::string mainScene;              // "projects/<folder>/scenes/main.json"
		std::string profilePath;            // "packaging/<name>.json"
		std::string profileName;            // the profile name (= project name)
	};

	// Templates found under <root>/templates, sorted by id. Entries without a readable template.json are skipped.
	std::vector<TemplateInfo> List(const std::filesystem::path& root);

	// False with a message when the name is empty, too long or has characters outside the allowed set.
	bool ValidateName(const std::string& name, std::string& error);

	// Creates the project and its packaging profile. Refuses existing project folders and profiles unless
	// overwrite is set. On failure nothing is removed; files already written stay for inspection.
	bool Create(const CreateRequest& request, CreateResult& result, std::string& error);

	// The packaging profile (repo-relative "packaging/<name>.json") whose "project" is the given descriptor
	// ("projects/<name>/project.json"), or "" when no profile names it.
	std::string FindProfileForProject(const std::filesystem::path& root, const std::string& descriptor);

	// Profile name for a profile path ("packaging/x.json" -> "x").
	std::string ProfileNameFor(const std::string& profilePath);
}
