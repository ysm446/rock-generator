#include "node_graph.h"

#include "sdf_preview.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rock
{
namespace
{
int EffectiveMeshResolution(const OutputMeshSettings& settings)
{
    const int divisor = 1 << std::clamp(settings.lod, 0, 4);
    return std::clamp(settings.resolution / divisor, 16, 96);
}

struct QuantizedVertex
{
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const QuantizedVertex& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct QuantizedVertexHash
{
    size_t operator()(const QuantizedVertex& value) const
    {
        size_t h = static_cast<size_t>(value.x) * 73856093u;
        h ^= static_cast<size_t>(value.y) * 19349663u;
        h ^= static_cast<size_t>(value.z) * 83492791u;
        return h;
    }
};

uint64_t EdgeKey(uint32_t a, uint32_t b)
{
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

void AddEdge(MeshData& mesh, std::unordered_set<uint64_t>& edgeKeys, uint32_t a, uint32_t b)
{
    const uint64_t key = EdgeKey(a, b);
    if (edgeKeys.insert(key).second)
    {
        mesh.edges.push_back({std::min(a, b), std::max(a, b)});
    }
}

void AccumulateNormal(MeshVertex& vertex, float nx, float ny, float nz)
{
    vertex.nx += nx;
    vertex.ny += ny;
    vertex.nz += nz;
}

void ApplySdfGradientNormals(const GraphSettings& settings, const SdfPipeline& pipeline, const SdfPreviewStats& sdf, MeshData& mesh)
{
    const float e = std::max(sdf.voxelSize * 0.35f, 0.001f);
    for (MeshVertex& vertex : mesh.vertices)
    {
        const float dx = EvaluateSdfAt(settings, pipeline, vertex.x + e, vertex.y, vertex.z) -
            EvaluateSdfAt(settings, pipeline, vertex.x - e, vertex.y, vertex.z);
        const float dy = EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y + e, vertex.z) -
            EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y - e, vertex.z);
        const float dz = EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y, vertex.z + e) -
            EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y, vertex.z - e);
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length > 0.000001f)
        {
            vertex.nx = dx / length;
            vertex.ny = dy / length;
            vertex.nz = dz / length;
        }
    }
}

MeshData BuildMeshFromSdf(const GraphSettings& settings, const SdfPipeline& pipeline, const SdfPreviewStats& sdf)
{
    MeshData mesh;
    mesh.vertices.reserve(sdf.surfaceTriangles.size());
    mesh.triangles.reserve(sdf.surfaceTriangles.size());
    mesh.edges.reserve(sdf.surfaceTriangles.size() * 3);

    std::unordered_map<QuantizedVertex, uint32_t, QuantizedVertexHash> vertexMap;
    std::unordered_set<uint64_t> edgeKeys;
    constexpr float kQuantizeScale = 10000.0f;

    const auto vertexIndex = [&](float x, float y, float z) -> uint32_t {
        const QuantizedVertex key{
            static_cast<int>(std::lround(x * kQuantizeScale)),
            static_cast<int>(std::lround(y * kQuantizeScale)),
            static_cast<int>(std::lround(z * kQuantizeScale)),
        };
        if (const auto it = vertexMap.find(key); it != vertexMap.end())
        {
            return it->second;
        }

        const uint32_t index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({x, y, z, 0.0f, 0.0f, 0.0f});
        vertexMap.emplace(key, index);
        return index;
    };

    for (const SurfaceTriangle& triangle : sdf.surfaceTriangles)
    {
        const uint32_t a = vertexIndex(triangle.ax, triangle.ay, triangle.az);
        const uint32_t b = vertexIndex(triangle.bx, triangle.by, triangle.bz);
        const uint32_t c = vertexIndex(triangle.cx, triangle.cy, triangle.cz);
        if (a == b || b == c || c == a)
        {
            continue;
        }

        const float ux = triangle.bx - triangle.ax;
        const float uy = triangle.by - triangle.ay;
        const float uz = triangle.bz - triangle.az;
        const float vx = triangle.cx - triangle.ax;
        const float vy = triangle.cy - triangle.ay;
        const float vz = triangle.cz - triangle.az;
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;

        AccumulateNormal(mesh.vertices[a], nx, ny, nz);
        AccumulateNormal(mesh.vertices[b], nx, ny, nz);
        AccumulateNormal(mesh.vertices[c], nx, ny, nz);
        mesh.triangles.push_back({a, b, c});
        AddEdge(mesh, edgeKeys, a, b);
        AddEdge(mesh, edgeKeys, b, c);
        AddEdge(mesh, edgeKeys, c, a);
    }

    for (MeshVertex& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
        if (length > 0.000001f)
        {
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
        else
        {
            vertex.nx = 0.0f;
            vertex.ny = 1.0f;
            vertex.nz = 0.0f;
        }
    }

    ApplySdfGradientNormals(settings, pipeline, sdf, mesh);

    return mesh;
}
} // namespace

