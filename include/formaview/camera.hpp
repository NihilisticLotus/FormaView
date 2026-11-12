#pragma once
#include "formaview/skeletal.hpp"
#include <string>

namespace formaview {
// Source-space, read-only camera sample. Invalid or unavailable optical fields
// remain absent; the viewer never substitutes its orbit-camera defaults.
struct CameraSample {
  Matrix4 world{};
  std::string projection{"unknown"};
  std::string axes{"unknown"};
  double focal_mm{}, aperture_x_mm{}, aperture_y_mm{};
  double fov_x_deg{}, fov_y_deg{};
  double near_plane{}, far_plane{};
  bool has_focal{}, has_aperture_x{}, has_aperture_y{};
  bool has_fov_x{}, has_fov_y{}, has_near{}, has_far{};
};
struct CameraInfo {
  std::string path, name;
  std::uint32_t node{};
  CameraSample sample;
};
} // namespace formaview
