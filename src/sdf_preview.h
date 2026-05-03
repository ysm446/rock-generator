#pragma once

#include "node_graph.h"

namespace rock
{
SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, int resolution, PreviewStage stage);
}
