#pragma once

#include <QElapsedTimer>
#include <QVariantMap>
#include <QVector>
#include <dxgi1_4.h>

class VideoStatistics {
public:
    QVariantMap sample(IDXGIAdapter *adapter);

private:
    quint64 cpuTotal_ = 0, cpuIdle_ = 0, cpuProcess_ = 0;
    QVector<qint64> nodeTimes_;
    QElapsedTimer gpuClock_;
};
