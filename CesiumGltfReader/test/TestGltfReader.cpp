#include "CesiumGltfReader/GltfReader.h"

#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionCesiumRTC.h>
#include <CesiumGltf/ExtensionKhrDracoMeshCompression.h>
#include <CesiumUtility/Math.h>

#include <catch2/catch.hpp>
#include <glm/vec3.hpp>
#include <gsl/span>
#include <rapidjson/reader.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace CesiumGltf;
using namespace CesiumGltfReader;
using namespace CesiumUtility;

namespace {
std::vector<std::byte> readFile(const std::filesystem::path& fileName) {
  std::ifstream file(fileName, std::ios::binary | std::ios::ate);
  REQUIRE(file);

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<std::byte> buffer(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(buffer.data()), size);

  return buffer;
}
} // namespace

TEST_CASE("CesiumGltfReader::GltfReader") {
  using namespace std::string_literals;

  std::string s = R"(
    {
      "accessors": [
        {
          "count": 4,
          "componentType": 5121,
          "type": "VEC2",
          "max": [
            1,
            2.2,
            3.3
          ],
          "min": [
            0,
            -1.2
          ]
        }
      ],
      "meshes": [
        {
          "primitives": [
            {
              "attributes": {
                "POSITION": 0,
                "NORMAL": 1
              },
              "targets": [
                {
                  "POSITION": 10,
                  "NORMAL": 11
                }
              ]
            }
          ]
        }
      ],
      "surprise": {
        "foo": true
      }
    }
  )";

  GltfReader reader;
  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()));
  CHECK(result.errors.empty());
  REQUIRE(result.model.has_value());

  Model& model = result.model.value();
  REQUIRE(model.accessors.size() == 1);
  CHECK(model.accessors[0].count == 4);
  CHECK(
      model.accessors[0].componentType ==
      Accessor::ComponentType::UNSIGNED_BYTE);
  CHECK(model.accessors[0].type == Accessor::Type::VEC2);
  REQUIRE(model.accessors[0].min.size() == 2);
  CHECK(model.accessors[0].min[0] == 0.0);
  CHECK(model.accessors[0].min[1] == -1.2);
  REQUIRE(model.accessors[0].max.size() == 3);
  CHECK(model.accessors[0].max[0] == 1.0);
  CHECK(model.accessors[0].max[1] == 2.2);
  CHECK(model.accessors[0].max[2] == 3.3);

  REQUIRE(model.meshes.size() == 1);
  REQUIRE(model.meshes[0].primitives.size() == 1);
  CHECK(model.meshes[0].primitives[0].attributes["POSITION"] == 0);
  CHECK(model.meshes[0].primitives[0].attributes["NORMAL"] == 1);

  REQUIRE(model.meshes[0].primitives[0].targets.size() == 1);
  CHECK(model.meshes[0].primitives[0].targets[0]["POSITION"] == 10);
  CHECK(model.meshes[0].primitives[0].targets[0]["NORMAL"] == 11);
}

namespace {
struct VertexAttributeRange {
  glm::vec3 positionRange;
  glm::vec3 normalRange;
  glm::vec2 texCoordRange;
};

template <typename T>
T getRange(const CesiumGltf::AccessorView<T>& accessorView) {
  T min{std::numeric_limits<float>::max()};
  T max{std::numeric_limits<float>::lowest()};
  for (int32_t i = 0; i < accessorView.size(); ++i) {
    const T& value = accessorView[i];
    for (uint32_t j = 0; j < static_cast<uint32_t>(value.length()); ++j) {
      min[j] = glm::min<float>(min[j], value[j]);
      max[j] = glm::max<float>(max[j], value[j]);
    }
  }
  return max - min;
}

template <typename T> T getRange(const Model& model, int32_t accessor) {
  CesiumGltf::AccessorView<T> accessorView(model, accessor);
  REQUIRE(accessorView.status() == CesiumGltf::AccessorViewStatus::Valid);
  return getRange(accessorView);
}

VertexAttributeRange getVertexAttributeRange(const Model& model) {
  VertexAttributeRange var;
  model.forEachPrimitiveInScene(
      -1,
      [&var](
          const Model& model,
          const Node&,
          const Mesh&,
          const MeshPrimitive& primitive,
          const glm::dmat4& transform) {
        for (std::pair<const std::string, int32_t> attribute :
             primitive.attributes) {
          const std::string& attributeName = attribute.first;
          if (attributeName == "POSITION") {
            var.positionRange = glm::vec3(
                transform *
                glm::dvec4(getRange<glm::vec3>(model, attribute.second), 0));
          } else if (attributeName == "NORMAL") {
            var.normalRange =
                glm::normalize(getRange<glm::vec3>(model, attribute.second));
          } else if (attributeName.find("TEXCOORD") == 0) {
            var.texCoordRange = getRange<glm::vec2>(model, attribute.second);
          }
        }
      });
  return var;
}

template <typename T>
bool epsilonCompare(const T& v1, const T& v2, double epsilon) {
  for (uint32_t i = 0; i < static_cast<uint32_t>(v1.length()); ++i) {
    if (!CesiumUtility::Math::equalsEpsilon(v1[i], v2[i], epsilon)) {
      return false;
    }
  }
  return true;
}
} // namespace

