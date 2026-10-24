// M07 skeleton structure comparison tests. Synthetic cases run without
// arguments (ctest-safe); real read-only samples run when paths are given:
//   skeleton_compare_tests <fbxA> <fbxB> <psa> <psk> <output.json>
#include "formaview/skeleton_compare.hpp"
#ifdef FORMAVIEW_HAVE_FBX
#include "formaview/fbx.hpp"
#endif
#include "formaview/skeletal.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
using formaview::skeletal::Applicability;
using formaview::skeletal::BoneRef;
using formaview::skeletal::CompareInput;
using formaview::skeletal::CompareReport;
using formaview::skeletal::compare_skeletons;

struct Case {
  std::string name;
  CompareInput a, b;
  Applicability expected;
  bool expectHierarchy{};
};

CompareInput make(const std::vector<std::string> &names,
                  const std::vector<int> &parents) {
  CompareInput input;
  for (std::size_t i = 0; i < names.size(); ++i) {
    BoneRef bone;
    bone.index = static_cast<std::uint32_t>(i);
    bone.name = names[i];
    if (parents[i] >= 0)
      bone.parent = static_cast<std::uint32_t>(parents[i]);
    bone.has_default_transform = true;
    input.bones.push_back(bone);
  }
  return input;
}

int failures = 0;
void check(const std::string &name, bool condition,
           const std::string &detail = {}) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << name << (detail.empty() ? "" : ": " + detail)
              << "\n";
  } else {
    std::cout << "pass " << name << "\n";
  }
}

void runSynthetic() {
  {
    auto raw = make({"", "root", "", "tip"}, {-1, 0, 1, 2});
    raw.bones[1].index = 101;
    raw.bones[3].index = 303;
    const auto named = formaview::skeletal::named_compare_input(raw);
    check("filter.file_indices", named.bones.size() == 2 &&
          named.bones[0].index == 101 && named.bones[1].index == 303);
    check("filter.lift_parent", !named.bones[0].parent && named.bones[1].parent == 0);
    check("filter.self", compare_skeletons(named, named).hierarchy_matches);
    const auto bad = make({"root", "tip", "tip"}, {-1, 999, 0});
    check("invalid.parent", compare_skeletons(bad, bad).applicability == Applicability::Incompatible);
    const auto cycle = make({"root", "tip"}, {1, 0});
    check("invalid.cycle", compare_skeletons(cycle, cycle).applicability == Applicability::Incompatible);
    check("invalid.empty", compare_skeletons({}, {}).applicability == Applicability::Incompatible);
  }
  {
    std::vector<std::string> repeated(2000,"joint");
    std::vector<int> deep(2000);
    for (int i=0;i<2000;++i) deep[i]=i-1;
    const auto chain=make(repeated,deep);
    const auto result=compare_skeletons(chain,chain);
    check("duplicate.deep_chain",result.hierarchy_matches && result.matched.size()==2000 &&
          result.applicability==Applicability::StructuralDirect);
  }
  // 1. Same skeleton with reordered arrays: order and indices are never
  //    structural differences.
  const std::vector<std::string> names{"root", "pelvis", "spine", "arm_L"};
  const std::vector<int> parents{-1, 0, 1, 2};
  CompareInput a = make(names, parents);
  CompareInput b;
  b.bones = {a.bones[2], a.bones[0], a.bones[3], a.bones[1]};
  // Rebuild indices/parents for the shuffled copy.
  const std::vector<std::uint32_t> order{2, 0, 3, 1};
  for (std::size_t i = 0; i < b.bones.size(); ++i) {
    b.bones[i].index = static_cast<std::uint32_t>(i);
    b.bones[i].parent.reset();
  }
  for (std::size_t i = 0; i < order.size(); ++i) {
    const auto original = a.bones[order[i]];
    if (original.parent) {
      const auto it = std::find(order.begin(), order.end(), *original.parent);
      b.bones[i].parent =
          static_cast<std::uint32_t>(it - order.begin());
    }
  }
  const auto report = compare_skeletons(a, b);
  check("reorder.structural_direct",
        report.applicability == Applicability::StructuralDirect,
        report.summary());
  check("reorder.hierarchy", report.hierarchy_matches);

  // 2. Duplicate name resolvable by distinct parent chains.
  CompareInput dupA = make({"root", "chainA", "shoulder", "chainB", "shoulder"},
                           {-1, 0, 1, 0, 3});
  CompareInput dupB = make({"root", "chainB", "shoulder", "chainA", "shoulder"},
                           {-1, 0, 1, 0, 3});
  const auto dupReport = compare_skeletons(dupA, dupB);
  check("duplicate.resolved_by_chain",
        dupReport.applicability == Applicability::StructuralDirect &&
            dupReport.duplicates.size() == 1 &&
            dupReport.duplicates[0].resolved_by_chain,
        dupReport.summary());

  // 3. Duplicate name with identical chains stays ambiguous, never guessed.
  CompareInput ambA = make({"root", "shoulder", "shoulder"}, {-1, 0, 0});
  CompareInput ambB = make({"root", "shoulder", "shoulder"}, {-1, 0, 0});
  const auto ambReport = compare_skeletons(ambA, ambB);
  check("duplicate.ambiguous",
        ambReport.applicability == Applicability::Ambiguous &&
            ambReport.duplicates[0].ambiguous,
        ambReport.summary());

  // 4. Parent conflict is incompatible.
  const auto unmatchedDuplicate = compare_skeletons(
      make({"root", "branch", "tip"}, {-1, 0, 0}),
      make({"root", "branch", "tip", "tip"}, {-1, 0, 0, 1}));
  check("duplicate.unmatched_external", unmatchedDuplicate.applicability == Applicability::Ambiguous &&
        !unmatchedDuplicate.hierarchy_matches && unmatchedDuplicate.duplicates[0].ambiguous);

  CompareInput conA = make({"root", "x", "y"}, {-1, 0, 1});
  CompareInput conB = make({"root", "x", "y"}, {-1, 0, 0});
  const auto conReport = compare_skeletons(conA, conB);
  check("parent.conflict_incompatible",
        conReport.applicability == Applicability::Incompatible &&
            !conReport.conflicts.empty(),
        conReport.summary());

  // 5. Missing and extra bones are partial with reported scope.
  CompareInput partA = make({"root", "a", "b", "c"}, {-1, 0, 0, 1});
  CompareInput partB = make({"root", "a", "b", "z"}, {-1, 0, 0, 1});
  const auto partReport = compare_skeletons(partA, partB);
  check("scope.partial",
        partReport.applicability == Applicability::Partial &&
            partReport.missing_in_b.size() == 1 &&
            partReport.extra_in_b.size() == 1,
        partReport.summary());
}

