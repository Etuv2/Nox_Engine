# NOX Engine

![NOX Engine](images/logo.png)

A modern, high-performance OpenGL 4.5+ rendering engine with advanced features for real-time 3D graphics, physics simulation, and interactive scene editing.

---

## Features

### Advanced Rendering
- **Deferred Rendering Pipeline**: Efficient multi-light rendering with advanced shading
- **Cascaded Shadow Mapping**: 4-level cascade shadow maps for directional lights
- **Percentage Closer Soft Shadows (PCSS)**: High-quality soft shadows with hardware filtering
- **Temporal Anti-Aliasing (TAA)**: Reduces aliasing artifacts and improves image quality
- **Bloom Post-Processing**: Realistic glow effects for bright surfaces
- **PBR Material System**: Physically-based rendering with metallic and roughness parameters
- **HDR Rendering**: High dynamic range support with configurable tone mapping

### Lighting System
- **Multi-Light Support**: Directional, Point, and Spot lights
- **Real-Time Shadow Mapping**: Dynamic shadow updates every frame
- **Shadow Array System**: Unified GPU buffer packing for all light types
- **Light Manager**: Efficient collection and management of scene lights
- **Configurable Light Properties**: Color, intensity, attenuation, and shadow parameters

### Physics Engine
- **Rigid Body Dynamics**: Static, Dynamic, and Kinematic body types
- **Fixed Timestep Simulation**: Stable physics at 60 Hz by default
- **Dynamic BVH Broadphase**: Efficient collision detection acceleration
- **Impulse-Based Constraint Solver**: Robust contact resolution
- **Warm Starting**: Improved stability for stacked objects
- **Sleep Management**: Performance optimization through body sleeping
- **Gizmo Manipulation**: Direct control of physics bodies via transform gizmo
- **Scene-Physics Synchronization**: Seamless integration between scene graph and physics

### Animation & Scene Management
- **glTF 2.0 Support**: Industry-standard 3D model loading
- **Skeletal Animation**: Full support for armature-based animations
- **Animation System**: Frame-based animation updates with interpolation
- **Scene Graph**: Hierarchical scene management with parent-child relationships
- **Scene Serialization**: Save and load complete scene states
- **JSON Scene Format**: Human-readable scene configuration files

### Editor Features
- **ImGui-Based Interface**: Modern, responsive editor UI
- **3D Transform Gizmo**: Visual manipulation of object transforms
- **Scene Hierarchy Browser**: Navigate and select scene objects
- **Property Inspector**: Edit object properties in real-time
- **Multiple Editor Windows**:
  - Status Window: Scene overview and quick actions
  - Camera Controls: Adjust camera parameters
  - Lighting System: Configure lights and shadows
  - Scene Hierarchy: Inspect scene graph
  - Gizmo Controls: Transform manipulation
  - Rendering Settings: Quality and performance options
  - Performance Monitor: Real-time performance metrics
  - Help & Controls: Comprehensive help system

### Performance & Debugging
- **Performance Profiler**: Frame time tracking and FPS display
- **Performance Recording**: Export frame data to CSV/JSON
- **State Export/Import**: Save and load game/editor states
- **Debug Visualization**: Optional debug drawing modes
- **Profiling Data**: Broadphase time, narrowphase time, solver time

### Input System
- **Flexible Input Mapping**: Customizable keyboard and gamepad controls
- **Multiple Input Contexts**: Global, Camera, Editor, Game, and Menu contexts
- **Mouse Lock**: Toggle mouse lock for camera control
- **Gamepad Support**: Full gamepad axis and button support
- **Action-Based System**: High-level action definitions for clean input handling

---

## Getting Started

### System Requirements
- **OS**: Windows 10/11
- **GPU**: OpenGL 4.5+ compatible graphics card
- **RAM**: 8 GB minimum, 16 GB recommended
- **Compiler**: Visual Studio 2019 or later (C++17)

