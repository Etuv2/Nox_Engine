#include "GuiNode.h"
#include "ShaderLoader.h"
#include <SDL/SDL_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <algorithm>

GuiNode::GuiNode(int screenW, int screenH)
    : m_font(nullptr), m_quadVAO(0), m_quadVBO(0), m_screenW(screenW), m_screenH(screenH)
{
    // Create quad vertex data
    float quad[] = {
        0, 1, 0, 0,
        1, 1, 1, 0,
        0, 0, 0, 1,
        1, 0, 1, 1
    };
    glGenVertexArrays(1, &m_quadVAO);
    glGenBuffers(1, &m_quadVBO);
    glBindVertexArray(m_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    InitializeShaders();
}

GuiNode::~GuiNode() {
    if (m_quadVBO) glDeleteBuffers(1, &m_quadVBO);
    if (m_quadVAO) glDeleteVertexArrays(1, &m_quadVAO);
    if (m_textureShader) glDeleteProgram(m_textureShader);
    if (m_unifiedRectShader) glDeleteProgram(m_unifiedRectShader);
}

void GuiNode::InitializeShaders() {
    m_textureShader = CreateShaderProgram("shaders/gui_vert.glsl", "shaders/gui_frag.glsl");
    if (!m_textureShader) {
        std::cerr << "[GuiNode] Failed to load texture shader." << std::endl;
    }

    m_unifiedRectShader = CreateShaderProgram("shaders/gui_unified_vert.glsl", "shaders/gui_unified_frag.glsl");
    if (!m_unifiedRectShader) {
        std::cerr << "[GuiNode] Failed to load unified rectangle shader." << std::endl;
    }
}

void GuiNode::LoadFont(const std::string& path, int size) {
    m_font = TTF_OpenFont(path.c_str(), size);
    if (!m_font) {
        SDL_Log("[GuiNode] Failed to load font: %s", TTF_GetError());
    }
}

void GuiNode::AddText(const std::string& text, float x, float y, SDL_Color color) {
    m_elements.push_back({ GuiType::TEXT, text, x, y, 0, 0, color });
}

void GuiNode::AddImage(const std::string& path, float x, float y, float w, float h) {
    SDL_Surface* image = IMG_Load(path.c_str());
    if (!image) {
        SDL_Log("[GuiNode] Failed to load image: %s", IMG_GetError());
        return;
    }

    GLuint texID = 0;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    GLenum format = (image->format->BytesPerPixel == 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, format, image->w, image->h, 0, format, GL_UNSIGNED_BYTE, image->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(image);

    m_elements.push_back({ GuiType::IMAGE, path, x, y, w, h, {255, 255, 255, 255}, texID });
}

int GuiNode::AddSolidRect(float x, float y, float width, float height, SDL_Color color) {
    GuiElement element;
    element.type = GuiType::RECT_SOLID;
    element.x = x;
    element.y = y;
    element.width = width;
    element.height = height;
    element.color = color;
    
    m_elements.push_back(element);
    return static_cast<int>(m_elements.size() - 1);
}

int GuiNode::AddGradientRect(float x, float y, float width, float height, const GradientStyle& gradient) {
    GuiElement element;
    element.type = GuiType::RECT_GRADIENT;
    element.x = x;
    element.y = y;
    element.width = width;
    element.height = height;
    element.gradient = gradient;
    
    m_elements.push_back(element);
    return static_cast<int>(m_elements.size() - 1);
}

int GuiNode::AddBevelRect(float x, float y, float width, float height, SDL_Color baseColor, const BevelStyle& bevel) {
    GuiElement element;
    element.type = GuiType::RECT_BEVEL;
    element.x = x;
    element.y = y;
    element.width = width;
    element.height = height;
    element.color = baseColor;
    element.bevel = bevel;
    
    m_elements.push_back(element);
    return static_cast<int>(m_elements.size() - 1);
}

void GuiNode::SetElementPosition(int elementIndex, float x, float y) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        m_elements[elementIndex].x = x;
        m_elements[elementIndex].y = y;
        m_elements[elementIndex].useRelativePositioning = false;
    }
}