#ifdef FORMAVIEW_HAVE_FBX
CompareInput fromFbx(const std::filesystem::path &path, std::string &error) {
  CompareInput input;
  try {
    const auto document = formaview::skeletal::FbxDocument::read(path);
    input.source = path.string();
    const auto &data = document.data();
    for (std::uint32_t i = 0; i < data.nodes.size(); ++i) {
      const auto &node = data.nodes[i];
      BoneRef bone;
      bone.index = i;
      bone.name = node.source.original_name;
      bone.parent = node.parent;
      bone.has_default_transform = true;
      input.bones.push_back(bone);
    }
  } catch (const std::exception &failure) {
    error = failure.what();
  }
  return input;
}
#endif

CompareInput fromActor(
    const formaview::skeletal::ActorAsset &asset,
    const std::vector<std::optional<std::uint32_t>> *referenceParents = nullptr) {
  // PSA raw parent records are not trusted structure; a reference skeleton
  // (Pairing.parents) supplies the meaningful hierarchy when available.
  CompareInput input;
  for (std::uint32_t i = 0; i < asset.bones.size(); ++i) {
    const auto &bone = asset.bones[i];
    BoneRef ref;
    ref.index = i;
    ref.name = bone.source.original_name;
    ref.parent = bone.parent;
    if (referenceParents && i < referenceParents->size())
      ref.parent = (*referenceParents)[i];
    ref.has_default_transform = bone.default_local_valid;
    input.bones.push_back(ref);
  }
  return input;
}

QJsonObject toJson(const CompareReport &report) {
  QJsonArray matched, missing, extra, duplicates, conflicts, notes;
  for (const auto &pair : report.matched)
    matched.append(QJsonObject{{"a", static_cast<int>(pair.a)},
                               {"b", static_cast<int>(pair.b)},
                               {"parents_verified", pair.parents_verified}});
  for (const auto &name : report.missing_in_b)
    missing.append(QString::fromStdString(name));
  for (const auto &name : report.extra_in_b)
    extra.append(QString::fromStdString(name));
  for (const auto &duplicate : report.duplicates)
    duplicates.append(QJsonObject{
        {"name", QString::fromStdString(duplicate.name)},
        {"count_a", static_cast<int>(duplicate.count_a)},
        {"count_b", static_cast<int>(duplicate.count_b)},
        {"resolved_by_chain", duplicate.resolved_by_chain},
        {"ambiguous", duplicate.ambiguous}});
  for (const auto &conflict : report.conflicts)
    conflicts.append(QString::fromStdString(conflict));
  for (const auto &note : report.notes)
    notes.append(QString::fromStdString(note));
  return QJsonObject{{"applicability", QString::fromStdString(report.summary())},
                     {"hierarchy_matches", report.hierarchy_matches},
                     {"bones_a", static_cast<int>(report.bones_a)},
                     {"bones_b", static_cast<int>(report.bones_b)},
                     {"matched", matched},
                     {"missing_in_b", missing},
                     {"extra_in_b", extra},
                     {"duplicates", duplicates},
                     {"conflicts", conflicts},
                     {"notes", notes}};
}

