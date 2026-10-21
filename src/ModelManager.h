// ModelManager.h
#pragma once
#include <string>
#include <memory>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "ModelHotReload.h"

class Scene;
class ModelManager {
public:
    ModelManager();
    ~ModelManager();

    std::shared_ptr<Scene> LoadModel(const std::string& path);
    void UnloadModel(const std::string& path);

    // Get the original path for a loaded model
    std::string GetModelPath(const Scene* model) const;

    // Hot reload (ModelHotReload.h). DecodeModel parses and builds a new Scene without touching the cache; stampBefore
    // receives the model's files as they were before the decode, so a write during the decode is seen by the next poll.
    // Null when the file does not load.
    std::shared_ptr<Scene> DecodeModel(const std::string& path, ModelFileStamp* stampBefore);
    // Makes model the cached one for path and returns the model it replaces (null when there was none).
    std::shared_ptr<Scene> InstallModel(const std::string& path, const std::shared_ptr<Scene>& model, const ModelFileStamp& decodedFrom);
    // The cached model of path without loading it (null when not cached).
    std::shared_ptr<Scene> CachedModel(const std::string& path) const;
    // The files the cached model of path was decoded from; null when the path is not cached.
    const ModelFileStamp* DecodedStamp(const std::string& path) const;
    std::vector<std::string> CachedModelPaths() const;

    // Scene-load prefetch: decodes the CPU half of these glTF files (parse, image decode) on a worker thread.
    // A later LoadModel of the same path takes the decoded copy, so only the GPU build runs on the caller's thread.
    // The loaded model is identical to a direct LoadModel. Main thread only, except the worker itself.
    void QueuePrefetch(const std::vector<std::string>& paths);
    // Drops prefetches that have not started and waits for the one in progress.
    void CancelPrefetch();
    struct PrefetchProgress {
        size_t decoded = 0;  // files whose CPU decode finished since the last CancelPrefetch
        size_t queued = 0;   // files queued since the last CancelPrefetch
    };
    PrefetchProgress GetPrefetchProgress() const;
    // True while a queued or in-flight prefetch of this path has not finished decoding (LoadModel would wait).
    bool IsPrefetchAwaiting(const std::string& path) const;
    // Waits until none of these paths is still decoding, for at most maxMs (maxMs < 0: no limit). True when ready.
    bool WaitWhileAwaiting(const std::vector<std::string>& paths, double maxMs);

private:
    std::unordered_map<std::string, std::shared_ptr<Scene>> m_modelCache;

    // Reverse lookup from model to path
    std::unordered_map<const Scene*, std::string> m_modelToPathMap;

    // Files each cached model was decoded from (hot reload compares them with the disk).
    std::unordered_map<std::string, ModelFileStamp> m_decodedStamps;

    struct Prefetcher;
    std::unique_ptr<Prefetcher> m_prefetch;
};
