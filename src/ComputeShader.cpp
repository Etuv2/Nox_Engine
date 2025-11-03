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
	: m_programID(0)
	, m_isValid(false)
	, m_timingEnabled(false)
	, m_lastExecutionTime(0.0f)
{
	m_timeQueries[0] = 0;
	m_timeQueries[1] = 0;

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

	// Load shader source from file
	std::string source;
	if (!LoadFileToString(computeShaderPath.c_str(), source)) {
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
	return m_isValid && glIsProgram(m_programID);
}

GLuint ComputeShader::GetProgramID() const {
	return m_programID;
}

void ComputeShader::Cleanup() {
	if (m_programID != 0) {
		glDeleteProgram(m_programID);
		m_programID = 0;
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
	if (m_timingEnabled) {
		glQueryCounter(m_timeQueries[0], GL_TIMESTAMP);
	}

	// Use the program and dispatch
	glUseProgram(m_programID);
	glDispatchCompute(m_dispatchConfig.numGroupsX,
		m_dispatchConfig.numGroupsY,
		m_dispatchConfig.numGroupsZ);

	// End timing if enabled
	if (m_timingEnabled) {
		glQueryCounter(m_timeQueries[1], GL_TIMESTAMP);

		// Get timing results (this will block until GPU is done)
		GLuint64 startTime, endTime;
		glGetQueryObjectui64v(m_timeQueries[0], GL_QUERY_RESULT, &startTime);
		glGetQueryObjectui64v(m_timeQueries[1], GL_QUERY_RESULT, &endTime);

		m_lastExecutionTime = (endTime - startTime) / 1000000.0f; // Convert to milliseconds
	}
}

void ComputeShader::Dispatch(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
	m_dispatchConfig.numGroupsX = numGroupsX;
	m_dispatchConfig.numGroupsY = numGroupsY;
	m_dispatchConfig.numGroupsZ = numGroupsZ;
	m_dispatchConfig.autoCalculateGroups = false;

	Dispatch();
}

void ComputeShader::WaitForCompletion(GLbitfield barriers) {
	glMemoryBarrier(barriers);
}

void ComputeShader::BindBuffer(GLuint buffer, GLuint bindingPoint, GLenum access) {
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, buffer);

	// Track bound buffers for cleanup
	if (std::find(m_boundBuffers.begin(), m_boundBuffers.end(), bindingPoint) == m_boundBuffers.end()) {
		m_boundBuffers.push_back(bindingPoint);
	}
}

void ComputeShader::BindTexture(GLuint texture, GLuint unit, GLint level,
	GLboolean layered, GLint layer,
	GLenum access, GLenum format) {
	glBindImageTexture(unit, texture, level, layered, layer, access, format);

	// Track bound textures for cleanup
	if (std::find(m_boundTextures.begin(), m_boundTextures.end(), unit) == m_boundTextures.end()) {
		m_boundTextures.push_back(unit);
	}
}

void ComputeShader::BindTextureRead(GLuint texture, GLuint unit, GLint level) {
	// For read-only access, we need to determine the format
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);

	GLint internalFormat;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);

	BindTexture(texture, unit, level, GL_FALSE, 0, GL_READ_ONLY, internalFormat);
}

void ComputeShader::BindTextureWrite(GLuint texture, GLuint unit, GLint level, GLenum format) {
	BindTexture(texture, unit, level, GL_FALSE, 0, GL_WRITE_ONLY, format);
}

void ComputeShader::UnbindBuffer(GLuint bindingPoint) {
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, 0);

	// Remove from tracking
	auto it = std::find(m_boundBuffers.begin(), m_boundBuffers.end(), bindingPoint);
	if (it != m_boundBuffers.end()) {
		m_boundBuffers.erase(it);
	}
}

void ComputeShader::UnbindTexture(GLuint unit) {
	glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);

	// Remove from tracking
	auto it = std::find(m_boundTextures.begin(), m_boundTextures.end(), unit);
	if (it != m_boundTextures.end()) {
		m_boundTextures.erase(it);
	}
}

void ComputeShader::SetUniform(const std::string& name, int value) {
	GLint location = GetUniformLocation(name);
	if (location != -1) {
		glUseProgram(m_programID);
		glUniform1i(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, float value) {
	GLint location = GetUniformLocation(name);
	if (location != -1) {
		glUseProgram(m_programID);
		glUniform1f(location, value);
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::vec2& value) {
	GLint location = GetUniformLocation(name);
	if (location != -1) {
		glUseProgram(m_programID);
		glUniform2fv(location, 1, glm::value_ptr(value));
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::vec3& value) {
	GLint location = GetUniformLocation(name);
	if (location != -1) {
		glUseProgram(m_programID);
		glUniform3fv(location, 1, glm::value_ptr(value));
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::vec4& value) {
	GLint location = GetUniformLocation(name);
	if (location != -1) {
		glUseProgram(m_programID);
		glUniform4fv(location, 1, glm::value_ptr(value));
	}
}

void ComputeShader::SetUniform(const std::string& name, const glm::mat4& value) {
	GLint location = GetUniformLocation(name);
	if (location != -1) {
		glUseProgram(m_programID);
		glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(value));
	}
}

void ComputeShader::SetUniform(const std::string& name, const std::vector<float>& values) {
	GLint location = GetUniformLocation(name);
	if (location != -1 && !values.empty()) {
		glUseProgram(m_programID);
		glUniform1fv(location, static_cast<GLsizei>(values.size()), values.data());
	}
}

void ComputeShader::SetUniform(const std::string& name, const std::vector<glm::vec3>& values) {
	GLint location = GetUniformLocation(name);
	if (location != -1 && !values.empty()) {
		glUseProgram(m_programID);
		glUniform3fv(location, static_cast<GLsizei>(values.size()), glm::value_ptr(values[0]));
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

	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &info.maxWorkGroupSizeX);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &info.maxWorkGroupSizeY);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, &info.maxWorkGroupSizeZ);

	glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &info.maxWorkGroupInvocations);

	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &info.maxComputeWorkGroupCount[0]);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &info.maxComputeWorkGroupCount[1]);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &info.maxComputeWorkGroupCount[2]);

	glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &info.maxSharedMemorySize);

	return info;
}