NodeGraph NodeGraph::CreateDefaultRockGraph()
{
    NodeGraph graph;

    const GraphId primitive = graph.AddNode(NodeKind::PrimitiveSdf, "Primitive SDF");
    const GraphId noise = graph.AddNode(NodeKind::NoiseWarp, "Noise Warp");
    const GraphId crack = graph.AddNode(NodeKind::CrackField, "Crack Field");
    const GraphId output = graph.AddNode(NodeKind::OutputMesh, "Output Mesh");

    const GraphId primitiveOut = graph.AddPin(primitive, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
    const GraphId noiseIn = graph.AddPin(noise, PinKind::Input, ValueType::SdfGrid, "SDFGrid");
    const GraphId noiseOut = graph.AddPin(noise, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
    const GraphId crackIn = graph.AddPin(crack, PinKind::Input, ValueType::SdfGrid, "SDFGrid");
    const GraphId crackOut = graph.AddPin(crack, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
    const GraphId outputIn = graph.AddPin(output, PinKind::Input, ValueType::SdfGrid, "SDFGrid");

    graph.AddInitialLink(primitiveOut, noiseIn);
    graph.AddInitialLink(noiseOut, crackIn);
    graph.AddInitialLink(crackOut, outputIn);
    graph.Evaluate();

    return graph;
}

const std::vector<Node>& NodeGraph::Nodes() const
{
    return nodes_;
}

const std::vector<Link>& NodeGraph::Links() const
{
    return links_;
}

GraphSettings& NodeGraph::Settings()
{
    return settings_;
}

const GraphSettings& NodeGraph::Settings() const
{
    return settings_;
}

const EvaluationSummary& NodeGraph::Evaluation() const
{
    return evaluation_;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        const auto input = std::ranges::find_if(node.inputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (input != node.inputs.end())
        {
            return &*input;
        }

        const auto output = std::ranges::find_if(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (output != node.outputs.end())
        {
            return &*output;
        }
    }

    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) { return node.id == nodeId; });
    return it == nodes_.end() ? nullptr : &*it;
}

bool NodeGraph::IsInputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Input;
}

bool NodeGraph::IsOutputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Output;
}

bool NodeGraph::PinHasLink(GraphId pinId) const
{
    return std::ranges::any_of(links_, [pinId](const Link& link) {
        return link.startPin == pinId || link.endPin == pinId;
    });
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const
{
    if (startPin == 0 || endPin == 0 || startPin == endPin)
    {
        return false;
    }

    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId || start->valueType != end->valueType)
    {
        return false;
    }

    return (start->kind == PinKind::Output && end->kind == PinKind::Input) ||
           (start->kind == PinKind::Input && end->kind == PinKind::Output);
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin)
{
    if (!CanCreateLink(startPin, endPin))
    {
        return false;
    }

    if (IsInputPin(startPin))
    {
        std::swap(startPin, endPin);
    }

    std::erase_if(links_, [endPin](const Link& link) {
        return link.endPin == endPin;
    });
    links_.push_back({nextLinkId_++, startPin, endPin});
    MarkDirty("Link changed");
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId)
{
    const auto oldSize = links_.size();
    std::erase_if(links_, [linkId](const Link& link) { return link.id == linkId; });
    if (links_.size() == oldSize)
    {
        return false;
    }

    MarkDirty("Link deleted");
    return true;
}

void NodeGraph::ReplaceLinks(std::vector<Link> links)
{
    links_ = std::move(links);
    nextLinkId_ = 101;
    for (const Link& link : links_)
    {
        nextLinkId_ = std::max(nextLinkId_, link.id + 1);
    }
    MarkDirty("Project links loaded");
}

bool NodeGraph::SetPreviewStage(PreviewStage stage)
{
    if (evaluation_.previewStage == stage)
    {
        return false;
    }

    evaluation_.previewStage = stage;
    MarkDirty(std::format("Preview stage changed to {}", ToString(stage)));
    return true;
}

PreviewStage NodeGraph::Preview() const
{
    return evaluation_.previewStage;
}

SdfPipeline NodeGraph::PipelineFor(PreviewStage stage) const
{
    switch (stage)
    {
    case PreviewStage::Primitive:
        return PipelineTo(NodeKind::PrimitiveSdf);
    case PreviewStage::Noise:
        return PipelineTo(NodeKind::NoiseWarp);
    case PreviewStage::Crack:
        return PipelineTo(NodeKind::CrackField);
    case PreviewStage::Output:
    default:
        return PipelineTo(NodeKind::OutputMesh);
    }
}

