#pragma once
#include <GL/glew.h>
#include "GLBuffer.h"
#include <memory>

class ScreenQuad
{
public:
    ScreenQuad()
        : m_vao(0)
    {
        // Create and configure the full-screen quad
        static const float quadVertices[] = {
            // positions   // texcoords
            -1.0f,  1.0f,   0.0f, 1.0f,
            -1.0f, -1.0f,   0.0f, 0.0f,
             1.0f,  1.0f,   1.0f, 1.0f,
             1.0f, -1.0f,   1.0f, 0.0f,
        };

        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);

        // Create vertex buffer using GLBuffer wrapper
        m_vbo = std::make_unique<GLBuffer>(
            BufferType::Vertex,
            sizeof(quadVertices),
            quadVertices,
            BufferUsage::StaticDraw
        );
        m_vbo->SetLabel("ScreenQuad_VBO");

        // Position attribute
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0,
            2,
            GL_FLOAT,
            GL_FALSE,
            4 * sizeof(float),
            (void*)0
        );

        // TexCoord attribute
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1,
            2,
            GL_FLOAT,
            GL_FALSE,
            4 * sizeof(float),
            (void*)(2 * sizeof(float))
        );

        glBindVertexArray(0);
    }

    // Delete copy constructor and assignment for RAII 
    ScreenQuad(const ScreenQuad&) = delete;
    ScreenQuad& operator=(const ScreenQuad&) = delete;

    // Permit move semantics if desired
    ScreenQuad(ScreenQuad&& other) noexcept
        : m_vao(other.m_vao)
        , m_vbo(std::move(other.m_vbo))
    {
        other.m_vao = 0;
    }

    ScreenQuad& operator=(ScreenQuad&& other) noexcept
    {
        if (this != &other)
        {
            // Release our current resources
            cleanup();

            // Take ownership of other's
            m_vao = other.m_vao;
            m_vbo = std::move(other.m_vbo);

            // Nullify the source
            other.m_vao = 0;
        }
        return *this;
    }

    ~ScreenQuad()
    {
        cleanup();
    }

    void Render() const
    {
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

private:
    void cleanup()
    {
        m_vbo.reset();  // GLBuffer handles its own cleanup
        if (m_vao)
            glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }

    GLuint m_vao;
    GLBufferPtr m_vbo;
};
