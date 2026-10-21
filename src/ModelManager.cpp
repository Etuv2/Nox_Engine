#include "ModelManager.h"
#include "Scene.h"
#include "SceneNode.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

// Workers that decode glTF files ahead of LoadModel. Only Scene::PrepareGLTF runs on them: it touches no GL,
// no Scene state and no ModelManager state. Everything else stays on the thread that owns the caches.
struct ModelManager::Prefetcher {
    mutable std::mutex mutex;
    std::condition_variable workAvailable;   // workers wait for queue entries
    std::condition_variable resultReady;     // LoadModel and CancelPrefetch wait for decodes
    std::deque<std::string> queue;           // not started yet
    std::set<std::string> inFlight;          // being decoded right now
    std::unordered_map<std::string, std::shared_ptr<PreparedGltf>> results;  // decoded, not yet taken
    std::set<std::string> pending;           // queued or in flight, for de-duplication
    size_t decoded = 0;
    size_t queued = 0;
    bool stop = false;
    std::vector<std::thread> workers;

    static size_t WorkerCount() {
        // Decoding is CPU-bound and independent per file. Leave cores for the main thread and the driver.
        const unsigned hw = std::thread::hardware_concurrency();
        const size_t n = hw > 2 ? hw - 2 : 1;
        return std::min<size_t>(n, 4);
    }

    void Run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            workAvailable.wait(lock, [this] { return stop || !queue.empty(); });
            if (stop) return;
            const std::string path = std::move(queue.front());
            queue.pop_front();
            inFlight.insert(path);
            lock.unlock();
            std::shared_ptr<PreparedGltf> prepared = Scene::PrepareGLTF(path);
            lock.lock();
            inFlight.erase(path);
            results[path] = std::move(prepared);
            ++decoded;
            resultReady.notify_all();
        }
    }
};

ModelManager::ModelManager() : m_prefetch(std::make_unique<Prefetcher>()) {}

ModelManager::~ModelManager() {
    {
        std::lock_guard<std::mutex> lock(m_prefetch->mutex);
        m_prefetch->stop = true;
        m_prefetch->queue.clear();
    }
    m_prefetch->workAvailable.notify_all();
    for (std::thread& worker : m_prefetch->workers) {
        if (worker.joinable()) worker.join();
    }
}

void ModelManager::QueuePrefetch(const std::vector<std::string>& paths)
{
    std::unique_lock<std::mutex> lock(m_prefetch->mutex);
    for (const std::string& path : paths) {
        if (m_modelCache.count(path) || m_prefetch->pending.count(path)) continue;
        m_prefetch->pending.insert(path);
        m_prefetch->queue.push_back(path);
        ++m_prefetch->queued;
    }
    if (!m_prefetch->queue.empty() && m_prefetch->workers.empty()) {
        const size_t count = Prefetcher::WorkerCount();
        for (size_t i = 0; i < count; ++i) {
            m_prefetch->workers.emplace_back([this] { m_prefetch->Run(); });
        }
    }
    lock.unlock();
    m_prefetch->workAvailable.notify_all();
}

void ModelManager::CancelPrefetch()
{
    std::unique_lock<std::mutex> lock(m_prefetch->mutex);
    m_prefetch->queue.clear();
    m_prefetch->resultReady.wait(lock, [this] { return m_prefetch->inFlight.empty(); });
    m_prefetch->results.clear();
    m_prefetch->pending.clear();
    m_prefetch->decoded = 0;
    m_prefetch->queued = 0;
}

ModelManager::PrefetchProgress ModelManager::GetPrefetchProgress() const
{
    std::lock_guard<std::mutex> lock(m_prefetch->mutex);
    return PrefetchProgress{ m_prefetch->decoded, m_prefetch->queued };
}

bool ModelManager::IsPrefetchAwaiting(const std::string& path) const
{
    if (m_modelCache.count(path)) return false;
    std::lock_guard<std::mutex> lock(m_prefetch->mutex);
    return m_prefetch->pending.count(path) != 0 && m_prefetch->results.count(path) == 0;
}