void GuiNode::SetElementSize(int elementIndex, float width, float height) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        GuiElement& element = m_elements[elementIndex];
        
        // Apply size constraints if element is resizable
        if (element.isResizable) {
            width = std::max(width, element.minWidth);
            height = std::max(height, element.minHeight);
            
            if (element.maxWidth > 0) width = std::min(width, element.maxWidth);
            if (element.maxHeight > 0) height = std::min(height, element.maxHeight);
        }
        
        element.width = width;
        element.height = height;
        element.useRelativeSize = false;
    }
}

void GuiNode::SetElementRelativePosition(int elementIndex, float relX, float relY) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        m_elements[elementIndex].relativePosition = glm::vec2(relX, relY);
        m_elements[elementIndex].useRelativePositioning = true;
    }
}

void GuiNode::SetElementRelativeSize(int elementIndex, float relWidth, float relHeight) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        m_elements[elementIndex].relativeSize = glm::vec2(relWidth, relHeight);
        m_elements[elementIndex].useRelativeSize = true;
    }
}

void GuiNode::SetElementResizable(int elementIndex, bool resizable, float minW, float minH, float maxW, float maxH) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        GuiElement& element = m_elements[elementIndex];
        element.isResizable = resizable;
        element.minWidth = minW;
        element.minHeight = minH;
        element.maxWidth = maxW;
        element.maxHeight = maxH;
    }
}

void GuiNode::SetElementAnchor(int elementIndex, GuiElement::Anchor anchor) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        m_elements[elementIndex].anchor = anchor;
    }
}

void GuiNode::UpdateScreenSize(int newWidth, int newHeight) {
    m_screenW = newWidth;
    m_screenH = newHeight;
    RefreshElementPositions();
}

void GuiNode::RefreshElementPositions() {
    for (auto& element : m_elements) {
        if (element.useRelativePositioning) {
            glm::vec2 absPos = CalculateAbsolutePosition(element);
            element.x = absPos.x;
            element.y = absPos.y;
        }
        
        if (element.useRelativeSize) {
            glm::vec2 absSize = CalculateAbsoluteSize(element);
            element.width = absSize.x;
            element.height = absSize.y;
        }
    }
}

void GuiNode::RemoveElement(int elementIndex) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        // Clean up texture if it's an image
        if (m_elements[elementIndex].type == GuiType::IMAGE && m_elements[elementIndex].textureID != 0) {
            glDeleteTextures(1, &m_elements[elementIndex].textureID);
        }
        m_elements.erase(m_elements.begin() + elementIndex);
    }
}

void GuiNode::ClearAllElements() {
    // Clean up textures
    for (const auto& element : m_elements) {
        if (element.type == GuiType::IMAGE && element.textureID != 0) {
            glDeleteTextures(1, &element.textureID);
        }
    }
    m_elements.clear();
}