### Dependencies
- OpenGL 4.5+
- GLEW (OpenGL Extension Wrangler)
- SDL2 (Simple DirectMedia Layer)
- GLM (OpenGL Mathematics)
- Dear ImGui (Immediate Mode GUI)
- ImGuizmo (Transform Gizmo)
- nlohmann JSON (JSON for Modern C++)
- tinyglTF (glTF 2.0 loader)

### Building from Source

#### Prerequisites
1. Install Visual Studio 2019 or later with C++17 support
2. Clone the repository:
   ```bash
   git clone https://github.com/yourusername/Nox_Engine.git
   cd Nox_Engine
   ```

#### Build Steps
1. Open `Nox_Engine.sln` in Visual Studio
2. Set the build configuration to `Release` or `Debug`
3. Build the solution (Ctrl+Shift+B)
4. Run the executable from `bin/` directory

### First Run
1. Ensure `config.json` is in the application root directory
2. The engine will load the default scene specified in config.json
3. Use the Help Window (F12) to learn keyboard controls

---

## Usage Guide

### Basic Controls

#### Camera Controls
- **WASD**: Move camera forward/backward/left/right
- **Mouse**: Look around (when mouse is unlocked)
- **Scroll Wheel**: Zoom in/out
- **Shift+Click**: Toggle mouse lock

#### Transform Gizmo
- **Q**: Translate mode
- **E**: Rotate mode
- **R**: Scale mode
- **G**: Toggle gizmo visibility
- **Left Mouse**: Drag to manipulate

#### Window Management
- **F1**: Engine Status window
- **F2**: Camera Controls window
- **F3**: Lighting System window
- **F4**: Scene Hierarchy window
- **F5**: Gizmo Controls window
- **F6**: Rendering Settings window
- **F7**: Performance Monitor window
- **F12**: Help & Controls window

### Creating a Scene

Create a JSON scene file in the `scenes/` directory:

```json
{
  "scene_name": "My Scene",
  "exposure": 1.0,
  "gamma": 2.2,
  "physics_enabled": true,
  "nodes": [
    {
      "name": "Cube",
      "type": "mesh",
      "position": [0, 0, 0],
      "rotation": [0, 0, 0],
      "scale": [1, 1, 1],
      "model_path": "models/cube.gltf",
      "physics": {
        "type": "dynamic",
        "mass": 1.0,
        "collider": {
          "type": "box",
          "size": [1, 1, 1]
        }
      }
    }
  ],
  "lights": [
    {
      "name": "DirectionalLight",
      "type": "directional",
      "position": [10, 10, 10],
      "direction": [-1, -1, -1],
      "intensity": 1.0,
      "color": [1, 1, 1],
      "shadows_enabled": true
    }
  ]
}
```

### Configuring the Engine

Edit `config.json` to customize engine settings:

```json
{
  "windowWidth": 1920,
  "windowHeight": 1080,
  "windowTitle": "NOX Engine",
  "vsync": false,
  "first_scene": "scenes/gundam.json",
  "camera": {
    "position": [-1.0, 1.0, -1.0],
    "fov": 120,
    "near": 0.1,
    "far": 600.0,
    "movement_speed": 30.0,
    "mouse_sensitivity": 0.1
  },
  "light": {
    "shadows_enabled": true,
    "shadow_size": 2048,
    "cascade_count": 4,
    "pcf_samples": 25
  },
  "physics": {
    "gravity": [0.0, -9.81, 0.0],
    "fixed_delta_time": 0.01667,
    "max_substeps": 4
  }
}
```

---

## Architecture

### Core Subsystems

#### Rendering System (ModularRenderer)
- Multi-pass rendering pipeline
- G-Buffer geometry pass
- Lighting pass with shadow mapping
- Post-processing effects
- ImGui overlay rendering

#### Physics Engine
- RigidBody physics simulation
- Dynamic BVH for broadphase
- Contact manifold narrowphase
- Impulse-based constraint solver
- Scene synchronization

#### Scene Management
- SceneGraph hierarchical structure
- SceneLoader for JSON deserialization
- SceneNode with ECS entity linking
- Model and animation management

#### Input System (InputIntegration)
- Action-based input mapping
- Multiple input contexts
- Keyboard and gamepad support
- Mouse lock management

