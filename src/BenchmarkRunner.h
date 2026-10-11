#pragma once
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"

class Camera;
class SceneGraph;
class SceneNode;

/*
* Deterministic camera-path benchmark and capture runner (docs/nfs2015/README.md, M0).
*
* Shots are authored in the scene file under "benchmark": { "shots": [...] }. Each shot drives
* the camera (keyframed or following a node) and optionally animates named nodes along spline
* paths. Simulation uses a fixed timestep so captures are repeatable.
*
* Environment:
*   NOX_BENCHMARK=all | name[,name...]   enable and select shots
*   NOX_BENCHMARK_MODE=timing | capture  timing: no image writes; capture: PNG frames at
*                                        capture_times (and every frame with capture_sequence)
*   NOX_BENCHMARK_OUT=dir                output directory (default captures/benchmark)
*   NOX_BENCHMARK_REPEAT=n               repeat the shot list n times (timing stability)
*
* Timing definitions: frame_ms = wall time between consecutive rendered frames (what the player
* sees, includes the swap); cpu_ms = main-thread update + render submission of the frame; gpu_ms =
* sum of the per-pass GPU timestamp durations (queries read back a few frames late).
*
* Outputs: frames.csv (one row per frame), summary.json and summary.md (median/p95/p99/max of
* frame and GPU time per shot plus per-pass GPU medians), PNGs in capture mode.
*/
class BenchmarkRunner {
public:
	struct PassSample {
		std::string name;
		float gpuMs = 0.0f;
		float cpuMs = 0.0f;
	};

	// Returns nullptr when NOX_BENCHMARK is unset or the scene has no shots.
	static std::unique_ptr<BenchmarkRunner> CreateFromEnvironment(const std::string& sceneFilePath);

	float FixedDeltaTime() const { return m_fixedDt; }
	bool IsCaptureMode() const { return m_captureMode; }
	bool IsFinished() const { return m_finished; }

	// Poses the camera and animated nodes for the current frame. Call once per Update.
	void Update(Camera& camera, SceneGraph& sceneGraph);
	// True once when a shot begins (temporal history should be reset).
	bool ConsumeCameraCut();
	// Call after the scene is rendered (before UI) with the latest per-pass metrics. cpuMs is the
	// main-thread time of this frame from the start of the update until the render was submitted
	// (excludes the buffer swap, where the CPU waits for the GPU or the display).
	void OnFrameRendered(int width, int height, const std::vector<PassSample>& passes, float cpuMs);

private:
	struct Key {
		float t = 0.0f;
		glm::vec3 position{ 0.0f };
		glm::vec3 lookAt{ 0.0f, 0.0f, 1.0f };
		glm::vec3 rotation{ 0.0f }; // node keys: Euler degrees
		float fov = 60.0f;
	};
	struct NodeTrack {
		std::string nodeName;
		std::vector<Key> keys;
		bool faceMotion = true;
		float yawOffset = 0.0f;
		std::weak_ptr<SceneNode> node;
	};
	struct Follow {
		std::string nodeName;
		glm::vec3 offset{ 0.0f, 1.6f, -6.0f };   // node-local
		glm::vec3 lookOffset{ 0.0f, 0.8f, 4.0f }; // node-local
		float damping = 0.0f;                     // 0 = rigid
		float fov = 60.0f;
		float speedFovGain = 0.0f;                // extra degrees per m/s
		std::weak_ptr<SceneNode> node;
	};
	struct Shot {
		std::string name;
		float duration = 5.0f;
		std::vector<Key> cameraKeys;
		bool hasFollow = false;
		Follow follow;
		std::vector<NodeTrack> tracks;
		std::vector<float> captureTimes;
		bool captureSequence = false;
	};
	struct FrameRecord {
		int shot = -1;
		int frame = 0;
		float simTime = 0.0f;
		float wallMs = 0.0f;
		float cpuMs = 0.0f;
		float gpuMs = 0.0f;
		std::string passes;
	};

	bool Load(const nlohmann::json& benchmarkJson, const std::string& filter);
	static Key SampleKeys(const std::vector<Key>& keys, float t);
	void ResolveNodes(SceneGraph& sceneGraph, Shot& shot);
	void BeginShot(int index);
	void WriteResults();

	std::vector<Shot> m_shots;
	std::string m_outDir;
	std::string m_sceneName;
	float m_fixedDt = 1.0f / 60.0f;
	int m_warmupFrames = 90;
	int m_repeat = 1;
	bool m_captureMode = false;

	int m_currentShot = -1;
	int m_pass = 0;
	int m_shotFrame = 0;
	int m_warmupRemaining = 0;
	float m_shotTime = 0.0f;
	bool m_cut = false;
	bool m_finished = false;
	size_t m_nextCapture = 0;
	bool m_frameIsMeasured = false;
	glm::vec3 m_followPos{ 0.0f };
	glm::vec3 m_prevFollowTarget{ 0.0f };
	bool m_followInitialized = false;

	std::chrono::steady_clock::time_point m_lastFrameEnd{};
	bool m_haveLastFrameEnd = false;
	std::chrono::steady_clock::time_point m_createdAt{};
	float m_firstFrameMs = -1.0f;
	std::vector<FrameRecord> m_records;
	unsigned long long m_firstFrameFileTime = 0; // FILETIME (100 ns) of the first measured frame
	std::vector<float> m_warmupWallMs;
};
