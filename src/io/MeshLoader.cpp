#include "io/MeshLoader.hpp"

#include "core/Math.hpp"
#include "io/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// The binary glTF container
// ---------------------------------------------------------------------------
//
// A .glb file is a small header followed by chunks: one JSON chunk describing
// the scene and one BIN chunk holding the vertex data. Everything below reads
// that container; the JSON is handed to io/Json.cpp.

namespace {

constexpr std::uint32_t kGlbMagic = 0x46546C67u;  // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534Au;  // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942u;  // "BIN\0"

// glTF accessor component types.
constexpr std::uint32_t kComponentByte = 5120;
constexpr std::uint32_t kComponentUnsignedByte = 5121;
constexpr std::uint32_t kComponentShort = 5122;
constexpr std::uint32_t kComponentUnsignedShort = 5123;
constexpr std::uint32_t kComponentUnsignedInt = 5125;
constexpr std::uint32_t kComponentFloat = 5126;

/// glTF primitive modes. Only the default, triangle lists, is implemented.
constexpr int kModeTriangles = 4;

/// How deep the scene graph may nest before the file is assumed to be malformed.
/// Nothing legitimate comes close; the limit only exists so a cyclic `children`
/// list fails with a message instead of a stack overflow.
constexpr int kMaxNodeDepth = 64;

/// Little-endian reads. Every platform the renderer builds for is little-endian
/// (glTF is defined as little-endian too), so a byte swap is not needed; the
/// memcpy keeps the reads free of alignment assumptions.
std::uint16_t readU16(const std::uint8_t* bytes) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

std::uint32_t readU32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open '" + path + "'");
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("'" + path + "' is empty");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) {
        throw std::runtime_error("Failed to read '" + path + "'");
    }
    return bytes;
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

/// The file name without its directory or extension, used as a fallback label
/// when the file itself does not name the mesh.
std::string fileStem(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return name;
}

struct GlbContainer {
    std::string json;

    /// The BIN chunk. Points into the buffer returned by readFileBytes, which
    /// the caller keeps alive for as long as the container is used.
    const std::uint8_t* binary = nullptr;
    std::size_t binarySize = 0;
};

GlbContainer parseGlbContainer(const std::vector<std::uint8_t>& bytes, const std::string& path) {
    if (bytes.size() < 12) {
        throw std::runtime_error("'" + path + "' is too small to be a binary glTF file");
    }
    if (readU32(bytes.data()) != kGlbMagic) {
        throw std::runtime_error("'" + path + "' has no glTF magic number - only binary .glb files are supported");
    }

    const std::uint32_t version = readU32(bytes.data() + 4);
    if (version != 2) {
        throw std::runtime_error("'" + path + "' uses glTF version " + std::to_string(version) +
                                 "; only version 2 is supported");
    }

    const std::uint32_t declaredLength = readU32(bytes.data() + 8);
    if (declaredLength > bytes.size()) {
        throw std::runtime_error("'" + path + "' is truncated");
    }

    GlbContainer container;
    std::size_t offset = 12;
    while (offset + 8 <= declaredLength) {
        const std::uint32_t chunkLength = readU32(bytes.data() + offset);
        const std::uint32_t chunkType = readU32(bytes.data() + offset + 4);
        offset += 8;

        if (offset + chunkLength > bytes.size()) {
            throw std::runtime_error("'" + path + "' has a chunk that runs past the end of the file");
        }

        if (chunkType == kChunkJson) {
            container.json.assign(reinterpret_cast<const char*>(bytes.data() + offset), chunkLength);
        } else if (chunkType == kChunkBin) {
            container.binary = bytes.data() + offset;
            container.binarySize = chunkLength;
        }
        // Unknown chunk types are skipped: the spec requires readers to ignore
        // anything they do not recognise.

        offset += chunkLength;
    }

    if (container.json.empty()) {
        throw std::runtime_error("'" + path + "' has no JSON chunk");
    }
    return container;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::size_t componentSize(std::uint32_t componentType) {
    switch (componentType) {
        case kComponentByte:
        case kComponentUnsignedByte: return 1;
        case kComponentShort:
        case kComponentUnsignedShort: return 2;
        case kComponentUnsignedInt:
        case kComponentFloat: return 4;
        default:
            throw std::runtime_error("Unsupported glTF component type: " +
                                     std::to_string(componentType));
    }
}

int componentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    throw std::runtime_error("Unsupported glTF accessor type: '" + type + "'");
}

