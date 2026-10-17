#pragma once

#include "vk/Material.hpp"
#include "vk/Mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

/// A mesh as it comes off disk, before anything is uploaded to the GPU.
///
/// This is the hand-off between the loaders and the renderer: the renderer
/// turns it into a Mesh and a material update without knowing which file format
/// it came from.
///
/// Positions are in the renderer's world convention - **+Z up, -X forward** -
/// with the file's node transforms already baked in, so all the renderer has to
/// do is apply its own spin. A loader for a format that uses another convention
/// is responsible for rotating its data into this one.
struct MeshData {
    std::string name;

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    /// The material the file assigns to the mesh, or the defaults when it
    /// assigns none.
    PbrMaterial material;

    /// Axis-aligned bounds of `vertices`, used to center the mesh and to frame
    /// the camera on it after an import.
    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};
};

/// Loads a mesh file, picking the reader from the file extension. Throws
/// std::runtime_error with a message that is short enough to show in the UI.
MeshData loadMesh(const std::string& path);

/// Reads a binary glTF container (`.glb`): the JSON chunk that describes the
/// scene and the BIN chunk that holds the vertex data. The external-buffer
/// flavour of glTF (`.gltf` plus a `.bin`) is deliberately not supported.
MeshData loadGlb(const std::string& path);

/// Built-in geometry: a unit cube with per-face normals.
///
/// It is the fallback for when the default model file cannot be found, so the
/// renderer can still start and report the problem instead of failing to open a
/// window. It also doubles as a known-good shape for checking that the PBR
/// shader and depth testing work while the import path is being changed.
MeshData makeCubeMesh();
