// Model hot reload (docs/editor.md, "Model hot reload"). The bookkeeping here touches no GL and no window: which files a
// model was decoded from, when a changed file is stable enough to decode, the generation each model path has reached,
// and which scene nodes use a model. ModelHotReloader ties them to the ModelManager and the scene graph; the editor
// calls it once per frame at the frame boundary, and the automation command assets.reload calls it on demand.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Scene;
class SceneNode;
class SceneGraph;
class ModelManager;

// One file a model was decoded from. A file that does not exist (yet) keeps its entry with exists == false.
struct ModelFileEntry {
    std::string path;          // logical asset path
    bool exists = false;
    uint64_t size = 0;
    int64_t mtime = -1;        // seconds since epoch; -1 when unknown

    bool operator==(const ModelFileEntry& other) const {
        return path == other.path && exists == other.exists && size == other.size && mtime == other.mtime;
    }
    bool operator!=(const ModelFileEntry& other) const { return !(*this == other); }
};
using ModelFileStamp = std::vector<ModelFileEntry>;

// The model file first, then the external buffers and images a .gltf names, resolved next to it (data: URIs are
// skipped). A .glb or .obj lists only itself. Reads the .gltf text through AssetIO.
std::vector<std::string> ModelDependencyPaths(const std::string& modelPath);

// Size and modification time of each path now (AssetIO, so loose and packed projects resolve the same way).
ModelFileStamp StatModelFiles(const std::vector<std::string>& paths);

// Per-path generation and debounce state. A model path enters with Track() when it is decoded. Poll() says when a
// changed set of files is ready to decode; RecordSuccess() or RecordFailure() then records the attempt.
class ModelReloadWatch {
public:
    // A changed stamp must stay unchanged this long before it is decoded (a file still being written).
    static constexpr double kDebounceSeconds = 0.3;

    // Starts watching a path whose current decoded copy came from decodedFrom. Generation 0.
    void Track(const std::string& path, const ModelFileStamp& decodedFrom);
    void Untrack(const std::string& path);
    bool IsTracked(const std::string& path) const;
    std::vector<std::string> TrackedPaths() const;

    // Called with the current stamp of a tracked path. True when the stamp differs from the last attempted one and
    // has been unchanged for kDebounceSeconds: decode now. A missing main file is never ready (the write may be in
    // progress; the entry is kept and the debounce restarts when it reappears).
    bool Poll(const std::string& path, const ModelFileStamp& current, double nowSeconds);
    // Stamp of the last attempt that differs from the current one (used by forced reloads to see "changed").
    bool Changed(const std::string& path, const ModelFileStamp& current) const;

    // The decode succeeded: the generation advances by one and decodedFrom becomes the reference stamp.
    void RecordSuccess(const std::string& path, const ModelFileStamp& decodedFrom);
    // The decode failed: the generation stays, the old model stays, and the same bytes are not retried until they change.
    void RecordFailure(const std::string& path, const ModelFileStamp& attempted);

    uint64_t Generation(const std::string& path) const;   // 0 for an untracked path

private:
    struct Entry {
        uint64_t generation = 0;
        ModelFileStamp attempted;          // last decoded or attempted stamp
        bool hasCandidate = false;
        ModelFileStamp candidate;          // changed stamp seen by Poll, not yet stable
        double candidateSince = 0.0;
    };
    std::map<std::string, Entry> m_entries;
};

// Every node in the subtree (the root included) whose model is `model`, depth first.
std::vector<std::shared_ptr<SceneNode>> CollectNodesUsingModel(const std::shared_ptr<SceneNode>& root, const Scene* model);

// Driver for the editor and the automation command. Owns the watch and the list of retired models: a model replaced by
// a reload is kept until the next frame starts, so no draw of the frame that retargeted still refers to freed buffers.
class ModelHotReloader {
public:
    struct Outcome {
        std::string path;
        bool reloaded = false;
        int nodes = 0;             // nodes retargeted (when reloaded)
        std::string message;       // the log line, also printed to the console
    };

    // Once per editor frame. Stats the cached models (at most every 0.5 s), and reloads those whose files changed and
    // have been stable. Returns the reloads attempted by this call (empty when nothing changed).
    std::vector<Outcome> Poll(ModelManager& models, SceneGraph* graph, double nowSeconds);
    // assets.reload: path empty = every cached model whose files changed; otherwise that model, decoded again whether or
    // not its files changed. A path that is not loaded gives one failed outcome.
    std::vector<Outcome> Reload(ModelManager& models, SceneGraph* graph, const std::string& path);
    // Frees models retired by earlier reloads. Call at the start of a frame.
    void FreeRetired();
    size_t RetiredCount() const { return m_retired.size(); }
    const ModelReloadWatch& Watch() const { return m_watch; }

private:
    Outcome ReloadOne(ModelManager& models, SceneGraph* graph, const std::string& path);
    void SyncTracked(ModelManager& models);

    ModelReloadWatch m_watch;
    struct Retired {
        std::shared_ptr<Scene> model;
        int framesHeld = 0;
    };
    std::vector<Retired> m_retired;
    double m_lastPollSeconds = -1.0;
};
