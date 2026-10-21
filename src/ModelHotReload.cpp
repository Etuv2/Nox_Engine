#include "ModelHotReload.h"

#include "ModelManager.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneNode.h"
#include "assets/AssetIO.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>

namespace {

// The .gltf is re-read from disk on every reload, so its external files are tracked by the names it lists now.
bool EndsWithGltf(const std::string& path) {
    if (path.size() < 5) return false;
    std::string ext = path.substr(path.size() - 5);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".gltf";
}

} // namespace

std::vector<std::string> ModelDependencyPaths(const std::string& modelPath)
{
    std::vector<std::string> paths{ modelPath };
    if (!EndsWithGltf(modelPath)) return paths;

    const std::optional<std::string> text = AssetIO::ReadText(modelPath);
    if (!text) return paths;
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
    if (!doc.is_object()) return paths;

    auto addUris = [&](const char* key) {
        if (!doc.contains(key) || !doc[key].is_array()) return;
        for (const nlohmann::json& item : doc[key]) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) continue;
            const std::string uri = item["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) continue;
            const std::optional<std::string> resolved = AssetIO::ResolveRelative(modelPath, uri);
            if (resolved && std::find(paths.begin(), paths.end(), *resolved) == paths.end()) paths.push_back(*resolved);
        }
    };
    addUris("buffers");
    addUris("images");
    return paths;
}

ModelFileStamp StatModelFiles(const std::vector<std::string>& paths)
{
    ModelFileStamp stamp;
    stamp.reserve(paths.size());
    for (const std::string& path : paths) {
        ModelFileEntry entry;
        entry.path = path;
        if (const auto info = AssetIO::Stat(path); info && !info->isDirectory) {
            entry.exists = true;
            entry.size = info->size;
            entry.mtime = info->modifiedTime;
        }
        stamp.push_back(entry);
    }
    return stamp;
}

// ===== ModelReloadWatch =====

void ModelReloadWatch::Track(const std::string& path, const ModelFileStamp& decodedFrom)
{
    Entry& entry = m_entries[path];
    if (entry.attempted.empty()) entry.attempted = decodedFrom;
}

void ModelReloadWatch::Untrack(const std::string& path)
{
    m_entries.erase(path);
}

bool ModelReloadWatch::IsTracked(const std::string& path) const
{
    return m_entries.count(path) != 0;
}

std::vector<std::string> ModelReloadWatch::TrackedPaths() const
{
    std::vector<std::string> paths;
    paths.reserve(m_entries.size());
    for (const auto& entry : m_entries) paths.push_back(entry.first);
    return paths;
}

bool ModelReloadWatch::Poll(const std::string& path, const ModelFileStamp& current, double nowSeconds)
{
    auto it = m_entries.find(path);
    if (it == m_entries.end()) return false;
    Entry& entry = it->second;
    if (current.empty() || !current.front().exists) {   // the model file itself is missing: mid-write or moved
        entry.hasCandidate = false;
        return false;
    }
    if (current == entry.attempted) {
        entry.hasCandidate = false;
        return false;
    }
    if (!entry.hasCandidate || current != entry.candidate) {
        entry.hasCandidate = true;
        entry.candidate = current;
        entry.candidateSince = nowSeconds;
        return false;
    }
    if (nowSeconds - entry.candidateSince < kDebounceSeconds) return false;
    entry.hasCandidate = false;
    return true;
}

bool ModelReloadWatch::Changed(const std::string& path, const ModelFileStamp& current) const
{
    auto it = m_entries.find(path);
    if (it == m_entries.end()) return false;
    return !current.empty() && current.front().exists && current != it->second.attempted;
}

void ModelReloadWatch::RecordSuccess(const std::string& path, const ModelFileStamp& decodedFrom)
{
    Entry& entry = m_entries[path];
    ++entry.generation;
    entry.attempted = decodedFrom;
    entry.hasCandidate = false;
}

void ModelReloadWatch::RecordFailure(const std::string& path, const ModelFileStamp& attempted)
{
    Entry& entry = m_entries[path];
    entry.attempted = attempted;
    entry.hasCandidate = false;
}

uint64_t ModelReloadWatch::Generation(const std::string& path) const
{
    auto it = m_entries.find(path);
    return it != m_entries.end() ? it->second.generation : 0;
}

// ===== Scene graph =====

std::vector<std::shared_ptr<SceneNode>> CollectNodesUsingModel(const std::shared_ptr<SceneNode>& root, const Scene* model)
{
    std::vector<std::shared_ptr<SceneNode>> nodes;
    if (!root || !model) return nodes;
    std::vector<std::shared_ptr<SceneNode>> stack{ root };
    while (!stack.empty()) {
        std::shared_ptr<SceneNode> node = stack.back();
        stack.pop_back();
        if (node->GetModel().get() == model) nodes.push_back(node);
        for (auto child = node->children.rbegin(); child != node->children.rend(); ++child) stack.push_back(*child);
    }
    return nodes;
}

// ===== ModelHotReloader =====