/// A decoded view onto one accessor: where its bytes start and how to step
/// through them. glTF lets several accessors share (and interleave inside) one
/// buffer view, which is what `stride` carries.
struct AccessorView {
    const std::uint8_t* base = nullptr;
    std::size_t count = 0;
    std::size_t stride = 0;
    std::uint32_t componentType = 0;
    int components = 0;
    bool normalized = false;
};

AccessorView openAccessor(const json::Value& gltf, const GlbContainer& container, int accessorIndex) {
    const json::Value& accessor =
        gltf.at("accessors").at(static_cast<std::size_t>(accessorIndex));
    if (!accessor.isObject()) {
        throw std::runtime_error("glTF accessor " + std::to_string(accessorIndex) + " is missing");
    }
    if (accessor.at("sparse").isObject()) {
        throw std::runtime_error("glTF sparse accessors are not supported");
    }

    const int bufferViewIndex = accessor.at("bufferView").intValue(-1);
    if (bufferViewIndex < 0) {
        throw std::runtime_error("glTF accessor " + std::to_string(accessorIndex) +
                                 " has no buffer view");
    }

    const json::Value& view = gltf.at("bufferViews").at(static_cast<std::size_t>(bufferViewIndex));
    if (!view.isObject()) {
        throw std::runtime_error("glTF bufferView " + std::to_string(bufferViewIndex) +
                                 " is missing");
    }

    // In a .glb the first buffer is the BIN chunk itself. A `uri` on it - or any
    // other buffer index - means the data lives in a separate file, which this
    // loader deliberately does not follow.
    if (gltf.at("buffers").at(0).at("uri").isString()) {
        throw std::runtime_error("This file references an external buffer; export a self-contained .glb");
    }
    if (view.at("buffer").intValue(0) != 0) {
        throw std::runtime_error("This file references a buffer that is not the .glb data chunk");
    }
    if (container.binary == nullptr) {
        throw std::runtime_error("The .glb file has no binary data chunk");
    }

    AccessorView result;
    result.componentType = static_cast<std::uint32_t>(accessor.at("componentType").intValue(0));
    result.components = componentCount(accessor.at("type").string());
    result.count = static_cast<std::size_t>(accessor.at("count").intValue(0));
    result.normalized = accessor.at("normalized").boolean(false);

    const std::size_t elementSize =
        static_cast<std::size_t>(result.components) * componentSize(result.componentType);
    const std::size_t declaredStride = static_cast<std::size_t>(view.at("byteStride").intValue(0));
    result.stride = declaredStride != 0 ? declaredStride : elementSize;

    const std::size_t viewOffset = static_cast<std::size_t>(view.at("byteOffset").intValue(0));
    const std::size_t accessorOffset = static_cast<std::size_t>(accessor.at("byteOffset").intValue(0));

    const std::size_t lastByte =
        viewOffset + accessorOffset + (result.count == 0 ? 0 : (result.count - 1) * result.stride + elementSize);
    if (lastByte > container.binarySize) {
        throw std::runtime_error("glTF accessor " + std::to_string(accessorIndex) +
                                 " reads past the end of the binary chunk");
    }

    result.base = container.binary + viewOffset + accessorOffset;
    return result;
}

