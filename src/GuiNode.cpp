#include "GuiNode.h"
#include "GpuBuffer.h"
#include "ShaderLoader.h"
#include "assets/AssetIOSdl.h"
#include <SDL/SDL_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <algorithm>

GuiNode::GuiNode(int screenW, int screenH)
    : m_font(nullptr), m_quadVAO(), m_screenW(screenW), m_screenH(screenH)
{
    // Create quad vertex data
    float quad[] = {
        0, 1, 0, 0,
        1, 1, 1, 0,
        0, 0, 0, 1,
        1, 0, 1, 1
    };
    
    // Create VBO using GpuBuffer
    m_quadVBO = MakeBuffer(BufferType::Vertex, BufferUsage::StaticDraw);
    m_quadVBO->SetData(quad, 16); // 16 floats
    m_quadVBO->SetLabel("GuiNode_QuadVBO");
    
    // Set up vertex attributes (position xy, texcoord uv) on the quad VBO
    rhi::VertexBufferBinding binding;
    binding.buffer = m_quadVBO->GetID();
    binding.stride = static_cast<uint32_t>(4 * sizeof(float));
    rhi::VertexInputDesc vaoDesc;
    vaoDesc.buffers.push_back(binding);
    rhi::VertexAttribute positionAttrib;
    positionAttrib.location = 0;
    positionAttrib.bufferIndex = 0;
    positionAttrib.components = 2;
    positionAttrib.type = rhi::ScalarType::Float;
    positionAttrib.offset = 0;
    vaoDesc.attributes.push_back(positionAttrib);
    rhi::VertexAttribute texcoordAttrib;
    texcoordAttrib.location = 1;
    texcoordAttrib.bufferIndex = 0;
    texcoordAttrib.components = 2;
    texcoordAttrib.type = rhi::ScalarType::Float;
    texcoordAttrib.offset = static_cast<uint32_t>(2 * sizeof(float));
    vaoDesc.attributes.push_back(texcoordAttrib);
    m_quadVAO = rhi::Gpu().CreateVertexInput(vaoDesc);

    InitializeShaders();
}

GuiNode::~GuiNode() {
    // GpuBuffer cleans itself up via RAII (m_quadVBO is unique_ptr)
    if (m_quadVAO) rhi::Gpu().DestroyVertexInput(m_quadVAO);
    if (m_textureShader) rhi::Gpu().DestroyProgram(m_textureShader);
    if (m_unifiedRectShader) rhi::Gpu().DestroyProgram(m_unifiedRectShader);
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
    
    // Cache all uniform locations once at initialization
    CacheUniformLocations();
}

void GuiNode::CacheUniformLocations() {
    // Cache texture shader uniforms
    if (m_textureShader) {
        m_texUniforms.mvp = rhi::Gpu().GetUniformLocation(m_textureShader, "mvp");
        m_texUniforms.tintColor = rhi::Gpu().GetUniformLocation(m_textureShader, "tintColor");
        m_texUniforms.tex = rhi::Gpu().GetUniformLocation(m_textureShader, "tex");
    }
    
    // Cache unified rect shader uniforms
    if (m_unifiedRectShader) {
        m_rectUniforms.mvp = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "mvp");
        m_rectUniforms.rectSize = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "rectSize");
        m_rectUniforms.renderMode = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "renderMode");
        m_rectUniforms.baseColor = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "baseColor");
        m_rectUniforms.gradientType = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "gradientType");
        m_rectUniforms.startColor = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "startColor");
        m_rectUniforms.endColor = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "endColor");
        m_rectUniforms.gradientCenter = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "gradientCenter");
        m_rectUniforms.gradientRadius = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "gradientRadius");
        m_rectUniforms.highlightColor = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "highlightColor");
        m_rectUniforms.shadowColor = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "shadowColor");
        m_rectUniforms.cornerRadius = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "cornerRadius");
        m_rectUniforms.bevelSize = rhi::Gpu().GetUniformLocation(m_unifiedRectShader, "bevelSize");
    }
}