TEST_CASE("Can decompress meshes using EXT_meshopt_compression") {

  VertexAttributeRange originalVar;
  {
    GltfReader reader;
    GltfReaderResult result = reader.readGltf(readFile(
        CesiumGltfReader_TEST_DATA_DIR +
        std::string("/DucksMeshopt/Duck.glb")));
    const Model& model = result.model.value();
    originalVar = getVertexAttributeRange(model);
  }

  for (int n = 3; n <= 15; n += 3) {
    std::string filename = CesiumGltfReader_TEST_DATA_DIR +
                           std::string("/DucksMeshopt/Duck") + "-vp-" +
                           std::to_string(n) + "-vt-" + std::to_string(n) +
                           "-vn-" + std::to_string(n) + ".glb";
    if (std::filesystem::exists(filename)) {
      std::vector<std::byte> data = readFile(filename);
      GltfReader reader;
      GltfReaderResult result = reader.readGltf(data);
      REQUIRE(result.model);
      REQUIRE(result.warnings.empty());
      const Model& model = result.model.value();
      VertexAttributeRange compressedVar = getVertexAttributeRange(model);
      double error = 1.0f / (pow(2, n - 1));
      REQUIRE(epsilonCompare(
          originalVar.positionRange,
          compressedVar.positionRange,
          error));
      REQUIRE(epsilonCompare(
          originalVar.normalRange,
          compressedVar.normalRange,
          error));
      REQUIRE(epsilonCompare(
          originalVar.texCoordRange,
          compressedVar.texCoordRange,
          error));
    }
  }
}

TEST_CASE("Read TriangleWithoutIndices") {
  std::filesystem::path gltfFile = CesiumGltfReader_TEST_DATA_DIR;
  gltfFile /=
      "TriangleWithoutIndices/glTF-Embedded/TriangleWithoutIndices.gltf";
  std::vector<std::byte> data = readFile(gltfFile);
  GltfReader reader;
  GltfReaderResult result = reader.readGltf(data);
  REQUIRE(result.model);

  const Model& model = result.model.value();
  REQUIRE(model.meshes.size() == 1);
  REQUIRE(model.meshes[0].primitives.size() == 1);
  REQUIRE(model.meshes[0].primitives[0].attributes.size() == 1);
  REQUIRE(model.meshes[0].primitives[0].attributes.begin()->second == 0);

  AccessorView<glm::vec3> position(model, 0);
  REQUIRE(position.size() == 3);
  CHECK(position[0] == glm::vec3(0.0, 0.0, 0.0));
  CHECK(position[1] == glm::vec3(1.0, 0.0, 0.0));
  CHECK(position[2] == glm::vec3(0.0, 1.0, 0.0));
}