void ModelHotReloader::SyncTracked(ModelManager& models)
{
    for (const std::string& path : models.CachedModelPaths()) {
        if (const ModelFileStamp* stamp = models.DecodedStamp(path)) m_watch.Track(path, *stamp);
    }
    for (const std::string& path : m_watch.TrackedPaths()) {
        if (!models.CachedModel(path)) m_watch.Untrack(path);
    }
}

std::vector<ModelHotReloader::Outcome> ModelHotReloader::Poll(ModelManager& models, SceneGraph* graph, double nowSeconds)
{
    std::vector<Outcome> outcomes;
    if (m_lastPollSeconds >= 0.0 && nowSeconds - m_lastPollSeconds < 0.5) return outcomes;
    m_lastPollSeconds = nowSeconds;

    SyncTracked(models);
    for (const std::string& path : m_watch.TrackedPaths()) {
        const ModelFileStamp* decoded = models.DecodedStamp(path);
        if (!decoded) continue;
        std::vector<std::string> files;
        for (const ModelFileEntry& entry : *decoded) files.push_back(entry.path);
        if (m_watch.Poll(path, StatModelFiles(files), nowSeconds)) {
            outcomes.push_back(ReloadOne(models, graph, path));
        }
    }
    return outcomes;
}

std::vector<ModelHotReloader::Outcome> ModelHotReloader::Reload(ModelManager& models, SceneGraph* graph, const std::string& path)
{
    std::vector<Outcome> outcomes;
    if (!path.empty()) {
        if (!models.CachedModel(path)) {
            Outcome outcome;
            outcome.path = path;
            outcome.message = "model is not loaded: " + path;
            outcomes.push_back(outcome);
            return outcomes;
        }
        outcomes.push_back(ReloadOne(models, graph, path));
        return outcomes;
    }

    SyncTracked(models);
    for (const std::string& tracked : m_watch.TrackedPaths()) {
        const ModelFileStamp* decoded = models.DecodedStamp(tracked);
        if (!decoded) continue;
        std::vector<std::string> files;
        for (const ModelFileEntry& entry : *decoded) files.push_back(entry.path);
        if (m_watch.Changed(tracked, StatModelFiles(files))) outcomes.push_back(ReloadOne(models, graph, tracked));
    }
    return outcomes;
}

void ModelHotReloader::FreeRetired()
{
    // A replaced model is released with its last reference: MeshComponent frees its GPU vertex input and buffers in
    // its destructor. A render item cache may still hold the model until the next frame is prepared, so a model that
    // is still shared is kept for a few frames before the retired entry is dropped.
    constexpr int kMaxFramesHeld = 3;
    std::vector<Retired> keep;
    for (Retired& retired : m_retired) {
        if (retired.model.use_count() == 1) continue;  // last reference: destroyed with this entry
        if (++retired.framesHeld < kMaxFramesHeld) {
            keep.push_back(std::move(retired));
        }
        // else: dropped here; the model lives on in its remaining users and is released with the last of them.
    }
    m_retired = std::move(keep);
}

ModelHotReloader::Outcome ModelHotReloader::ReloadOne(ModelManager& models, SceneGraph* graph, const std::string& path)
{
    Outcome outcome;
    outcome.path = path;
    const std::shared_ptr<Scene> previous = models.CachedModel(path);

    ModelFileStamp stamp;
    std::shared_ptr<Scene> fresh = models.DecodeModel(path, &stamp);
    if (!fresh) {
        m_watch.RecordFailure(path, stamp);
        outcome.message = "[HotReload] could not decode " + path + "; keeping the previous model until its files change again";
        std::cerr << outcome.message << std::endl;
        return outcome;
    }

    std::shared_ptr<SceneNode> root = graph ? graph->GetRoot() : nullptr;
    const std::vector<std::shared_ptr<SceneNode>> users = CollectNodesUsingModel(root, previous.get());
    int retargeted = 0;
    if (!users.empty()) {
        ComponentManager* components = graph->GetComponentManager();
        retargeted = SceneLoader::RetargetModelNodes(components, users, *previous, fresh, path);
        if (retargeted < 0) {
            m_watch.RecordFailure(path, stamp);
            outcome.message = "[HotReload] could not reload model " + path + ": its node count changed ("
                + std::to_string(previous->nodes.size()) + " -> " + std::to_string(fresh->nodes.size())
                + "); keeping the previous model. Reload the scene to use the new one";
            std::cerr << outcome.message << std::endl;
            return outcome;
        }
    }

    if (std::shared_ptr<Scene> replaced = models.InstallModel(path, fresh, stamp)) {
        m_retired.push_back(Retired{ std::move(replaced), 0 });
    }
    m_watch.RecordSuccess(path, stamp);

    outcome.reloaded = true;
    outcome.nodes = retargeted;
    outcome.message = "[HotReload] reloaded model " + path + " (" + std::to_string(retargeted) + " nodes retargeted)";
    std::cout << outcome.message << std::endl;
    return outcome;
}