void GuiNode::LoadFont(const std::string& path, int size) {
    SDL_RWops* fontRw = AssetIO::OpenRWops(path); // TTF closes the RWops (and releases the bytes) with the font
    m_font = fontRw ? TTF_OpenFontRW(fontRw, 1, size) : nullptr;
    if (!m_font) {
        SDL_Log("[GuiNode] Failed to load font: %s", TTF_GetError());
    }
}

void GuiNode::AddText(const std::string& text, float x, float y, SDL_Color color) {
    GuiElement element;
    element.type = GuiType::TEXT;
    element.content = text;
    element.x = x;
    element.y = y;
    element.color = color;
    element.animator = std::make_shared<GuiElementAnimator>();
    m_elements.push_back(element);
}

void GuiNode::AddImage(const std::string& path, float x, float y, float w, float h) {
    SDL_RWops* imageRw = AssetIO::OpenRWops(path);
    SDL_Surface* image = imageRw ? IMG_Load_RW(imageRw, 1) : nullptr;
    if (!image) {
        SDL_Log("[GuiNode] Failed to load image: %s", IMG_GetError());
        return;
    }

    rhi::Device& gpu = rhi::Gpu();
    rhi::TextureDesc texDesc;
    texDesc.format = (image->format->BytesPerPixel == 4) ? rhi::Format::RGBA8 : rhi::Format::RGB8;
    texDesc.width = image->w;
    texDesc.height = image->h;
    rhi::TextureHandle texID = gpu.CreateTexture(texDesc);
    const rhi::PixelFormat format = (image->format->BytesPerPixel == 4) ? rhi::PixelFormat::RGBA : rhi::PixelFormat::RGB;
    gpu.UpdateTexture(texID, rhi::TextureRegion{}, format, rhi::PixelType::UByte, image->pixels);
    gpu.SetTextureFilter(texID, rhi::Filter::Linear, rhi::Filter::Linear);
    gpu.SetTextureWrapAxis(texID, 0, rhi::Wrap::ClampToEdge);
    gpu.SetTextureWrapAxis(texID, 1, rhi::Wrap::ClampToEdge);
    SDL_FreeSurface(image);

    GuiElement element;
    element.type = GuiType::IMAGE;
    element.content = path;
    element.x = x;
    element.y = y;
    element.width = w;
    element.height = h;
    element.color = {255, 255, 255, 255};
    element.textureID = texID;
    element.animator = std::make_shared<GuiElementAnimator>();
    m_elements.push_back(element);
}

int GuiNode::AddSolidRect(float x, float y, float width, float height, SDL_Color color) {
    GuiElement element;
    element.type = GuiType::RECT_SOLID;
    element.x = x;
    element.y = y;
    element.width = width;
    element.height = height;
    element.color = color;
    element.animator = std::make_shared<GuiElementAnimator>();
    
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
    element.animator = std::make_shared<GuiElementAnimator>();
    
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
    element.animator = std::make_shared<GuiElementAnimator>();
    
    m_elements.push_back(element);
    return static_cast<int>(m_elements.size() - 1);
}

int GuiNode::AddProgressBar(float x, float y, float width, float height, const ProgressBarStyle& style) {
    GuiElement element;
    element.type = GuiType::PROGRESS_BAR;
    element.x = x;
    element.y = y;
    element.width = width;
    element.height = height;
    element.progressStyle = style;
    element.progressValue = 0.0f;
    element.animator = std::make_shared<GuiElementAnimator>();
    
    m_elements.push_back(element);
    return static_cast<int>(m_elements.size() - 1);
}

void GuiNode::SetProgressValue(int elementIndex, float value) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        m_elements[elementIndex].progressValue = std::clamp(value, 0.0f, 1.0f);
    }
}

void GuiNode::SetProgressLabel(int elementIndex, const std::string& label) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        m_elements[elementIndex].progressLabel = label;
    }
}