#### Lighting System (LightManager)
- Light collection and management
- Shadow atlas generation
- GPU buffer packing
- Multi-light rendering support

### Key Classes

| Class | Purpose |
|-------|---------|
| `Core` | Main engine coordinator |
| `ModularRenderer` | Rendering pipeline |
| `PhysicsEngine` | Physics simulation |
| `SceneGraph` | Scene hierarchy |
| `Camera` | View frustum and projection |
| `DirectionalLight` | Cascaded shadow mapping |
| `LightManager` | Multi-light management |
| `InputIntegration` | Input handling |
| `ImGuiInterface` | Editor UI |

---

## Project Structure

```
Nox_Engine/
??? src/
?   ??? Core.h/.cpp                 # Main engine coordinator
?   ??? ModularRenderer.h/.cpp      # Rendering pipeline
?   ??? PhysicsEngine.h/.cpp        # Physics simulation
?   ??? SceneGraph.h/.cpp           # Scene management
?   ??? SceneLoader.h/.cpp          # Scene serialization
?   ??? Camera.h/.cpp               # Camera system
?   ??? Lighting/
?   ?   ??? DirectionalLight.h/.cpp
?   ?   ??? LightManager.h/.cpp
?   ?   ??? LightNode.h/.cpp
?   ??? Physics/
?   ?   ??? RigidBody.h/.cpp
?   ?   ??? PhysicsCollision.h/.cpp
?   ?   ??? PhysicsBVH.h/.cpp
?   ??? Input/
?   ?   ??? InputIntegration.h/.cpp
?   ?   ??? InputSystem.h/.cpp
?   ??? ImGui/
?       ??? ImGuiInterface.h/.cpp
?       ??? ImGuiWindowManager.h/.cpp
?       ??? Windows/
?           ??? StatusWindow.h/.cpp
?           ??? CameraWindow.h/.cpp
?           ??? LightingWindow.h/.cpp
?           ??? ...
??? shaders/                        # GLSL shader programs
??? scenes/                         # Scene JSON files
??? models/                         # 3D model assets
??? fonts/                          # Font files
??? config.json                     # Engine configuration
??? Nox_Engine.sln                  # Visual Studio solution
```

---

## Rendering Pipeline

### Multi-Pass Rendering

1. **Shadow Pass**: Render to shadow atlas for all lights
2. **G-Buffer Pass**: Render scene to deferred buffers (Position, Normal, Albedo, etc.)
3. **Lighting Pass**: Combine lights with shadow mapping
4. **Post-Processing**:
   - Temporal Anti-Aliasing
   - Bloom extraction and blur
   - Tone mapping and color grading
5. **UI Overlay**: ImGui windows and 3D gizmo

### Cascaded Shadow Mapping
- 4-level cascade splits
- Configurable cascade distribution
- PCSS soft shadow filtering
- PCF for hardware filtering

---

## Physics Configuration

### Simulation Parameters (config.json)

```json
"physics": {
  "gravity": [0.0, -9.81, 0.0],
  "fixed_delta_time": 0.01667,
  "max_substeps": 4,
  "velocity_iterations": 10,
  "position_iterations": 4,
  "sleep_enabled": true,
  "default_friction": 0.5,
  "default_restitution": 0.0
}
```

### Body Types
- **Static**: Infinite mass, doesn't move
- **Dynamic**: Simulated with forces and constraints
- **Kinematic**: Controlled by scene, no physics simulation

---

## Animation System

### Supported Formats
- glTF 2.0 animations
- Skeletal animation with bone transforms
- Multiple animation tracks per model

### Animation Features
- Frame-based interpolation
- Blending between states
- Animation callbacks
- Root motion support

---

## Performance Optimization

### Built-In Optimizations
- **BVH Acceleration**: Hierarchical spatial queries
- **Frustum Culling**: GPU-based frustum culling in geometry shader
- **Shadow Atlas**: Unified texture array for all shadow maps
- **Instanced Rendering**: MDI (Multi-Draw Indirect) for batch rendering
- **Body Sleeping**: Physics bodies sleep when stationary
- **Temporal Effects**: TAA with history buffer reuse

