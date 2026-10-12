// ShaderLoader.h
#pragma once

#include "rhi/RHI.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <set>
#include <filesystem>
#include <optional>
#include "assets/AssetIO.h"

// === Include Processing ===

// Extract directory from a file path
inline std::string GetDirectoryFromPath(const std::string& path) {
    std::filesystem::path p(path);
    return p.parent_path().string();
}

// Normalize path separators and resolve relative paths
inline std::string NormalizePath(const std::string& basePath, const std::string& includePath) {
    std::filesystem::path base(basePath);
    std::filesystem::path include(includePath);
    
    // If include path is absolute, use it directly
    if (include.is_absolute()) {
        return include.lexically_normal().string();
    }
    
    // Combine base directory with include path
    std::filesystem::path combined = base / include;
    return combined.lexically_normal().string();
}

// Normalize path separators for display in #line directives (use forward slashes)
inline std::string NormalizePathForDisplay(const std::string& path) {
    std::string result = path;
    for (size_t i = 0; i < result.length(); ++i) {
        if (result[i] == '\\') {
            result[i] = '/';
        }
    }
    return result;
}

// Process #include directives recursively
// Returns true on success, false on error (circular include or file not found)
inline bool ProcessIncludesRecursive(const std::string& filePath, 
                                      std::string& outSource,
                                      std::set<std::string>& includedFiles,
                                      int depth = 0) {
    // Prevent infinite recursion
    const int MAX_INCLUDE_DEPTH = 32;
    if (depth > MAX_INCLUDE_DEPTH) {
        std::cerr << "ERROR::SHADER::INCLUDE_DEPTH_EXCEEDED: " << filePath << "\n";
        return false;
    }
    
    // Logical path (AssetIO normalization, no physical canonicalization) identifies the file
    std::string normalizedPath = AssetIO::Normalize(filePath).value_or(filePath);
    
    // Check for circular includes
    if (includedFiles.count(normalizedPath) > 0) {
        // Already included - skip (this is not an error, just skip re-inclusion)
        outSource = "";
        return true;
    }
    includedFiles.insert(normalizedPath);
    
    // Load the file through the asset service
    std::optional<std::string> fileText = AssetIO::ReadText(filePath);
    if (!fileText) {
        std::cerr << "ERROR::SHADER::INCLUDE_FILE_NOT_FOUND: " << filePath << "\n";
        return false;
    }
    std::istringstream file(*fileText);
    std::ostringstream result;
    std::string line;
    int lineNumber = 0;
    
    while (std::getline(file, line)) {
        lineNumber++;
        
        // Check for #include directive
        size_t includePos = line.find("#include");
        if (includePos != std::string::npos) {
            // Find the include path (supports both <> and "" syntax)
            size_t startQuote = line.find_first_of("\"<", includePos + 8);
            size_t endQuote = std::string::npos;
            
            if (startQuote != std::string::npos) {
                char closeChar = (line[startQuote] == '"') ? '"' : '>';
                endQuote = line.find(closeChar, startQuote + 1);
            }
            
            if (startQuote == std::string::npos || endQuote == std::string::npos) {
                std::cerr << "ERROR::SHADER::MALFORMED_INCLUDE at " << filePath 
                          << ":" << lineNumber << "\n";
                return false;
            }
            
            std::string includePath = line.substr(startQuote + 1, endQuote - startQuote - 1);
            // Includes resolve relative to the including file's logical directory.
            std::optional<std::string> resolved = AssetIO::ResolveRelative(filePath, includePath);
            if (!resolved) {
                std::cerr << "ERROR::SHADER::INVALID_INCLUDE: " << includePath << " at " << filePath
                          << ":" << lineNumber << "\n";
                return false;
            }
            std::string fullIncludePath = *resolved;
            
            // Add #line directive for better error messages (use forward slashes in path)
            std::string displayPath = NormalizePathForDisplay(fullIncludePath);
            result << "\n#line 1 \"" << displayPath << "\"\n";
            
            // Recursively process the included file
            std::string includedContent;
            if (!ProcessIncludesRecursive(fullIncludePath, includedContent, includedFiles, depth + 1)) {
                std::cerr << "  included from " << filePath << ":" << lineNumber << "\n";
                return false;
            }
            
            result << includedContent;
            
            // Restore line directive to current file (use forward slashes)
            std::string currentDisplayPath = NormalizePathForDisplay(filePath);
            result << "\n#line " << (lineNumber + 1) << " \"" << currentDisplayPath << "\"\n";
        } else {
            result << line << "\n";
        }
    }
    
    outSource = result.str();
    return true;
}

// Main function to load a shader file with include processing
inline bool LoadShaderSourceWithIncludes(const std::string& filePath, std::string& outSource) {
    std::set<std::string> includedFiles;
    return ProcessIncludesRecursive(filePath, outSource, includedFiles);
}

