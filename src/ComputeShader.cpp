#include "ComputeShader.h"
#include "Texture.h"
#include "ShaderLoader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <glm/gtc/type_ptr.hpp>

// Static member initialization
bool ComputeShader::s_supportChecked = false;
bool ComputeShader::s_isSupported = false;

ComputeShader::ComputeShader()
	: m_isValid(false)
	, m_timingEnabled(false)
	, m_lastExecutionTime(0.0f)
{

	// Set default dispatch configuration
	m_dispatchConfig.numGroupsX = 1;
	m_dispatchConfig.numGroupsY = 1;
	m_dispatchConfig.numGroupsZ = 1;
	m_dispatchConfig.autoCalculateGroups = false;
}

ComputeShader::~ComputeShader() {
	Cleanup();
}

bool ComputeShader::CreateFromFile(const std::string& computeShaderPath,
	const std::vector<std::string>& defines) {
	// Check compute shader support
	if (!IsSupported()) {
		std::cerr << "[ComputeShader] Compute shaders are not supported on this hardware" << std::endl;
		return false;
	}

	// Load shader source from file with include processing
	std::string source;
	if (!LoadFileWithIncludes(computeShaderPath.c_str(), source)) {
		std::cerr << "[ComputeShader] Failed to load compute shader file: " << computeShaderPath << std::endl;
		return false;
	}

	std::cout << "[ComputeShader] Loading compute shader: " << computeShaderPath << std::endl;

	return CompileShader(source, defines);
}

bool ComputeShader::CreateFromSource(const std::string& source,
	const std::vector<std::string>& defines) {
	// Check compute shader support
	if (!IsSupported()) {
		std::cerr << "[ComputeShader] Compute shaders are not supported on this hardware" << std::endl;
		return false;
	}

	std::cout << "[ComputeShader] Creating compute shader from source" << std::endl;

	return CompileShader(source, defines);
}

bool ComputeShader::IsValid() const {
	return m_isValid && rhi::Gpu().IsProgram(m_programID);
}

rhi::ProgramHandle ComputeShader::GetProgramID() const {
	return m_programID;
}

void ComputeShader::Cleanup() {
	if (m_programID && rhi::HasGpu()) {
		rhi::Gpu().DestroyProgram(m_programID);
	}

	CleanupTiming();

	m_isValid = false;
	m_uniformLocations.clear();
	m_boundBuffers.clear();
	m_boundTextures.clear();
}

void ComputeShader::SetDispatchConfig(const DispatchConfig& config) {
	m_dispatchConfig = config;

	// Auto-calculate work groups if requested
	if (m_dispatchConfig.autoCalculateGroups) {
		m_dispatchConfig.numGroupsX = CalculateWorkGroups(
			m_dispatchConfig.problemSizeX, m_dispatchConfig.localWorkGroupX);
		m_dispatchConfig.numGroupsY = CalculateWorkGroups(
			m_dispatchConfig.problemSizeY, m_dispatchConfig.localWorkGroupY);
		m_dispatchConfig.numGroupsZ = CalculateWorkGroups(
			m_dispatchConfig.problemSizeZ, m_dispatchConfig.localWorkGroupZ);

		std::cout << "[ComputeShader] Auto-calculated work groups: "
			<< m_dispatchConfig.numGroupsX << "x"
			<< m_dispatchConfig.numGroupsY << "x"
			<< m_dispatchConfig.numGroupsZ << std::endl;
	}
}