### Configuration Tips
1. Reduce shadow map resolution for lower-end GPUs
2. Disable PCSS for better performance
3. Adjust cascade count (3-4 typical)
4. Reduce PCF sample count (16-25)
5. Use object pooling for frequently created objects

---

## Debugging

### Debug Windows
- **Performance Monitor**: View frame times and statistics
- **Scene Hierarchy**: Inspect scene graph structure
- **Gizmo Controls**: Verify transform manipulations
- **Console Output**: Check for errors and warnings

### Debug Features
- Light priority selection debugging
- Contact information logging
- Physics body state inspection
- Transform synchronization tracking

---

## Scene File Format

### JSON Schema

```json
{
  "scene_name": "string",
  "exposure": "float",
  "gamma": "float",
  "physics_enabled": "boolean",
  "nodes": [
    {
      "name": "string",
      "type": "mesh|light|audio|camera|empty",
      "position": [x, y, z],
      "rotation": [x, y, z],
      "scale": [x, y, z],
      "model_path": "string (optional)",
      "physics": { /* physics component */ },
      "children": [ /* nested nodes */ ]
    }
  ],
  "lights": [ /* light definitions */ ]
}
```

---

## Extending the Engine

### Adding a Custom Renderer Pass

```cpp
class MyCustomPass : public RenderPass {
    void Render(const SceneGraph& scene, const Camera& camera) override;
    void Resize(int width, int height) override;
};
```

### Creating Custom Physics Bodies

```cpp
auto body = std::make_shared<RigidBody>();
body->SetBodyType(RigidBody::BodyType::Dynamic);
body->setSphere(1.0f);  // Sphere with radius 1.0
physicsEngine->AddBody(body);
```

### Custom Input Actions

```cpp
inputSystem->RegisterAction(Input::Actions::CUSTOM_ACTION, 
    InputBinding(SDL_SCANCODE_T, InputModifier::NONE));
```

---

## Documentation

- **Help Window**: Press F12 in-engine for comprehensive help
- **Code Comments**: Extensive documentation in source code
- **Example Scenes**: Multiple pre-built scenes in `scenes/` directory
- **Config Files**: Well-documented JSON configuration

---

## Contributing

We welcome contributions! Please:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Coding Standards
- Use C++17 features
- Follow Google C++ style guide
- Document public APIs
- Include unit tests for new features

---

## License

This project is licensed under the MIT License - see the LICENSE file for details.

---

## Acknowledgments

- **GLEW**: OpenGL Extension Wrangler Library
- **SDL2**: Simple DirectMedia Layer
- **GLM**: OpenGL Mathematics
- **Dear ImGui**: Immediate Mode GUI
- **ImGuizmo**: Transform Gizmo for ImGui
- **nlohmann/json**: JSON for Modern C++
- **tinyglTF**: glTF 2.0 loader

---

## Contact & Support

For questions, issues, or suggestions:
- **GitHub Issues**: Report bugs and request features
- **Discussions**: Ask questions and share ideas
- **Email**: [your-email@example.com](mailto:your-email@example.com)

---

## Roadmap

### Upcoming Features
- [ ] Compute shader support for advanced post-processing
- [ ] NVIDIA DLSS integration
- [ ] Mesh shader support
- [ ] Improved animation blending system
- [ ] Audio engine enhancements
- [ ] Network multiplayer support
- [ ] VR headset support
- [ ] Mobile platform support (Android/iOS)

### Performance Improvements
- [ ] GPU-driven rendering pipeline
- [ ] Bindless texture support
- [ ] Virtual texture system
- [ ] Streaming asset system

---

## Version History

### v1.0.0 (Current)
- Initial public release
- Full rendering pipeline
- Physics engine
- Animation system
- Scene editor
- Multi-light support

### v0.9.0 (Beta)
- Core engine architecture
- Basic rendering
- Physics foundation

---

Made with care by the NOX Engine Team

---

*Last Updated: 2024*
*Repository: https://github.com/yourusername/Nox_Engine*