bool ModelManager::WaitWhileAwaiting(const std::vector<std::string>& paths, double maxMs)
{
    std::unique_lock<std::mutex> lock(m_prefetch->mutex);
    auto ready = [this, &paths] {
        for (const std::string& path : paths) {
            if (m_modelCache.count(path)) continue;
            if (m_prefetch->pending.count(path) && !m_prefetch->results.count(path)) return false;
        }
        return true;
    };
    if (maxMs < 0.0) {
        m_prefetch->resultReady.wait(lock, ready);
        return true;
    }
    return m_prefetch->resultReady.wait_for(lock, std::chrono::duration<double, std::milli>(maxMs), ready);
}

std::shared_ptr<Scene> ModelManager::LoadModel(const std::string& path)
{
    auto it = m_modelCache.find(path);
    if (it != m_modelCache.end())
        return it->second;

    // Take the decoded copy when the prefetch produced one, wait for the decode in progress, or decode here.
    std::shared_ptr<PreparedGltf> prepared;
    bool havePrepared = false;
    {
        std::unique_lock<std::mutex> lock(m_prefetch->mutex);
        if (m_prefetch->pending.count(path)) {
            if (m_prefetch->inFlight.count(path)) {
                m_prefetch->resultReady.wait(lock, [this, &path] { return m_prefetch->results.count(path) != 0; });
            }
            auto found = m_prefetch->results.find(path);
            if (found != m_prefetch->results.end()) {
                prepared = std::move(found->second);
                m_prefetch->results.erase(found);
                havePrepared = true;
            }
            else {
                // Still queued: remove it so a worker does not decode it a second time.
                for (auto q = m_prefetch->queue.begin(); q != m_prefetch->queue.end(); ++q) {
                    if (*q == path) { m_prefetch->queue.erase(q); break; }
                }
            }
            m_prefetch->pending.erase(path);
        }
    }
    if (havePrepared && !prepared) {
        std::cerr << "Failed to load model: " << path << std::endl;
        return nullptr;
    }

    const ModelFileStamp stamp = StatModelFiles(ModelDependencyPaths(path));
    std::shared_ptr<Scene> model = std::make_shared<Scene>();
    if (!model->LoadFromGLTF(path, prepared)) {
        std::cerr << "Failed to load model: " << path << std::endl;
        return nullptr;
    }

    m_modelCache[path] = model;
    m_decodedStamps[path] = stamp;

    // Track the path for this model
    m_modelToPathMap[model.get()] = path;

    return model;
}

std::shared_ptr<Scene> ModelManager::DecodeModel(const std::string& path, ModelFileStamp* stampBefore)
{
    if (stampBefore) *stampBefore = StatModelFiles(ModelDependencyPaths(path));
    auto model = std::make_shared<Scene>();
    if (!model->LoadFromGLTF(path, nullptr)) return nullptr;
    return model;
}

std::shared_ptr<Scene> ModelManager::InstallModel(const std::string& path, const std::shared_ptr<Scene>& model, const ModelFileStamp& decodedFrom)
{
    std::shared_ptr<Scene> previous;
    auto it = m_modelCache.find(path);
    if (it != m_modelCache.end()) {
        previous = it->second;
        m_modelToPathMap.erase(previous.get());
    }
    m_modelCache[path] = model;
    m_modelToPathMap[model.get()] = path;
    m_decodedStamps[path] = decodedFrom;
    return previous;
}

std::shared_ptr<Scene> ModelManager::CachedModel(const std::string& path) const
{
    auto it = m_modelCache.find(path);
    return it != m_modelCache.end() ? it->second : nullptr;
}

const ModelFileStamp* ModelManager::DecodedStamp(const std::string& path) const
{
    auto it = m_decodedStamps.find(path);
    return it != m_decodedStamps.end() ? &it->second : nullptr;
}

std::vector<std::string> ModelManager::CachedModelPaths() const
{
    std::vector<std::string> paths;
    paths.reserve(m_modelCache.size());
    for (const auto& entry : m_modelCache) paths.push_back(entry.first);
    return paths;
}

void ModelManager::UnloadModel(const std::string& path)
{
    auto it = m_modelCache.find(path);
    if (it != m_modelCache.end()) {
        // Remove from reverse map
        m_modelToPathMap.erase(it->second.get());
        m_modelCache.erase(it);
    }
    m_decodedStamps.erase(path);
}

// Get the original path for a loaded model
std::string ModelManager::GetModelPath(const Scene* model) const {
    auto it = m_modelToPathMap.find(model);
    if (it != m_modelToPathMap.end()) {
        return it->second;
    }
    return "";
}
