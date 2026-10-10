#include "BenchmarkRunner.h"

#include "Camera.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "stb_image_write.h"

#include "rhi/RHI.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
	std::string GetEnv(const char* name) {
#if defined(_MSC_VER)
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
			return {};
		}
		std::string result(value);
		std::free(value);
		return result;
#else
		const char* value = std::getenv(name);
		return value ? std::string(value) : std::string{};
#endif
	}

	glm::vec3 ReadVec3(const json& j, const char* key, const glm::vec3& fallback) {
		if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) {
			return fallback;
		}
		return glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
	}

	// Centripetal-free uniform Catmull-Rom; endpoints are clamped.
	glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t) {
		const float t2 = t * t, t3 = t2 * t;
		return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
			(-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
	}

	std::shared_ptr<SceneNode> FindByName(const std::shared_ptr<SceneNode>& node, const std::string& name) {
		if (!node) return nullptr;
		if (node->GetName() == name) return node;
		for (const auto& child : node->children) {
			if (auto found = FindByName(child, name)) return found;
		}
		return nullptr;
	}

	float Percentile(std::vector<float> values, float p) {
		if (values.empty()) return 0.0f;
		std::sort(values.begin(), values.end());
		const float rank = p * static_cast<float>(values.size() - 1);
		const size_t lo = static_cast<size_t>(std::floor(rank));
		const size_t hi = std::min(lo + 1, values.size() - 1);
		const float f = rank - static_cast<float>(lo);
		return values[lo] * (1.0f - f) + values[hi] * f;
	}

	bool SavePNG(int width, int height, const fs::path& path) {
		std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
		rhi::Gpu().BindReadFramebuffer(rhi::kBackBuffer);
		rhi::Gpu().ReadPixels(rhi::kBackBuffer, 0, { 0, 0, width, height }, rhi::PixelFormat::RGB, rhi::PixelType::UByte, pixels.data(), 1);
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		stbi_flip_vertically_on_write(1);
		const int ok = stbi_write_png(path.string().c_str(), width, height, 3, pixels.data(), width * 3);
		stbi_flip_vertically_on_write(0);
		return ok != 0;
	}
}

std::unique_ptr<BenchmarkRunner> BenchmarkRunner::CreateFromEnvironment(const std::string& sceneFilePath) {
	const std::string filter = GetEnv("NOX_BENCHMARK");
	if (filter.empty() || filter == "0") {
		return nullptr;
	}
	std::ifstream in(sceneFilePath);
	if (!in) {
		std::cerr << "[Benchmark] Cannot open scene file " << sceneFilePath << std::endl;
		return nullptr;
	}
	json scene = json::parse(in, nullptr, false);
	if (!scene.is_object() || !scene.contains("benchmark")) {
		std::cerr << "[Benchmark] Scene has no \"benchmark\" block: " << sceneFilePath << std::endl;
		return nullptr;
	}
	auto runner = std::unique_ptr<BenchmarkRunner>(new BenchmarkRunner());
	runner->m_sceneName = scene.value("scene_name", std::string("scene"));
	runner->m_captureMode = GetEnv("NOX_BENCHMARK_MODE") == "capture";
	const std::string out = GetEnv("NOX_BENCHMARK_OUT");
	runner->m_outDir = out.empty() ? std::string("captures/benchmark") : out;
	const std::string repeat = GetEnv("NOX_BENCHMARK_REPEAT");
	runner->m_repeat = repeat.empty() ? 1 : std::max(1, std::atoi(repeat.c_str()));
	if (!runner->Load(scene["benchmark"], filter)) {
		return nullptr;
	}
	runner->m_createdAt = std::chrono::steady_clock::now();
	std::cout << "[Benchmark] " << runner->m_shots.size() << " shot(s), mode="
		<< (runner->m_captureMode ? "capture" : "timing") << ", out=" << runner->m_outDir << std::endl;
	return runner;
}

