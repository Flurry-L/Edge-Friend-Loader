#include "obj_io.h"
#include "rapidobj.hpp"
#include <fstream>
#include <sstream>
#include <span>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace ObjIO {

namespace {

struct ParsedObjData {
    std::vector<glm::vec3> vertices;
    std::vector<glm::ivec4> faces;
};

int ParseIndexToken(const std::string& token) {
    const auto slash = token.find('/');
    return std::stoi((slash == std::string::npos) ? token : token.substr(0, slash));
}

ParsedObjData ParseObjForComparison(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open OBJ file: " + path.string());
    }

    ParsedObjData data;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            std::istringstream s(line.substr(2));
            glm::vec3 v{};
            s >> v.x >> v.y >> v.z;
            if (!s.fail()) data.vertices.push_back(v);
        }
        else if (line.size() > 2 && line[0] == 'f' && line[1] == ' ') {
            std::istringstream s(line.substr(2));
            std::string t0, t1, t2, t3;
            s >> t0 >> t1 >> t2 >> t3;
            if (!s.fail()) {
                data.faces.emplace_back(
                    ParseIndexToken(t0), ParseIndexToken(t1),
                    ParseIndexToken(t2), ParseIndexToken(t3));
            }
        }
    }
    return data;
}

} // anonymous namespace

RawMesh LoadRawMesh(const std::filesystem::path& path) {
    auto model = rapidobj::ParseFile(path);
    if (model.error) {
        throw std::runtime_error("OBJ file could not be loaded: " + path.string());
    }
    if (model.shapes.empty()) {
        throw std::runtime_error("OBJ file does not contain a mesh: " + path.string());
    }
    if (model.shapes.size() != 1) {
        std::cerr << "Warning: only processing the first shape/object.\n";
    }

    const auto& mesh = model.shapes.front().mesh;

    auto posSpan = std::span<glm::vec3>(
        reinterpret_cast<glm::vec3*>(model.attributes.positions.data()),
        model.attributes.positions.size() / 3);

    RawMesh result;
    result.positions.assign(posSpan.begin(), posSpan.end());

    result.indices.reserve(mesh.indices.size());
    std::transform(mesh.indices.begin(), mesh.indices.end(),
        std::back_inserter(result.indices),
        [](const rapidobj::Index& idx) { return idx.position_index; });

    result.indicesOffsets.reserve(mesh.num_face_vertices.size());
    std::size_t startIndex = 0;
    for (const auto faceSize : mesh.num_face_vertices) {
        result.indicesOffsets.push_back(static_cast<int>(startIndex));
        startIndex += faceSize;
    }

    result.creases.reserve(mesh.creases.size());
    for (const auto& crease : mesh.creases) {
        const auto [mn, mx] = std::minmax(crease.position_index_from, crease.position_index_to);
        result.creases.emplace(glm::ivec2(mn, mx), crease.sharpness);
    }

    return result;
}

void WriteGeometry(const std::filesystem::path& path,
                   const Edgefriend::EdgefriendGeometry& geometry) {
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to open output file: " + path.string());
    }

    for (const auto& p : geometry.positions) {
        out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    for (int i = 0; i < static_cast<int>(geometry.friendsAndSharpnesses.size()); ++i) {
        out << 'f';
        for (int j = 0; j < 4; ++j) {
            out << ' ' << geometry.indices[4 * i + j] + 1;
        }
        out << '\n';
    }
}

bool CompareFiles(const std::filesystem::path& pathA,
                  const std::filesystem::path& pathB,
                  float positionEpsilon) {
    const auto a = ParseObjForComparison(pathA);
    const auto b = ParseObjForComparison(pathB);

    if (a.vertices.size() != b.vertices.size()) {
        std::cerr << "[Check] Vertex count mismatch: " << a.vertices.size()
                  << " vs " << b.vertices.size() << '\n';
        return false;
    }
    if (a.faces.size() != b.faces.size()) {
        std::cerr << "[Check] Face count mismatch: " << a.faces.size()
                  << " vs " << b.faces.size() << '\n';
        return false;
    }

    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        const auto& va = a.vertices[i];
        const auto& vb = b.vertices[i];
        if (std::abs(va.x - vb.x) > positionEpsilon ||
            std::abs(va.y - vb.y) > positionEpsilon ||
            std::abs(va.z - vb.z) > positionEpsilon) {
            std::cerr << "[Check] Vertex mismatch at " << i
                      << ": (" << va.x << ", " << va.y << ", " << va.z
                      << ") vs (" << vb.x << ", " << vb.y << ", " << vb.z << ")\n";
            return false;
        }
    }

    for (std::size_t i = 0; i < a.faces.size(); ++i) {
        if (a.faces[i] != b.faces[i]) {
            std::cerr << "[Check] Face mismatch at " << i << '\n';
            return false;
        }
    }

    return true;
}

} // namespace ObjIO
