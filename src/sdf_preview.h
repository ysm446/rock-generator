#pragma once

#include "node_graph.h"

#include <vector>

namespace rock
{
float EvaluateSdfAt(const GraphSettings& settings, float x, float y, float z, PreviewStage stage);
SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, int resolution, PreviewStage stage);
SdfPreviewStats BuildDenseSdfPreviewFromValues(int resolution, const std::vector<float>& sdfValues);
}
