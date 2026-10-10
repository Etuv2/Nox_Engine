#pragma once

#include "rhi/RHI.h"
#include <vector>
#include <memory>

// FrameBuffer: an rhi framebuffer with owned colour textures and a depth (or depth-stencil) target.
// Colour targets are 2D textures; depth is a 2D texture, a 2D array (layered rendering, e.g. shadow
// cascades) or a depth-only target. Resize() keeps every handle valid.
class FrameBuffer {
public:
    FrameBuffer(int width,
        int height,
        const std::vector<rhi::Format>& colorFormats,
        bool useDepthAsTexture = false,
        bool useDepthAsTextureArray = false,
        int  arrayLayers = 1,
        bool useStencil = false,
        rhi::Format internalDepthFormat = rhi::Format::D24);

    ~FrameBuffer();

    // Move-only
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;
    FrameBuffer(FrameBuffer&&) noexcept;
    FrameBuffer& operator=(FrameBuffer&&) noexcept;

    void Bind() const;
    static void Unbind();          // binds PresentTarget::Get()
    bool IsComplete() const;
    void Resize(int newWidth, int newHeight);

    rhi::TextureHandle GetColorAttachment(unsigned int index = 0) const;
    rhi::TextureHandle GetDepthTexture() const { return depthTexture; }
    rhi::TextureHandle GetDepthArray()   const { return depthArray; }
    rhi::FramebufferHandle GetFBO()      const { return fbo; }
    int   GetWidth()         const { return width; }
    int   GetHeight()        const { return height; }
    int   GetArrayLayers()   const { return arrayLayers; }

protected:
    void Init();

private:
    void Release();

    rhi::FramebufferHandle fbo;
    std::vector<rhi::TextureHandle> colorTextures;
    std::vector<rhi::Format> internalColorFormats;

    rhi::TextureHandle depthTexture;       // single 2D (sampled)
    rhi::TextureHandle depthTarget;        // depth-only target (not sampled by callers)
    rhi::TextureHandle depthArray;         // 2D array

    int   width = 0;
    int   height = 0;
    bool  useDepthAsTexture = false;
    bool  useDepthAsTextureArray = false;
    int   arrayLayers = 1;
    bool  useStencil = false;
    rhi::Format internalDepthFormat = rhi::Format::D24;
};