void ComputeShader::Dispatch() {
	if (!IsValid()) {
		std::cerr << "[ComputeShader] Cannot dispatch invalid compute shader" << std::endl;
		return;
	}

	// Start timing if enabled
	rhi::Device& gpu = rhi::Gpu();
	if (m_timingEnabled) {
		gpu.WriteTimestamp(m_timeQueries[0]);
	}

	// Use the program and dispatch
	gpu.UseProgram(m_programID);
	gpu.Dispatch(m_dispatchConfig.numGroupsX,
		m_dispatchConfig.numGroupsY,
		m_dispatchConfig.numGroupsZ);

	// End timing if enabled
	if (m_timingEnabled) {
		gpu.WriteTimestamp(m_timeQueries[1]);

		// Get timing results (this will block until GPU is done)
		const uint64_t startTime = gpu.GetQueryResult(m_timeQueries[0]);
		const uint64_t endTime = gpu.GetQueryResult(m_timeQueries[1]);

		m_lastExecutionTime = (endTime - startTime) / 1000000.0f; // Convert to milliseconds
	}
}

void ComputeShader::Dispatch(uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ) {
	m_dispatchConfig.numGroupsX = numGroupsX;
	m_dispatchConfig.numGroupsY = numGroupsY;
	m_dispatchConfig.numGroupsZ = numGroupsZ;
	m_dispatchConfig.autoCalculateGroups = false;

	Dispatch();
}

void ComputeShader::WaitForCompletion(uint32_t barriers) {
	rhi::Gpu().PipelineBarrier(barriers);
}

void ComputeShader::BindBuffer(rhi::BufferHandle buffer, uint32_t bindingPoint) {
	rhi::Gpu().BindStorageBuffer(bindingPoint, buffer);

	// Track bound buffers for cleanup
	if (std::find(m_boundBuffers.begin(), m_boundBuffers.end(), bindingPoint) == m_boundBuffers.end()) {
		m_boundBuffers.push_back(bindingPoint);
	}
}

void ComputeShader::BindTexture(rhi::TextureHandle texture, uint32_t unit, int level,
	bool layered, int layer,
	rhi::ImageAccess access, rhi::Format format) {
	rhi::Gpu().BindImage(unit, texture, level, layered, layer, access, format);

	// Track bound textures for cleanup
	if (std::find(m_boundTextures.begin(), m_boundTextures.end(), unit) == m_boundTextures.end()) {
		m_boundTextures.push_back(unit);
	}
}

void ComputeShader::BindTextureRead(rhi::TextureHandle texture, uint32_t unit, int level) {
	// Sampled on `unit`, and bound read-only as an image in its own format.
	rhi::Gpu().BindTexture(unit, texture);
	const rhi::Format format = rhi::Gpu().GetTextureInfo(texture).format;
	BindTexture(texture, unit, level, false, 0, rhi::ImageAccess::ReadOnly, format);
}

void ComputeShader::BindTextureWrite(rhi::TextureHandle texture, uint32_t unit, int level, rhi::Format format) {
	BindTexture(texture, unit, level, false, 0, rhi::ImageAccess::WriteOnly, format);
}

void ComputeShader::UnbindBuffer(uint32_t bindingPoint) {
	rhi::Gpu().BindStorageBuffer(bindingPoint, {});

	// Remove from tracking
	auto it = std::find(m_boundBuffers.begin(), m_boundBuffers.end(), bindingPoint);
	if (it != m_boundBuffers.end()) {
		m_boundBuffers.erase(it);
	}
}

void ComputeShader::UnbindTexture(uint32_t unit) {
	rhi::Gpu().BindImage(unit, {}, 0, false, 0, rhi::ImageAccess::ReadOnly, rhi::Format::RGBA8);

	// Remove from tracking
	auto it = std::find(m_boundTextures.begin(), m_boundTextures.end(), unit);
	if (it != m_boundTextures.end()) {
		m_boundTextures.erase(it);
	}
}

void ComputeShader::SetUniform(const std::string& name, int value) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniform(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, float value) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniform(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::vec2& value) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniform(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::vec3& value) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniform(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::vec4& value) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniform(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::mat4& value) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniform(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, const std::vector<float>& values) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1 && !values.empty()) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniformArray(location, values.data(), static_cast<int>(values.size()));
	}
}