/// One component as a float, undoing the normalization that glTF uses to store
/// small values compactly (texture coordinates, mostly).
float readComponentAsFloat(const std::uint8_t* bytes, std::uint32_t componentType, bool normalized) {
    switch (componentType) {
        case kComponentFloat: {
            float value = 0.0f;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        case kComponentUnsignedByte: {
            const float value = static_cast<float>(*bytes);
            return normalized ? value / 255.0f : value;
        }
        case kComponentByte: {
            signed char raw = 0;
            std::memcpy(&raw, bytes, sizeof(raw));
            const float value = static_cast<float>(raw);
            return normalized ? std::max(value / 127.0f, -1.0f) : value;
        }
        case kComponentUnsignedShort: {
            const float value = static_cast<float>(readU16(bytes));
            return normalized ? value / 65535.0f : value;
        }
        case kComponentShort: {
            std::int16_t raw = 0;
            std::memcpy(&raw, bytes, sizeof(raw));
            const float value = static_cast<float>(raw);
            return normalized ? std::max(value / 32767.0f, -1.0f) : value;
        }
        case kComponentUnsignedInt: {
            return static_cast<float>(readU32(bytes));
        }
        default:
            throw std::runtime_error("Unsupported glTF component type: " +
                                     std::to_string(componentType));
    }
}

std::vector<float> readAccessorFloats(const json::Value& gltf, const GlbContainer& container,
                                      int accessorIndex, int expectedComponents) {
    const AccessorView view = openAccessor(gltf, container, accessorIndex);
    if (view.components != expectedComponents) {
        throw std::runtime_error("glTF accessor " + std::to_string(accessorIndex) + " has " +
                                 std::to_string(view.components) + " components, expected " +
                                 std::to_string(expectedComponents));
    }

    const std::size_t components = static_cast<std::size_t>(view.components);
    const std::size_t componentBytes = componentSize(view.componentType);

    std::vector<float> out(view.count * components);
    for (std::size_t element = 0; element < view.count; ++element) {
        const std::uint8_t* source = view.base + element * view.stride;
        for (std::size_t c = 0; c < components; ++c) {
            out[element * components + c] =
                readComponentAsFloat(source + c * componentBytes, view.componentType, view.normalized);
        }
    }
    return out;
}

std::vector<std::uint32_t> readAccessorIndices(const json::Value& gltf,
                                               const GlbContainer& container, int accessorIndex) {
    const AccessorView view = openAccessor(gltf, container, accessorIndex);
    if (view.components != 1) {
        throw std::runtime_error("glTF index accessor " + std::to_string(accessorIndex) +
                                 " is not a scalar array");
    }

    std::vector<std::uint32_t> out(view.count);
    for (std::size_t element = 0; element < view.count; ++element) {
        const std::uint8_t* source = view.base + element * view.stride;
        switch (view.componentType) {
            case kComponentUnsignedByte: out[element] = static_cast<std::uint32_t>(*source); break;
            case kComponentUnsignedShort: out[element] = readU16(source); break;
            case kComponentUnsignedInt: out[element] = readU32(source); break;
            default:
                throw std::runtime_error("glTF index accessor must use an unsigned integer type");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

math::Mat4 quaternionToMat4(const float q[4]) {
    // glTF orders quaternions x, y, z, w. The result is the usual rotation
    // matrix written straight into column-major storage.
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];

    math::Mat4 result;
    result.m[0] = 1.0f - 2.0f * (y * y + z * z);
    result.m[1] = 2.0f * (x * y + z * w);
    result.m[2] = 2.0f * (x * z - y * w);
    result.m[4] = 2.0f * (x * y - z * w);
    result.m[5] = 1.0f - 2.0f * (x * x + z * z);
    result.m[6] = 2.0f * (y * z + x * w);
    result.m[8] = 2.0f * (x * z + y * w);
    result.m[9] = 2.0f * (y * z - x * w);
    result.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return result;
}

math::Mat4 nodeTransform(const json::Value& node) {
    const json::Value& matrix = node.at("matrix");
    if (matrix.isArray() && matrix.size() == 16) {
        math::Mat4 result;
        for (std::size_t i = 0; i < 16; ++i) {
            result.m[i] = matrix.at(i).floatValue();
        }
        return result;
    }

    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};

    node.at("translation").numbers(translation, 3);
    node.at("rotation").numbers(rotation, 4);
    node.at("scale").numbers(scale, 3);

    math::Mat4 scaleMatrix{};
    scaleMatrix.m[0] = scale[0];
    scaleMatrix.m[5] = scale[1];
    scaleMatrix.m[10] = scale[2];

    // Column vectors mean the rightmost factor is applied first: scale, then
    // rotate, then translate.
    return math::multiply(
        math::translation(math::Vec3{translation[0], translation[1], translation[2]}),
        math::multiply(quaternionToMat4(rotation), scaleMatrix));
}

math::Vec3 transformPoint(const math::Mat4& m, const math::Vec3& p) {
    return math::Vec3{m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
                      m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
                      m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

/// The inverse transpose of the rotation/scale part of `m`, written into `out`
/// as a row-major 3x3.
///
/// Normals cannot simply be rotated by the model matrix: a non-uniform scale
/// would shear them away from the surface. Going through the inverse transpose
/// fixes that, and the 3x3 inverse is spelled out through the adjugate so no
/// general matrix inverse is needed. Returns false for a singular transform, in
/// which case the caller falls back to the matrix itself.
bool normalMatrix(const math::Mat4& m, float out[9]) {
    const float a = m.m[0], d = m.m[4], g = m.m[8];
    const float b = m.m[1], e = m.m[5], h = m.m[9];
    const float c = m.m[2], f = m.m[6], i = m.m[10];

    const float determinant = a * (e * i - f * h) - d * (b * i - c * h) + g * (b * f - c * e);
    if (determinant > -1e-12f && determinant < 1e-12f) {
        return false;
    }

    const float inverseDeterminant = 1.0f / determinant;

    // Cofactor matrix divided by the determinant is the inverse transpose.
    out[0] = (e * i - f * h) * inverseDeterminant;
    out[1] = (c * h - b * i) * inverseDeterminant;
    out[2] = (b * f - c * e) * inverseDeterminant;
    out[3] = (f * g - d * i) * inverseDeterminant;
    out[4] = (a * i - c * g) * inverseDeterminant;
    out[5] = (c * d - a * f) * inverseDeterminant;
    out[6] = (d * h - e * g) * inverseDeterminant;
    out[7] = (b * g - a * h) * inverseDeterminant;
    out[8] = (a * e - b * d) * inverseDeterminant;
    return true;
}

/// Fills in normals for a primitive that does not carry any, by accumulating
/// each face's normal into its three corners. Coplanar faces then agree on the
/// normal (a cube stays faceted) while curved ones average smoothly.
void computeNormals(std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices,
                    std::size_t firstIndex, std::uint32_t baseVertex) {
    std::vector<math::Vec3> accumulated(vertices.size(), math::Vec3{});

    for (std::size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
        const std::uint32_t i0 = indices[i];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];

        const auto toVec3 = [&vertices](std::uint32_t index) {
            return math::Vec3{vertices[index].position[0], vertices[index].position[1],
                              vertices[index].position[2]};
        };

        const math::Vec3 normal =
            math::cross(toVec3(i1) - toVec3(i0), toVec3(i2) - toVec3(i0));

        accumulated[i0] = accumulated[i0] + normal;
        accumulated[i1] = accumulated[i1] + normal;
        accumulated[i2] = accumulated[i2] + normal;
    }

    for (std::uint32_t index = baseVertex; index < vertices.size(); ++index) {
        const math::Vec3 normal = math::normalize(accumulated[index]);
        vertices[index].normal[0] = normal.x;
        vertices[index].normal[1] = normal.y;
        vertices[index].normal[2] = normal.z;
    }
}

/// The renderer's world convention: +Z up, -X forward.
///
/// glTF specifies +Y up and says the front of an asset faces +Z, so every
/// imported file is rotated on the way in. The mapping is
/// `world = (-gltf.z, -gltf.x, gltf.y)`:
///
///   glTF +Y (up)                 -> world +Z (up)
///   glTF +Z (front of the asset) -> world -X (forward)
///   glTF +X                      -> world -Y
///
/// The third line is a consequence of the first two: the world axes are
/// right-handed, so glTF's +X, +Y, +Z have to map onto a right-handed triple,
/// and there is only one that keeps it that way. It is a rotation, not a
/// reflection (the determinant is +1), so face winding survives unchanged.
///
/// This is prepended to the scene graph walk as the root transform, so it flows
/// through the same matrix that carries the node transforms - which means the
/// normal matrix and the bounds pick it up too, with nothing extra to keep in
/// sync.
math::Mat4 worldConventionTransform() {
    // Column-major, and the identity underneath: only the 3x3 part changes.
    math::Mat4 result;

    // Column 0: where glTF's +X ends up.
    result.m[0] = 0.0f;
    result.m[1] = -1.0f;

    // Column 1: where glTF's +Y (up) ends up.
    result.m[4] = 0.0f;
    result.m[5] = 0.0f;
    result.m[6] = 1.0f;

    // Column 2: where glTF's +Z (the front of an asset) ends up.
    result.m[8] = -1.0f;
    result.m[9] = 0.0f;
    result.m[10] = 0.0f;

    return result;
}

/// Reads the shading parameters of one glTF material. Everything the file does
/// not specify keeps the glTF default, which is why the struct is already
/// filled in before any of it is read.
PbrMaterial readMaterial(const json::Value& material) {
    PbrMaterial result;

    if (!material.isObject()) {
        return result;
    }

    if (!material.at("name").string().empty()) {
        result.name = material.at("name").string();
    }

    // Only the four scalar factors are read: a base color *texture* would need
    // image decoding and a sampler, which this renderer does not have yet.
    const json::Value& pbr = material.at("pbrMetallicRoughness");
    pbr.at("baseColorFactor").numbers(result.baseColorFactor, 4);
    result.metallicFactor = pbr.at("metallicFactor").floatValue(1.0f);
    result.roughnessFactor = pbr.at("roughnessFactor").floatValue(1.0f);
    material.at("emissiveFactor").numbers(result.emissiveFactor, 3);
    result.doubleSided = material.at("doubleSided").boolean(false);

    return result;
}

// ---------------------------------------------------------------------------
// Scene graph walk
// ---------------------------------------------------------------------------

/// Flattens a glTF scene into one vertex and index buffer.
///
/// glTF describes a tree of nodes that may share meshes, and each mesh may hold
/// several primitives, each with its own material. The renderer draws a single
/// mesh with a single material, so this walk concatenates every primitive it
/// finds - baking each node's transform into the positions - and keeps the
/// material of the first primitive that names one.
struct SceneBuilder {
    const json::Value& gltf;
    const GlbContainer& container;
    MeshData data;
    bool materialAssigned = false;

    void appendNode(int nodeIndex, const math::Mat4& parentTransform, int depth) {
        if (depth > kMaxNodeDepth) {
            throw std::runtime_error("The glTF scene graph is nested too deeply (or contains a cycle)");
        }

        const json::Value& node = gltf.at("nodes").at(static_cast<std::size_t>(nodeIndex));
        if (!node.isObject()) {
            return;
        }

        const math::Mat4 transform = math::multiply(parentTransform, nodeTransform(node));

        const int meshIndex = node.at("mesh").intValue(-1);
        if (meshIndex >= 0) {
            const json::Value& mesh = gltf.at("meshes").at(static_cast<std::size_t>(meshIndex));
            if (data.name.empty()) {
                data.name = mesh.at("name").string();
            }

            const json::Value& primitives = mesh.at("primitives");
            for (std::size_t i = 0; i < primitives.size(); ++i) {
                appendPrimitive(primitives.at(i), transform);
            }
        }

        const json::Value& children = node.at("children");
        for (std::size_t i = 0; i < children.size(); ++i) {
            appendNode(children.at(i).intValue(-1), transform, depth + 1);
        }
    }

    void appendPrimitive(const json::Value& primitive, const math::Mat4& transform) {
        if (!primitive.isObject()) {
            return;
        }

        const int mode = primitive.at("mode").intValue(kModeTriangles);
        if (mode != kModeTriangles) {
            throw std::runtime_error("Only triangle meshes are supported (glTF primitive mode " +
                                     std::to_string(mode) + ")");
        }

        const json::Value& attributes = primitive.at("attributes");
        const int positionAccessor = attributes.at("POSITION").intValue(-1);
        if (positionAccessor < 0) {
            throw std::runtime_error("A glTF primitive has no POSITION attribute");
        }

        const std::vector<float> positions = readAccessorFloats(gltf, container, positionAccessor, 3);
        const std::size_t vertexCount = positions.size() / 3;

        const int normalAccessor = attributes.at("NORMAL").intValue(-1);
        const int uvAccessor = attributes.at("TEXCOORD_0").intValue(-1);

        std::vector<float> normals;
        if (normalAccessor >= 0) {
            normals = readAccessorFloats(gltf, container, normalAccessor, 3);
        }
        std::vector<float> uvs;
        if (uvAccessor >= 0) {
            uvs = readAccessorFloats(gltf, container, uvAccessor, 2);
        }

        float normalTransform[9];
        const bool hasNormalTransform = normalMatrix(transform, normalTransform);

        const std::uint32_t baseVertex = static_cast<std::uint32_t>(data.vertices.size());
        data.vertices.reserve(data.vertices.size() + vertexCount);

        for (std::size_t v = 0; v < vertexCount; ++v) {
            const math::Vec3 position{positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
            const math::Vec3 transformed = transformPoint(transform, position);

            Vertex vertex{};
            vertex.position[0] = transformed.x;
            vertex.position[1] = transformed.y;
            vertex.position[2] = transformed.z;

            if (normals.size() == vertexCount * 3) {
                const math::Vec3 normal{normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]};
                const math::Vec3 rotated =
                    hasNormalTransform
                        ? math::Vec3{normalTransform[0] * normal.x + normalTransform[1] * normal.y + normalTransform[2] * normal.z,
                                     normalTransform[3] * normal.x + normalTransform[4] * normal.y + normalTransform[5] * normal.z,
                                     normalTransform[6] * normal.x + normalTransform[7] * normal.y + normalTransform[8] * normal.z}
                        : normal;
                const math::Vec3 unit = math::normalize(rotated);
                vertex.normal[0] = unit.x;
                vertex.normal[1] = unit.y;
                vertex.normal[2] = unit.z;
            }

            if (uvs.size() == vertexCount * 2) {
                vertex.uv[0] = uvs[v * 2];
                // glTF puts the texture origin at the top-left, Vulkan at the
                // bottom-left, so the V axis is flipped here rather than in the
                // shader.
                vertex.uv[1] = 1.0f - uvs[v * 2 + 1];
            }

            data.vertices.push_back(vertex);
        }

        const std::size_t firstIndex = data.indices.size();
        const int indexAccessor = primitive.at("indices").intValue(-1);
        if (indexAccessor >= 0) {
            const std::vector<std::uint32_t> indices =
                readAccessorIndices(gltf, container, indexAccessor);
            data.indices.reserve(data.indices.size() + indices.size());
            for (const std::uint32_t index : indices) {
                data.indices.push_back(baseVertex + index);
            }
        } else {
            // A primitive without indices is a plain sequence of triangles, one
            // vertex each.
            data.indices.reserve(data.indices.size() + vertexCount);
            for (std::uint32_t v = 0; v < vertexCount; ++v) {
                data.indices.push_back(baseVertex + v);
            }
        }

        if (normals.empty()) {
            computeNormals(data.vertices, data.indices, firstIndex, baseVertex);
        }

        const int materialIndex = primitive.at("material").intValue(-1);
        if (materialIndex >= 0 && !materialAssigned) {
            data.material = readMaterial(gltf.at("materials").at(static_cast<std::size_t>(materialIndex)));
            materialAssigned = true;
        }
    }

    /// Bounds are computed after the walk because a node transform can move any
    /// primitive anywhere.
    void computeBounds() {
        if (data.vertices.empty()) {
            return;
        }

        for (int axis = 0; axis < 3; ++axis) {
            data.boundsMin[axis] = data.vertices[0].position[axis];
            data.boundsMax[axis] = data.vertices[0].position[axis];
        }

        for (const Vertex& vertex : data.vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                data.boundsMin[axis] = std::min(data.boundsMin[axis], vertex.position[axis]);
                data.boundsMax[axis] = std::max(data.boundsMax[axis], vertex.position[axis]);
            }
        }
    }
};

}  // namespace

MeshData loadGlb(const std::string& path) {
    const std::vector<std::uint8_t> bytes = readFileBytes(path);
    const GlbContainer container = parseGlbContainer(bytes, path);
    const json::Value gltf = json::parse(container.json);

    const json::Value& assetVersion = gltf.at("asset").at("version");
    if (assetVersion.isString() && assetVersion.string() != "2.0") {
        throw std::runtime_error("'" + path + "' declares glTF version " + assetVersion.string() +
                                 "; only 2.0 is supported");
    }

    const json::Value& scenes = gltf.at("scenes");
    const json::Value& scene = scenes.at(static_cast<std::size_t>(gltf.at("scene").intValue(0)));
    if (!scene.isObject()) {
        throw std::runtime_error("'" + path + "' has no scene to load");
    }

    SceneBuilder builder{gltf, container, MeshData{}, false};

    // The file's +Y up / +Z front is rotated into the renderer's +Z up / -X
    // forward here, as the root of the scene graph walk.
    const math::Mat4 rootTransform = worldConventionTransform();

    const json::Value& roots = scene.at("nodes");
    for (std::size_t i = 0; i < roots.size(); ++i) {
        builder.appendNode(roots.at(i).intValue(-1), rootTransform, 0);
    }
    builder.computeBounds();

    if (builder.data.vertices.empty() || builder.data.indices.empty()) {
        throw std::runtime_error("'" + path + "' contains no triangle geometry");
    }
    if (builder.data.name.empty()) {
        builder.data.name = fileStem(path);
    }
    return std::move(builder.data);
}

MeshData loadMesh(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string() : toLower(path.substr(dot));

    if (extension == ".glb") {
        return loadGlb(path);
    }
    if (extension == ".gltf") {
        throw std::runtime_error("Text .gltf files are not supported; export the mesh as a binary .glb");
    }
    throw std::runtime_error("Unsupported mesh format '" + extension + "' (supported: .glb)");
}

MeshData makeCubeMesh() {
    // Written directly in the renderer's convention (+Z up, -X forward) rather
    // than in glTF's: this is the one mesh that never goes through a loader, and
    // it is symmetric anyway, so converting it would be a comment pretending to
    // be code.
    //
    // Each face is described by its outward normal plus the two in-plane axes,
    // ordered so that normal = u x v. Corners are then visited as
    // (-u,-v), (+u,-v), (+u,+v), (-u,+v), which keeps the winding
    // counter-clockwise as seen from outside the cube.
    struct Face {
        float normal[3];
        float u[3];
        float v[3];
    };

    const Face faces[6] = {
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        // The last two are the top and the bottom, since Z is up.
        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    };

    const float cornerSigns[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    const float cornerUv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    constexpr float kHalfExtent = 0.5f;

    MeshData data;
    data.name = "built-in cube";
    data.material.name = "fallback cube";
    data.material.baseColorFactor[0] = 0.80f;
    data.material.baseColorFactor[1] = 0.80f;
    data.material.baseColorFactor[2] = 0.83f;
    data.material.metallicFactor = 0.0f;
    data.material.roughnessFactor = 0.45f;
    data.material.doubleSided = true;

    for (const Face& face : faces) {
        const std::uint32_t base = static_cast<std::uint32_t>(data.vertices.size());

        for (int corner = 0; corner < 4; ++corner) {
            Vertex vertex{};
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = (face.normal[axis] +
                                         face.u[axis] * cornerSigns[corner][0] +
                                         face.v[axis] * cornerSigns[corner][1]) *
                                        kHalfExtent;
                vertex.normal[axis] = face.normal[axis];
            }
            vertex.uv[0] = cornerUv[corner][0];
            vertex.uv[1] = cornerUv[corner][1];
            data.vertices.push_back(vertex);
        }

        data.indices.push_back(base + 0);
        data.indices.push_back(base + 1);
        data.indices.push_back(base + 2);
        data.indices.push_back(base + 0);
        data.indices.push_back(base + 2);
        data.indices.push_back(base + 3);
    }

    for (int axis = 0; axis < 3; ++axis) {
        data.boundsMin[axis] = -kHalfExtent;
        data.boundsMax[axis] = kHalfExtent;
    }
    return data;
}
