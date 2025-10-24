#include "GLState.h"
#include "TextureUnits.h"
#include <iostream>
#include <GL/glew.h>

// GLStateManager implementation

void GLStateManager::GLState::Capture() {
    // Capture texture state
    glGetIntegerv(GL_ACTIVE_TEXTURE, reinterpret_cast<GLint*>(&activeTexture));
    
    for (int i = 0; i < 32; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, reinterpret_cast<GLint*>(&textureUnits[i].texture2D));
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, reinterpret_cast<GLint*>(&textureUnits[i].textureCube));
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, reinterpret_cast<GLint*>(&textureUnits[i].texture2DArray));
        glGetIntegerv(GL_TEXTURE_BINDING_3D, reinterpret_cast<GLint*>(&textureUnits[i].texture3D));
    }
    
    // Restore active texture
    glActiveTexture(activeTexture);
    
    // Capture framebuffer state
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&framebufferDraw));
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&framebufferRead));
    
    // Capture shader state
    glGetIntegerv(GL_CURRENT_PROGRAM, reinterpret_cast<GLint*>(&currentProgram));
    
    // Capture viewport and scissor
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    
    // Capture depth and stencil state
    depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetIntegerv(GL_DEPTH_FUNC, reinterpret_cast<GLint*>(&depthFunc));
    stencilTestEnabled = glIsEnabled(GL_STENCIL_TEST);
    
    // Capture blending state
    blendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, reinterpret_cast<GLint*>(&blendSrcRGB));
    glGetIntegerv(GL_BLEND_DST_RGB, reinterpret_cast<GLint*>(&blendDstRGB));
    glGetIntegerv(GL_BLEND_SRC_ALPHA, reinterpret_cast<GLint*>(&blendSrcAlpha));
    glGetIntegerv(GL_BLEND_DST_ALPHA, reinterpret_cast<GLint*>(&blendDstAlpha));
    glGetIntegerv(GL_BLEND_EQUATION_RGB, reinterpret_cast<GLint*>(&blendEquationRGB));
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, reinterpret_cast<GLint*>(&blendEquationAlpha));
    
    // Capture face culling
    cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CULL_FACE_MODE, reinterpret_cast<GLint*>(&cullFaceMode));
    glGetIntegerv(GL_FRONT_FACE, reinterpret_cast<GLint*>(&frontFace));
    
    // Capture polygon mode
    GLint polygonMode[2];
    glGetIntegerv(GL_POLYGON_MODE, polygonMode);
    polygonModeFront = static_cast<GLenum>(polygonMode[0]);
    polygonModeBack = static_cast<GLenum>(polygonMode[1]);
    
    // Capture vertex array state
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, reinterpret_cast<GLint*>(&vertexArray));
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, reinterpret_cast<GLint*>(&arrayBuffer));
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, reinterpret_cast<GLint*>(&elementArrayBuffer));
}

void GLStateManager::GLState::Restore() const {
    // Restore texture state
    for (int i = 0; i < 32; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textureUnits[i].texture2D);
        glBindTexture(GL_TEXTURE_CUBE_MAP, textureUnits[i].textureCube);
        glBindTexture(GL_TEXTURE_2D_ARRAY, textureUnits[i].texture2DArray);
        glBindTexture(GL_TEXTURE_3D, textureUnits[i].texture3D);
    }
    
    // Restore active texture
    glActiveTexture(activeTexture);
    
    // Restore framebuffer state
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebufferDraw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebufferRead);
    
    // Restore shader state
    glUseProgram(currentProgram);
    
    // Restore viewport and scissor
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    
    // Restore depth and stencil state
    if (depthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(depthMask);
    glDepthFunc(depthFunc);
    if (stencilTestEnabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    
    // Restore blending state
    if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    glBlendEquationSeparate(blendEquationRGB, blendEquationAlpha);
    
    // Restore face culling
    if (cullFaceEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glCullFace(cullFaceMode);
    glFrontFace(frontFace);
    
    // Restore polygon mode
    glPolygonMode(GL_FRONT, polygonModeFront);
    glPolygonMode(GL_BACK, polygonModeBack);
    
    // Restore vertex array state
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementArrayBuffer);
}

void GLStateManager::CaptureState() {
    m_currentState.Capture();
    m_stateDirty = false;
}

void GLStateManager::RestoreState() {
    if (m_stateDirty) {
        m_currentState.Restore();
        m_stateDirty = false;
    }
}

void GLStateManager::PushState() {
    GLState currentState;
    currentState.Capture();
    m_stateStack.push(currentState);
}

void GLStateManager::PopState() {
    if (!m_stateStack.empty()) {
        m_stateStack.top().Restore();
        m_stateStack.pop();
    }
}

void GLStateManager::BindTexture2D(GLenum unit, GLuint texture) {
    GLenum textureUnit = GL_TEXTURE0 + (unit - GL_TEXTURE0);
    glActiveTexture(textureUnit);
    glBindTexture(GL_TEXTURE_2D, texture);
    m_stateDirty = true;
}

void GLStateManager::BindTextureCube(GLenum unit, GLuint texture) {
    GLenum textureUnit = GL_TEXTURE0 + (unit - GL_TEXTURE0);
    glActiveTexture(textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
    m_stateDirty = true;
}

void GLStateManager::BindTexture2DArray(GLenum unit, GLuint texture) {
    GLenum textureUnit = GL_TEXTURE0 + (unit - GL_TEXTURE0);
    glActiveTexture(textureUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    m_stateDirty = true;
}

void GLStateManager::BindFramebuffer(GLenum target, GLuint fbo) {
    glBindFramebuffer(target, fbo);
    m_stateDirty = true;
}

void GLStateManager::UseProgram(GLuint program) {
    glUseProgram(program);
    m_stateDirty = true;
}

void GLStateManager::SetViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    glViewport(x, y, width, height);
    m_stateDirty = true;
}

void GLStateManager::SetScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
    m_stateDirty = true;
}

void GLStateManager::EnableScissor(bool enable) {
    if (enable) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    m_stateDirty = true;
}

void GLStateManager::SetDepthTest(bool enable) {
    if (enable) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    m_stateDirty = true;
}

void GLStateManager::SetDepthMask(GLboolean mask) {
    glDepthMask(mask);
    m_stateDirty = true;
}

void GLStateManager::SetDepthFunc(GLenum func) {
    glDepthFunc(func);
    m_stateDirty = true;
}

void GLStateManager::SetStencilTest(bool enable) {
    if (enable) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    m_stateDirty = true;
}

void GLStateManager::SetBlending(bool enable) {
    if (enable) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    m_stateDirty = true;
}

void GLStateManager::SetBlendFunc(GLenum sfactor, GLenum dfactor) {
    glBlendFunc(sfactor, dfactor);
    m_stateDirty = true;
}

void GLStateManager::SetBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
    m_stateDirty = true;
}

