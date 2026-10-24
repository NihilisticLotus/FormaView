#pragma once
#include "formaview/skeletal.hpp"
#include <cgltf.h>
#include <stop_token>

namespace formaview::gltf {
// cgltf_validate() reads sparse indices before validating their buffer views.
// Establish all byte ranges first, using subtraction/division to avoid wrap.
// reject_meshopt only reflects what the caller can consume: loaded fallback
// data is still range-checked, so callers that read plain accessors may allow it.
inline void validate_loaded_storage(const cgltf_data &data,
                                    const skeletal::Limits &limits,
                                    std::stop_token stop = {},
                                    bool reject_meshopt = true) {
  const auto invalid = [](const char *message) {
    throw skeletal::Failure(ErrorCode::InvalidArgument, message);
  };
  if (data.nodes_count > limits.bones || data.animations_count > limits.clips)
    invalid("glTF scene exceeds node/clip limits");
  for (size_t i = 0; i < data.buffer_views_count; ++i) {
    const auto &v = data.buffer_views[i];
    if (!v.buffer || !v.buffer->data || v.offset > v.buffer->size ||
        v.size > v.buffer->size - v.offset)
      invalid("glTF buffer view outside loaded buffer");
    if (reject_meshopt && v.has_meshopt_compression)
      throw skeletal::Failure(ErrorCode::Unsupported, "Meshopt compressed glTF needs decoding before reading");
  }
  const auto range = [&](const cgltf_buffer_view *view, size_t offset,
                        size_t count, size_t stride, size_t element,
                        size_t alignment) {
    // Bounds first: the glTF alignment rule applies to the combined offset.
    if (!view || !element || !alignment || !count || stride < element ||
        stride % alignment || offset > view->size ||
        element > view->size - offset ||
        count - 1 > (view->size - offset - element) / stride ||
        (view->offset + offset) % alignment)
      invalid("glTF accessor outside its buffer view");
  };
  for (size_t i = 0; i < data.accessors_count; ++i) {
    if (stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled, "glTF validation cancelled");
    const auto &a = data.accessors[i];
    const auto element = cgltf_calc_size(a.type, a.component_type);
    const auto component = cgltf_component_size(a.component_type);
    if (!element || !component || !a.count || a.count > limits.elements)
      invalid("Invalid glTF accessor layout/count");
    if (a.buffer_view)
      range(a.buffer_view, a.offset, a.count, a.stride, element, component);
    if (a.is_sparse) {
      const auto &sp = a.sparse;
      if (sp.indices_component_type != cgltf_component_type_r_8u &&
          sp.indices_component_type != cgltf_component_type_r_16u &&
          sp.indices_component_type != cgltf_component_type_r_32u)
        invalid("Invalid glTF sparse index type");
      const auto index_size = cgltf_component_size(sp.indices_component_type);
      if (sp.count > a.count)
        invalid("Invalid glTF sparse count");
      range(sp.indices_buffer_view, sp.indices_byte_offset, sp.count,
            index_size, index_size, index_size);
      range(sp.values_buffer_view, sp.values_byte_offset, sp.count,
            element, element, component);
    }
  }
  for (size_t i = 0; i < data.nodes_count; ++i) {
    size_t depth{};
    for (auto *p = data.nodes[i].parent; p; p = p->parent)
      if (++depth > 256 || depth > data.nodes_count)
        invalid("Cyclic or excessively deep glTF hierarchy");
  }
}
} // namespace formaview::gltf
