#include "obj_exporter.h"

#include <fstream>

namespace rock
{
bool ExportDebugTrianglesObj(const SdfPreviewStats& sdf, const std::filesystem::path& path, std::string* errorMessage)
{
    if (sdf.surfaceTriangles.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "No debug triangles to export. Evaluate the graph first.";
        }
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create export directory: " + ec.message();
        }
        return false;
    }

    std::ofstream file(path);
    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to open OBJ file for writing.";
        }
        return false;
    }

    file << "# Rock Generator debug OBJ\n";
    file << "# Prototype triangles from SDF sign-change quads\n";
    file << "o rock_debug\n";

    for (const SurfaceTriangle& triangle : sdf.surfaceTriangles)
    {
        file << "v " << triangle.ax << ' ' << triangle.ay << ' ' << triangle.az << '\n';
        file << "v " << triangle.bx << ' ' << triangle.by << ' ' << triangle.bz << '\n';
        file << "v " << triangle.cx << ' ' << triangle.cy << ' ' << triangle.cz << '\n';
    }

    file << "s off\n";
    for (size_t i = 0; i < sdf.surfaceTriangles.size(); ++i)
    {
        const size_t base = i * 3 + 1;
        file << "f " << base << ' ' << (base + 1) << ' ' << (base + 2) << '\n';
    }

    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed while writing OBJ file.";
        }
        return false;
    }

    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    return true;
}
} // namespace rock
