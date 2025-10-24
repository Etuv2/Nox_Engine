# Nox Engine

A modern C++17 3D engine using SDL2 and OpenGL 4.6. Focused on a modular deferred renderer, real-time lighting and shadows, scene graph with JSON loading, editor-style ImGui UI, unified input, and 3D spatial audio.

## Engine Features

- Rendering
  - OpenGL 4.6 core, GLEW
  - Modular deferred pipeline (G-Buffer → lighting → post)
  - Cascaded shadow mapping (directional light)
  - SSAO, SSGI (screen-space GI), TAA, Bloom
  - Transparency forward pass
  - Skybox and IBL (irradiance, prefilter, BRDF LUT)
- Lighting
  - Directional, point, and spot lights
  - Central LightManager (GPU buffers, proxies for picking)
- Scene & Assets
  - Scene graph with hierarchical transforms
  - Scene loading from JSON (scenes/*.json)
  - glTF 2.0 model/material loading (TinyGLTF)
- Editor & UI
  - ImGui panels for scene, camera, lights, rendering settings
  - Performance graphs and gizmo-friendly selection
- Input
  - Unified input system with contexts (keyboard/mouse/gamepad)
  - Mouse-look camera, rebindable actions (optional config)
- Audio
  - 3D spatial audio nodes (SDL_mixer)
  - Listener updates from camera
- Physics & Utilities
  - Physics hooks with pause/resume and interpolation
  - BVH acceleration and precise ray picking
