// ShaderLoader.h
#pragma once

#include <GL/glew.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

// Helper: load entire file into a string
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

// Compile a single shader from source strings
inline GLuint CompileShaderFromStrings(const std::vector<const char*>& srcs, GLenum type)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, (GLsizei)srcs.size(), srcs.data(), nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[1024];
        glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
        std::cerr << "ERROR::SHADER::COMPILATION_FAILED\n" << infoLog << "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Original two-arg loader
inline GLuint LoadShader(const char* filePath, GLenum shaderType) {
    std::string code;
    if (!LoadFileToString(filePath, code))
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

// NEW OVERLOAD: inject defines *after* the #version line in the fragment shader
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

// NEW: Create compute shader program
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

// NEW: Create shader program with geometry shader
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
