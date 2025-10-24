# Nox Engine

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-brightgreen.svg)](https://en.cppreference.com/w/)
[![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)](#platform-support)

A modern, feature-rich 3D game engine built with C++17, designed for developers who want a flexible and extensible framework for creating interactive 3D applications.

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Project Structure](#project-structure)
- [Usage Examples](#usage-examples)
- [API Documentation](#api-documentation)
- [Dependencies](#dependencies)
- [Building](#building)
- [Contributing](#contributing)
- [License](#license)

## Features

### Core Engine Systems

- **Scene Graph Management**: Hierarchical node-based scene organization with transform inheritance
- **3D Graphics Rendering**: Built on modern graphics APIs with support for meshes and materials
- **Audio System**: Spatial 3D audio with distance attenuation and listener positioning (SDL_mixer)
- **Animation System**: 
  - Keyframe-based animations with multiple interpolation modes
  - Skeletal animation support for rigged models
  - Morph target/blend shape animations
  - Animation blending and state management
- **Model Loading**: Support for glTF 2.0 format with full material and texture support
- **Camera System**: Flexible perspective and orthographic projection support
- **Input Handling**: Keyboard and mouse input management
- **Mathematics**: Full 3D math library (GLM) with matrix, vector, and quaternion operations

### Advanced Features

- **Hierarchy System**: Full parent-child transform relationships with automatic propagation
- **3D Positional Audio**: Listener-based spatial audio with configurable hearing distances
- **Material System**: Support for PBR materials with textures
- **Transform System**: Local and world space transformations with automatic updates
- **Animation Interpolation**: Linear, step, and cubic spline interpolation modes

## Architecture

### High-Level Overview

```
┌─────────────────────────────────────┐
│        Application Layer            │
├─────────────────────────────────────┤
│    Scene & Node Management          │
├─────────────────────────────────────┤
│  Graphics | Audio | Animation       │
├─────────────────────────────────────┤
│    Core Systems & Utilities         │
├─────────────────────────────────────┤
│  SDL | GLM | TinyGLTF | OpenGL      │
└─────────────────────────────────────┘
```

### Key Components

#### SceneNode (Core Scene Graph)
The fundamental building block of the engine. Every entity in your scene is a `SceneNode` with:
- **Transform System**: Position, rotation, scale (TRS)
- **Hierarchy**: Parent-child relationships
- **Components**: Can be extended for custom behavior
- **Lifecycle**: Automatic update and render propagation

```cpp
// Example: SceneNode hierarchy
auto rootNode = std::make_shared<SceneNode>("Root");
auto childNode = std::make_shared<SceneNode>("Child");
rootNode->AddChild(childNode);
// childNode automatically inherits parent's transform
```

#### Scene
Container for all nodes, animations, and render data. Manages:
- Node hierarchy traversal
- Animation playback
- Mesh rendering
- Audio updates

#### Camera
Specialized node that defines view and projection:
- Perspective and orthographic projections
- View matrix computation
- Mouse and keyboard input handling

#### AudioNode
Extends SceneNode with 3D spatial audio:
- Listener-relative positioning
- Distance-based attenuation
- Pitch and volume control
- Looping support

#### Animation System
Sophisticated animation framework supporting:
- **Channel Types**: Translation, rotation, scale, morph weights
- **Interpolation Modes**: Linear, step, cubic spline
- **Keyframe Animation**: Time-based interpolation with wrapping
- **State Management**: Animation blending and weight control

## Installation

### Prerequisites

- **C++17** or later
- **Visual Studio 2019+** (or compatible C++ compiler)
- **CMake 3.16+** (optional, if using CMake build)
- **Windows** operating system

### Required Dependencies

The engine depends on the following libraries (included in the repository):

- **GLM** - Mathematics library for 3D graphics
- **TinyGLTF** - glTF 2.0 model loader
- **SDL2** - Cross-platform development library
- **SDL_mixer** - Audio mixing library
- **OpenGL** - Graphics API

### Clone the Repository

```bash
git clone https://github.com/Etuv2/Nox_Engine.git
cd Nox_Engine
```

## Quick Start

### Basic Scene Setup

```cpp
#include "Scene.h"
#include "Camera.h"
#include "AudioNode.h"

int main() {
    // Create the main scene
    Scene scene;
    
    // Create a camera
    auto camera = std::make_shared<Camera>(
        glm::vec3(0, 5, 10),      // Position
        glm::vec3(0, 1, 0),       // World up
        -90.0f, -20.0f,           // Yaw, pitch
        45.0f,                    // FOV
        1920.0f / 1080.0f,        // Aspect ratio
        0.1f, 1000.0f             // Near/far planes
    );
    scene.AddNode(camera);
    
    // Create an audio source
    auto audioNode = std::make_shared<AudioNode>();
    audioNode->initAudio("sounds/ambient.mp3");
    audioNode->play(true, true);  // Loop, 3D audio
    scene.AddNode(audioNode);
    
    // Load a glTF model
    auto model = scene.LoadModel("models/character.gltf");
    
    // Update scene audio from listener position
    scene.UpdateAudioNodes(
        camera->GetCameraPosition(),
        camera->GetCameraFacingAngle()
    );
    
    return 0;
}
```

### Creating Custom Nodes

```cpp
class PlayerNode : public SceneNode {
private:
    float m_health = 100.0f;
    float m_speed = 5.0f;
    
public:
    PlayerNode() : SceneNode("Player") {}
    
    void Update(float deltaTime) override {
        // Custom update logic
        // Transform is automatically propagated to children
    }
    
    void TakeDamage(float damage) {
        m_health -= damage;
    }
    
    float GetHealth() const { return m_health; }
};
```

## Project Structure

```
Nox_Engine/
├── src/                          # Source files
│   ├── AudioNode.cpp            # 3D audio implementation
│   ├── Scene.cpp                # Scene management
│   ├── Camera.cpp               # Camera system
│   ├── Animation.cpp            # Animation system
│   └── ...                      # Other core systems
│
├── include/                      # Header files
│   ├── AudioNode.h
│   ├── Scene.h
│   ├── SceneNode.h
│   ├── Camera.h
│   ├── Animation.h
│   └── ...
│
├── third_party/                  # External dependencies
│   ├── glm/                     # Math library
│   ├── tinygltf/                # Model loading
│   └── SDL/                     # Platform layer
│
├── assets/                       # Game assets (gitignored)
│   ├── models/
│   ├── textures/
│   ├── sounds/
│   └── shaders/
│
├── CMakeLists.txt               # CMake build configuration
├── Nox_Engine.sln               # Visual Studio solution
├── Nox_Engine.vcxproj           # Visual Studio project
└── README.md                    # This file
```

## Usage Examples

### Loading and Playing Animations

```cpp
// Load a model with animations
auto model = scene.LoadModel("models/character.gltf");

// Get animation by index
auto& animations = model->GetAnimations();
if (!animations.empty()) {
    Animation& animation = animations[0];
    
    // Set animation properties
    animation.isLooping = true;
    animation.weight = 1.0f;
    
    // Query animation state
    float duration = animation.duration;
    bool hasSkeletalAnimation = animation.HasSkeletalAnimation();
}
```

### Spatial Audio Example

```cpp
// Create audio node in the world
auto musicNode = std::make_shared<AudioNode>();
musicNode->initAudio("sounds/music.mp3");
musicNode->setVolume(0.8f);
musicNode->setHearingDistance(500.0f);
musicNode->setPitch(1.0f);

// Place it in the scene
musicNode->SetLocalPosition(glm::vec3(10, 0, 0));
scene.AddNode(musicNode);
musicNode->play(true, true);  // Loop, 3D audio

// Update audio based on listener
scene.UpdateAudioNodes(
    cameraPos,
    cameraYaw
);
```

### Camera Control

```cpp
// Setup perspective camera
camera->SetPerspective(
    45.0f,              // FOV in degrees
    1920.0f / 1080.0f,  // Aspect ratio
    0.1f,               // Near plane
    1000.0f             // Far plane
);

// Get view/projection matrices
glm::mat4 viewMatrix = camera->GetViewMatrix();
glm::mat4 projMatrix = camera->GetProjectionMatrix();
auto [view, proj] = camera->GetViewProjectionMatrix();

// Handle input
const Uint8* keystate = SDL_GetKeyboardState(nullptr);
camera->ProcessKeyboard(keystate, deltaTime);
camera->ProcessMouseMovement(mouseDeltaX, mouseDeltaY);
camera->ProcessMouseScroll(scrollDelta);
```

### Transform Hierarchy

```cpp
// Create hierarchy
auto parent = std::make_shared<SceneNode>("Parent");
auto child1 = std::make_shared<SceneNode>("Child1");
auto child2 = std::make_shared<SceneNode>("Child2");

parent->AddChild(child1);
parent->AddChild(child2);

// Set transforms
parent->SetLocalPosition(glm::vec3(0, 0, 0));
parent->SetLocalRotation(glm::quat(1, 0, 0, 0));
parent->SetLocalScale(glm::vec3(1, 1, 1));

// Child inherits parent transform
child1->SetLocalPosition(glm::vec3(5, 0, 0));

// Get world position
glm::vec3 childWorldPos = child1->GetGlobalTransform()[3];
```

## API Documentation

### SceneNode

Core scene graph node class.

**Key Methods:**
```cpp
// Transform operations
void SetLocalPosition(const glm::vec3& pos);
void SetLocalRotation(const glm::quat& rot);
void SetLocalScale(const glm::vec3& scale);
glm::mat4 GetGlobalTransform(const glm::mat4& parentWorld);

// Hierarchy operations
void AddChild(std::shared_ptr<SceneNode> child);
void RemoveChild(std::shared_ptr<SceneNode> child);
const std::vector<std::shared_ptr<SceneNode>>& GetChildren() const;

// Lifecycle
virtual void Update(float deltaTime);
virtual void Draw(const glm::mat4& projection, const glm::mat4& view);
```

### AudioNode

Spatial 3D audio node.

**Key Methods:**
```cpp
// Initialization
bool initAudio(const std::string& soundFile);

// Playback control
void play(bool loop, bool is3d);
void stop();
void updateAudio(const glm::vec3& listenerPos, float listenerAngle);

// Audio properties
void setPitch(float pitch);           // [0.1, 10.0]
void setVolume(float volume);         // [0.0, 1.0]
void setHearingDistance(float dist);  // Distance for attenuation

// State queries
bool IsPlaying() const;
```

### Camera

View and projection management.

**Key Methods:**
```cpp
// Projection setup
void SetPerspective(float fov, float aspect, float near, float far);
void SetOrthographic(float left, float right, float bottom, float top, float near, float far);

// Matrix getters
glm::mat4 GetViewMatrix() const;
glm::mat4 GetProjectionMatrix() const;
std::pair<glm::mat4, glm::mat4> GetViewProjectionMatrix() const;

// Input handling
void ProcessKeyboard(const Uint8* keystate, float deltaTime);
void ProcessMouseMovement(float xoffset, float yoffset, bool constrainPitch = true);
void ProcessMouseScroll(float yoffset);
```

### Animation

Animation playback and interpolation.

**Key Methods:**
```cpp
// Animation queries
float GetDuration() const;
bool HasSkeletalAnimation() const;
bool HasMorphTargets() const;
std::vector<int> GetAnimatedNodes() const;

// Interpolation
glm::vec3 InterpolateTranslation(const Channel& channel, float time) const;
glm::quat InterpolateRotation(const Channel& channel, float time) const;
glm::vec3 InterpolateScale(const Channel& channel, float time) const;

// State
glm::mat4 GetNodeTransform(float time) const;
```

### Scene

Container and manager for all scene content.

**Key Methods:**
```cpp
// Node management
void AddNode(std::shared_ptr<SceneNode> node);
void RemoveNode(std::shared_ptr<SceneNode> node);

// Model loading
std::shared_ptr<SceneNode> LoadModel(const std::string& filepath);

// Update and render
void Update(float deltaTime);
void Render(const glm::mat4& projection, const glm::mat4& view);
void UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle);
```

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| GLM | Latest | 3D Mathematics (vectors, matrices, quaternions) |
| TinyGLTF | v2.9.x | glTF 2.0 model format support |
| SDL2 | 2.0+ | Cross-platform windowing and input |
| SDL_mixer | 2.0+ | Audio mixing and playback |
| OpenGL | 4.5+ | Graphics rendering API |

All dependencies are included in the `third_party/` directory.

## Building

### Using Visual Studio

1. Open `Nox_Engine.sln`
2. Select your configuration (Debug/Release)
3. Build → Build Solution (Ctrl+Shift+B)

### Using CMake (Optional)

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### Build Artifacts

- **Debug**: `bin/Debug/` - Debug executable with symbols
- **Release**: `bin/Release/` - Optimized executable

## Contributing

We welcome contributions! Here's how to help:

### Getting Started

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes with clear, descriptive commits
4. Push to your branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request with a detailed description

### Contribution Guidelines

- **Code Style**: Follow existing code conventions (C++17)
- **Documentation**: Add comments for complex logic and update README if needed
- **Testing**: Test your changes thoroughly before submitting
- **Commit Messages**: Use clear, concise commit messages
- **No Breaking Changes**: Maintain backward compatibility where possible

### Areas for Contribution

- Additional audio features (effects, streaming)
- Performance optimizations
- Extended animation support
- Additional model formats
- Better error handling
- Unit tests
- Documentation improvements
- Example projects

## Known Limitations & Future Work

### Current Limitations
- **Windows Only**: Currently supports Windows platforms only
- **Single Threaded**: Audio and rendering are not threaded
- **Basic Physics**: No built-in physics engine (use Bullet, PhysX, etc.)
- **No Particle System**: Particles must be implemented separately
- **Limited Shaders**: Basic shader support; advanced shaders need implementation

### Future Roadmap
- [ ] Cross-platform support (Linux, macOS)
- [ ] Multi-threaded rendering and audio
- [ ] Built-in particle system
- [ ] Physics engine integration
- [ ] Deferred rendering pipeline
- [ ] Advanced material system (PBR enhancements)
- [ ] Networking support
- [ ] Editor tools
- [ ] Profiling and debugging tools

## Performance Considerations

### Optimization Tips

1. **Batch Rendering**: Group similar meshes to reduce draw calls
2. **LOD System**: Implement Level of Detail for distant objects
3. **Audio Optimization**: Use compressed audio formats
4. **Frustum Culling**: Avoid rendering off-screen objects
5. **Memory Management**: Use object pooling for frequently created/destroyed entities

### Profiling

- Use Visual Studio's built-in profiler for performance analysis
- Monitor draw call count and vertex throughput
- Profile audio update overhead, especially with many spatial sources

## Troubleshooting

### Common Issues

**Issue: Audio not playing**
```cpp
// Solution: Ensure audio system is initialized
if (!audioNode.initAudio("path/to/sound.mp3")) {
    std::cerr << "Failed to load audio file" << std::endl;
    // Check file path and format
}
```

**Issue: Model not loading**
```cpp
// Solution: Verify model format and path
auto model = scene.LoadModel("models/character.gltf");
// Ensure file exists and is valid glTF 2.0
```

**Issue: Transform not updating**
```cpp
// Solution: Ensure hierarchy is correct and Update() is called
scene.Update(deltaTime);  // Propagates transforms to all nodes
```

**Issue: Camera view incorrect**
```cpp
// Solution: Verify projection and view matrix settings
camera->SetPerspective(45.0f, aspect, 0.1f, 1000.0f);
camera->updateCameraVectors();
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- **GLM** - OpenGL Mathematics library
- **TinyGLTF** - glTF file format loader
- **SDL/SDL_mixer** - Cross-platform development and audio
- **Community Contributors** - Thank you to all who have contributed!

## Contact & Support

- **Issues**: Please report bugs via [GitHub Issues](https://github.com/Etuv2/Nox_Engine/issues)
- **Discussions**: Join our [GitHub Discussions](https://github.com/Etuv2/Nox_Engine/discussions)
- **Email**: For other inquiries, please open an issue

## Changelog

### v1.0.0 (Current)
- Initial release
- Core scene graph system
- 3D audio support with spatial positioning
- Animation system with keyframe interpolation
- glTF 2.0 model loading
- Camera system with perspective and orthographic projections
- Input handling (keyboard, mouse)

---

**Last Updated**: 2025
**Maintainer**: Etuv2

For the latest updates and examples, visit the [GitHub repository](https://github.com/Etuv2/Nox_Engine).
