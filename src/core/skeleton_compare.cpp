#include "formaview/skeleton_compare.hpp"
#include <algorithm>
#include <map>
#include <limits>
#include <set>

namespace formaview::skeletal {
CompareInput named_compare_input(CompareInput input) {
  constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> remap(input.bones.size(), invalid);
  CompareInput result;
  result.source = std::move(input.source);
  for (std::size_t i = 0; i < input.bones.size(); ++i)
    if (!input.bones[i].name.empty()) {
      remap[i] = static_cast<std::uint32_t>(result.bones.size());
      result.bones.push_back(input.bones[i]);
    }
  for (auto &bone : result.bones) {
    auto parent = bone.parent;
    std::size_t steps = 0;
    while (parent && *parent < input.bones.size() && remap[*parent] == invalid &&
           steps++ < input.bones.size())
      parent = input.bones[*parent].parent;
    bone.parent = parent ? std::optional<std::uint32_t>(
                               *parent < remap.size() ? remap[*parent] : invalid)
                         : std::nullopt;
  }
  return result;
}

std::string CompareReport::summary() const {
  switch (applicability) {
  case Applicability::Incompatible:
    return "Incompatible: parent structure, roots or required chains conflict";
  case Applicability::Ambiguous:
    return "Ambiguous: a required name mapping is not unique";
  case Applicability::Partial:
    return "Partial: missing or extra bones; influence scope reported";
  case Applicability::StructuralDirect:
    return "Structural direct: unique mapping, aligned chains; reference "
           "sufficiency is judged separately";
  }
  return {};
}

CompareReport compare_skeletons(const CompareInput &a, const CompareInput &b) {
  CompareReport report;
  report.bones_a = static_cast<std::uint32_t>(a.bones.size());
  report.bones_b = static_cast<std::uint32_t>(b.bones.size());

  // Validate before chain traversal, including duplicate-name disambiguation.
  // Iterative colouring bounds the work and avoids recursion on deep chains.
  const auto valid = [](const CompareInput &input) {
    if (input.bones.empty()) return false;
    std::vector<unsigned char> colour(input.bones.size());
    for (std::uint32_t start = 0; start < input.bones.size(); ++start) {
      if (colour[start] == 2) continue;
      std::vector<std::uint32_t> path;
      std::optional<std::uint32_t> at = start;
      while (at) {
        if (*at >= input.bones.size() || colour[*at] == 1) return false;
        if (colour[*at] == 2) break;
        colour[*at] = 1;
        path.push_back(*at);
        at = input.bones[*at].parent;
      }
      for (auto index : path) colour[index] = 2;
    }
    return true;
  };
  if (!valid(a) || !valid(b)) {
    report.conflicts.push_back("Empty skeleton or invalid/cyclic parent offsets");
    return report;
  }

  // Exact-name groups on both sides. Indices and sibling order never match.
  std::map<std::string, std::vector<std::uint32_t>> byNameA, byNameB;
  for (std::uint32_t i = 0; i < a.bones.size(); ++i)
    byNameA[a.bones[i].name].push_back(i);
  for (std::uint32_t i = 0; i < b.bones.size(); ++i)
    byNameB[b.bones[i].name].push_back(i);

  // Duplicate-name groups are recorded up front.
  for (const auto &[name, group] : byNameA) {
    const auto found = byNameB.find(name);
    const std::uint32_t countB =
        found == byNameB.end() ? 0u
                               : static_cast<std::uint32_t>(found->second.size());
    if (group.size() > 1 || countB > 1) {
      CompareReport::Duplicate duplicate;
      duplicate.name = name;
      duplicate.count_a = static_cast<std::uint32_t>(group.size());
      duplicate.count_b = countB;
      report.duplicates.push_back(duplicate);
    }
  }
  for (const auto &[name, group] : byNameB) {
    if (byNameA.find(name) == byNameA.end() && group.size() > 1) {
      CompareReport::Duplicate duplicate;
      duplicate.name = name;
      duplicate.count_a = 0;
      duplicate.count_b = static_cast<std::uint32_t>(group.size());
      report.duplicates.push_back(duplicate);
    }
  }

  // Name membership differences (exact names only).
  for (const auto &[name, group] : byNameA)
    if (byNameB.find(name) == byNameB.end())
      report.missing_in_b.insert(report.missing_in_b.end(), group.size(), name);
  for (const auto &[name, group] : byNameB)
    if (byNameA.find(name) == byNameA.end())
      report.extra_in_b.insert(report.extra_in_b.end(), group.size(), name);

  // Intern parent/name paths across both inputs. Each node is processed once;
  // repeated names on deep chains must not trigger N*N full-chain walks on GUI.
  std::map<std::pair<std::size_t,std::string>,std::size_t> paths;
  const auto signatures = [&](const CompareInput &input) {
    std::vector<std::size_t> ids(input.bones.size());
    for (std::uint32_t start=0;start<input.bones.size();++start) {
      std::vector<std::uint32_t> pending;
      std::optional<std::uint32_t> at=start;
      while (at && !ids[*at]) { pending.push_back(*at); at=input.bones[*at].parent; }
      for (auto it=pending.rbegin();it!=pending.rend();++it) {
        const auto &bone=input.bones[*it];
        const auto parent=bone.parent ? ids[*bone.parent] : 0;
        ids[*it]=paths.emplace(std::pair{parent,bone.name},paths.size()+1).first->second;
      }
    }
    return ids;
  };
  const auto pathsA=signatures(a), pathsB=signatures(b);

  // Match shared names. Unique<->unique is a direct candidate; duplicate
  // groups are matched by full parent-name chain when that is unambiguous.
  std::map<std::uint32_t, std::uint32_t> aToB;
  std::set<std::string> ambiguousNames;
  bool ambiguous = false;
  for (const auto &[name, groupA] : byNameA) {
    const auto found = byNameB.find(name);
    if (found == byNameB.end())
      continue;
    const auto &groupB = found->second;
    if (groupA.size() == 1 && groupB.size() == 1) {
      aToB[groupA.front()] = groupB.front();
      continue;
    }
    // Duplicate name: chain disambiguation only, never guess by order.
    std::map<std::size_t,std::vector<std::uint32_t>> byPathB;
    for (const auto ib : groupB) byPathB[pathsB[ib]].push_back(ib);
    bool groupResolved = true;
    for (std::uint32_t ia : groupA) {
      const auto matches = byPathB.find(pathsA[ia]);
      if (matches != byPathB.end() && matches->second.size() == 1) {
        aToB[ia] = matches->second.front();
      } else {
        groupResolved = false;
      }
    }
    // A B-side bone claimed by two A bones, or unmatched duplicates, stays
    // ambiguous; it is never silently resolved.
    std::set<std::uint32_t> claimed;
    for (const std::uint32_t ia : groupA) {
      const auto m = aToB.find(ia);
      if (m != aToB.end() && !claimed.insert(m->second).second)
        groupResolved = false;
    }
    if (claimed.size() != groupB.size())
      groupResolved = false;
    for (auto &duplicate : report.duplicates)
      if (duplicate.name == name) {
        duplicate.resolved_by_chain = groupResolved;
        duplicate.ambiguous = !groupResolved;
      }
    if (!groupResolved) {
      ambiguous = true;
      ambiguousNames.insert(name);
    }
  }

  // Parent verification through the name mapping; roots must correspond.
  bool conflicts = false;
  for (const auto &[ia, ib] : aToB) {
    const auto &boneA = a.bones[ia];
    const auto &boneB = b.bones[ib];
    bool verified = false;
    bool parentAmbiguous = false;
    if (!boneA.parent && !boneB.parent) {
      verified = true; // Both roots.
    } else if (boneA.parent && boneB.parent) {
      const auto mapped = aToB.find(*boneA.parent);
      verified = mapped != aToB.end() && mapped->second == *boneB.parent;
      parentAmbiguous = !verified &&
          pathsA[*boneA.parent] == pathsB[*boneB.parent] &&
          ambiguousNames.contains(a.bones[*boneA.parent].name);
    }
    report.matched.push_back({ia, ib, verified,
        parentAmbiguous ? "Parent belongs to an unresolved duplicate-name group" : ""});
    if (!verified && !parentAmbiguous) {
      conflicts = true;
      report.conflicts.push_back(
          boneA.name + " (#" + std::to_string(ia) + "): parent chain " +
          (boneA.parent ? "mismatch" : "is root on A only"));
    }
  }

  report.hierarchy_matches = report.missing_in_b.empty() &&
                             report.extra_in_b.empty() && !conflicts &&
                             !ambiguous &&
                             report.bones_a == report.bones_b;

  // Conservative applicability in the reviewed order.
  if (conflicts)
    report.applicability = Applicability::Incompatible;
  else if (ambiguous)
    report.applicability = Applicability::Ambiguous;
  else if (!report.missing_in_b.empty() || !report.extra_in_b.empty())
    report.applicability = Applicability::Partial;
  else
    report.applicability = Applicability::StructuralDirect;

  report.notes.push_back(
      "Sibling order, array indices and clip duration are not compared; name "
      "ignoring rules are opt-in candidates and stay unused.");
  report.notes.push_back(
      "Auxiliary chain folding is not applied; actual chains are kept.");
  return report;
}

} // namespace formaview::skeletal