bool BenchmarkRunner::Load(const json& b, const std::string& filter) {
	m_fixedDt = 1.0f / std::max(b.value("fixed_hz", 60.0f), 1.0f);
	m_warmupFrames = std::max(b.value("warmup_frames", 90), 0);
	m_warmupRemaining = m_warmupFrames;
	if (!b.contains("shots") || !b["shots"].is_array()) {
		return false;
	}
	std::vector<std::string> wanted;
	if (filter != "all" && filter != "1") {
		std::stringstream ss(filter);
		std::string item;
		while (std::getline(ss, item, ',')) {
			if (!item.empty()) wanted.push_back(item);
		}
	}
	auto parseKeys = [](const json& arr, bool node) {
		std::vector<Key> keys;
		if (!arr.is_array()) return keys;
		for (const json& k : arr) {
			Key key;
			key.t = k.value("t", 0.0f);
			key.position = ReadVec3(k, "position", glm::vec3(0.0f));
			key.lookAt = ReadVec3(k, "look_at", key.position + glm::vec3(0, 0, 1));
			key.rotation = ReadVec3(k, "rotation", glm::vec3(0.0f));
			key.fov = k.value("fov", 60.0f);
			keys.push_back(key);
		}
		std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& c) { return a.t < c.t; });
		(void)node;
		return keys;
	};
	for (const json& s : b["shots"]) {
		Shot shot;
		shot.name = s.value("name", std::string("shot"));
		if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), shot.name) == wanted.end()) {
			continue;
		}
		shot.duration = std::max(s.value("duration", 5.0f), m_fixedDt);
		if (s.contains("camera")) {
			const json& cam = s["camera"];
			shot.cameraKeys = parseKeys(cam.value("keys", json::array()), false);
			if (cam.contains("follow")) {
				const json& f = cam["follow"];
				shot.hasFollow = true;
				shot.follow.nodeName = f.value("node", std::string());
				shot.follow.offset = ReadVec3(f, "offset", shot.follow.offset);
				shot.follow.lookOffset = ReadVec3(f, "look_offset", shot.follow.lookOffset);
				shot.follow.damping = f.value("damping", 0.0f);
				shot.follow.fov = f.value("fov", 60.0f);
				shot.follow.speedFovGain = f.value("speed_fov_gain", 0.0f);
			}
		}
		if (s.contains("animate") && s["animate"].is_array()) {
			for (const json& a : s["animate"]) {
				NodeTrack track;
				track.nodeName = a.value("node", std::string());
				track.keys = parseKeys(a.value("keys", json::array()), true);
				track.faceMotion = a.value("face_motion", true);
				track.yawOffset = a.value("yaw_offset", 0.0f);
				shot.tracks.push_back(track);
			}
		}
		if (s.contains("capture_times") && s["capture_times"].is_array()) {
			for (const json& t : s["capture_times"]) shot.captureTimes.push_back(t.get<float>());
			std::sort(shot.captureTimes.begin(), shot.captureTimes.end());
		}
		shot.captureSequence = s.value("capture_sequence", false);
		if (shot.cameraKeys.empty() && !shot.hasFollow) {
			std::cerr << "[Benchmark] Shot '" << shot.name << "' has no camera keys or follow target; skipped" << std::endl;
			continue;
		}
		m_shots.push_back(std::move(shot));
	}
	return !m_shots.empty();
}

BenchmarkRunner::Key BenchmarkRunner::SampleKeys(const std::vector<Key>& keys, float t) {
	if (keys.size() == 1 || t <= keys.front().t) return keys.front();
	if (t >= keys.back().t) return keys.back();
	size_t i = 0;
	while (i + 1 < keys.size() && keys[i + 1].t < t) ++i;
	const Key& k1 = keys[i];
	const Key& k2 = keys[i + 1];
	const Key& k0 = keys[i > 0 ? i - 1 : i];
	const Key& k3 = keys[std::min(i + 2, keys.size() - 1)];
	const float u = (t - k1.t) / std::max(k2.t - k1.t, 1e-5f);
	Key out;
	out.t = t;
	out.position = CatmullRom(k0.position, k1.position, k2.position, k3.position, u);
	out.lookAt = CatmullRom(k0.lookAt, k1.lookAt, k2.lookAt, k3.lookAt, u);
	out.rotation = glm::mix(k1.rotation, k2.rotation, u);
	out.fov = glm::mix(k1.fov, k2.fov, u);
	return out;
}