SdfPipeline NodeGraph::PreviewPipeline() const
{
    return PipelineFor(evaluation_.previewStage);
}

SdfPipeline NodeGraph::FinalPipeline() const
{
    return PipelineFor(PreviewStage::Output);
}

void NodeGraph::MarkDirty(std::string_view reason)
{
    evaluation_.dirty = true;
    evaluation_.status = std::string(reason);
}

const Node* NodeGraph::FindFirstNode(NodeKind kind) const
{
    const auto it = std::ranges::find_if(nodes_, [kind](const Node& node) {
        return node.kind == kind;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

const Node* NodeGraph::FindNodeByOutputPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        if (std::ranges::any_of(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; }))
        {
            return &node;
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindUpstreamNode(const Node& node) const
{
    if (node.inputs.empty())
    {
        return nullptr;
    }

    const GraphId inputPin = node.inputs.front().id;
    const auto linkIt = std::find_if(links_.rbegin(), links_.rend(), [inputPin](const Link& link) {
        return link.endPin == inputPin;
    });
    if (linkIt == links_.rend())
    {
        return nullptr;
    }

    return FindNodeByOutputPin(linkIt->startPin);
}

SdfPipeline NodeGraph::PipelineTo(NodeKind targetKind) const
{
    SdfPipeline pipeline;
    const Node* node = FindFirstNode(targetKind);
    int guard = 0;
    while (node != nullptr && guard++ < 16)
    {
        if (node->kind == NodeKind::NoiseWarp)
        {
            pipeline.useNoise = true;
        }
        else if (node->kind == NodeKind::CrackField)
        {
            pipeline.useCrack = true;
        }
        else if (node->kind == NodeKind::OutputMesh)
        {
            pipeline.applyOutputIso = true;
        }
        else if (node->kind == NodeKind::PrimitiveSdf)
        {
            break;
        }

        node = FindUpstreamNode(*node);
    }
    return pipeline;
}

void NodeGraph::Evaluate()
{
    const int meshResolution = EffectiveMeshResolution(settings_.outputMesh);
    const SdfPipeline previewPipeline = PreviewPipeline();
    const SdfPipeline finalPipeline = FinalPipeline();
    evaluation_.requestedPreviewBackend = settings_.previewBackend;
    evaluation_.effectivePreviewBackend = ComputeBackend::Cpu;
    evaluation_.previewBackendFallback = settings_.previewBackend != ComputeBackend::Cpu;
    evaluation_.previewSdf = BuildDenseSdfPreview(settings_, previewPipeline, meshResolution);
    evaluation_.finalSdf = BuildDenseSdfPreview(settings_, finalPipeline, meshResolution);
    evaluation_.previewMesh = BuildMeshFromSdf(settings_, previewPipeline, evaluation_.previewSdf);
    evaluation_.finalMesh = BuildMeshFromSdf(settings_, finalPipeline, evaluation_.finalSdf);
    ++evaluation_.version;
    evaluation_.dirty = false;
    evaluation_.status = std::format(
        "{} preview [{}{}] -> {}{}{} -> mesh LOD {} / iso {:.3f} -> {} verts / {} tris",
        ToString(evaluation_.previewStage),
        ToString(evaluation_.effectivePreviewBackend),
        evaluation_.previewBackendFallback ? " fallback" : "",
        ToString(settings_.primitive.kind),
        finalPipeline.useNoise ? std::format(" -> noise {:.2f}/{:.2f}/{}", settings_.noise.amplitude, settings_.noise.frequency, settings_.noise.octaves) : "",
        finalPipeline.useCrack ? std::format(" -> crack {:.3f}/{:.2f}/{:.2f}", settings_.crack.width, settings_.crack.depth, settings_.crack.roughness) : "",
        settings_.outputMesh.lod,
        settings_.outputMesh.isoValue,
        evaluation_.previewMesh.vertices.size(),
        evaluation_.previewMesh.triangles.size());
}

void NodeGraph::EvaluateWithPreview(SdfPreviewStats previewSdf, ComputeBackend requestedBackend, ComputeBackend effectiveBackend, bool fallback)
{
    const int meshResolution = EffectiveMeshResolution(settings_.outputMesh);
    const SdfPipeline previewPipeline = PreviewPipeline();
    const SdfPipeline finalPipeline = FinalPipeline();
    evaluation_.requestedPreviewBackend = requestedBackend;
    evaluation_.effectivePreviewBackend = effectiveBackend;
    evaluation_.previewBackendFallback = fallback;
    evaluation_.previewSdf = std::move(previewSdf);
    evaluation_.finalSdf = BuildDenseSdfPreview(settings_, finalPipeline, meshResolution);
    evaluation_.previewMesh = BuildMeshFromSdf(settings_, previewPipeline, evaluation_.previewSdf);
    evaluation_.finalMesh = BuildMeshFromSdf(settings_, finalPipeline, evaluation_.finalSdf);
    ++evaluation_.version;
    evaluation_.dirty = false;
    evaluation_.status = std::format(
        "{} preview [{}{}] -> {}{}{} -> mesh LOD {} / iso {:.3f} -> {} verts / {} tris",
        ToString(evaluation_.previewStage),
        ToString(evaluation_.effectivePreviewBackend),
        evaluation_.previewBackendFallback ? " fallback" : "",
        ToString(settings_.primitive.kind),
        finalPipeline.useNoise ? std::format(" -> noise {:.2f}/{:.2f}/{}", settings_.noise.amplitude, settings_.noise.frequency, settings_.noise.octaves) : "",
        finalPipeline.useCrack ? std::format(" -> crack {:.3f}/{:.2f}/{:.2f}", settings_.crack.width, settings_.crack.depth, settings_.crack.roughness) : "",
        settings_.outputMesh.lod,
        settings_.outputMesh.isoValue,
        evaluation_.previewMesh.vertices.size(),
        evaluation_.previewMesh.triangles.size());
}

GraphId NodeGraph::AddNode(NodeKind kind, std::string title)
{
    const GraphId id = nextNodeId_++;
    nodes_.push_back({id, kind, std::move(title), {}, {}});
    return id;
}

GraphId NodeGraph::AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label)
{
    Node* node = nullptr;
    for (Node& candidate : nodes_)
    {
        if (candidate.id == nodeId)
        {
            node = &candidate;
            break;
        }
    }

    if (node == nullptr)
    {
        return 0;
    }

    const GraphId id = nextPinId_++;
    Pin pin{id, nodeId, kind, valueType, std::move(label)};
    if (kind == PinKind::Input)
    {
        node->inputs.push_back(std::move(pin));
    }
    else
    {
        node->outputs.push_back(std::move(pin));
    }
    return id;
}

void NodeGraph::AddInitialLink(GraphId startPin, GraphId endPin)
{
    links_.push_back({nextLinkId_++, startPin, endPin});
}

std::string_view ToString(PrimitiveKind kind)
{
    switch (kind)
    {
    case PrimitiveKind::Sphere:
        return "Sphere";
    case PrimitiveKind::Box:
        return "Box";
    case PrimitiveKind::Capsule:
        return "Capsule";
    case PrimitiveKind::Ellipsoid:
        return "Ellipsoid";
    case PrimitiveKind::RockBlob:
        return "Rock Blob";
    default:
        return "Unknown";
    }
}

std::string_view ToString(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        return "Primitive SDF";
    case NodeKind::NoiseWarp:
        return "Noise Warp";
    case NodeKind::CrackField:
        return "Crack Field";
    case NodeKind::OutputMesh:
        return "Output Mesh";
    default:
        return "Unknown";
    }
}

std::string_view ToString(PreviewStage stage)
{
    switch (stage)
    {
    case PreviewStage::Primitive:
        return "Primitive";
    case PreviewStage::Noise:
        return "Noise Warp";
    case PreviewStage::Crack:
        return "Crack Field";
    case PreviewStage::Output:
        return "Output Mesh";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ValueType type)
{
    switch (type)
    {
    case ValueType::SdfGrid:
        return "SDFGrid";
    case ValueType::Mesh:
        return "Mesh";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ComputeBackend backend)
{
    switch (backend)
    {
    case ComputeBackend::Cpu:
        return "CPU";
    case ComputeBackend::GpuPreview:
        return "GPU Preview";
    case ComputeBackend::Auto:
        return "Auto";
    default:
        return "Unknown";
    }
}

std::string_view ToString(MeshDisplayMode mode)
{
    switch (mode)
    {
    case MeshDisplayMode::Mesh:
        return "Mesh";
    case MeshDisplayMode::Voxels:
        return "Voxels";
    default:
        return "Unknown";
    }
}

PreviewStage PreviewStageFor(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        return PreviewStage::Primitive;
    case NodeKind::NoiseWarp:
        return PreviewStage::Noise;
    case NodeKind::CrackField:
        return PreviewStage::Crack;
    case NodeKind::OutputMesh:
    default:
        return PreviewStage::Output;
    }
}

} // namespace rock