void ComputeShader::SetUniform(const std::string& name, const std::vector<glm::vec3>& values) {
	const rhi::UniformLocation location = GetUniformLocation(name);
	if (location != -1 && !values.empty()) {
		rhi::Gpu().UseProgram(m_programID);
		rhi::Gpu().SetUniformArray(location, values.data(), static_cast<int>(values.size()));
	}
}

void ComputeShader::EnableTiming(bool enable) {
	if (enable && !m_timingEnabled) {
		SetupTiming();
	}
	else if (!enable && m_timingEnabled) {
		CleanupTiming();
	}
	m_timingEnabled = enable;
}

float ComputeShader::GetLastExecutionTime() const {
	return m_lastExecutionTime;
}

ComputeShader::WorkGroupInfo ComputeShader::GetWorkGroupLimits() {
	WorkGroupInfo info;
	const rhi::DeviceLimits& l = rhi::Gpu().Limits();
	info.maxWorkGroupSizeX = l.maxComputeWorkGroupSize.x;
	info.maxWorkGroupSizeY = l.maxComputeWorkGroupSize.y;
	info.maxWorkGroupSizeZ = l.maxComputeWorkGroupSize.z;
	info.maxWorkGroupInvocations = l.maxComputeWorkGroupInvocations;
	info.maxComputeWorkGroupCount[0] = l.maxComputeWorkGroupCount.x;
	info.maxComputeWorkGroupCount[1] = l.maxComputeWorkGroupCount.y;
	info.maxComputeWorkGroupCount[2] = l.maxComputeWorkGroupCount.z;
	info.maxSharedMemorySize = l.maxComputeSharedMemory;

	return info;
}

void ComputeShader::PrintDebugInfo() const {
	std::cout << "[ComputeShader] Debug Information:" << std::endl;
	std::cout << "  Program ID: " << m_programID.id << std::endl;
	std::cout << "  Valid: " << (m_isValid ? "Yes" : "No") << std::endl;
	std::cout << "  Timing Enabled: " << (m_timingEnabled ? "Yes" : "No") << std::endl;
	std::cout << "  Last Execution Time: " << m_lastExecutionTime << " ms" << std::endl;
	std::cout << "  Dispatch Config:" << std::endl;
	std::cout << "    Groups: " << m_dispatchConfig.numGroupsX << "x"
		<< m_dispatchConfig.numGroupsY << "x" << m_dispatchConfig.numGroupsZ << std::endl;
	std::cout << "    Auto-calculate: " << (m_dispatchConfig.autoCalculateGroups ? "Yes" : "No") << std::endl;
	std::cout << "  Bound Buffers: " << m_boundBuffers.size() << std::endl;
	std::cout << "  Bound Textures: " << m_boundTextures.size() << std::endl;

	// Print work group limits
	auto limits = GetWorkGroupLimits();
	std::cout << "  Work Group Limits:" << std::endl;
	std::cout << "    Max Work Group Size: " << limits.maxWorkGroupSizeX << "x"
		<< limits.maxWorkGroupSizeY << "x" << limits.maxWorkGroupSizeZ << std::endl;
	std::cout << "    Max Invocations: " << limits.maxWorkGroupInvocations << std::endl;
	std::cout << "    Max Work Group Count: " << limits.maxComputeWorkGroupCount[0] << "x"
		<< limits.maxComputeWorkGroupCount[1] << "x" << limits.maxComputeWorkGroupCount[2] << std::endl;
	std::cout << "    Max Shared Memory: " << limits.maxSharedMemorySize << " bytes" << std::endl;
}

bool ComputeShader::IsSupported() {
	if (!s_supportChecked) {
		s_supportChecked = true;
		// Compute requires OpenGL 4.3+ on the GL backend (always available on explicit APIs).
		const rhi::DeviceInfo& info = rhi::Gpu().Info();
		const bool gl = info.backend.rfind("OpenGL", 0) == 0;
		s_isSupported = !gl || info.apiMajor > 4 || (info.apiMajor == 4 && info.apiMinor >= 3);
		if (s_isSupported) {
			std::cout << "[ComputeShader] Compute shaders supported (" << info.backend << ")" << std::endl;
		}
		else {
			std::cout << "[ComputeShader] Compute shaders not supported (requires OpenGL 4.3+, got "
				<< info.backend << ")" << std::endl;
		}
	}

	return s_isSupported;
}

