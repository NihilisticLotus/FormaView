#pragma once
#include "formaview/render_item.hpp"
#include <QByteArray>
#include <stop_token>

std::shared_ptr<formaview::render::Texture> readModelTexture(const QString &,
                                                             std::stop_token);
std::shared_ptr<formaview::render::Texture>
decodeModelTexture(const QByteArray &, std::stop_token);