TEST_CASE("Nested extras deserializes properly") {
  const std::string s = R"(
    {
        "asset" : {
            "version" : "1.1"
        },
        "extras": {
            "A": "Hello World",
            "B": 1234567,
            "C": {
                "C1": {},
                "C2": [1,2,3,4,5]
            }
        }
    }
  )";

  GltfReader reader;
  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()));

  REQUIRE(result.errors.empty());
  REQUIRE(result.model.has_value());

  Model& model = result.model.value();
  auto cit = model.extras.find("C");
  REQUIRE(cit != model.extras.end());

  JsonValue* pC2 = cit->second.getValuePtrForKey("C2");
  REQUIRE(pC2 != nullptr);

  CHECK(pC2->isArray());
  std::vector<JsonValue>& array = std::get<std::vector<JsonValue>>(pC2->value);
  CHECK(array.size() == 5);
  CHECK(array[0].getSafeNumber<double>() == 1.0);
  CHECK(array[1].getSafeNumber<std::uint64_t>() == 2);
  CHECK(array[2].getSafeNumber<std::uint8_t>() == 3);
  CHECK(array[3].getSafeNumber<std::int16_t>() == 4);
  CHECK(array[4].getSafeNumber<std::int32_t>() == 5);
}

TEST_CASE("Can deserialize KHR_draco_mesh_compression") {
  const std::string s = R"(
    {
      "asset": {
        "version": "2.0"
      },
      "meshes": [
        {
          "primitives": [
            {
              "extensions": {
                "KHR_draco_mesh_compression": {
                  "bufferView": 1,
                  "attributes": {
                    "POSITION": 0
                  }
                }
              }
            }
          ]
        }
      ]
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;
  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  REQUIRE(result.errors.empty());
  REQUIRE(result.model.has_value());

  Model& model = result.model.value();
  REQUIRE(model.meshes.size() == 1);
  REQUIRE(model.meshes[0].primitives.size() == 1);

  MeshPrimitive& primitive = model.meshes[0].primitives[0];
  ExtensionKhrDracoMeshCompression* pDraco =
      primitive.getExtension<ExtensionKhrDracoMeshCompression>();
  REQUIRE(pDraco);

  CHECK(pDraco->bufferView == 1);
  CHECK(pDraco->attributes.size() == 1);

  REQUIRE(pDraco->attributes.find("POSITION") != pDraco->attributes.end());
  CHECK(pDraco->attributes.find("POSITION")->second == 0);

  // Repeat test but this time the extension should be deserialized as a
  // JsonValue.
  reader.getOptions().setExtensionState(
      "KHR_draco_mesh_compression",
      CesiumJsonReader::ExtensionState::JsonOnly);

  GltfReaderResult result2 = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  REQUIRE(result2.errors.empty());
  REQUIRE(result2.model.has_value());

  Model& model2 = result2.model.value();
  REQUIRE(model2.meshes.size() == 1);
  REQUIRE(model2.meshes[0].primitives.size() == 1);

  MeshPrimitive& primitive2 = model2.meshes[0].primitives[0];
  JsonValue* pDraco2 =
      primitive2.getGenericExtension("KHR_draco_mesh_compression");
  REQUIRE(pDraco2);

  REQUIRE(pDraco2->getValuePtrForKey("bufferView"));
  CHECK(
      pDraco2->getValuePtrForKey("bufferView")
          ->getSafeNumberOrDefault<int64_t>(0) == 1);

  REQUIRE(pDraco2->getValuePtrForKey("attributes"));
  REQUIRE(pDraco2->getValuePtrForKey("attributes")->isObject());
  REQUIRE(
      pDraco2->getValuePtrForKey("attributes")->getValuePtrForKey("POSITION"));
  REQUIRE(
      pDraco2->getValuePtrForKey("attributes")
          ->getValuePtrForKey("POSITION")
          ->getSafeNumberOrDefault<int64_t>(1) == 0);

  // Repeat test but this time the extension should not be deserialized at all.
  reader.getOptions().setExtensionState(
      "KHR_draco_mesh_compression",
      CesiumJsonReader::ExtensionState::Disabled);

  GltfReaderResult result3 = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  REQUIRE(result3.errors.empty());
  REQUIRE(result3.model.has_value());

  Model& model3 = result3.model.value();
  REQUIRE(model3.meshes.size() == 1);
  REQUIRE(model3.meshes[0].primitives.size() == 1);

  MeshPrimitive& primitive3 = model3.meshes[0].primitives[0];

  REQUIRE(!primitive3.getGenericExtension("KHR_draco_mesh_compression"));
  REQUIRE(!primitive3.getExtension<ExtensionKhrDracoMeshCompression>());
}

TEST_CASE("Extensions deserialize to JsonVaue iff "
          "a default extension is registered") {
  const std::string s = R"(
    {
        "asset" : {
            "version" : "2.0"
        },
        "extensions": {
            "A": {
              "test": "Hello World"
            },
            "B": {
              "another": "Goodbye World"
            }
        }
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;
  GltfReaderResult withCustomExtModel = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  REQUIRE(withCustomExtModel.errors.empty());
  REQUIRE(withCustomExtModel.model.has_value());

  REQUIRE(withCustomExtModel.model->extensions.size() == 2);

  JsonValue* pA = withCustomExtModel.model->getGenericExtension("A");
  JsonValue* pB = withCustomExtModel.model->getGenericExtension("B");
  REQUIRE(pA != nullptr);
  REQUIRE(pB != nullptr);

  REQUIRE(pA->getValuePtrForKey("test"));
  REQUIRE(
      pA->getValuePtrForKey("test")->getStringOrDefault("") == "Hello World");

  REQUIRE(pB->getValuePtrForKey("another"));
  REQUIRE(
      pB->getValuePtrForKey("another")->getStringOrDefault("") ==
      "Goodbye World");

  // Repeat test but this time the extension should be skipped.
  reader.getOptions().setExtensionState(
      "A",
      CesiumJsonReader::ExtensionState::Disabled);
  reader.getOptions().setExtensionState(
      "B",
      CesiumJsonReader::ExtensionState::Disabled);

  GltfReaderResult withoutCustomExt = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  auto& zeroExtensions = withoutCustomExt.model->extensions;
  REQUIRE(zeroExtensions.empty());
}

TEST_CASE("Unknown MIME types are handled") {
  const std::string s = R"(
    {
        "asset" : {
            "version" : "2.0"
        },
        "images": [
            {
              "mimeType" : "image/webp"
            }
        ]
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;
  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  // Note: The result.errors will not be empty,
  // because no images could be read.
  REQUIRE(result.model.has_value());
}

TEST_CASE("Can parse doubles with no fractions as integers") {
  std::string s = R"(
    {
      "accessors": [
        {
          "count": 4.0,
          "componentType": 5121.0
        }
      ]
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;
  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);

  CHECK(result.warnings.empty());
  Model& model = result.model.value();
  CHECK(model.accessors[0].count == 4);
  CHECK(
      model.accessors[0].componentType ==
      Accessor::ComponentType::UNSIGNED_BYTE);
  s = R"(
    {
      "accessors": [
        {
          "count": 4.0,
          "componentType": 5121.1
        }
      ]
    }
  )";
  result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);
  CHECK(!result.warnings.empty());
}

TEST_CASE("Test KTX2") {
  std::filesystem::path gltfFile = CesiumGltfReader_TEST_DATA_DIR;
  gltfFile /= "CesiumBalloonKTX2Hacky.glb";
  std::vector<std::byte> data = readFile(gltfFile.string());
  CesiumGltfReader::GltfReader reader;
  GltfReaderResult result = reader.readGltf(data);
  REQUIRE(result.model);

  const Model& model = result.model.value();
  REQUIRE(model.meshes.size() == 1);
}

TEST_CASE("Can apply RTC CENTER if model uses Cesium RTC extension") {
  const std::string s = R"(
    {
      "extensions": {
          "CESIUM_RTC": {
              "center": [6378137.0, 0.0, 0.0]
          }
      }
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;
  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);
  REQUIRE(result.model.has_value());
  Model& model = result.model.value();
  const ExtensionCesiumRTC* cesiumRTC =
      model.getExtension<ExtensionCesiumRTC>();
  REQUIRE(cesiumRTC);
  std::vector<double> rtcCenter = {6378137.0, 0.0, 0.0};
  CHECK(cesiumRTC->center == rtcCenter);
}

TEST_CASE("Can correctly interpret mipmaps in KTX2 files") {
  {
    // This KTX2 file has a single mip level and no further mip levels should be
    // generated. `mipPositions` should reflect this single mip level.
    std::filesystem::path ktx2File = CesiumGltfReader_TEST_DATA_DIR;
    ktx2File /= "ktx2/kota-onelevel.ktx2";
    std::vector<std::byte> data = readFile(ktx2File.string());
    ImageReaderResult imageResult =
        GltfReader::readImage(data, Ktx2TranscodeTargets{});
    REQUIRE(imageResult.image.has_value());

    const ImageCesium& image = *imageResult.image;
    REQUIRE(image.mipPositions.size() == 1);
    CHECK(image.mipPositions[0].byteOffset == 0);
    CHECK(image.mipPositions[0].byteSize > 0);
    CHECK(
        image.mipPositions[0].byteSize ==
        size_t(image.width * image.height * image.channels));
    CHECK(image.mipPositions[0].byteSize == image.pixelData.size());
  }

  {
    // This KTX2 file has only a base image but further mip levels can be
    // generated. This image effectively has no mip levels.
    std::filesystem::path ktx2File = CesiumGltfReader_TEST_DATA_DIR;
    ktx2File /= "ktx2/kota-automipmap.ktx2";
    std::vector<std::byte> data = readFile(ktx2File.string());
    ImageReaderResult imageResult =
        GltfReader::readImage(data, Ktx2TranscodeTargets{});
    REQUIRE(imageResult.image.has_value());

    const ImageCesium& image = *imageResult.image;
    REQUIRE(image.mipPositions.size() == 0);
    CHECK(image.pixelData.size() > 0);
  }

  {
    // This KTX2 file has a complete mip chain.
    std::filesystem::path ktx2File = CesiumGltfReader_TEST_DATA_DIR;
    ktx2File /= "ktx2/kota-mipmaps.ktx2";
    std::vector<std::byte> data = readFile(ktx2File.string());
    ImageReaderResult imageResult =
        GltfReader::readImage(data, Ktx2TranscodeTargets{});
    REQUIRE(imageResult.image.has_value());

    const ImageCesium& image = *imageResult.image;
    REQUIRE(image.mipPositions.size() == 9);
    CHECK(image.mipPositions[0].byteSize > 0);
    CHECK(
        image.mipPositions[0].byteSize ==
        size_t(image.width * image.height * image.channels));
    CHECK(image.mipPositions[0].byteSize < image.pixelData.size());

    size_t smallerThan = image.mipPositions[0].byteSize;
    for (size_t i = 1; i < image.mipPositions.size(); ++i) {
      CHECK(image.mipPositions[i].byteSize < smallerThan);
      smallerThan = image.mipPositions[i].byteSize;
    }
  }
}

TEST_CASE("Can read unknown properties from a glTF") {
  const std::string s = R"(
    {
      "someUnknownProperty": "test",
      "asset": {
        "unknownInsideKnown": "this works too"
      }
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;

  reader.getOptions().setCaptureUnknownProperties(true);

  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);
  REQUIRE(result.model.has_value());

  auto unknownIt1 = result.model->unknownProperties.find("someUnknownProperty");
  REQUIRE(unknownIt1 != result.model->unknownProperties.end());
  CHECK(unknownIt1->second.getStringOrDefault("") == "test");

  auto unknownIt2 =
      result.model->asset.unknownProperties.find("unknownInsideKnown");
  REQUIRE(unknownIt2 != result.model->asset.unknownProperties.end());
  CHECK(unknownIt2->second.getStringOrDefault("") == "this works too");
}

TEST_CASE("Ignores unknown properties if requested") {
  const std::string s = R"(
    {
      "someUnknownProperty": "test",
      "asset": {
        "unknownInsideKnown": "this works too"
      }
    }
  )";

  GltfReaderOptions options;
  GltfReader reader;

  reader.getOptions().setCaptureUnknownProperties(false);

  GltfReaderResult result = reader.readGltf(
      gsl::span(reinterpret_cast<const std::byte*>(s.c_str()), s.size()),
      options);
  REQUIRE(result.model.has_value());
  CHECK(result.model->unknownProperties.empty());
  CHECK(result.model->asset.unknownProperties.empty());
}

TEST_CASE("Decodes images with data uris") {
  GltfReader reader;
  GltfReaderResult result = reader.readGltf(readFile(
      CesiumGltfReader_TEST_DATA_DIR + std::string("/BoxTextured.gltf")));

  REQUIRE(result.warnings.empty());
  REQUIRE(result.errors.empty());

  const Model& model = result.model.value();

  REQUIRE(model.images.size() == 1);

  const ImageCesium& image = model.images.front().cesium;

  CHECK(image.width == 256);
  CHECK(image.height == 256);
  CHECK(!image.pixelData.empty());
}