float GuiNode::GetProgressValue(int elementIndex) const {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        return m_elements[elementIndex].progressValue;
    }
    return 0.0f;
}

void GuiNode::AddAnimationToElement(int elementIndex, const GuiAnimation::PropertyAnimation& anim) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        if (!m_elements[elementIndex].animator) {
            m_elements[elementIndex].animator = std::make_shared<GuiElementAnimator>();
        }
        m_elements[elementIndex].animator->AddPropertyAnimation(anim);
    }
}

void GuiNode::AddKeyframeAnimationToElement(int elementIndex, const GuiAnimation::KeyframeAnimation& anim) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        if (!m_elements[elementIndex].animator) {
            m_elements[elementIndex].animator = std::make_shared<GuiElementAnimator>();
        }
        m_elements[elementIndex].animator->AddKeyframeAnimation(anim);
    }
}

void GuiNode::RemoveAnimationFromElement(int elementIndex, GuiAnimation::PropertyType property) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        if (m_elements[elementIndex].animator) {
            m_elements[elementIndex].animator->RemoveAnimation(property);
        }
    }
}

void GuiNode::ClearAnimationsFromElement(int elementIndex) {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        if (m_elements[elementIndex].animator) {
            m_elements[elementIndex].animator->ClearAnimations();
        }
    }
}

bool GuiNode::ElementHasAnimations(int elementIndex) const {
    if (elementIndex >= 0 && elementIndex < static_cast<int>(m_elements.size())) {
        if (m_elements[elementIndex].animator) {
            return m_elements[elementIndex].animator->HasActiveAnimations();
        }
    }
    return false;
}

void GuiNode::Update(float deltaTime) {
    // Update all element animations
    for (auto& element : m_elements) {
        if (element.animator) {
            element.animator->Update(deltaTime);
            
            // Apply animated properties to element
            element.displayAlpha = element.animator->GetAnimatedValue(GuiAnimation::PropertyType::ALPHA, element.displayAlpha);
            element.displayScale.x = element.animator->GetAnimatedValue(GuiAnimation::PropertyType::SCALE_X, element.displayScale.x);
            element.displayScale.y = element.animator->GetAnimatedValue(GuiAnimation::PropertyType::SCALE_Y, element.displayScale.y);
            element.displayRotation = element.animator->GetAnimatedValue(GuiAnimation::PropertyType::ROTATION, element.displayRotation);
            
            // Handle position animation
            if (element.animator->HasAnimation(GuiAnimation::PropertyType::POSITION_X)) {
                element.x = element.animator->GetAnimatedValue(GuiAnimation::PropertyType::POSITION_X, element.x);
            }
            if (element.animator->HasAnimation(GuiAnimation::PropertyType::POSITION_Y)) {
                element.y = element.animator->GetAnimatedValue(GuiAnimation::PropertyType::POSITION_Y, element.y);
            }
        }
    }
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
        if (m_elements[elementIndex].type == GuiType::IMAGE && m_elements[elementIndex].textureID.IsValid()) {
            rhi::Gpu().DestroyTexture(m_elements[elementIndex].textureID);
        }
        m_elements.erase(m_elements.begin() + elementIndex);
    }
}

void GuiNode::ClearAllElements() {
    // Clean up textures
    for (const auto& element : m_elements) {
        if (element.type == GuiType::IMAGE && element.textureID.IsValid()) {
            rhi::TextureHandle texture = element.textureID;
            rhi::Gpu().DestroyTexture(texture);
        }
    }
    m_elements.clear();
}

