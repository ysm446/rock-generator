#include "sdf_preview.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rock
{
namespace
{
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

float Length(Vec3 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float Dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Abs(Vec3 v)
{
    return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
}

Vec3 Sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Max(Vec3 v, float s)
{
    return {std::max(v.x, s), std::max(v.y, s), std::max(v.z, s)};
}

float MaxComponent(Vec3 v)
{
    return std::max(v.x, std::max(v.y, v.z));
}

Vec3 GridPoint(int x, int y, int z, float voxelSize)
{
    return {
        -1.0f + static_cast<float>(x) * voxelSize,
        -1.0f + static_cast<float>(y) * voxelSize,
        -1.0f + static_cast<float>(z) * voxelSize,
    };
}

size_t GridIndex(int x, int y, int z, int resolution)
{
    return static_cast<size_t>((z * resolution + y) * resolution + x);
}

float Hash(Vec3 p)
{
    const float n = std::sin(Dot(p, {12.9898f, 78.233f, 37.719f})) * 43758.5453f;
    return n - std::floor(n);
}

float ValueNoise(Vec3 p)
{
    Vec3 i{std::floor(p.x), std::floor(p.y), std::floor(p.z)};
    Vec3 f{p.x - i.x, p.y - i.y, p.z - i.z};
    f = {
        f.x * f.x * (3.0f - 2.0f * f.x),
        f.y * f.y * (3.0f - 2.0f * f.y),
        f.z * f.z * (3.0f - 2.0f * f.z),
    };

    auto sample = [&](float x, float y, float z) {
        return Hash({i.x + x, i.y + y, i.z + z});
    };

    const float x00 = std::lerp(sample(0, 0, 0), sample(1, 0, 0), f.x);
    const float x10 = std::lerp(sample(0, 1, 0), sample(1, 1, 0), f.x);
    const float x01 = std::lerp(sample(0, 0, 1), sample(1, 0, 1), f.x);
    const float x11 = std::lerp(sample(0, 1, 1), sample(1, 1, 1), f.x);
    const float y0 = std::lerp(x00, x10, f.y);
    const float y1 = std::lerp(x01, x11, f.y);
    return std::lerp(y0, y1, f.z) * 2.0f - 1.0f;
}

float Fbm(Vec3 p, int octaves)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float total = 0.0f;
    for (int i = 0; i < std::clamp(octaves, 1, 8); ++i)
    {
        value += ValueNoise({p.x * frequency, p.y * frequency, p.z * frequency}) * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return total > 0.0f ? value / total : value;
}

float BoxSdf(Vec3 p, Vec3 halfExtents)
{
    Vec3 q = Sub(Abs(p), halfExtents);
    return Length(Max(q, 0.0f)) + std::min(MaxComponent(q), 0.0f);
}

float CapsuleSdf(Vec3 p)
{
    const float halfHeight = 0.42f;
    p.y -= std::clamp(p.y, -halfHeight, halfHeight);
    return Length(p) - 0.38f;
}

float PrimitiveSdf(Vec3 p, PrimitiveKind kind)
{
    switch (kind)
    {
    case PrimitiveKind::Sphere:
        return Length(p) - 0.62f;
    case PrimitiveKind::Box:
        return BoxSdf(p, {0.48f, 0.42f, 0.55f});
    case PrimitiveKind::Capsule:
        return CapsuleSdf(p);
    case PrimitiveKind::Ellipsoid:
        return (Length({p.x / 0.72f, p.y / 0.48f, p.z / 0.56f}) - 1.0f) * 0.56f;
    case PrimitiveKind::RockBlob:
    default:
        return Length({p.x / 0.70f, p.y / 0.55f, p.z / 0.62f}) * 0.58f - 0.58f;
    }
}

float ApplyNoise(float sdf, Vec3 p, const NoiseSettings& noise)
{
    const float n = Fbm({p.x * noise.frequency, p.y * noise.frequency, p.z * noise.frequency}, noise.octaves);
    return sdf + n * noise.amplitude * 0.12f;
}

float ApplyCracks(float sdf, Vec3 p, const CrackSettings& crack)
{
    const Vec3 normals[] = {
        {0.92f, 0.18f, 0.34f},
        {-0.25f, 0.96f, 0.11f},
        {0.16f, -0.38f, 0.91f},
    };
    const float offsets[] = {-0.17f, 0.11f, 0.29f};

    float crackSdf = -1.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float rough = Fbm({p.x * 7.0f + i * 13.0f, p.y * 7.0f, p.z * 7.0f}, 3) * crack.roughness * 0.045f;
        const float plane = crack.width - std::fabs(Dot(p, normals[i]) + offsets[i] + rough);
        crackSdf = std::max(crackSdf, plane * (0.6f + crack.depth));
    }

    return std::max(sdf, crackSdf);
}

float EvaluateStageSdf(Vec3 p, const GraphSettings& settings, PreviewStage stage)
{
    float sdf = PrimitiveSdf(p, settings.primitive.kind);
    if (stage == PreviewStage::Primitive)
    {
        return sdf;
    }

    sdf = ApplyNoise(sdf, p, settings.noise);
    if (stage == PreviewStage::Noise)
    {
        return sdf;
    }

    return ApplyCracks(sdf, p, settings.crack);
}
} // namespace

SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, int resolution, PreviewStage stage)
{
    SdfPreviewStats stats;
    stats.resolution = std::max(8, resolution);
    stats.sliceResolution = stats.resolution;
    stats.totalVoxels = stats.resolution * stats.resolution * stats.resolution;
    stats.voxelSize = 2.0f / static_cast<float>(stats.resolution - 1);
    stats.minSdf = std::numeric_limits<float>::max();
    stats.maxSdf = std::numeric_limits<float>::lowest();
    stats.centerSlice.assign(static_cast<size_t>(stats.sliceResolution * stats.sliceResolution), 0.0f);
    stats.surfacePoints.reserve(2200);
    stats.surfaceSegments.reserve(4200);
    stats.surfaceTriangles.reserve(3600);
    std::vector<float> sdfValues(static_cast<size_t>(stats.totalVoxels), 0.0f);
    const int sliceZ = stats.resolution / 2;
    const float surfaceBand = stats.voxelSize * 0.82f;
    constexpr size_t kMaxSurfacePoints = 2200;
    constexpr size_t kMaxSurfaceSegments = 4200;
    constexpr size_t kMaxSurfaceTriangles = 3600;

    for (int z = 0; z < stats.resolution; ++z)
    {
        for (int y = 0; y < stats.resolution; ++y)
        {
            for (int x = 0; x < stats.resolution; ++x)
            {
                Vec3 p = GridPoint(x, y, z, stats.voxelSize);

                float sdf = EvaluateStageSdf(p, settings, stage);
                sdfValues[GridIndex(x, y, z, stats.resolution)] = sdf;

                stats.minSdf = std::min(stats.minSdf, sdf);
                stats.maxSdf = std::max(stats.maxSdf, sdf);
                if (z == sliceZ)
                {
                    stats.centerSlice[static_cast<size_t>(y * stats.sliceResolution + x)] = sdf;
                }
                if (sdf < 0.0f)
                {
                    ++stats.insideVoxels;
                }
                if (std::fabs(sdf) <= surfaceBand && stats.surfacePoints.size() < kMaxSurfacePoints && ((x + y * 3 + z * 5) % 3 == 0))
                {
                    stats.surfacePoints.push_back({p.x, p.y, p.z, sdf});
                }
            }
        }
    }

    auto addSegment = [&](Vec3 a, Vec3 b) {
        if (stats.surfaceSegments.size() >= kMaxSurfaceSegments)
        {
            return;
        }
        stats.surfaceSegments.push_back({a.x, a.y, a.z, b.x, b.y, b.z});
    };

    auto addQuad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        if (stats.surfaceTriangles.size() + 2 > kMaxSurfaceTriangles)
        {
            return;
        }
        stats.surfaceTriangles.push_back({a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
        stats.surfaceTriangles.push_back({a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z});
    };

    for (int z = 0; z < stats.resolution - 1; ++z)
    {
        for (int y = 0; y < stats.resolution - 1; ++y)
        {
            for (int x = 0; x < stats.resolution - 1; ++x)
            {
                if (((x + y + z) % 2) != 0)
                {
                    continue;
                }

                const float center = sdfValues[GridIndex(x, y, z, stats.resolution)];
                const Vec3 p = GridPoint(x, y, z, stats.voxelSize);
                const float half = stats.voxelSize * 0.42f;

                const float sx = sdfValues[GridIndex(x + 1, y, z, stats.resolution)];
                const float sy = sdfValues[GridIndex(x, y + 1, z, stats.resolution)];
                const float sz = sdfValues[GridIndex(x, y, z + 1, stats.resolution)];

                if ((center < 0.0f) != (sx < 0.0f))
                {
                    addSegment({p.x + half, p.y - half, p.z}, {p.x + half, p.y + half, p.z});
                    addSegment({p.x + half, p.y, p.z - half}, {p.x + half, p.y, p.z + half});
                    addQuad(
                        {p.x + half, p.y - half, p.z - half},
                        {p.x + half, p.y + half, p.z - half},
                        {p.x + half, p.y + half, p.z + half},
                        {p.x + half, p.y - half, p.z + half});
                }
                if ((center < 0.0f) != (sy < 0.0f))
                {
                    addSegment({p.x - half, p.y + half, p.z}, {p.x + half, p.y + half, p.z});
                    addSegment({p.x, p.y + half, p.z - half}, {p.x, p.y + half, p.z + half});
                    addQuad(
                        {p.x - half, p.y + half, p.z - half},
                        {p.x + half, p.y + half, p.z - half},
                        {p.x + half, p.y + half, p.z + half},
                        {p.x - half, p.y + half, p.z + half});
                }
                if ((center < 0.0f) != (sz < 0.0f))
                {
                    addSegment({p.x - half, p.y, p.z + half}, {p.x + half, p.y, p.z + half});
                    addSegment({p.x, p.y - half, p.z + half}, {p.x, p.y + half, p.z + half});
                    addQuad(
                        {p.x - half, p.y - half, p.z + half},
                        {p.x + half, p.y - half, p.z + half},
                        {p.x + half, p.y + half, p.z + half},
                        {p.x - half, p.y + half, p.z + half});
                }
            }
        }
    }

    stats.fillRatio = stats.totalVoxels > 0 ? static_cast<float>(stats.insideVoxels) / static_cast<float>(stats.totalVoxels) : 0.0f;
    stats.estimatedVolume = static_cast<float>(stats.insideVoxels) * stats.voxelSize * stats.voxelSize * stats.voxelSize;
    return stats;
}
} // namespace rock
