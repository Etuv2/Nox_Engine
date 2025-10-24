#pragma once
#include <GL/glew.h>
#include <array>
#include <stack>
#include <unordered_map>
#include <vector>

// Comprehensive OpenGL state management system
class GLStateManager {
public:
    static GLStateManager& Instance() {
        static GLStateManager instance;
        return instance;
    }

    // Capture current OpenGL state
    void CaptureState();
    
    // Restore previously captured state
    void RestoreState();
    
    // Push current state to stack (for nested operations)
    void PushState();
    
    // Pop and restore state from stack
    void PopState();
    
    // Smart texture binding with automatic state tracking
    void BindTexture2D(GLenum unit, GLuint texture);
    void BindTextureCube(GLenum unit, GLuint texture);
    void BindTexture2DArray(GLenum unit, GLuint texture);
    
    // Framebuffer management
    void BindFramebuffer(GLenum target, GLuint fbo);
    
    // Shader program management
    void UseProgram(GLuint program);
    
    // Viewport and scissor management
    void SetViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void SetScissor(GLint x, GLint y, GLsizei width, GLsizei height);
    void EnableScissor(bool enable);
    
    // Depth and stencil state
    void SetDepthTest(bool enable);
    void SetDepthMask(GLboolean mask);
    void SetDepthFunc(GLenum func);
    void SetStencilTest(bool enable);
    
    // Blending state
    void SetBlending(bool enable);
    void SetBlendFunc(GLenum sfactor, GLenum dfactor);
    void SetBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void SetBlendEquation(GLenum mode);
    void SetBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    
    // Face culling
    void SetCullFace(bool enable);
    void SetCullFaceMode(GLenum mode);
    void SetFrontFace(GLenum mode);
    
    // Polygon mode
    void SetPolygonMode(GLenum face, GLenum mode);
    
    // Clear current texture bindings for specific units (for cleanup)
    void ClearTextureBinding(GLenum unit);
    void ClearAllTextureBindings();
    
    // Debug utilities
    void LogCurrentState() const;
    bool ValidateState() const;

private:
    GLStateManager() = default;
    ~GLStateManager() = default;
    GLStateManager(const GLStateManager&) = delete;
    GLStateManager& operator=(const GLStateManager&) = delete;

    struct GLState {
        // Texture state tracking
        struct TextureState {
            GLuint texture2D = 0;
            GLuint textureCube = 0;
            GLuint texture2DArray = 0;
            GLuint texture3D = 0;
        };
        std::array<TextureState, 32> textureUnits; // Support up to 32 texture units
        GLenum activeTexture = GL_TEXTURE0;
        
        // Framebuffer state
        GLuint framebufferDraw = 0;
        GLuint framebufferRead = 0;
        
        // Shader state
        GLuint currentProgram = 0;
        
        // Viewport and scissor
        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissorBox[4] = {0, 0, 0, 0};
        GLboolean scissorEnabled = GL_FALSE;
        
        // Depth and stencil state
        GLboolean depthTestEnabled = GL_FALSE;
        GLboolean depthMask = GL_TRUE;
        GLenum depthFunc = GL_LESS;
        GLboolean stencilTestEnabled = GL_FALSE;
        
        // Blending state
        GLboolean blendEnabled = GL_FALSE;
        GLenum blendSrcRGB = GL_ONE;
        GLenum blendDstRGB = GL_ZERO;
        GLenum blendSrcAlpha = GL_ONE;
        GLenum blendDstAlpha = GL_ZERO;
        GLenum blendEquationRGB = GL_FUNC_ADD;
        GLenum blendEquationAlpha = GL_FUNC_ADD;
        
        // Face culling
        GLboolean cullFaceEnabled = GL_FALSE;
        GLenum cullFaceMode = GL_BACK;
        GLenum frontFace = GL_CCW;
        
        // Polygon mode
        GLenum polygonModeFront = GL_FILL;
        GLenum polygonModeBack = GL_FILL;

		//Wireframe mode (not part of core OpenGL state, but useful for debugging)
		bool wireframeMode = false;
        
        // Vertex array state
        GLuint vertexArray = 0;
        GLuint arrayBuffer = 0;
        GLuint elementArrayBuffer = 0;
        
        void Capture();
        void Restore() const;
    };
    
    GLState m_currentState;
    std::stack<GLState> m_stateStack;
    
    // Performance optimization: track what actually needs to be restored
    mutable bool m_stateDirty = false;
};

// RAII guard for automatic state management
class GLStateGuard {
public:
    GLStateGuard() {
        GLStateManager::Instance().PushState();
    }
    
    explicit GLStateGuard(bool captureNow) {
        if (captureNow) {
            GLStateManager::Instance().CaptureState();
        }
        GLStateManager::Instance().PushState();
    }

    ~GLStateGuard() {
        GLStateManager::Instance().PopState();
    }

    void Restore() {
        if (!m_restored) {
            GLStateManager::Instance().RestoreState();
            m_restored = true;
        }
    }

private:
    bool m_restored = false;
    
    // Non-copyable
    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;
};

// Specialized guards for specific operations
class GLTextureStateGuard {
public:
    GLTextureStateGuard() {
        // Capture only texture-related state
        CaptureTextureState();
    }
    
