#pragma once
#include "rhi/RHI.h"
#include "GpuBuffer.h"
#include <memory>

// Full-screen quad (triangle strip): location 0 = position (NDC xy), location 1 = texcoord.
class ScreenQuad
{
public:
    ScreenQuad()
    {
        static const float quadVertices[] = {
            // positions   // texcoords
            -1.0f,  1.0f,   0.0f, 1.0f,
            -1.0f, -1.0f,   0.0f, 0.0f,
             1.0f,  1.0f,   1.0f, 1.0f,
             1.0f, -1.0f,   1.0f, 0.0f,
        };

        m_vbo = std::make_unique<GpuBuffer>(BufferType::Vertex, sizeof(quadVertices), quadVertices, BufferUsage::StaticDraw);
        m_vbo->SetLabel("ScreenQuad_VBO");

        rhi::VertexInputDesc desc;
        desc.buffers.push_back({ m_vbo->GetID(), 4 * sizeof(float) });
        desc.attributes.push_back({ 0, 0, 2, rhi::ScalarType::Float, false, false, 0 });
        desc.attributes.push_back({ 1, 0, 2, rhi::ScalarType::Float, false, false, 2 * sizeof(float) });
        m_input = rhi::Gpu().CreateVertexInput(desc);
    }

    ScreenQuad(const ScreenQuad&) = delete;
    ScreenQuad& operator=(const ScreenQuad&) = delete;

    ScreenQuad(ScreenQuad&& other) noexcept
        : m_input(other.m_input)
        , m_vbo(std::move(other.m_vbo))
    {
        other.m_input = {};
    }

    ScreenQuad& operator=(ScreenQuad&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            m_input = other.m_input;
            m_vbo = std::move(other.m_vbo);
            other.m_input = {};
        }
        return *this;
    }

    ~ScreenQuad()
    {
        cleanup();
    }

    // Draws the quad with depth testing disabled, then re-enables depth testing.
    void Render() const
    {
        rhi::Device& gpu = rhi::Gpu();
        gpu.SetDepthTest(false);
        gpu.BindVertexInput(m_input);
        gpu.Draw(rhi::Topology::TriangleStrip, 0, 4);
        gpu.BindVertexInput({});
        gpu.SetDepthTest(true);
    }

private:
    void cleanup()
    {
        if (m_input && rhi::HasGpu())
            rhi::Gpu().DestroyVertexInput(m_input);
        m_vbo.reset();
    }

    rhi::VertexInputHandle m_input;
    GpuBufferPtr m_vbo;
};