void BenchmarkRunner::ResolveNodes(SceneGraph& sceneGraph, Shot& shot) {
	auto root = sceneGraph.GetRoot();
	for (NodeTrack& track : shot.tracks) {
		if (track.node.expired()) {
			track.node = FindByName(root, track.nodeName);
			if (track.node.expired()) {
				std::cerr << "[Benchmark] Node '" << track.nodeName << "' not found for shot " << shot.name << std::endl;
			}
		}
	}
	if (shot.hasFollow && shot.follow.node.expired()) {
		shot.follow.node = FindByName(root, shot.follow.nodeName);
	}
}

void BenchmarkRunner::BeginShot(int index) {
	m_currentShot = index;
	m_shotFrame = 0;
	m_shotTime = 0.0f;
	m_nextCapture = 0;
	m_cut = true;
	m_followInitialized = false;
	std::cout << "[Benchmark] Shot " << m_shots[index].name << " (pass " << (m_pass + 1) << "/" << m_repeat << ")" << std::endl;
}

bool BenchmarkRunner::ConsumeCameraCut() {
	const bool cut = m_cut;
	m_cut = false;
	return cut;
}

void BenchmarkRunner::Update(Camera& camera, SceneGraph& sceneGraph) {
	if (m_finished) return;
	if (m_currentShot < 0) {
		BeginShot(0);
	}
	Shot& shot = m_shots[m_currentShot];
	ResolveNodes(sceneGraph, shot);

	// During warmup the first shot's opening pose is held so shaders/streaming settle.
	const float t = m_warmupRemaining > 0 ? 0.0f : m_shotTime;

	for (NodeTrack& track : shot.tracks) {
		auto node = track.node.lock();
		if (!node || track.keys.empty()) continue;
		const Key k = SampleKeys(track.keys, t);
		glm::quat orientation = glm::quat(glm::radians(k.rotation));
		if (track.faceMotion && track.keys.size() > 1) {
			const Key ahead = SampleKeys(track.keys, t + 0.05f);
			const Key behind = SampleKeys(track.keys, std::max(t - 0.05f, 0.0f));
			glm::vec3 dir = ahead.position - behind.position;
			dir.y = 0.0f;
			if (glm::dot(dir, dir) > 1e-6f) {
				// Model forward is +Z; yaw about +Y.
				const float yaw = std::atan2(dir.x, dir.z) + glm::radians(track.yawOffset);
				orientation = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
			}
		}
		node->SetLocalTRS(k.position, orientation, node->GetScale());
	}

	glm::vec3 eye, target;
	float fov = 60.0f;
	if (shot.hasFollow) {
		auto node = shot.follow.node.lock();
		const glm::mat4 world = node ? node->GetWorldPosition4x4() : glm::mat4(1.0f);
		// Remove scale from the basis so offsets stay in metres.
		const glm::vec3 right = glm::normalize(glm::vec3(world[0]));
		const glm::vec3 up = glm::normalize(glm::vec3(world[1]));
		const glm::vec3 fwd = glm::normalize(glm::vec3(world[2]));
		const glm::vec3 origin = glm::vec3(world[3]);
		const glm::vec3 desired = origin + right * shot.follow.offset.x + up * shot.follow.offset.y + fwd * shot.follow.offset.z;
		target = origin + right * shot.follow.lookOffset.x + up * shot.follow.lookOffset.y + fwd * shot.follow.lookOffset.z;
		if (!m_followInitialized || shot.follow.damping <= 0.0f) {
			m_followPos = desired;
			m_prevFollowTarget = origin;
			m_followInitialized = true;
		}
		else {
			const float a = 1.0f - std::exp(-shot.follow.damping * m_fixedDt);
			m_followPos = glm::mix(m_followPos, desired, a);
		}
		const float speed = glm::length(origin - m_prevFollowTarget) / m_fixedDt;
		m_prevFollowTarget = origin;
		eye = m_followPos;
		fov = shot.follow.fov + shot.follow.speedFovGain * speed;
	}
	else {
		const Key k = SampleKeys(shot.cameraKeys, t);
		eye = k.position;
		target = k.lookAt;
		fov = k.fov;
	}
	glm::vec3 f = target - eye;
	if (glm::dot(f, f) < 1e-8f) f = glm::vec3(0, 0, 1);
	f = glm::normalize(f);
	const float pitch = glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f)));
	const float yaw = glm::degrees(std::atan2(f.z, f.x));
	camera.SetPose(eye, glm::vec3(0, 1, 0), yaw, pitch);
	camera.SetPerspective(glm::clamp(fov, 10.0f, 120.0f), 1.0f, camera.GetCameraNearPlane(), camera.GetCameraFarPlane());
}

