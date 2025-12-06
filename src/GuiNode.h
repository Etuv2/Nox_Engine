#pragma once

#include "SceneNode.h"
#include "GuiAnimation.h"
#include <string>
#include <vector>
#include <memory>
#include <SDL/SDL_ttf.h>
#include <GL/glew.h>
#include <glm/glm.hpp>

class GuiNode : public SceneNode {
public:
    enum class GuiType { TEXT, IMAGE, RECT_SOLID, RECT_GRADIENT, RECT_BEVEL, PROGRESS_BAR };

    enum class GradientType { LINEAR_HORIZONTAL, LINEAR_VERTICAL, RADIAL };

    struct BevelStyle {
        float radius = 0.0f;          // Corner radius
        float bevelSize = 2.0f;       // Bevel effect size
        SDL_Color highlightColor = {255, 255, 255, 128}; // Light edge
        SDL_Color shadowColor = {0, 0, 0, 128};           // Dark edge
    };

    struct GradientStyle {
        GradientType type = GradientType::LINEAR_VERTICAL;
        SDL_Color startColor;
        SDL_Color endColor;
        glm::vec2 center = glm::vec2(0.5f, 0.5f); // For radial gradients
        float radius = 1.0f; // For radial gradients
    };

    struct ProgressBarStyle {
        SDL_Color backgroundColor;
        SDL_Color fillColor;
        SDL_Color borderColor;
        float borderWidth = 2.0f;
        float cornerRadius = 0.0f;
        bool showLabel = true;
        int labelFontSize = 16;
    };

    struct GuiElement {
        GuiType type;
        std::string content;
        float x, y, width, height;
        SDL_Color color;
        GLuint textureID = 0;
        
        // Rectangle-specific properties
        GradientStyle gradient;
        BevelStyle bevel;
        
        // Progress bar specific
        ProgressBarStyle progressStyle;
        float progressValue = 0.0f;  // 0.0 to 1.0
        std::string progressLabel;
        
        // Animation support
        std::shared_ptr<GuiElementAnimator> animator;
        
        // Display properties (affected by animation)
        float displayAlpha = 1.0f;     // Current alpha (can be animated)
        glm::vec2 displayScale = glm::vec2(1.0f, 1.0f);  // Current scale
        float displayRotation = 0.0f;   // Current rotation in degrees
        
        // Dynamic sizing properties
        bool isResizable = false;
        float minWidth = 10.0f;
        float minHeight = 10.0f;
        float maxWidth = -1.0f; // -1 means no limit
        float maxHeight = -1.0f; // -1 means no limit
        
        // Anchoring for responsive layout
        enum class Anchor { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT, CENTER };
        Anchor anchor = Anchor::TOP_LEFT;
        glm::vec2 relativePosition = glm::vec2(0.0f); // Position as ratio of screen size (0-1)
        glm::vec2 relativeSize = glm::vec2(0.0f); // Size as ratio of screen size (0-1)
        bool useRelativePositioning = false;
        bool useRelativeSize = false;
    };

    GuiNode(int screenWidth, int screenHeight);
    virtual ~GuiNode();

    // Font management
    void LoadFont(const std::string& path, int size);
    
    // Text elements
    void AddText(const std::string& text, float x, float y, SDL_Color color);
    
    // Image elements
    void AddImage(const std::string& imagePath, float x, float y, float width, float height);
    
    // Rectangle elements
    int AddSolidRect(float x, float y, float width, float height, SDL_Color color);
    int AddGradientRect(float x, float y, float width, float height, const GradientStyle& gradient);
    int AddBevelRect(float x, float y, float width, float height, SDL_Color baseColor, const BevelStyle& bevel);
    
    // Progress bar elements
    int AddProgressBar(float x, float y, float width, float height, const ProgressBarStyle& style);
    void SetProgressValue(int elementIndex, float value);
    void SetProgressLabel(int elementIndex, const std::string& label);
    float GetProgressValue(int elementIndex) const;
    
    // Dynamic resizing and positioning
    void SetElementPosition(int elementIndex, float x, float y);
    void SetElementSize(int elementIndex, float width, float height);
    void SetElementRelativePosition(int elementIndex, float relX, float relY);
    void SetElementRelativeSize(int elementIndex, float relWidth, float relHeight);
    void SetElementResizable(int elementIndex, bool resizable, float minW = 10.0f, float minH = 10.0f, float maxW = -1.0f, float maxH = -1.0f);
    void SetElementAnchor(int elementIndex, GuiElement::Anchor anchor);
    
    // Animation control
    void AddAnimationToElement(int elementIndex, const GuiAnimation::PropertyAnimation& anim);
    void AddKeyframeAnimationToElement(int elementIndex, const GuiAnimation::KeyframeAnimation& anim);
    void RemoveAnimationFromElement(int elementIndex, GuiAnimation::PropertyType property);
    void ClearAnimationsFromElement(int elementIndex);
    bool ElementHasAnimations(int elementIndex) const;
    
    // Screen size management for responsive design
    void UpdateScreenSize(int newWidth, int newHeight);
    void RefreshElementPositions(); // Recalculate positions after screen size change
    
    // Element management
    void RemoveElement(int elementIndex);
    void ClearAllElements();
    int GetElementCount() const { return static_cast<int>(m_elements.size()); }
    
    // Update loop (must be called each frame)
    void Update(float deltaTime);
    
    // Rendering
    void RenderHUD() const;

private:
    void RenderText(const GuiElement& e) const;
    void RenderImage(const GuiElement& e) const;
    void RenderRect(const GuiElement& e) const; // Unified rectangle rendering
    void RenderProgressBar(const GuiElement& e) const; // Progress bar rendering
    
    void drawQuad(GLuint tex, float x, float y, float w, float h, const glm::vec4& tint) const;
    void drawRect(float x, float y, float w, float h, const GuiElement& element) const; // Unified rect drawing
    
    // Helper methods
    glm::vec2 CalculateAbsolutePosition(const GuiElement& element) const;
    glm::vec2 CalculateAbsoluteSize(const GuiElement& element) const;
    SDL_Color LerpColor(const SDL_Color& a, const SDL_Color& b, float t) const;
    
    // Initialize shaders
    void InitializeShaders();

    TTF_Font* m_font;
    GLuint m_quadVAO, m_quadVBO;
    GLuint m_textureShader;     // For text and images
    GLuint m_unifiedRectShader; // Unified shader for all rectangle types
    int m_screenW, m_screenH;
    std::vector<GuiElement> m_elements;
};