    ~GLTextureStateGuard() {
        RestoreTextureState();
    }

private:
    struct TextureSnapshot {
        GLenum activeTexture;
        std::vector<std::pair<GLenum, GLuint>> bindings;
    } m_snapshot;
    
    void CaptureTextureState();
    void RestoreTextureState();
    
    GLTextureStateGuard(const GLTextureStateGuard&) = delete;
    GLTextureStateGuard& operator=(const GLTextureStateGuard&) = delete;
};

class GLFramebufferStateGuard {
public:
    GLFramebufferStateGuard() {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_previousFBO);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_previousReadFBO);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_previousDrawFBO);
        glGetIntegerv(GL_VIEWPORT, m_previousViewport);
    }
    
    ~GLFramebufferStateGuard() {
        glBindFramebuffer(GL_FRAMEBUFFER, m_previousFBO);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_previousReadFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_previousDrawFBO);
        glViewport(m_previousViewport[0], m_previousViewport[1], 
                  m_previousViewport[2], m_previousViewport[3]);
    }

private:
    GLint m_previousFBO = 0;
    GLint m_previousReadFBO = 0; 
    GLint m_previousDrawFBO = 0;
    GLint m_previousViewport[4] = {0, 0, 0, 0};
    
    GLFramebufferStateGuard(const GLFramebufferStateGuard&) = delete;
    GLFramebufferStateGuard& operator=(const GLFramebufferStateGuard&) = delete;
};

class GLRenderStateGuard {
public:
    GLRenderStateGuard() {
        // Capture common rendering state
        m_depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
        m_cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
        m_blendEnabled = glIsEnabled(GL_BLEND);
        m_scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        
        glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
        glGetIntegerv(GL_CULL_FACE_MODE, &m_cullMode);
        glGetIntegerv(GL_FRONT_FACE, &m_frontFace);
        
        glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &m_blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEqAlpha);
    }
    
    ~GLRenderStateGuard() {
        // Restore state
        if (m_depthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (m_cullFaceEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (m_blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (m_scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        
        glDepthFunc(m_depthFunc);
        glDepthMask(m_depthMask);
        glCullFace(m_cullMode);
        glFrontFace(m_frontFace);
        
        glBlendFuncSeparate((GLenum)m_blendSrcRGB, (GLenum)m_blendDstRGB, 
                           (GLenum)m_blendSrcAlpha, (GLenum)m_blendDstAlpha);
        glBlendEquationSeparate((GLenum)m_blendEqRGB, (GLenum)m_blendEqAlpha);
    }

private:
    GLboolean m_depthTestEnabled = GL_FALSE;
    GLboolean m_cullFaceEnabled = GL_FALSE;
    GLboolean m_blendEnabled = GL_FALSE;
    GLboolean m_scissorEnabled = GL_FALSE;
    
    GLint m_depthFunc = GL_LESS;
    GLboolean m_depthMask = GL_TRUE;
    GLint m_cullMode = GL_BACK;
    GLint m_frontFace = GL_CCW;
    
    GLint m_blendSrcRGB = GL_ONE, m_blendDstRGB = GL_ZERO;
    GLint m_blendSrcAlpha = GL_ONE, m_blendDstAlpha = GL_ZERO;
    GLint m_blendEqRGB = GL_FUNC_ADD, m_blendEqAlpha = GL_FUNC_ADD;
    
    GLRenderStateGuard(const GLRenderStateGuard&) = delete;
    GLRenderStateGuard& operator=(const GLRenderStateGuard&) = delete;
};

// Convenience macros for state management
#define SCOPED_GL_STATE() GLStateGuard _glStateGuard
#define SCOPED_GL_TEXTURE_STATE() GLTextureStateGuard _glTexStateGuard
#define SCOPED_GL_FRAMEBUFFER_STATE() GLFramebufferStateGuard _glFBStateGuard
#define SCOPED_GL_RENDER_STATE() GLRenderStateGuard _glRenderStateGuard

// Legacy compatibility - keeping the original simple guard
class GLStateGuardLegacy {
public:
    GLStateGuardLegacy()
    {
        depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
        cullFaceEnabled  = glIsEnabled(GL_CULL_FACE);
        blendEnabled     = glIsEnabled(GL_BLEND);

        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
    }

    ~GLStateGuardLegacy()
    {
        restore();
    }

    void restore()
    {
        if (restored) return;
        // Depth test
        if (depthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthFunc(depthFunc);
        glDepthMask(depthMask);
        // Culling
        if (cullFaceEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        // Blending
        if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glBlendFuncSeparate((GLenum)blendSrcRGB, (GLenum)blendDstRGB, (GLenum)blendSrcAlpha, (GLenum)blendDstAlpha);
        glBlendEquationSeparate((GLenum)blendEqRGB, (GLenum)blendEqAlpha);
        restored = true;
    }

private:
    // toggles
    GLboolean depthTestEnabled = GL_FALSE;
    GLboolean cullFaceEnabled  = GL_FALSE;
    GLboolean blendEnabled     = GL_FALSE;
    // depth
    GLint depthFunc = GL_LESS;
    GLboolean depthMask = GL_TRUE;
    // blend
    GLint blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    GLint blendEqRGB = GL_FUNC_ADD, blendEqAlpha = GL_FUNC_ADD;

    bool restored = false;
};