void BenchmarkRunner::OnFrameRendered(int width, int height, const std::vector<PassSample>& passes) {
	if (m_finished || m_currentShot < 0) return;
	const auto now = std::chrono::steady_clock::now();
	const float wallMs = m_haveLastFrameEnd
		? std::chrono::duration<float, std::milli>(now - m_lastFrameEnd).count()
		: 0.0f;
	if (m_firstFrameMs < 0.0f) {
		m_firstFrameMs = std::chrono::duration<float, std::milli>(now - m_createdAt).count();
	}
	m_lastFrameEnd = now;
	m_haveLastFrameEnd = true;

	if (m_warmupRemaining > 0) {
		m_warmupWallMs.push_back(wallMs);
		if (--m_warmupRemaining == 0) {
			m_cut = true; // reset temporal history before measuring
		}
		return;
	}

	Shot& shot = m_shots[m_currentShot];
	float gpuMs = 0.0f;
	std::ostringstream passText;
	for (const PassSample& p : passes) {
		gpuMs += p.gpuMs;
		passText << p.name << ':' << std::fixed << std::setprecision(3) << p.gpuMs << '|';
	}
	m_records.push_back({ m_currentShot, m_shotFrame, m_shotTime, wallMs, gpuMs, passText.str() });

	if (m_captureMode) {
		const fs::path dir = fs::path(m_outDir) / shot.name;
		if (shot.captureSequence) {
			std::ostringstream name;
			name << "seq_" << std::setw(5) << std::setfill('0') << m_shotFrame << ".png";
			SavePNG(width, height, dir / name.str());
		}
		while (m_nextCapture < shot.captureTimes.size() && m_shotTime + 1e-4f >= shot.captureTimes[m_nextCapture]) {
			std::ostringstream name;
			name << "t" << std::fixed << std::setprecision(2) << shot.captureTimes[m_nextCapture] << ".png";
			SavePNG(width, height, dir / name.str());
			++m_nextCapture;
		}
	}

	++m_shotFrame;
	m_shotTime = static_cast<float>(m_shotFrame) * m_fixedDt;
	if (m_shotTime > shot.duration + 1e-4f) {
		if (m_currentShot + 1 < static_cast<int>(m_shots.size())) {
			BeginShot(m_currentShot + 1);
		}
		else if (m_pass + 1 < m_repeat) {
			++m_pass;
			BeginShot(0);
		}
		else {
			WriteResults();
			m_finished = true;
		}
	}
}

