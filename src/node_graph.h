#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rock
{
using GraphId = int;

enum class NodeKind
{
    PrimitiveSdf,
    NoiseWarp,
    CrackField,
    OutputMesh,
};

enum class PinKind
{
    Input,
    Output,
};

enum class ValueType
{
    SdfGrid,
    Mesh,
};

enum class PrimitiveKind
{
    Sphere,
    Box,
    Capsule,
    Ellipsoid,
    RockBlob,
};

enum class PreviewStage
{
    Primitive,
    Noise,
    Crack,
    Output,
};

struct Pin
{
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::SdfGrid;
    std::string label;
};

struct Node
{
    GraphId id = 0;
    NodeKind kind = NodeKind::PrimitiveSdf;
    std::string title;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
};

struct Link
{
    GraphId id = 0;
    GraphId startPin = 0;
    GraphId endPin = 0;
};

struct PrimitiveSettings
{
    PrimitiveKind kind = PrimitiveKind::RockBlob;
};

struct NoiseSettings
{
    float amplitude = 0.35f;
    float frequency = 2.0f;
    int octaves = 4;
};

struct CrackSettings
{
    float width = 0.035f;
    float depth = 0.42f;
    float roughness = 0.65f;
};

struct GraphSettings
{
    PrimitiveSettings primitive;
    NoiseSettings noise;
    CrackSettings crack;
};

struct SurfacePoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float sdf = 0.0f;
};

struct SurfaceSegment
{
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
};

struct SurfaceTriangle
{
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
};

struct SdfPreviewStats
{
    int resolution = 0;
    int sliceResolution = 0;
    int totalVoxels = 0;
    int insideVoxels = 0;
    float voxelSize = 0.0f;
    float minSdf = 0.0f;
    float maxSdf = 0.0f;
    float fillRatio = 0.0f;
    float estimatedVolume = 0.0f;
    std::vector<float> centerSlice;
    std::vector<SurfacePoint> surfacePoints;
    std::vector<SurfaceSegment> surfaceSegments;
    std::vector<SurfaceTriangle> surfaceTriangles;
};

struct EvaluationSummary
{
    uint64_t version = 0;
    bool dirty = true;
    std::string status = "Graph has not been evaluated";
    PreviewStage previewStage = PreviewStage::Output;
    SdfPreviewStats previewSdf;
    SdfPreviewStats finalSdf;
};

class NodeGraph
{
public:
    static NodeGraph CreateDefaultRockGraph();

    const std::vector<Node>& Nodes() const;
    const std::vector<Link>& Links() const;
    GraphSettings& Settings();
    const GraphSettings& Settings() const;
    const EvaluationSummary& Evaluation() const;

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    bool IsInputPin(GraphId pinId) const;
    bool IsOutputPin(GraphId pinId) const;
    bool PinHasLink(GraphId pinId) const;
    bool CanCreateLink(GraphId startPin, GraphId endPin) const;

    bool CreateLink(GraphId startPin, GraphId endPin);
    bool DeleteLink(GraphId linkId);
    bool SetPreviewStage(PreviewStage stage);
    PreviewStage Preview() const;
    void MarkDirty(std::string_view reason);
    void Evaluate();

private:
    GraphId AddNode(NodeKind kind, std::string title);
    GraphId AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label);
    void AddInitialLink(GraphId startPin, GraphId endPin);

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    GraphSettings settings_;
    EvaluationSummary evaluation_;
    GraphId nextNodeId_ = 1;
    GraphId nextPinId_ = 11;
    GraphId nextLinkId_ = 101;
};

std::string_view ToString(PrimitiveKind kind);
std::string_view ToString(NodeKind kind);
std::string_view ToString(PreviewStage stage);
std::string_view ToString(ValueType type);
PreviewStage PreviewStageFor(NodeKind kind);

} // namespace rock
