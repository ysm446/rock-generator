#include "node_graph.h"

#include "sdf_preview.h"

#include <algorithm>
#include <format>
#include <utility>

namespace rock
{
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

void NodeGraph::MarkDirty(std::string_view reason)
{
    evaluation_.dirty = true;
    evaluation_.status = std::string(reason);
}

void NodeGraph::Evaluate()
{
    evaluation_.requestedPreviewBackend = settings_.previewBackend;
    evaluation_.effectivePreviewBackend = ComputeBackend::Cpu;
    evaluation_.previewBackendFallback = settings_.previewBackend != ComputeBackend::Cpu;
    evaluation_.previewSdf = BuildDenseSdfPreview(settings_, 48, evaluation_.previewStage);
    evaluation_.finalSdf = BuildDenseSdfPreview(settings_, 48, PreviewStage::Output);
    ++evaluation_.version;
    evaluation_.dirty = false;
    evaluation_.status = std::format(
        "{} preview [{}{}] -> {} -> noise {:.2f}/{:.2f}/{} -> crack {:.3f}/{:.2f}/{:.2f} -> dense SDF {}^3",
        ToString(evaluation_.previewStage),
        ToString(evaluation_.effectivePreviewBackend),
        evaluation_.previewBackendFallback ? " fallback" : "",
        ToString(settings_.primitive.kind),
        settings_.noise.amplitude,
        settings_.noise.frequency,
        settings_.noise.octaves,
        settings_.crack.width,
        settings_.crack.depth,
        settings_.crack.roughness,
        evaluation_.previewSdf.resolution);
}

void NodeGraph::EvaluateWithPreview(SdfPreviewStats previewSdf, ComputeBackend requestedBackend, ComputeBackend effectiveBackend, bool fallback)
{
    evaluation_.requestedPreviewBackend = requestedBackend;
    evaluation_.effectivePreviewBackend = effectiveBackend;
    evaluation_.previewBackendFallback = fallback;
    evaluation_.previewSdf = std::move(previewSdf);
    evaluation_.finalSdf = BuildDenseSdfPreview(settings_, 48, PreviewStage::Output);
    ++evaluation_.version;
    evaluation_.dirty = false;
    evaluation_.status = std::format(
        "{} preview [{}{}] -> {} -> noise {:.2f}/{:.2f}/{} -> crack {:.3f}/{:.2f}/{:.2f} -> dense SDF {}^3",
        ToString(evaluation_.previewStage),
        ToString(evaluation_.effectivePreviewBackend),
        evaluation_.previewBackendFallback ? " fallback" : "",
        ToString(settings_.primitive.kind),
        settings_.noise.amplitude,
        settings_.noise.frequency,
        settings_.noise.octaves,
        settings_.crack.width,
        settings_.crack.depth,
        settings_.crack.roughness,
        evaluation_.previewSdf.resolution);
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