void GLStateManager::SetBlendEquation(GLenum mode) {
    glBlendEquation(mode);
    m_stateDirty = true;
}

void GLStateManager::SetBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    glBlendEquationSeparate(modeRGB, modeAlpha);
    m_stateDirty = true;
}

void GLStateManager::SetCullFace(bool enable) {
    if (enable) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    m_stateDirty = true;
}

void GLStateManager::SetCullFaceMode(GLenum mode) {
    glCullFace(mode);
    m_stateDirty = true;
}

void GLStateManager::SetFrontFace(GLenum mode) {
    glFrontFace(mode);
    m_stateDirty = true;
}

void GLStateManager::SetPolygonMode(GLenum face, GLenum mode) {
    glPolygonMode(face, mode);
    m_stateDirty = true;
}

void GLStateManager::ClearTextureBinding(GLenum unit) {
    GLenum textureUnit = GL_TEXTURE0 + (unit - GL_TEXTURE0);
    glActiveTexture(textureUnit);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glBindTexture(GL_TEXTURE_3D, 0);
    m_stateDirty = true;
}

void GLStateManager::ClearAllTextureBindings() {
    for (int i = 0; i < 32; ++i) {
        ClearTextureBinding(GL_TEXTURE0 + i);
    }
}

void GLStateManager::LogCurrentState() const {
    std::cout << "[GLStateManager] Current OpenGL State:" << std::endl;
    
    GLint activeTexture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    std::cout << "  Active Texture: GL_TEXTURE" << (activeTexture - GL_TEXTURE0) << std::endl;
    
    GLint currentProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    std::cout << "  Current Program: " << currentProgram << std::endl;
    
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    std::cout << "  Viewport: " << viewport[0] << ", " << viewport[1] << ", " << viewport[2] << ", " << viewport[3] << std::endl;
    
    std::cout << "  Depth Test: " << (glIsEnabled(GL_DEPTH_TEST) ? "Enabled" : "Disabled") << std::endl;
    std::cout << "  Blend: " << (glIsEnabled(GL_BLEND) ? "Enabled" : "Disabled") << std::endl;
    std::cout << "  Cull Face: " << (glIsEnabled(GL_CULL_FACE) ? "Enabled" : "Disabled") << std::endl;
}

bool GLStateManager::ValidateState() const {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::cerr << "[GLStateManager] OpenGL Error: 0x" << std::hex << error << std::dec << std::endl;
        return false;
    }
    return true;
}

// GLTextureStateGuard implementation
void GLTextureStateGuard::CaptureTextureState() {
    glGetIntegerv(GL_ACTIVE_TEXTURE, reinterpret_cast<GLint*>(&m_snapshot.activeTexture));
    
    // Capture bindings for commonly used texture units
    std::vector<GLenum> units = {
        GL_TEXTURE0, GL_TEXTURE1, GL_TEXTURE2, GL_TEXTURE3, GL_TEXTURE4,
        GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY,
        GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP,
        GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP,
        GL_TEXTURE0 + TextureUnits::BRDF_LUT,
        GL_TEXTURE0 + TextureUnits::SKYBOX_CUBEMAP
    };
    
    for (GLenum unit : units) {
        glActiveTexture(unit);
        
        GLuint tex2D, texCube, tex2DArray;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, reinterpret_cast<GLint*>(&tex2D));
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, reinterpret_cast<GLint*>(&texCube));
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, reinterpret_cast<GLint*>(&tex2DArray));
        
        if (tex2D != 0) {
            m_snapshot.bindings.emplace_back(unit | (GL_TEXTURE_2D << 16), tex2D);
        }
        if (texCube != 0) {
            m_snapshot.bindings.emplace_back(unit | (GL_TEXTURE_CUBE_MAP << 16), texCube);
        }
        if (tex2DArray != 0) {
            m_snapshot.bindings.emplace_back(unit | (GL_TEXTURE_2D_ARRAY << 16), tex2DArray);
        }
    }
    
    // Restore active texture
    glActiveTexture(m_snapshot.activeTexture);
}

void GLTextureStateGuard::RestoreTextureState() {
    for (const auto& binding : m_snapshot.bindings) {
        GLenum unit = binding.first & 0xFFFF;
        GLenum target = (binding.first >> 16) & 0xFFFF;
        GLuint texture = binding.second;
        
        glActiveTexture(unit);
        glBindTexture(target, texture);
    }
    
    glActiveTexture(m_snapshot.activeTexture);
}