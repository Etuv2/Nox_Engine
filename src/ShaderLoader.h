// ShaderLoader.h
#pragma once

#include <GL/glew.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <set>
#include <filesystem>

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
    
    // Normalize the file path
    std::string normalizedPath;
    try {
        normalizedPath = std::filesystem::canonical(filePath).string();
    } catch (const std::filesystem::filesystem_error& e) {
        // File doesn't exist, try with original path
        normalizedPath = filePath;
    }
    
    // Check for circular includes
    if (includedFiles.count(normalizedPath) > 0) {
        // Already included - skip (this is not an error, just skip re-inclusion)
        outSource = "";
        return true;
    }
    includedFiles.insert(normalizedPath);
    
    // Load the file
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "ERROR::SHADER::INCLUDE_FILE_NOT_FOUND: " << filePath << "\n";
        return false;
    }
    
    std::string baseDir = GetDirectoryFromPath(filePath);
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
            std::string fullIncludePath = NormalizePath(baseDir, includePath);
            
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
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR::SHADER::FILE_NOT_FOUND: " << path << "\n";
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
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

// Compile a single shader from source strings
inline GLuint CompileShaderFromStrings(const std::vector<const char*>& srcs, GLenum type)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, (GLsizei)srcs.size(), srcs.data(), nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[2048];
        glGetShaderInfoLog(shader, 2048, nullptr, infoLog);
        std::cerr << "ERROR::SHADER::COMPILATION_FAILED\n" << infoLog << "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Load shader with include support
inline GLuint LoadShader(const char* filePath, GLenum shaderType) {
    std::string code;
    if (!LoadFileWithIncludes(filePath, code))
        return 0;
    const char* src = code.c_str();
    return CompileShaderFromStrings({ src }, shaderType);
}

inline GLuint CreateShaderProgram(const char* vertexShaderPath,
    const char* fragmentShaderPath)
{
    GLuint vs = LoadShader(vertexShaderPath, GL_VERTEX_SHADER);
    GLuint fs = LoadShader(fragmentShaderPath, GL_FRAGMENT_SHADER);
    if (!vs || !fs) {
        std::cerr << "ERROR::SHADER::PROGRAM_CREATION_FAILED (load)\n";
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

//inject defines *after* the #version line in the fragment shader
inline GLuint CreateShaderProgram(const char* vertexShaderPath,
    const char* fragmentShaderPath,
    const std::vector<std::string>& definesAfterVersion)
{
    // 1) Vertex: normal load
    GLuint vs = LoadShader(vertexShaderPath, GL_VERTEX_SHADER);
    if (!vs) return 0;

    // 2) Fragment: load, then insert defines
    std::string fragCode;
    if (!LoadFileToString(fragmentShaderPath, fragCode)) {
        glDeleteShader(vs);
        return 0;
    }

    // Find end of first line (#version ...)
    size_t pos = fragCode.find('\n');
    std::string header = fragCode.substr(0, pos + 1);
    std::string rest = fragCode.substr(pos + 1);

    // Build final source array: header, each define, rest
    std::vector<std::string> allSrc;
    allSrc.push_back(header);
    for (auto& d : definesAfterVersion)
        allSrc.push_back(d + "\n");
    allSrc.push_back(rest);

    // Prepare const char* array
    std::vector<const char*> srcPtrs;
    for (auto& s : allSrc)
        srcPtrs.push_back(s.c_str());

    GLuint fs = CompileShaderFromStrings(srcPtrs, GL_FRAGMENT_SHADER);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    // 3) Link program
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    // cleanup
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

//Create compute shader program
inline GLuint CreateComputeShader(const char* computeShaderPath)
{
    GLuint cs = LoadShader(computeShaderPath, GL_COMPUTE_SHADER);
    if (!cs) {
        std::cerr << "ERROR::COMPUTE_SHADER::LOAD_FAILED: " << computeShaderPath << "\n";
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, cs);
    glLinkProgram(prog);

    GLint success = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[1024];
        glGetProgramInfoLog(prog, 1024, nullptr, infoLog);
        std::cerr << "ERROR::COMPUTE_SHADER::LINK_FAILED\n" << infoLog << "\n";
        glDeleteShader(cs);
        glDeleteProgram(prog);
        return 0;
    }

    glDeleteShader(cs);
    return prog;
}

//Create shader program with geometry shader
inline GLuint CreateShaderProgramWithGeometry(const char* vertexShaderPath,
                                              const char* geometryShaderPath,
                                              const char* fragmentShaderPath)
{
    GLuint vs = LoadShader(vertexShaderPath, GL_VERTEX_SHADER);
    GLuint gs = LoadShader(geometryShaderPath, GL_GEOMETRY_SHADER);
    GLuint fs = LoadShader(fragmentShaderPath, GL_FRAGMENT_SHADER);

    if (!vs || !gs || !fs) {
        std::cerr << "ERROR::SHADER::GEOMETRY_PROGRAM_CREATION_FAILED\n";
        if (vs) glDeleteShader(vs);
        if (gs) glDeleteShader(gs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, gs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint success = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[1024];
        glGetProgramInfoLog(prog, 1024, nullptr, infoLog);
        std::cerr << "ERROR::SHADER::GEOMETRY_PROGRAM_LINK_FAILED\n" << infoLog << "\n";
        glDeleteShader(vs);
        glDeleteShader(gs);
        glDeleteShader(fs);
        glDeleteProgram(prog);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(gs);
    glDeleteShader(fs);
    return prog;
}