void GuiNode::RenderHUD() const {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Group elements by type to minimize shader switches
    // First render all rectangles with unified shader
    glUseProgram(m_unifiedRectShader);
    for (const auto& e : m_elements) {
        if (e.type == GuiType::RECT_SOLID || e.type == GuiType::RECT_GRADIENT || e.type == GuiType::RECT_BEVEL) {
            RenderRect(e);
        }
    }

    // Then render text and images with texture shader
    glUseProgram(m_textureShader);
    for (const auto& e : m_elements) {
        if (e.type == GuiType::TEXT) {
            RenderText(e);
        } else if (e.type == GuiType::IMAGE) {
            RenderImage(e);
        }
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void GuiNode::RenderText(const GuiElement& e) const {
    if (!m_font) return;
    
    SDL_Surface* surf = TTF_RenderText_Blended(m_font, e.content.c_str(), e.color);
    if (!surf) return;

    // Convert to a GL-safe format
    SDL_Surface* converted = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!converted) return;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, converted->w, converted->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, converted->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glm::vec4 tint = glm::vec4(
        e.color.r / 255.0f,
        e.color.g / 255.0f,
        e.color.b / 255.0f,
        e.color.a / 255.0f
    );
    drawQuad(tex, e.x, e.y, converted->w, converted->h, tint);

    glDeleteTextures(1, &tex);
    SDL_FreeSurface(converted);
}

void GuiNode::RenderImage(const GuiElement& e) const {
    glm::vec4 tint = glm::vec4(
        e.color.r / 255.0f,
        e.color.g / 255.0f,
        e.color.b / 255.0f,
        e.color.a / 255.0f
    );
    drawQuad(e.textureID, e.x, e.y, e.width, e.height, tint);
}

void GuiNode::RenderRect(const GuiElement& e) const {
    drawRect(e.x, e.y, e.width, e.height, e);
}

void GuiNode::drawQuad(GLuint tex, float x, float y, float w, float h, const glm::vec4& tint) const {
    glm::mat4 proj = glm::ortho(0.0f, float(m_screenW), 0.0f, float(m_screenH));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
    model = glm::scale(model, glm::vec3(w, h, 1.0f));
    glm::mat4 mvp = proj * model;

    glUniformMatrix4fv(glGetUniformLocation(m_textureShader, "mvp"), 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform4fv(glGetUniformLocation(m_textureShader, "tintColor"), 1, glm::value_ptr(tint));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(m_textureShader, "tex"), 0);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void GuiNode::drawRect(float x, float y, float w, float h, const GuiElement& element) const {
    glm::mat4 proj = glm::ortho(0.0f, float(m_screenW), 0.0f, float(m_screenH));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
    model = glm::scale(model, glm::vec3(w, h, 1.0f));
    glm::mat4 mvp = proj * model;

    // Set transform matrix
    glUniformMatrix4fv(glGetUniformLocation(m_unifiedRectShader, "mvp"), 1, GL_FALSE, glm::value_ptr(mvp));
    
    // Set rectangle size for SDF calculations
    glUniform2f(glGetUniformLocation(m_unifiedRectShader, "rectSize"), w, h);
    
    // Set render mode based on element type
    int renderMode = 0; // Default to solid
    if (element.type == GuiType::RECT_SOLID) {
        renderMode = 0;
    } else if (element.type == GuiType::RECT_GRADIENT) {
        renderMode = 1;
    } else if (element.type == GuiType::RECT_BEVEL) {
        renderMode = 2;
    }
    glUniform1i(glGetUniformLocation(m_unifiedRectShader, "renderMode"), renderMode);
    
    // Set base color
    glm::vec4 baseColorVec = glm::vec4(
        element.color.r / 255.0f,
        element.color.g / 255.0f,
        element.color.b / 255.0f,
        element.color.a / 255.0f
    );
    glUniform4fv(glGetUniformLocation(m_unifiedRectShader, "baseColor"), 1, glm::value_ptr(baseColorVec));
    
    // Set type-specific uniforms
    if (element.type == GuiType::RECT_GRADIENT) {
        // Gradient uniforms
        glUniform1i(glGetUniformLocation(m_unifiedRectShader, "gradientType"), static_cast<int>(element.gradient.type));
        
        glm::vec4 startColor = glm::vec4(
            element.gradient.startColor.r / 255.0f,
            element.gradient.startColor.g / 255.0f,
            element.gradient.startColor.b / 255.0f,
            element.gradient.startColor.a / 255.0f
        );
        glm::vec4 endColor = glm::vec4(
            element.gradient.endColor.r / 255.0f,
            element.gradient.endColor.g / 255.0f,
            element.gradient.endColor.b / 255.0f,
            element.gradient.endColor.a / 255.0f
        );
        
        glUniform4fv(glGetUniformLocation(m_unifiedRectShader, "startColor"), 1, glm::value_ptr(startColor));
        glUniform4fv(glGetUniformLocation(m_unifiedRectShader, "endColor"), 1, glm::value_ptr(endColor));
        glUniform2fv(glGetUniformLocation(m_unifiedRectShader, "gradientCenter"), 1, glm::value_ptr(element.gradient.center));
        glUniform1f(glGetUniformLocation(m_unifiedRectShader, "gradientRadius"), element.gradient.radius);
    } 
    else if (element.type == GuiType::RECT_BEVEL) {
        // Bevel uniforms
        glm::vec4 highlightColorVec = glm::vec4(
            element.bevel.highlightColor.r / 255.0f,
            element.bevel.highlightColor.g / 255.0f,
            element.bevel.highlightColor.b / 255.0f,
            element.bevel.highlightColor.a / 255.0f
        );
        glm::vec4 shadowColorVec = glm::vec4(
            element.bevel.shadowColor.r / 255.0f,
            element.bevel.shadowColor.g / 255.0f,
            element.bevel.shadowColor.b / 255.0f,
            element.bevel.shadowColor.a / 255.0f
        );
        
        glUniform4fv(glGetUniformLocation(m_unifiedRectShader, "highlightColor"), 1, glm::value_ptr(highlightColorVec));
        glUniform4fv(glGetUniformLocation(m_unifiedRectShader, "shadowColor"), 1, glm::value_ptr(shadowColorVec));
        glUniform1f(glGetUniformLocation(m_unifiedRectShader, "cornerRadius"), element.bevel.radius);
        glUniform1f(glGetUniformLocation(m_unifiedRectShader, "bevelSize"), element.bevel.bevelSize);
    }

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

glm::vec2 GuiNode::CalculateAbsolutePosition(const GuiElement& element) const {
    glm::vec2 pos = element.relativePosition;
    
    switch (element.anchor) {
        case GuiElement::Anchor::TOP_LEFT:
            return glm::vec2(pos.x * m_screenW, pos.y * m_screenH);
        case GuiElement::Anchor::TOP_RIGHT:
            return glm::vec2(m_screenW - pos.x * m_screenW - element.width, pos.y * m_screenH);
        case GuiElement::Anchor::BOTTOM_LEFT:
            return glm::vec2(pos.x * m_screenW, m_screenH - pos.y * m_screenH - element.height);
        case GuiElement::Anchor::BOTTOM_RIGHT:
            return glm::vec2(m_screenW - pos.x * m_screenW - element.width, m_screenH - pos.y * m_screenH - element.height);
        case GuiElement::Anchor::CENTER:
            return glm::vec2((m_screenW - element.width) * 0.5f + pos.x * m_screenW, (m_screenH - element.height) * 0.5f + pos.y * m_screenH);
        default:
            return glm::vec2(pos.x * m_screenW, pos.y * m_screenH);
    }
}

glm::vec2 GuiNode::CalculateAbsoluteSize(const GuiElement& element) const {
    glm::vec2 size = element.relativeSize;
    float absWidth = size.x * m_screenW;
    float absHeight = size.y * m_screenH;
    
    // Apply size constraints if element is resizable
    if (element.isResizable) {
        absWidth = std::max(absWidth, element.minWidth);
        absHeight = std::max(absHeight, element.minHeight);
        
        if (element.maxWidth > 0) absWidth = std::min(absWidth, element.maxWidth);
        if (element.maxHeight > 0) absHeight = std::min(absHeight, element.maxHeight);
    }
    
    return glm::vec2(absWidth, absHeight);
}

SDL_Color GuiNode::LerpColor(const SDL_Color& a, const SDL_Color& b, float t) const {
    t = std::clamp(t, 0.0f, 1.0f);
    return SDL_Color{
        static_cast<Uint8>(a.r + (b.r - a.r) * t),
        static_cast<Uint8>(a.g + (b.g - a.g) * t),
        static_cast<Uint8>(a.b + (b.b - a.b) * t),
        static_cast<Uint8>(a.a + (b.a - a.a) * t)
    };
}
