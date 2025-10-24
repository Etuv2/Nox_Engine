#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <SDL/SDL_image.h>
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <JSON/json.h>
#include "tiny_gltf.h"
#include "MeshComponent.h"
#include "Vertex.h"
#include "Animation.h"

/**
 * The Scene class holds:
 *   - Mesh data (vertex/index buffers) for all primitives
 *   - A glTF node hierarchy (nodes[]), each with a local transform
 *   - A Skin structure (joints + inverseBindMatrices) if it’s a skinned mesh
 *   - Animations loaded from glTF
 */
class Scene {
public:
    // Mesh data from the glTF file
    std::vector<MeshComponent> meshes;
    // Name of the model (file name)
    std::string m_model_name;
    // Animations loaded from glTF
    std::vector<Animation> animations;

    // Node info from the glTF: local transform, parent index, and child indices
    struct NodeInfo {
        std::string name;
        glm::mat4 localTransform = glm::mat4(1.0f);
        int parent = -1;
        std::vector<int> children;
    };
    std::vector<NodeInfo> nodes;

    // Skin data
    struct Skin {
        std::vector<int> joints;         // Node indices
        std::vector<glm::mat4> inverseBindMatrices;
    };
    Skin skin;
    bool hasSkin = false;
    bool hasTangents = false;

    // Loads the scene from a glTF file (ASCII or binary), including PBR material textures.
    bool LoadFromGLTF(const std::string& path);

    // Draw all loaded meshes, with no top-level transform
    void Draw();

    void ComputeTangents(std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);

    // Returns bounding box (min/max) of all vertex data in local space
    std::pair<glm::vec3, glm::vec3> GetBoundingBox() const;

    // Cleanup GPU resources
    void Cleanup();

    std::string GetName() {
        return !m_model_name.empty() ? m_model_name : "No name";
    }
};

