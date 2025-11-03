/** Path Tracing Compute Shader
 * This shader performs path tracing to simulate realistic lighting and reflections in a 3D scene.
 * In the first part of the shader, we set up the necessary data structures and uniforms.
 * The second part implements the main path tracing algorithm, including ray generation,
 * intersection tests, and light accumulation.
 */

#version 460 core

//CONSTANTS