// === Original Functions (updated to use include processing) ===

// Helper: load entire file into a string (simple version without includes)
inline bool LoadFileToString(const char* path, std::string& out)
{
    std::optional<std::string> text = AssetIO::ReadText(path);
    if (!text) {
        std::cerr << "ERROR::SHADER::FILE_NOT_FOUND: " << path << "\n";
        return false;
    }
    out = std::move(*text);
    if (out.empty()) {
        std::cerr << "ERROR::SHADER::EMPTY_SHADER_CODE: " << path << "\n";
        return false;
    }
    return true;
}

// Load file with include processing
inline bool LoadFileWithIncludes(const char* path, std::string& out)
{
    return LoadShaderSourceWithIncludes(std::string(path), out);
}

// === Program creation (rhi::Device) ===
// Sources are loaded here (with #include resolution); compilation and linking happen in the backend.
// Every helper returns an invalid handle (and logs) when a file is missing or the program fails.

inline rhi::ProgramHandle CreateProgramFromSources(std::vector<rhi::ShaderSource> stages, const std::string& name)
{
    rhi::ProgramDesc desc;
    desc.stages = std::move(stages);
    desc.debugName = name;
    return rhi::Gpu().CreateProgram(desc);
}

inline bool LoadStage(const char* path, rhi::ShaderStage stage, std::vector<rhi::ShaderSource>& out)
{
    rhi::ShaderSource src;
    src.stage = stage;
    src.debugPath = path;
    if (!LoadFileWithIncludes(path, src.source)) return false;
    out.push_back(std::move(src));
    return true;
}

inline rhi::ProgramHandle CreateShaderProgram(const char* vertexShaderPath,
    const char* fragmentShaderPath)
{
    std::vector<rhi::ShaderSource> stages;
    if (!LoadStage(vertexShaderPath, rhi::ShaderStage::Vertex, stages) ||
        !LoadStage(fragmentShaderPath, rhi::ShaderStage::Fragment, stages)) {
        std::cerr << "ERROR::SHADER::PROGRAM_CREATION_FAILED (load)\n";
        return {};
    }
    return CreateProgramFromSources(std::move(stages), std::string(vertexShaderPath) + " + " + fragmentShaderPath);
}

// Injects defines *after* the #version line of the fragment shader (fragment loaded without includes).
inline rhi::ProgramHandle CreateShaderProgram(const char* vertexShaderPath,
    const char* fragmentShaderPath,
    const std::vector<std::string>& definesAfterVersion)
{
    std::vector<rhi::ShaderSource> stages;
    if (!LoadStage(vertexShaderPath, rhi::ShaderStage::Vertex, stages)) return {};

    std::string fragCode;
    if (!LoadFileToString(fragmentShaderPath, fragCode)) return {};
    const size_t pos = fragCode.find('\n');
    std::string combined = fragCode.substr(0, pos + 1);
    for (const auto& d : definesAfterVersion) combined += d + "\n";
    combined += fragCode.substr(pos + 1);

    rhi::ShaderSource fs;
    fs.stage = rhi::ShaderStage::Fragment;
    fs.source = std::move(combined);
    fs.debugPath = fragmentShaderPath;
    stages.push_back(std::move(fs));
    return CreateProgramFromSources(std::move(stages), std::string(vertexShaderPath) + " + " + fragmentShaderPath);
}

// Create compute shader program
inline rhi::ProgramHandle CreateComputeShader(const char* computeShaderPath)
{
    std::vector<rhi::ShaderSource> stages;
    if (!LoadStage(computeShaderPath, rhi::ShaderStage::Compute, stages)) {
        std::cerr << "ERROR::COMPUTE_SHADER::LOAD_FAILED: " << computeShaderPath << "\n";
        return {};
    }
    return CreateProgramFromSources(std::move(stages), computeShaderPath);
}

// Create shader program with geometry shader
inline rhi::ProgramHandle CreateShaderProgramWithGeometry(const char* vertexShaderPath,
                                                         const char* geometryShaderPath,
                                                         const char* fragmentShaderPath)
{
    std::vector<rhi::ShaderSource> stages;
    if (!LoadStage(vertexShaderPath, rhi::ShaderStage::Vertex, stages) ||
        !LoadStage(geometryShaderPath, rhi::ShaderStage::Geometry, stages) ||
        !LoadStage(fragmentShaderPath, rhi::ShaderStage::Fragment, stages)) {
        std::cerr << "ERROR::SHADER::GEOMETRY_PROGRAM_CREATION_FAILED\n";
        return {};
    }
    return CreateProgramFromSources(std::move(stages), std::string(vertexShaderPath) + " + " + geometryShaderPath + " + " + fragmentShaderPath);
}
