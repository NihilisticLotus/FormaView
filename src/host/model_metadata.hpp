#pragma once
#include "formaview/contracts.hpp"
#include <QMatrix4x4>
#include <QVariantList>

inline QVariantList modelMatrixValues(const formaview::Matrix4 &matrix) {
  QVariantList values;
  values.reserve(16);
  for (double value : matrix)
    values.append(value);
  return values;
}
inline QVariantList modelMatrixValues(const QMatrix4x4 &matrix) {
  QVariantList values;
  values.reserve(16);
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      values.append(matrix(row, column));
  return values;
}
