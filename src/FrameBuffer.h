#pragma once

#include <GL/glew.h>
#include <vector>
#include <memory>


//FrameBuffer is a flexible wrapper for an OpenGL framebuffer object (FBO),
class FrameBuffer {
public:
    FrameBuffer(int width,
        int height,
        const std::vector<GLenum>& colorFormats,
        bool useDepthAsTexture = false,
        bool useDepthAsTextureArray = false,
        int  arrayLayers = 1,
        bool useStencil = false,
        GLenum internalDepthFormat = GL_DEPTH_COMPONENT24);

    ~FrameBuffer();

    // Move-only
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;
    FrameBuffer(FrameBuffer&&) noexcept;
    FrameBuffer& operator=(FrameBuffer&&) noexcept;

    void Bind() const;
    static void Unbind();
    bool IsComplete() const;
    void Resize(int newWidth, int newHeight);

    GLuint GetColorAttachment(unsigned int index = 0) const;
    GLuint GetDepthTexture() const { return depthTextureID; }
    GLuint GetDepthArray()   const { return depthArrayID; }
    GLuint GetFBO()          const { return fboID; }
    int   GetWidth()         const { return width; }
    int   GetHeight()        const { return height; }
    int   GetArrayLayers()   const { return arrayLayers; }

protected:
    void Init();

private:
    GLuint              fboID = 0;
    std::vector<GLuint> colorTextures;
    std::vector<GLenum> internalColorFormats;

    GLuint depthTextureID = 0;     // single 2D
    GLuint depthRenderbuffer = 0;  // renderbuffer fallback
    GLuint depthArrayID = 0;       // 2D-array

    int   width;
    int   height;
    bool  useDepthAsTexture;
    bool  useDepthAsTextureArray;
    int   arrayLayers;
    bool  useStencil;
    GLenum internalDepthFormat;
};