void ComputeShader::PrintDebugInfo() const {
	std::cout << "[ComputeShader] Debug Information:" << std::endl;
	std::cout << "  Program ID: " << m_programID << std::endl;
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

		// Check OpenGL version (compute shaders require OpenGL 4.3+)
		GLint majorVersion, minorVersion;
		glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
		glGetIntegerv(GL_MINOR_VERSION, &minorVersion);

		if (majorVersion > 4 || (majorVersion == 4 && minorVersion >= 3)) {
			// Check for compute shader support
			s_isSupported = (glDispatchCompute != nullptr);

			if (s_isSupported) {
				std::cout << "[ComputeShader] Compute shaders supported (OpenGL "
					<< majorVersion << "." << minorVersion << ")" << std::endl;
			}
			else {
				std::cout << "[ComputeShader] Compute shaders not supported (missing functions)" << std::endl;
			}
		}
		else {
			std::cout << "[ComputeShader] Compute shaders not supported (requires OpenGL 4.3+, got "
				<< majorVersion << "." << minorVersion << ")" << std::endl;
			s_isSupported = false;
		}
	}

	return s_isSupported;
}

GLuint ComputeShader::CalculateWorkGroups(GLuint problemSize, GLuint localWorkGroupSize) {
	return (problemSize + localWorkGroupSize - 1) / localWorkGroupSize;
}

GLuint ComputeShader::CreateComputeBuffer(GLsizeiptr size, const void* data, GLenum usage) {
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
	glBufferData(GL_SHADER_STORAGE_BUFFER, size, data, usage);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	std::cout << "[ComputeShader] Created compute buffer: " << buffer
		<< " (size: " << size << " bytes)" << std::endl;

	return buffer;
}

TexturePtr ComputeShader::CreateComputeTexture2D(GLuint width, GLuint height,
	GLenum format, const void* data) {
	// REFACTORED: Use Texture::Builder for cleaner, more maintainable code
	auto texture = Texture::Builder::Texture2D(width, height, format)
		.Data(data)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	if (!texture || !texture->IsValid()) {
		std::cerr << "[ComputeShader] Failed to create compute texture" << std::endl;
		return nullptr;
	}

	std::cout << "[ComputeShader] Created compute texture: " << texture->ID()
		<< " (" << width << "x" << height << ", format: 0x" << std::hex << format << std::dec << ")" << std::endl;

	return texture;
}

// === Private Methods ===

GLint ComputeShader::GetUniformLocation(const std::string& name) {
	auto it = m_uniformLocations.find(name);
	if (it != m_uniformLocations.end()) {
		return it->second;
	}

	GLint location = glGetUniformLocation(m_programID, name.c_str());
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

	// Create and compile compute shader
	GLuint computeShader = glCreateShader(GL_COMPUTE_SHADER);
	const char* sourcePtr = finalSource.c_str();
	glShaderSource(computeShader, 1, &sourcePtr, nullptr);
	glCompileShader(computeShader);

	// Check compilation status
	GLint success;
	glGetShaderiv(computeShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char infoLog[1024];
		glGetShaderInfoLog(computeShader, 1024, nullptr, infoLog);
		std::cerr << "[ComputeShader] Compilation failed:" << std::endl << infoLog << std::endl;
		glDeleteShader(computeShader);
		return false;
	}

	// Create program and attach shader
	m_programID = glCreateProgram();
	glAttachShader(m_programID, computeShader);
	glLinkProgram(m_programID);

	// Check linking status
	glGetProgramiv(m_programID, GL_LINK_STATUS, &success);
	if (!success) {
		char infoLog[1024];
		glGetProgramInfoLog(m_programID, 1024, nullptr, infoLog);
		std::cerr << "[ComputeShader] Linking failed:" << std::endl << infoLog << std::endl;
		glDeleteShader(computeShader);
		glDeleteProgram(m_programID);
		m_programID = 0;
		return false;
	}

	// Clean up shader object (no longer needed after linking)
	glDeleteShader(computeShader);

	m_isValid = true;

	// Set up timing if requested
	if (m_timingEnabled) {
		SetupTiming();
	}

	std::cout << "[ComputeShader] Successfully compiled and linked compute shader (ID: "
		<< m_programID << ")" << std::endl;

	return true;
}

void ComputeShader::SetupTiming() {
	if (m_timeQueries[0] == 0) {
		glGenQueries(2, m_timeQueries);
	}
}

void ComputeShader::CleanupTiming() {
	if (m_timeQueries[0] != 0) {
		glDeleteQueries(2, m_timeQueries);
		m_timeQueries[0] = 0;
		m_timeQueries[1] = 0;
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