#pragma once

#include <filesystem>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include "unordered_dense.h"
#include "edgefriend.h"

namespace ObjIO {

struct RawMesh {
    std::vector<glm::vec3> positions;
    std::vector<int> indices;
    std::vector<int> indicesOffsets;
    ankerl::unordered_dense::map<glm::ivec2, float> creases;
};

RawMesh LoadRawMesh(const std::filesystem::path& path);

void WriteGeometry(const std::filesystem::path& path,
                   const Edgefriend::EdgefriendGeometry& geometry);

bool CompareFiles(const std::filesystem::path& pathA,
                  const std::filesystem::path& pathB,
                  float positionEpsilon);

} // namespace ObjIO