uint32_t ComputeShader::CalculateWorkGroups(uint32_t problemSize, uint32_t localWorkGroupSize) {
	return (problemSize + localWorkGroupSize - 1) / localWorkGroupSize;
}

rhi::BufferHandle ComputeShader::CreateComputeBuffer(size_t size, const void* data, rhi::BufferUsage usage) {
	rhi::BufferDesc desc;
	desc.size = size;
	desc.initialData = data;
	desc.usage = usage;
	const rhi::BufferHandle buffer = rhi::Gpu().CreateBuffer(desc);

	std::cout << "[ComputeShader] Created compute buffer: " << buffer.id
		<< " (size: " << size << " bytes)" << std::endl;

	return buffer;
}

TexturePtr ComputeShader::CreateComputeTexture2D(uint32_t width, uint32_t height,
	rhi::Format format, const void* data) {
	auto texture = Texture::Builder::Texture2D(static_cast<int>(width), static_cast<int>(height), format)
		.Data(data)
		.FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
		.WrapMode(rhi::Wrap::ClampToEdge)
		.Build();

	if (!texture || !texture->IsValid()) {
		std::cerr << "[ComputeShader] Failed to create compute texture" << std::endl;
		return nullptr;
	}

	std::cout << "[ComputeShader] Created compute texture: " << texture->ID().id
		<< " (" << width << "x" << height << ", format: " << rhi::FormatName(format) << ")" << std::endl;

	return texture;
}

// === Private Methods ===

rhi::UniformLocation ComputeShader::GetUniformLocation(const std::string& name) {
	auto it = m_uniformLocations.find(name);
	if (it != m_uniformLocations.end()) {
		return it->second;
	}

	const rhi::UniformLocation location = rhi::Gpu().GetUniformLocation(m_programID, name.c_str());
	m_uniformLocations[name] = location;

	if (location == -1) {
		std::cerr << "[ComputeShader] Warning: Uniform '" << name << "' not found" << std::endl;
	}

	return location;
}

bool ComputeShader::CompileShader(const std::string& source, const std::vector<std::string>& defines) {
	// Clean up any existing shader
	Cleanup();

	// Inject preprocessor defines
	std::string finalSource = InjectDefines(source, defines);

	// Compile and link through the device (logs the compiler output on failure).
	rhi::ProgramDesc desc;
	desc.stages.push_back({ rhi::ShaderStage::Compute, finalSource, "compute" });
	desc.debugName = "compute";
	m_programID = rhi::Gpu().CreateProgram(desc);
	if (!m_programID) {
		std::cerr << "[ComputeShader] Compute shader compilation/link failed" << std::endl;
		return false;
	}

	m_isValid = true;

	// Set up timing if requested
	if (m_timingEnabled) {
		SetupTiming();
	}

	std::cout << "[ComputeShader] Successfully compiled and linked compute shader (ID: "
		<< m_programID.id << ")" << std::endl;

	return true;
}

void ComputeShader::SetupTiming() {
	if (!m_timeQueries[0]) {
		m_timeQueries[0] = rhi::Gpu().CreateQuery(rhi::QueryType::Timestamp);
		m_timeQueries[1] = rhi::Gpu().CreateQuery(rhi::QueryType::Timestamp);
	}
}

void ComputeShader::CleanupTiming() {
	if (m_timeQueries[0] && rhi::HasGpu()) {
		rhi::Gpu().DestroyQuery(m_timeQueries[0]);
		rhi::Gpu().DestroyQuery(m_timeQueries[1]);
	}
	m_lastExecutionTime = 0.0f;
}