// M07 acceptance: two 256-bone skeletons compare in P95 <= 200 ms once data
// is ready (parse time excluded). Shapes cover a balanced tree, a deepest
// chain and sibling-duplicate names disambiguated only by parent chains; B is
// index-reversed so no shortcut by identical ordering is possible.
void runScalePerformance() {
  constexpr int bones = 256, iterations = 200;
  const auto build = [&](const char *shape) {
    std::vector<std::string> names;
    std::vector<int> parents;
    for (int i = 0; i < bones; ++i) {
      const std::string shapeName = shape;
      if (shapeName == "chain") {
        names.push_back("bone_" + std::to_string(i));
        parents.push_back(i - 1);
      } else if (shapeName == "tree") {
        names.push_back("bone_" + std::to_string(i));
        parents.push_back(i == 0 ? -1 : (i - 1) / 2);
      } else {
        // Twins: 16 recurring names, each unique only through its chain.
        names.push_back("part_" + std::to_string(i % 16));
        parents.push_back(i == 0 ? -1 : (i - 1) / 2);
      }
    }
    return std::pair{names, parents};
  };
  const auto reversed = [&](const std::vector<std::string> &names,
                            const std::vector<int> &parents) {
    // Reverse the array order and remap parent offsets accordingly.
    std::vector<std::string> n(names.rbegin(), names.rend());
    std::vector<int> p(parents.size());
    for (int i = 0; i < bones; ++i) {
      const int old = bones - 1 - i;
      p[i] = parents[old] < 0 ? -1 : bones - 1 - parents[old];
    }
    return make(n, p);
  };
  for (const char *shape : {"tree", "chain", "twins"}) {
    const auto [names, parents] = build(shape);
    const auto a = make(names, parents);
    const auto b = reversed(names, parents);
    std::vector<double> ms;
    CompareReport last;
    for (int i = 0; i < iterations; ++i) {
      const auto begin = std::chrono::steady_clock::now();
      const auto aa = formaview::skeletal::named_compare_input(a);
      const auto bb = formaview::skeletal::named_compare_input(b);
      last = compare_skeletons(aa, bb);
      ms.push_back(std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - begin)
                       .count());
    }
    std::sort(ms.begin(), ms.end());
    const double p95 = ms[static_cast<std::size_t>(iterations * 0.95) - 1];
    const double p99 = ms[static_cast<std::size_t>(iterations * 0.99) - 1];
    std::cout << "scale " << shape << " bones=" << bones
              << " p95_ms=" << p95 << " p99_ms=" << p99
              << " max_ms=" << ms.back() << "\n";
    check(std::string("scale.p95_under_200ms.") + shape, p95 <= 200.0,
          std::to_string(p95));
    if (std::string(shape) != "twins") {
      check(std::string("scale.structural_direct.") + shape,
            last.applicability == Applicability::StructuralDirect &&
                last.matched.size() == static_cast<std::size_t>(bones),
            std::to_string(last.matched.size()));
    }
  }
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  runSynthetic();
  runScalePerformance();
  if (argc == 6) {
#ifdef FORMAVIEW_HAVE_FBX
    const std::filesystem::path fbxA(argv[1]), fbxB(argv[2]), psa(argv[3]),
                                 psk(argv[4]);
    const QString output = QString::fromWCharArray(argv[5]);
    QJsonObject evidence;
    std::string errorA, errorB;
    const auto inputA = fromFbx(fbxA, errorA);
    const auto inputB = fromFbx(fbxB, errorB);
    if (errorA.empty() && errorB.empty() && !inputA.bones.empty() &&
        !inputB.bones.empty()) {
      evidence["cloud_target_vs_animation"] = toJson(compare_skeletons(inputA, inputB));
    } else {
      evidence["cloud_target_vs_animation"] =
          QString::fromStdString("skipped: " + errorA + errorB);
    }
    try {
      const auto animation = formaview::skeletal::read_actor(psa);
      const auto mesh = formaview::skeletal::read_actor(psk);
      evidence["anke_psa_vs_psk"] =
          toJson(compare_skeletons(fromActor(animation), fromActor(mesh)));
      // Cross-check against the audited M02 pairing contract.
      const auto pairing = formaview::skeletal::pair(
          animation, mesh, 1e-4,
          formaview::skeletal::PairingPolicy::ReferenceSkeleton);
      evidence["m02_pair_compatible"] = pairing.compatible;
      evidence["m02_pair_reasons"] = static_cast<int>(pairing.reasons.size());
      if (pairing.compatible) {
        // Positive real case: PSA hierarchy via the reference PSK.
        evidence["anke_psa_vs_psk_reference_parents"] = toJson(
            compare_skeletons(fromActor(animation, &pairing.parents),
                              fromActor(mesh)));
      }
    } catch (const std::exception &failure) {
      evidence["actor_error"] = failure.what();
    }
    QFile file(output);
    if (file.open(QIODevice::WriteOnly))
      file.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
#else
    std::cerr << "FBX target unavailable in this build; real cases skipped\n";
#endif
  }
  if (failures) {
    std::cerr << failures << " synthetic check(s) failed\n";
    return 1;
  }
  std::cout << "skeleton compare tests complete\n";
  return 0;
}