void GuiNode::RenderHUD() const {
    rhi::Gpu().SetDepthTest(false);
    rhi::Gpu().SetBlendEnabled(true);
    rhi::Gpu().SetBlendFunc(rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::OneMinusSrcAlpha);

    // Group elements by type to minimize shader switches
    // First render all rectangles and progress bars with unified shader
    rhi::Gpu().UseProgram(m_unifiedRectShader);
    for (const auto& e : m_elements) {
        if (e.type == GuiType::RECT_SOLID || e.type == GuiType::RECT_GRADIENT || e.type == GuiType::RECT_BEVEL) {
            // Apply animation effects
            glm::vec4 finalColor = glm::vec4(
                e.color.r / 255.0f,
                e.color.g / 255.0f,
                e.color.b / 255.0f,
                (e.color.a / 255.0f) * e.displayAlpha
            );
            RenderRect(e);
        } else if (e.type == GuiType::PROGRESS_BAR) {
            RenderProgressBar(e);
        }
    }

    // Then render text and images with texture shader
    rhi::Gpu().UseProgram(m_textureShader);
    for (const auto& e : m_elements) {
        if (e.type == GuiType::TEXT) {
            RenderText(e);
        } else if (e.type == GuiType::IMAGE) {
            RenderImage(e);
        }
    }

    rhi::Gpu().SetBlendEnabled(false);
    rhi::Gpu().SetDepthTest(true);
}

