#pragma once
#include "scene_sdk.hpp"
#include <cgltf.h>
#include <map>
#include <vector>

namespace formaview::scene_export {
struct GltfAccessors {
  Scene &scene;
  std::map<const cgltf_accessor *, std::vector<float>> values;
  const std::vector<float> &read(const cgltf_accessor *);
};
using GltfMorphs = std::map<const cgltf_node *,
                            std::vector<std::vector<FbxBlendShapeChannel *>>>;
void gltf_animations(Scene &, const cgltf_data &, GltfAccessors &,
                     const std::vector<FbxNode *> &, const GltfMorphs &);
} // namespace formaview::scene_export
