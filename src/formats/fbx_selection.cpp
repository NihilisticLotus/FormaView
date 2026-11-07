#include "fbx_selection.hpp"
#include "formaview/skeletal.hpp"
#include <QJsonObject>
#include <QtEndian>
#include <limits>
#include <map>
#include <set>
#include <vector>
#include <bit>

namespace formaview::scene_export {
namespace {
struct Invalid {};
struct Node {
  qsizetype begin{}, end{}, properties{}, propertyEnd{}, children{};
  QByteArray name;
};
class Binary {
public:
  const QByteArray &source;
  qsizetype header;
  std::stop_token stop;
  Binary(const QByteArray &data, bool wide, std::stop_token token)
      : source(data), header(wide ? 25 : 13), stop(token) {}
  void check() const {
    if (stop.stop_requested())
      throw skeletal::Failure(ErrorCode::Cancelled,
                              "FBX take selection cancelled");
  }
  template <class T> T read(qsizetype offset) const {
    if (offset < 0 ||
        offset > source.size() - static_cast<qsizetype>(sizeof(T)))
      throw Invalid{};
    return qFromLittleEndian<T>(source.constData() + offset);
  }
  Node node(qsizetype offset, qsizetype bound) const {
    check();
    if (offset < 0 || bound > source.size() || offset > bound - header)
      throw Invalid{};
    const auto end =
        header == 25 ? read<quint64>(offset) : read<quint32>(offset);
    if (!end)
      return {}; // Null record / end of node list.
    const auto length =
        header == 25 ? read<quint64>(offset + 16) : read<quint32>(offset + 8);
    const auto nameLength = read<quint8>(offset + header - 1);
    if (end > static_cast<quint64>(bound) ||
        end < static_cast<quint64>(offset + header + nameLength) ||
        length > end - static_cast<quint64>(offset + header + nameLength))
      throw Invalid{};
    const auto props = offset + header + nameLength;
    return {offset,
            static_cast<qsizetype>(end),
            props,
            props + static_cast<qsizetype>(length),
            props + static_cast<qsizetype>(length),
            source.mid(offset + header, nameLength)};
  }
  std::vector<Node> children(const Node &parent) const {
    std::vector<Node> result;
    for (auto at = parent.children; at < parent.end;) {
      auto child = node(at, parent.end);
      if (!child.end)
        break;
      result.push_back(child);
      at = child.end;
    }
    return result;
  }
  qint64 integer(qsizetype &at, const Node &n) const {
    if (at > n.propertyEnd - 9 || source[at++] != 'L')
      throw Invalid{};
    const auto result = read<qint64>(at);
    at += 8;
    return result;
  }
  QByteArray string(qsizetype &at, const Node &n) const {
    if (at > n.propertyEnd - 5 || source[at++] != 'S')
      throw Invalid{};
    const auto size = read<quint32>(at);
    at += 4;
    if (size > static_cast<quint64>(n.propertyEnd - at))
      throw Invalid{};
    const auto result = source.mid(at, size);
    at += size;
    return result;
  }
  void endOffset(QByteArray &out, qsizetype at, qsizetype end) const {
    if (header == 25)
      qToLittleEndian<quint64>(static_cast<quint64>(end), out.data() + at);
    else {
      if (end > std::numeric_limits<quint32>::max())
        throw Invalid{};
      qToLittleEndian<quint32>(static_cast<quint32>(end), out.data() + at);
    }
  }
  void rebase(QByteArray &out, const Node &n, qsizetype delta,
              int depth = 0) const {
    if (depth > 256)
      throw Invalid{};
    endOffset(out, n.begin + delta, n.end + delta);
    for (const auto &child : children(n))
      rebase(out, child, delta, depth + 1);
  }
  void copy(QByteArray &out, const Node &n) const {
    check();
    const auto delta = out.size() - n.begin;
    out.append(source.constData() + n.begin, n.end - n.begin);
    rebase(out, n, delta);
  }
};
} // namespace
std::optional<QByteArray> select_fbx_takes(const QByteArray &source,
                                           const QJsonArray &catalog,
                                           const QJsonArray &selected,
                                           std::stop_token stop) {
  if (!source.startsWith(QByteArray("Kaydara FBX Binary  \0\x1a\0", 23)) ||
      source.size() < 27)
    return std::nullopt;
  if (selected.size() == catalog.size())
    return std::nullopt;
  try {
    const auto version = qFromLittleEndian<quint32>(source.constData() + 23);
    if (version < 7000 || version > 7700)
      return std::nullopt;
    Binary b(source, version >= 7500, stop);
    std::vector<Node> top;
    auto tail = qsizetype(27);
    while (tail < source.size()) {
      auto n = b.node(tail, source.size());
      if (!n.end)
        break;
      top.push_back(n);
      tail = n.end;
    }
    const Node *objects = nullptr, *connections = nullptr;
    for (const auto &n : top) {
      if (n.name == "Objects") {
        if (objects)
          return std::nullopt;
        objects = &n;
      }
      if (n.name == "Connections") {
        if (connections)
          return std::nullopt;
        connections = &n;
      }
    }
    if (!objects || !connections)
      return std::nullopt;
    const auto objectNodes = b.children(*objects),
               connectionNodes = b.children(*connections);
    std::set<qint64> animations, keep;
    std::map<QString, qint64> stacks;
    std::map<qsizetype, qint64> animationOffsets;
    for (const auto &n : objectNodes) {
      if (n.name != "AnimationStack" && n.name != "AnimationLayer" &&
          n.name != "AnimationCurveNode" && n.name != "AnimationCurve")
        continue;
      auto at = n.properties;
      const auto id = b.integer(at, n);
      if (!animations.insert(id).second)
        throw Invalid{};
      animationOffsets.emplace(n.begin, id);
      if (n.name == "AnimationStack") {
        auto raw = b.string(at, n);
        const auto separator = raw.indexOf(QByteArray("\0\1", 2));
        if (separator >= 0)
          raw.truncate(separator);
        if (raw.startsWith("AnimStack::"))
          raw.remove(0, 11);
        if (!stacks.emplace(QString::fromUtf8(raw), id).second)
          throw Invalid{};
      }
    }
    if (stacks.size() != static_cast<std::size_t>(catalog.size()))
      return std::nullopt;
    for (const auto &v : catalog) {
      const auto clip = v.toObject();
      const auto it = stacks.find(clip["name"].toString());
      if (it == stacks.end())
        return std::nullopt;
      if (selected.contains(clip["id"]))
        keep.insert(it->second);
    }
    if (keep.size() != static_cast<std::size_t>(selected.size()))
      return std::nullopt;
    std::map<qint64, std::vector<qint64>> incoming;
    std::map<qsizetype, std::pair<qint64, qint64>> edges;
    for (const auto &n : connectionNodes) {
      if (n.name != "C")
        throw Invalid{};
      auto at = n.properties;
      const auto kind = b.string(at, n);
      if (kind != "OO" && kind != "OP")
        return std::nullopt;
      const auto from = b.integer(at, n), to = b.integer(at, n);
      edges.emplace(n.begin, std::pair{from, to});
      if (animations.contains(from) && animations.contains(to))
        incoming[to].push_back(from);
    }
    std::vector<qint64> pending(keep.begin(), keep.end());
    for (std::size_t i = 0; i < pending.size(); ++i) {
      b.check();
      for (auto id : incoming[pending[i]])
        if (keep.insert(id).second)
          pending.push_back(id);
    }
    QByteArray out = source.left(27);
    out.reserve(source.size());
    for (const auto &n : top) {
      if (n.name != "Objects" && n.name != "Connections") {
        b.copy(out, n);
        continue;
      }
      const auto begin = out.size();
      out.append(source.constData() + n.begin, n.children - n.begin);
      const auto &children =
          n.name == "Objects" ? objectNodes : connectionNodes;
      for (const auto &child : children) {
        if (n.name == "Objects") {
          const auto it = animationOffsets.find(child.begin);
          if (it != animationOffsets.end() && !keep.contains(it->second))
            continue;
        } else {
          const auto [from, to] = edges.at(child.begin);
          if ((animations.contains(from) && !keep.contains(from)) ||
              (animations.contains(to) && !keep.contains(to)))
            continue;
        }
        b.copy(out, child);
      }
      const auto endOfChildren =
          children.empty() ? n.children : children.back().end;
      out.append(source.constData() + endOfChildren, n.end - endOfChildren);
      b.endOffset(out, begin, out.size());
    }
    out.append(source.constData() + tail, source.size() - tail);
    out.squeeze();
    return out;
  } catch (const Invalid &) {
    return std::nullopt;
  }
}
QByteArray restore_fbx_bind_matrices(const QByteArray &source,
                                    std::span<const BindMatrices> bindings,
                                    std::uint64_t output_budget, std::stop_token stop) {
  if (stop.stop_requested())
    throw skeletal::Failure(ErrorCode::Cancelled, "FBX binding serialization cancelled");
  if (bindings.empty()) return source;
  try {
    if (source.size() < 27 || !source.startsWith(QByteArray("Kaydara FBX Binary  \0\x1a\0", 23))) throw Invalid{};
    const Binary b(source, qFromLittleEndian<quint32>(source.constData() + 23) >= 7500, stop);
    std::map<QByteArray, const BindMatrices *> byMarker;
    for (const auto &record : bindings)
      if (record.marker.isEmpty() || !byMarker.emplace(record.marker, &record).second) throw Invalid{};
    std::set<QByteArray> restored;
    QByteArray out;
    const auto append = [&](const char *data, qsizetype size) {
      if (size < 0 || static_cast<std::uint64_t>(out.size()) > output_budget ||
          static_cast<std::uint64_t>(size) > output_budget - static_cast<std::uint64_t>(out.size()))
        throw skeletal::Failure(ErrorCode::ResourceLimit, "FBX binding output exceeds byte budget");
      out.append(data, size);
    };
    const auto copy = [&](const Node &n) {
      b.check();
      const auto delta = out.size() - n.begin;
      append(source.constData() + n.begin, n.end - n.begin);
      b.rebase(out, n, delta);
    };
    const auto propertySize = [&](qsizetype at, qsizetype size) {
      if (b.header == 25) qToLittleEndian<quint64>(static_cast<quint64>(size), out.data() + at + 16);
      else qToLittleEndian<quint32>(static_cast<quint32>(size), out.data() + at + 8);
    };
    const auto matrix = [&](const Node &n, const std::array<double, 16> &values) {
      if (n.children != n.end || (b.header == 25 ? b.read<quint64>(n.begin + 8) : b.read<quint32>(n.begin + 4)) != 1)
        throw Invalid{};
      const auto begin = out.size();
      append(source.constData() + n.begin, n.properties - n.begin);
      QByteArray payload(141, '\0'); payload[0] = 'd';
      qToLittleEndian<quint32>(16, payload.data() + 1);
      qToLittleEndian<quint32>(128, payload.data() + 9);
      for (std::size_t i = 0; i < values.size(); ++i)
        qToLittleEndian<quint64>(std::bit_cast<quint64>(values[i]), payload.data() + 13 + i * 8);
      append(payload.constData(), payload.size());
      propertySize(begin, payload.size()); b.endOffset(out, begin, out.size());
    };
    append(source.constData(), 27);
    qsizetype tail = 27;
    bool objectsSeen = false;
    while (tail < source.size()) {
      const auto top = b.node(tail, source.size());
      if (!top.end) break;
      tail = top.end;
      if (top.name != "Objects") { copy(top); continue; }
      if (objectsSeen) throw Invalid{};
      objectsSeen = true;
      const auto begin = out.size();
      append(source.constData() + top.begin, top.children - top.begin);
      const auto objects = b.children(top);
      for (const auto &n : objects) {
        b.check();
        if (n.name != "Deformer") { copy(n); continue; }
        auto at = n.properties;
        (void)b.integer(at, n);
        const auto nameBegin = at;
        const auto rawName = b.string(at, n);
        const auto nameEnd = at;
        const auto separator = rawName.indexOf(QByteArray("\0\1", 2));
        const auto marker = separator < 0 ? rawName : rawName.left(separator);
        const auto found = byMarker.find(marker);
        if (found == byMarker.end()) { copy(n); continue; }
        if (b.string(at, n) != "Cluster" || !restored.insert(marker).second) throw Invalid{};
        const auto &record = *found->second;
        const auto clusterBegin = out.size();
        append(source.constData() + n.begin, nameBegin - n.begin);
        const auto name = record.name + (separator < 0 ? QByteArray{} : rawName.mid(separator));
        QByteArray encoded(5, '\0'); encoded[0] = 'S';
        qToLittleEndian<quint32>(static_cast<quint32>(name.size()), encoded.data() + 1);
        append(encoded.constData(), encoded.size()); append(name.constData(), name.size());
        append(source.constData() + nameEnd, n.children - nameEnd);
        propertySize(clusterBegin, n.propertyEnd - n.properties + name.size() - rawName.size());
        std::set<QByteArray> seen;
        const auto children = b.children(n);
        for (const auto &child : children) {
          const std::array<double, 16> *values = nullptr;
          if (child.name == "Transform") values = &record.transform;
          else if (child.name == "TransformLink") values = &record.link;
          if (values) {
            // SDK 2020.2 writes an optional parent matrix as a second
            // "Transform" after DeepConvertScene. The first is the skin
            // inverse bind; retain the SDK's parent metadata unchanged.
            if (!seen.insert(child.name).second) {
              if (child.name != "Transform") throw Invalid{};
              copy(child); continue;
            }
            matrix(child, *values);
          } else copy(child);
        }
        if (!seen.contains("Transform") || !seen.contains("TransformLink")) throw Invalid{};
        const auto end = children.empty() ? n.children : children.back().end;
        append(source.constData() + end, n.end - end);
        b.endOffset(out, clusterBegin, out.size());
      }
      const auto end = objects.empty() ? top.children : objects.back().end;
      append(source.constData() + end, top.end - end);
      b.endOffset(out, begin, out.size());
    }
    if (restored.size() != bindings.size()) throw Invalid{};
    append(source.constData() + tail, source.size() - tail);
    return out;
  } catch (const Invalid &) {
    throw skeletal::Failure(ErrorCode::Internal, "Cannot match SDK binding matrix records safely");
  }
}
void validate_fbx_structure(const QByteArray &source,
                            std::uint64_t decoded_budget,
                            std::stop_token stop) {
  if (!source.startsWith(QByteArray("Kaydara FBX Binary  \0\x1a\0", 23)))
    return; // ASCII FBX: the SDK parser owns text validation.
  const auto malformed = [] {
    throw skeletal::Failure(ErrorCode::InvalidArgument,
                            "Malformed FBX binary record: count, length or "
                            "array size outside the file or decoded budget");
  };
  if (source.size() < 27)
    malformed();
  const auto version = qFromLittleEndian<quint32>(source.constData() + 23);
  const Binary b(source, version >= 7500, stop);
  std::uint64_t declared = 0;
  // Every count the SDK may allocate from must be backed by real bytes:
  // property counts by the property list, plain arrays by their payload,
  // deflate arrays by the 1032:1 deflate ratio, and all arrays together by
  // the scene decoded budget.
  const auto properties = [&](qsizetype offset, const Node &n) {
    const auto count = b.header == 25 ? b.read<quint64>(offset + 8)
                                      : b.read<quint32>(offset + 4);
    if (count > static_cast<quint64>(n.propertyEnd - n.properties))
      malformed();
    auto at = n.properties;
    for (quint64 i = 0; i < count; ++i) {
      if (at >= n.propertyEnd)
        malformed();
      const auto type = source[at++];
      const auto fixed = [&](qsizetype bytes) {
        if (bytes > n.propertyEnd - at)
          malformed();
        at += bytes;
      };
      switch (type) {
      case 'C': fixed(1); break;
      case 'Y': fixed(2); break;
      case 'I': case 'F': fixed(4); break;
      case 'D': case 'L': fixed(8); break;
      case 'S': case 'R': {
        fixed(4);
        fixed(static_cast<qsizetype>(b.read<quint32>(at - 4)));
        break;
      }
      case 'f': case 'i': case 'd': case 'l': case 'b': {
        fixed(12);
        const std::uint64_t length = b.read<quint32>(at - 12),
                            encoding = b.read<quint32>(at - 8),
                            stored = b.read<quint32>(at - 4);
        const std::uint64_t element =
            type == 'b' ? 1 : type == 'd' || type == 'l' ? 8 : 4;
        const auto bytes = length * element;
        if ((encoding == 0 && stored != bytes) ||
            (encoding == 1 && bytes > stored * 1032 + 64) || encoding > 1)
          malformed();
        if (bytes > decoded_budget - declared)
          throw skeletal::Failure(ErrorCode::ResourceLimit,
                                  "FBX declared array data exceeds the decoded budget");
        declared += bytes;
        fixed(static_cast<qsizetype>(stored));
        break;
      }
      default:
        malformed();
      }
    }
    if (at != n.propertyEnd)
      malformed();
  };
  // Child ranges still to walk: [first child, parent end). Records nest
  // strictly inside their parent, so every node is visited once.
  std::vector<std::pair<qsizetype, qsizetype>> pending;
  const auto walk = [&](qsizetype offset, qsizetype bound) {
    for (auto at = offset; at < bound;) {
      const auto n = b.node(at, bound);
      if (!n.end)
        return;
      properties(at, n);
      if (n.children < n.end)
        pending.push_back({n.children, n.end});
      at = n.end;
    }
  };
  try {
    walk(27, source.size());
    while (!pending.empty()) {
      const auto [children, end] = pending.back();
      pending.pop_back();
      walk(children, end);
    }
  } catch (const Invalid &) {
    malformed();
  }
}
} // namespace formaview::scene_export