void BenchmarkRunner::WriteResults() {
	std::error_code ec;
	fs::create_directories(m_outDir, ec);
	{
		std::ofstream csv(fs::path(m_outDir) / "frames.csv");
		csv << "shot,frame,sim_time_s,frame_ms,gpu_ms,passes\n";
		for (const FrameRecord& r : m_records) {
			csv << m_shots[r.shot].name << ',' << r.frame << ',' << r.simTime << ',' << r.wallMs << ',' << r.gpuMs
				<< ",\"" << r.passes << "\"\n";
		}
	}

	json summary;
	summary["scene"] = m_sceneName;
	summary["mode"] = m_captureMode ? "capture" : "timing";
	summary["fixed_dt"] = m_fixedDt;
	summary["repeat"] = m_repeat;
	summary["cold_start_first_frame_ms"] = m_firstFrameMs;
	summary["warmup"] = {
		{"frames", m_warmupWallMs.size()},
		{"max_ms", m_warmupWallMs.empty() ? 0.0f : *std::max_element(m_warmupWallMs.begin(), m_warmupWallMs.end())},
		{"median_ms", Percentile(m_warmupWallMs, 0.5f)}
	};
	std::ostringstream md;
	md << "# Benchmark: " << m_sceneName << " (" << (m_captureMode ? "capture" : "timing") << ")\n\n";
	md << "Cold start to first frame: " << std::fixed << std::setprecision(1) << m_firstFrameMs << " ms; warmup median "
		<< Percentile(m_warmupWallMs, 0.5f) << " ms, max " << summary["warmup"]["max_ms"].get<float>() << " ms\n\n";
	md << "| Shot | Frames | Frame median | p95 | p99 | max | GPU median | GPU p95 | GPU p99 |\n";
	md << "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";

	std::vector<float> allWall, allGpu;
	json shots = json::array();
	for (size_t s = 0; s < m_shots.size(); ++s) {
		std::vector<float> wall, gpu;
		std::map<std::string, std::vector<float>> perPass;
		for (const FrameRecord& r : m_records) {
			if (r.shot != static_cast<int>(s)) continue;
			// The first frame of a shot follows a cut; it is kept (cuts are part of gameplay).
			wall.push_back(r.wallMs);
			gpu.push_back(r.gpuMs);
			std::stringstream ps(r.passes);
			std::string item;
			while (std::getline(ps, item, '|')) {
				const size_t colon = item.find(':');
				if (colon != std::string::npos) {
					perPass[item.substr(0, colon)].push_back(std::stof(item.substr(colon + 1)));
				}
			}
		}
		allWall.insert(allWall.end(), wall.begin(), wall.end());
		allGpu.insert(allGpu.end(), gpu.begin(), gpu.end());
		json js;
		js["name"] = m_shots[s].name;
		js["frames"] = wall.size();
		js["frame_ms"] = { {"median", Percentile(wall, 0.5f)}, {"p95", Percentile(wall, 0.95f)}, {"p99", Percentile(wall, 0.99f)},
			{"max", wall.empty() ? 0.0f : *std::max_element(wall.begin(), wall.end())} };
		js["gpu_ms"] = { {"median", Percentile(gpu, 0.5f)}, {"p95", Percentile(gpu, 0.95f)}, {"p99", Percentile(gpu, 0.99f)} };
		json passesJson;
		for (const auto& [name, values] : perPass) {
			passesJson[name] = Percentile(values, 0.5f);
		}
		js["pass_gpu_median_ms"] = passesJson;
		shots.push_back(js);
		md << "| " << m_shots[s].name << " | " << wall.size() << " | " << std::setprecision(2)
			<< Percentile(wall, 0.5f) << " | " << Percentile(wall, 0.95f) << " | " << Percentile(wall, 0.99f) << " | "
			<< js["frame_ms"]["max"].get<float>() << " | " << Percentile(gpu, 0.5f) << " | " << Percentile(gpu, 0.95f)
			<< " | " << Percentile(gpu, 0.99f) << " |\n";
	}
	summary["shots"] = shots;
	summary["overall"] = {
		{"frame_ms", { {"median", Percentile(allWall, 0.5f)}, {"p95", Percentile(allWall, 0.95f)}, {"p99", Percentile(allWall, 0.99f)} }},
		{"gpu_ms", { {"median", Percentile(allGpu, 0.5f)}, {"p95", Percentile(allGpu, 0.95f)}, {"p99", Percentile(allGpu, 0.99f)} }}
	};
	md << "| **all** | " << allWall.size() << " | " << Percentile(allWall, 0.5f) << " | " << Percentile(allWall, 0.95f) << " | "
		<< Percentile(allWall, 0.99f) << " | - | " << Percentile(allGpu, 0.5f) << " | " << Percentile(allGpu, 0.95f) << " | "
		<< Percentile(allGpu, 0.99f) << " |\n\n";

	// Per-pass medians across all shots, largest first.
	std::map<std::string, std::vector<float>> passAll;
	for (const FrameRecord& r : m_records) {
		std::stringstream ps(r.passes);
		std::string item;
		while (std::getline(ps, item, '|')) {
			const size_t colon = item.find(':');
			if (colon != std::string::npos) passAll[item.substr(0, colon)].push_back(std::stof(item.substr(colon + 1)));
		}
	}
	std::vector<std::pair<float, std::string>> ranked;
	for (const auto& [name, values] : passAll) ranked.push_back({ Percentile(values, 0.5f), name });
	std::sort(ranked.rbegin(), ranked.rend());
	md << "| Pass | GPU median ms |\n| --- | --- |\n";
	for (const auto& [ms, name] : ranked) md << "| " << name << " | " << std::setprecision(3) << ms << " |\n";

	std::ofstream(fs::path(m_outDir) / "summary.json") << summary.dump(2);
	std::ofstream(fs::path(m_outDir) / "summary.md") << md.str();
	std::cout << "[Benchmark] Results written to " << m_outDir << std::endl << md.str() << std::endl;
}
