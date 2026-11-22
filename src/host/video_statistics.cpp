#include "video_statistics.hpp"
#include <QSysInfo>
#include <d3dkmthk.h>
#include <wrl/client.h>

namespace {
quint64 ticks(FILETIME time) {
    return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
}

QVariantMap VideoStatistics::sample(IDXGIAdapter *adapter) {
    QVariantMap out;
    out.insert("os", QSysInfo::prettyProductName() + " (" + QSysInfo::kernelVersion() + ")");
    FILETIME idle{}, kernel{}, user{}, created{}, exited{}, procKernel{}, procUser{};
    if (GetSystemTimes(&idle, &kernel, &user) &&
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &procKernel, &procUser)) {
        const auto total = ticks(kernel) + ticks(user);
        const auto process = ticks(procKernel) + ticks(procUser);
        if (cpuTotal_ && total > cpuTotal_) {
            const double scale = 100.0 / double(total - cpuTotal_);
            out.insert("cpu-system", qBound(0.0, 100.0 - double(ticks(idle) - cpuIdle_) * scale, 100.0));
            out.insert("cpu-process", qBound(0.0, double(process - cpuProcess_) * scale, 100.0));
        }
        cpuTotal_ = total;
        cpuIdle_ = ticks(idle);
        cpuProcess_ = process;
    }
    DXGI_ADAPTER_DESC desc{};
    if (!adapter || FAILED(adapter->GetDesc(&desc)))
        return out;
    out.insert("adapter", QString::fromWCharArray(desc.Description));
    Microsoft::WRL::ComPtr<IDXGIAdapter3> local;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&local))) &&
        SUCCEEDED(local->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
        out.insert("vram", quint64(memory.CurrentUsage));
        out.insert("vram-budget", quint64(memory.Budget));
    }
    D3DKMT_QUERYSTATISTICS query{};
    query.AdapterLuid = desc.AdapterLuid;
    query.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    if (D3DKMTQueryStatistics(&query) < 0)
        return out;
    const UINT count = query.QueryResult.AdapterInformation.NodeCount;
    const qint64 elapsed = gpuClock_.isValid() ? gpuClock_.nsecsElapsed() / 100 : 0;
    gpuClock_.start();
    nodeTimes_.resize(count, -1);
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = desc.AdapterLuid;
    const bool opened = D3DKMTOpenAdapterFromLuid(&open) >= 0;
    double busiest = -1, processing = -1;
    for (UINT n = 0; n < count; ++n) {
        query.Type = D3DKMT_QUERYSTATISTICS_NODE;
        query.QueryNode.NodeId = n;
        if (D3DKMTQueryStatistics(&query) < 0) {
            nodeTimes_[n] = -1;
            continue;
        }
        const auto &info = query.QueryResult.NodeInformation;
        const auto now = info.GlobalInformation.RunningTime.QuadPart;
        const auto last = nodeTimes_[n];
        nodeTimes_[n] = now;
        if (!elapsed || last < 0 || now < last)
            continue;
        const double busy = qBound(0.0, 100.0 * double(now - last) / double(elapsed), 100.0);
        busiest = qMax(busiest, busy);
        D3DKMT_NODEMETADATA metadata{};
        metadata.NodeOrdinalAndAdapterIndex = n;
        D3DKMT_QUERYADAPTERINFO request{};
        request.hAdapter = open.hAdapter;
        request.Type = KMTQAITYPE_NODEMETADATA;
        request.pPrivateDriverData = &metadata;
        request.PrivateDriverDataSize = sizeof(metadata);
        if (opened && D3DKMTQueryAdapterInfo(&request) >= 0) {
            if (metadata.NodeData.EngineType == DXGK_ENGINE_TYPE_VIDEO_PROCESSING)
                processing = qMax(processing, busy);
            if (metadata.NodeData.EngineType == DXGK_ENGINE_TYPE_3D && info.NodePerfData.Frequency)
                out.insert("gpu-mhz", double(info.NodePerfData.Frequency) / 1.0e6);
        }
    }
    if (opened) {
        D3DKMT_CLOSEADAPTER close{};
        close.hAdapter = open.hAdapter;
        D3DKMTCloseAdapter(&close);
    }
    if (busiest >= 0) out.insert("gpu-busy", busiest);
    if (processing >= 0) out.insert("vpe-busy", processing);
    return out;
}