std::string ComputeShader::InjectDefines(const std::string& source, const std::vector<std::string>& defines) {
	if (defines.empty()) {
		return source;
	}

	// Find the first line (should be #version)
	size_t versionEnd = source.find('\n');
	if (versionEnd == std::string::npos) {
		versionEnd = 0;
	}
	else {
		versionEnd++; // Include the newline
	}

	std::string result = source.substr(0, versionEnd);

	// Add defines after version line
	for (const auto& define : defines) {
		result += "#define " + define + "\n";
	}

	// Add rest of the source
	result += source.substr(versionEnd);

	return result;
}

// === ComputeShaderManager Implementation ===

ComputeShaderManager& ComputeShaderManager::Instance() {
	static ComputeShaderManager instance;
	return instance;
}

bool ComputeShaderManager::RegisterShader(const std::string& name, const std::string& shaderPath,
	const std::vector<std::string>& defines) {
	auto shader = std::make_shared<ComputeShader>();

	if (!shader->CreateFromFile(shaderPath, defines)) {
		std::cerr << "[ComputeShaderManager] Failed to register shader: " << name << std::endl;
		return false;
	}

	m_shaders[name] = shader;
	m_shaderPaths[name] = shaderPath;
	m_shaderDefines[name] = defines;

	std::cout << "[ComputeShaderManager] Registered compute shader: " << name << std::endl;
	return true;
}

std::shared_ptr<ComputeShader> ComputeShaderManager::GetShader(const std::string& name) {
	auto it = m_shaders.find(name);
	if (it != m_shaders.end()) {
		return it->second;
	}

	std::cerr << "[ComputeShaderManager] Shader not found: " << name << std::endl;
	return nullptr;
}

bool ComputeShaderManager::HasShader(const std::string& name) const {
	return m_shaders.find(name) != m_shaders.end();
}

void ComputeShaderManager::RemoveShader(const std::string& name) {
	m_shaders.erase(name);
	m_shaderPaths.erase(name);
	m_shaderDefines.erase(name);

	std::cout << "[ComputeShaderManager] Removed shader: " << name << std::endl;
}

void ComputeShaderManager::Clear() {
	m_shaders.clear();
	m_shaderPaths.clear();
	m_shaderDefines.clear();

	std::cout << "[ComputeShaderManager] Cleared all shaders" << std::endl;
}

ComputeShaderManager::PerformanceStats ComputeShaderManager::GetPerformanceStats() const {
	PerformanceStats stats;
	stats.totalShaders = static_cast<int>(m_shaders.size());
	stats.totalExecutionTime = 0.0f;
	stats.averageExecutionTime = 0.0f;
	stats.slowestTime = 0.0f;

	if (m_shaders.empty()) {
		return stats;
	}

	for (const auto& [name, shader] : m_shaders) {
		float execTime = shader->GetLastExecutionTime();
		stats.totalExecutionTime += execTime;

		if (execTime > stats.slowestTime) {
			stats.slowestTime = execTime;
			stats.slowestShader = name;
		}
	}

	stats.averageExecutionTime = stats.totalExecutionTime / stats.totalShaders;

	return stats;
}

void ComputeShaderManager::PrintPerformanceReport() const {
	auto stats = GetPerformanceStats();

	std::cout << "\n=== Compute Shader Performance Report ===" << std::endl;
	std::cout << "Total Shaders: " << stats.totalShaders << std::endl;
	std::cout << "Total Execution Time: " << stats.totalExecutionTime << " ms" << std::endl;
	std::cout << "Average Execution Time: " << stats.averageExecutionTime << " ms" << std::endl;

	if (!stats.slowestShader.empty()) {
		std::cout << "Slowest Shader: " << stats.slowestShader
			<< " (" << stats.slowestTime << " ms)" << std::endl;
	}

	std::cout << "\nIndividual Shader Times:" << std::endl;
	for (const auto& [name, shader] : m_shaders) {
		std::cout << "  " << name << ": " << shader->GetLastExecutionTime() << " ms" << std::endl;
	}
	std::cout << "========================================\n" << std::endl;
}