void GuiNode::RenderText(const GuiElement& e) const {
    if (!m_font) return;
    
    SDL_Surface* surf = TTF_RenderText_Blended(m_font, e.content.c_str(), e.color);
    if (!surf) return;

    // Convert to a GL-safe format
    SDL_Surface* converted = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(surf);
    if (!converted) return;

    rhi::Device& gpu = rhi::Gpu();
    rhi::TextureDesc texDesc;
    texDesc.width = converted->w;
    texDesc.height = converted->h;
    rhi::TextureHandle tex = gpu.CreateTexture(texDesc);
    gpu.UpdateTexture(tex, rhi::TextureRegion{}, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, converted->pixels);
    gpu.SetTextureFilter(tex, rhi::Filter::Linear, rhi::Filter::Linear);
    gpu.SetTextureWrapAxis(tex, 0, rhi::Wrap::ClampToEdge);
    gpu.SetTextureWrapAxis(tex, 1, rhi::Wrap::ClampToEdge);

    glm::vec4 tint = glm::vec4(
        e.color.r / 255.0f,
        e.color.g / 255.0f,
        e.color.b / 255.0f,
        e.color.a / 255.0f
    );
    drawQuad(tex, e.x, e.y, static_cast<float>(converted->w), static_cast<float>(converted->h), tint);

    rhi::Gpu().DestroyTexture(tex);
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

void GuiNode::RenderProgressBar(const GuiElement& e) const {
    if (e.width <= 0 || e.height <= 0) return;
    
    // Draw background
    drawRect(e.x, e.y, e.width, e.height, e);
    
    // Draw progress fill
    if (e.progressValue > 0.0f) {
        GuiElement fillElement = e;
        fillElement.type = GuiType::RECT_SOLID;
        fillElement.width = e.width * e.progressValue;
        fillElement.color = e.progressStyle.fillColor;
        drawRect(e.x, e.y, fillElement.width, e.height, fillElement);
    }
    
    // Draw label if enabled
    if (e.progressStyle.showLabel && !e.progressLabel.empty() && m_font) {
        // Center text on progress bar
        SDL_Surface* surf = TTF_RenderText_Blended(m_font, e.progressLabel.c_str(), {255, 255, 255, 255});
        if (surf) {
            SDL_Surface* converted = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
            SDL_FreeSurface(surf);
            if (converted) {
                rhi::Device& gpu = rhi::Gpu();
                rhi::TextureDesc texDesc;
                texDesc.width = converted->w;
                texDesc.height = converted->h;
                rhi::TextureHandle tex = gpu.CreateTexture(texDesc);
                gpu.UpdateTexture(tex, rhi::TextureRegion{}, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, converted->pixels);
                gpu.SetTextureFilter(tex, rhi::Filter::Linear, rhi::Filter::Linear);
                
                float labelX = e.x + (e.width - converted->w) * 0.5f;
                float labelY = e.y + (e.height - converted->h) * 0.5f;
                
                glm::vec4 tint(1.0f, 1.0f, 1.0f, 1.0f);
                drawQuad(tex, labelX, labelY, static_cast<float>(converted->w), static_cast<float>(converted->h), tint);
                
                rhi::Gpu().DestroyTexture(tex);
                SDL_FreeSurface(converted);
            }
        }
    }
}

void GuiNode::drawQuad(rhi::TextureHandle tex, float x, float y, float w, float h, const glm::vec4& tint) const {
    glm::mat4 proj = glm::ortho(0.0f, float(m_screenW), 0.0f, float(m_screenH));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
    model = glm::scale(model, glm::vec3(w, h, 1.0f));
    glm::mat4 mvp = proj * model;

    rhi::Gpu().SetUniform(m_texUniforms.mvp, mvp);
    rhi::Gpu().SetUniform(m_texUniforms.tintColor, tint);

    rhi::Gpu().BindTexture(0, tex);
    rhi::Gpu().SetUniform(m_texUniforms.tex, 0);

    rhi::Gpu().BindVertexInput(m_quadVAO);
    rhi::Gpu().Draw(rhi::Topology::TriangleStrip, 0, 4);
    rhi::Gpu().BindVertexInput(rhi::VertexInputHandle{});
}

void GuiNode::drawRect(float x, float y, float w, float h, const GuiElement& element) const {
    glm::mat4 proj = glm::ortho(0.0f, float(m_screenW), 0.0f, float(m_screenH));
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
    model = glm::scale(model, glm::vec3(w, h, 1.0f));
    glm::mat4 mvp = proj * model;

    // Set transform matrix
    rhi::Gpu().SetUniform(m_rectUniforms.mvp, mvp);
    
    // Set rectangle size for SDF calculations
    rhi::Gpu().SetUniform(m_rectUniforms.rectSize, glm::vec2(w, h));
    
    // Set render mode based on element type
    int renderMode = 0; // Default to solid
    if (element.type == GuiType::RECT_SOLID) {
        renderMode = 0;
    } else if (element.type == GuiType::RECT_GRADIENT) {
        renderMode = 1;
    } else if (element.type == GuiType::RECT_BEVEL) {
        renderMode = 2;
    }
    rhi::Gpu().SetUniform(m_rectUniforms.renderMode, renderMode);
    
    // Set base color
    glm::vec4 baseColorVec = glm::vec4(
        element.color.r / 255.0f,
        element.color.g / 255.0f,
        element.color.b / 255.0f,
        element.color.a / 255.0f
    );
    rhi::Gpu().SetUniform(m_rectUniforms.baseColor, baseColorVec);
    
    // Set type-specific uniforms
    if (element.type == GuiType::RECT_GRADIENT) {
        // Gradient uniforms
        rhi::Gpu().SetUniform(m_rectUniforms.gradientType, static_cast<int>(element.gradient.type));
        
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
        
        rhi::Gpu().SetUniform(m_rectUniforms.startColor, startColor);
        rhi::Gpu().SetUniform(m_rectUniforms.endColor, endColor);
        rhi::Gpu().SetUniform(m_rectUniforms.gradientCenter, element.gradient.center);
        rhi::Gpu().SetUniform(m_rectUniforms.gradientRadius, element.gradient.radius);
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
        
        rhi::Gpu().SetUniform(m_rectUniforms.highlightColor, highlightColorVec);
        rhi::Gpu().SetUniform(m_rectUniforms.shadowColor, shadowColorVec);
        rhi::Gpu().SetUniform(m_rectUniforms.cornerRadius, element.bevel.radius);
        rhi::Gpu().SetUniform(m_rectUniforms.bevelSize, element.bevel.bevelSize);
    }

    rhi::Gpu().BindVertexInput(m_quadVAO);
    rhi::Gpu().Draw(rhi::Topology::TriangleStrip, 0, 4);
    rhi::Gpu().BindVertexInput(rhi::VertexInputHandle{});
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
