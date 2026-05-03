#pragma once

#include "node_graph.h"

#include <vector>

namespace rock
{
SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, int resolution, PreviewStage stage);
SdfPreviewStats BuildDenseSdfPreviewFromValues(int resolution, const std::vector<float>& sdfValues);
}
