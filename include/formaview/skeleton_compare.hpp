#pragma once
#include "formaview/contracts.hpp"
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace formaview::skeletal {

// Identity view of one skeleton for structure comparison. Stable identity is
// (name, full parent-name chain); file-local indices are never compared.
struct BoneRef {
  std::uint32_t index{};
  std::string name;
  std::optional<std::uint32_t> parent; // CompareInput array offset, NOT index.
  bool has_default_transform{};
};

struct CompareInput {
  std::string source; // Provenance label only; never used for matching.
  std::vector<BoneRef> bones;
};

// Remove unnamed entries, lift their children to the nearest named ancestor,
// and remap parent offsets. BoneRef::index remains the original file identity.
// Malformed chains remain invalid for compare_skeletons to reject safely.
[[nodiscard]] CompareInput named_compare_input(CompareInput input);

// Applicability per the reviewed contract order: conflicts/structural
// mismatch -> Incompatible; required mapping not unique -> Ambiguous; partial
// coverage / target-only -> Partial; mapping unique but reference data
// insufficient -> Insufficient (caller combines); full evidence -> Direct.
// The engine judges structure and mapping uniqueness only; reference-data
// sufficiency is combined by the caller, so Direct here means
// "structure and mapping would allow direct application".
enum class Applicability {
  Incompatible,
  Ambiguous,
  Partial,
  StructuralDirect,
};

struct BonePair {
  std::uint32_t a{}, b{};
  bool parents_verified{};
  std::string note;
};

struct CompareReport {
  Applicability applicability{Applicability::Incompatible};
  bool hierarchy_matches{}; // Counts, names, roots and parent chains align.
  std::uint32_t bones_a{}, bones_b{};
  std::vector<BonePair> matched;
  std::vector<std::string> missing_in_b; // Present in A only.
  std::vector<std::string> extra_in_b;   // Present in B only (target-only).
  struct Duplicate {
    std::string name;
    std::uint32_t count_a{}, count_b{};
    bool resolved_by_chain{};
    bool ambiguous{};
  };
  std::vector<Duplicate> duplicates;
  std::vector<std::string> conflicts; // Parent-chain or root mismatches.
  std::vector<std::string> notes;     // Policy boundaries applied.

  [[nodiscard]] std::string summary() const;
};

// Compares two skeletons by exact names and full parent-name chains.
// Sibling order, array indices and clip duration are never structural
// differences. Name-ignore rules (namespace/prefix/case) are opt-in
// candidates and are NOT applied here; ambiguity is always reported.
[[nodiscard]] CompareReport compare_skeletons(const CompareInput &a,
                                              const CompareInput &b);

} // namespace formaview::skeletal
