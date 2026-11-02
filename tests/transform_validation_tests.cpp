#include "formaview/transform_validation.hpp"
#include <cmath>
#include <iostream>
#include <limits>
using namespace formaview::skeletal::transform;
namespace {
int checks=0, failures=0;
void check(const char *name, bool ok) { ++checks; if(!ok) { ++failures; std::cerr<<"FAIL "<<name<<'\n'; } }
Matrix identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
Evidence fixture(char hash='a') {
  Evidence e; e.sha256=std::string(64,hash);e.adapterVersion="synthetic-v1";
  e.fullNodeDomain=true;e.coordinates=Coordinates{1,{1,2,3}};
  e.nodes={{71,std::nullopt,Trs{},Evaluation{}},{800,0,Trs{},Evaluation{}}};
  e.nodes[1].restLocal->translation={0,1,0};
  e.bindings=std::vector<Binding>{{0,identity(),identity(),identity()}};
  return e;
}
const std::vector<Pair> map{{0,0},{1,1}};
bool reason(const Report &r,Verdict v,Reason why) {return r.verdict==v&&r.reason==why;}
}
int main() {
  auto a=fixture(),b=fixture();
  check("same_sha_shortcut",reason(validate(a,b,map),Verdict::Compatible,Reason::Identity));
  b.sha256=std::string(64,'b');
  check("complete_cross_file",validate(a,b,map).verdict==Verdict::Compatible);
  b.coordinates.reset();check("missing_coordinate",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.coordinates->metresPerUnit=.01;check("unit_conflict",reason(validate(a,b,map),Verdict::Incompatible,Reason::CoordinateConflict));b=fixture('b');
  b.coordinates->axes={1,-2,3};check("handedness",validate(a,b,map).verdict==Verdict::Incompatible);b=fixture('b');
  b.nodes[1].evaluation->rotationOrder=2;check("rotation_order",reason(validate(a,b,map),Verdict::Incompatible,Reason::EvaluationConflict));b=fixture('b');
  b.nodes[1].evaluation->rotationPivot={1,0,0};check("pivot",validate(a,b,map).verdict==Verdict::Incompatible);b=fixture('b');
  b.nodes[1].evaluation->inheritance=1;check("inheritance",validate(a,b,map).verdict==Verdict::Incompatible);b=fixture('b');
  b.nodes[1].evaluation.reset();check("missing_evaluation",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.nodes[1].restLocal->translation[0]=.01;check("rest_translation",reason(validate(a,b,map),Verdict::Incompatible,Reason::RestConflict));b=fixture('b');
  b.nodes[1].restLocal->rotation={0,0,std::sin(.01),std::cos(.01)};check("rest_rotation",validate(a,b,map).verdict==Verdict::Incompatible);b=fixture('b');
  b.nodes[1].restLocal->rotation={0,0,0,-1};check("quaternion_sign",validate(a,b,map).verdict==Verdict::Compatible);b=fixture('b');
  b.nodes[1].restLocal->scale[0]=2;check("rest_scale",validate(a,b,map).verdict==Verdict::Incompatible);b=fixture('b');
  (*b.bindings)[0].geometryToBone[12]=1;check("bind_not_default",reason(validate(a,b,map),Verdict::Incompatible,Reason::BindingConflict));
  {const auto r=validate(a,b,map);const auto &d=r.bindingFault;
   check("diag_closure_located",d&&d->fault==BindingFault::ClosureResidual&&d->closureSide==1&&d->matrix==BindingMatrix::GeometryWorld&&d->element==12&&d->targetInstance==0&&d->targetJoint==0&&!d->sourceJoint&&d->witness==0&&std::abs(d->residual-1)<1e-12);}
  b=fixture('b');
  check("diag_absent_when_compatible",!validate(a,b,map).bindingFault);
  b.bindings.reset();check("missing_binding",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.nodes[1].parent=99;check("bad_parent",reason(validate(a,b,map),Verdict::Insufficient,Reason::InvalidInput));b=fixture('b');
  b.nodes[0].parent=1;check("cycle",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.nodes[1].fileIndex=71;check("duplicate_file_identity",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.nodes[1].parent.reset();check("mapped_parent",reason(validate(a,b,map),Verdict::Incompatible,Reason::MappingConflict));b=fixture('b');
  check("duplicate_mapping",validate(a,b,std::vector<Pair>{{0,0},{1,0}}).verdict==Verdict::Insufficient);
  check("partial_mapping",validate(a,b,std::vector<Pair>{{0,0}}).verdict==Verdict::Insufficient);
  std::swap(b.nodes[0],b.nodes[1]);b.nodes[0].parent=1;b.nodes[1].parent.reset();(*b.bindings)[0].joint=1;(*b.bindings)[0].instance=1;
  check("reordered_file_indices",validate(a,b,std::vector<Pair>{{0,1},{1,0}}).verdict==Verdict::Compatible);b=fixture('b');
  a.format=b.format=Format::Actor;check("actor_cross_file",reason(validate(a,b,map),Verdict::Insufficient,Reason::UnsupportedFormat));
  b.sha256=a.sha256;check("actor_same_sha_legacy",validate(a,b,map).verdict==Verdict::Compatible);a=fixture();b=fixture('b');
  b.format=Format::Gltf;check("mixed_formats",validate(a,b,map).verdict==Verdict::Insufficient);a.format=Format::Gltf;
  check("gltf_same_format",validate(a,b,map).verdict==Verdict::Compatible);a=fixture();b=fixture('b');
  b.nodes[0].restLocal->translation[0]=std::numeric_limits<double>::quiet_NaN();check("nonfinite",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.nodes[0].restLocal->scale[0]=0;check("singular_scale",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  (*b.bindings)[0].bindWorld={};check("singular_binding",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  b.sha256="";check("empty_sha_not_identity",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture();
  b.nodes[0].restLocal.reset();check("same_sha_requires_defaults",validate(a,b,map).verdict==Verdict::Insufficient);b=fixture('b');
  Policy budget;budget.maxNodes=1;check("budget",reason(validate(a,b,map,budget),Verdict::Insufficient,Reason::BudgetExceeded));
  budget={};budget.rotationRadians=-1;check("bad_budget",validate(a,b,map,budget).verdict==Verdict::Insufficient);
  std::stop_source stop;stop.request_stop();check("cancel_precedes_identity",reason(validate(a,a,map,{},stop.get_token()),Verdict::Cancelled,Reason::Cancelled));
  a=fixture();b=fixture();a.coordinates.reset();b.coordinates.reset();
  b.nodes[1].restLocal->translation[0]=1;
  check("identity_inconsistent_defaults",validate(a,b,map).verdict!=Verdict::Compatible);
  a=fixture();b=fixture();b.format=Format::Gltf;
  check("identity_inconsistent_format",validate(a,b,map).verdict!=Verdict::Compatible);
  a=fixture();b=fixture('b');a.nodes[0].restLocal->scale[0]=1e-12;b.nodes[0].restLocal->scale[0]=2e-12;
  check("small_relative_scale",validate(a,b,map).verdict==Verdict::Incompatible);
  a=fixture();b=fixture('b');a.format=b.format=static_cast<Format>(99);
  check("invalid_format_enum",validate(a,b,map).verdict!=Verdict::Compatible);
  a=fixture();b=fixture('b');b.coordinates->axes={1,1,3};check("invalid_basis",validate(a,b,map).verdict==Verdict::Insufficient);
  b=fixture('b');b.nodes[0].restLocal->rotation={0,0,0,2};check("invalid_quaternion",validate(a,b,map).verdict==Verdict::Insufficient);
  b=fixture('b');b.bindings->push_back({0,identity(),identity(),identity()});(*b.bindings)[1].geometryToBone[12]=.2;
  check("all_instances_checked",validate(a,b,map).verdict==Verdict::Incompatible);
  b=fixture('b');(*b.bindings)[0].bindWorld[12]=3;(*b.bindings)[0].geometryToBone[12]=-2;(*b.bindings)[0].geometryWorld[12]=1;
  // This test isolates correct column-major closure with the SAME epoch;
  // cross-file different epochs have a dedicated rejection test below.
  a.bindings=b.bindings;
  check("binding_column_major_product",validate(a,b,map).verdict==Verdict::Compatible);a=fixture();
  b=fixture('b');b.nodes[1].restLocal->translation[0]=.5e-7;check("inside_metres_budget",validate(a,b,map).verdict==Verdict::Compatible);
  b.nodes[1].restLocal->translation[0]=2e-7;check("outside_metres_budget",validate(a,b,map).verdict==Verdict::Incompatible);
  b=fixture('b');b.nodes[1].restLocal->rotation={0,0,std::sin(1e-7),std::cos(1e-7)};
  check("small_angle_budget",validate(a,b,map).verdict==Verdict::Incompatible);
  b=fixture('b');b.coordinates.reset();b.nodes[0].evaluation->inheritance=1;
  check("known_conflict_over_missing",validate(a,b,map).verdict==Verdict::Incompatible);
  a=fixture();b=fixture();a.coordinates.reset();b.coordinates.reset();a.bindings.reset();b.bindings.reset();
  a.nodes[0].evaluation.reset();b.nodes[0].evaluation.reset();
  check("identity_with_legacy_evidence",validate(a,b,map).verdict==Verdict::Compatible);
  a=fixture();b=fixture();b.sha256=std::string(64,'A');check("hex_case_identity",validate(a,b,map).reason==Reason::Identity);
  b=fixture('b');b.fullNodeDomain=false;check("filtered_domain_rejected",validate(a,b,map).verdict==Verdict::Insufficient);
  a=fixture();b=fixture('b');a.nodes[1].parent.reset();b.nodes[1].parent.reset();
  check("multiple_roots",validate(a,b,map).verdict==Verdict::Compatible);
  a=fixture();b=fixture('b');a.nodes[0].restLocal->scale[0]=-1;b.nodes[0].restLocal->scale[0]=-1;
  check("matching_reflection",validate(a,b,map).verdict==Verdict::Compatible);
  a=fixture();b=fixture('b');a.nodes.clear();b.nodes.clear();a.bindings=std::vector<Binding>{};b.bindings=std::vector<Binding>{};std::vector<Pair> deep;
  for(std::size_t i=0;i<10000;++i){Node n{static_cast<std::uint32_t>(i+100),i?std::optional<std::size_t>(i-1):std::nullopt,Trs{},Evaluation{}};a.nodes.push_back(n);b.nodes.push_back(n);deep.push_back({i,i});}
  check("deep_chain_linear_no_recursion",validate(a,b,deep).verdict==Verdict::Compatible);
  a=fixture();b=fixture('b');a.format=b.format=Format::Actor;b.coordinates->metresPerUnit=.01;
  check("actor_never_runs_cross_file_transform_proof",reason(validate(a,b,map),Verdict::Insufficient,Reason::UnsupportedFormat));
  a=fixture();b=fixture('b');const auto provenance=validate(a,b,map);
  check("report_provenance",provenance.sourceSha==a.sha256&&provenance.targetSha==b.sha256&&provenance.sourceAdapter==a.adapterVersion&&provenance.mapping.size()==2&&provenance.policy.translationMetres==Policy{}.translationMetres);
  a=fixture();b=fixture('b');b.nodes[1].evaluation->rotationSpace=false;
  check("rotation_space_switch",reason(validate(a,b,map),Verdict::Incompatible,Reason::EvaluationConflict));
  b=fixture('b');a.nodes[1].inheritScaleParent=0;b.nodes[1].inheritScaleParent=0;
  check("scale_ancestor_same",validate(a,b,map).verdict==Verdict::Compatible);
  b.nodes[1].inheritScaleParent.reset();
  check("scale_ancestor_missing",validate(a,b,map).verdict==Verdict::Incompatible);
  b.nodes[1].inheritScaleParent=99;
  check("scale_ancestor_bounds",validate(a,b,map).verdict==Verdict::Insufficient);
  a=fixture();b=fixture('b');b.nodes[0].syntheticRoot=true;
  check("synthetic_root_role",validate(a,b,map).verdict==Verdict::Incompatible);
  a=fixture();b=fixture('b');a.format=b.format=Format::Gltf;
  a.nodes[0].restLocal.reset();b.nodes[0].restLocal.reset();
  a.nodes[0].restMatrix=identity();b.nodes[0].restMatrix=identity();
  check("static_matrix_without_decomposition",validate(a,b,map).verdict==Verdict::Compatible);
  (*b.nodes[0].restMatrix)[12]=1;
  check("static_matrix_conflict",validate(a,b,map).verdict==Verdict::Incompatible);
  b.sha256=a.sha256;(*b.nodes[0].restMatrix)[12]=1e-8;
  check("identity_matrix_translation_mismatch",reason(validate(a,b,map),Verdict::Incompatible,Reason::RestConflict));
  b.sha256=std::string(64,'b');
  check("cross_file_matrix_translation_tolerance",validate(a,b,map).verdict==Verdict::Compatible);
  a=fixture();b=fixture();a.format=b.format=Format::Gltf;
  a.nodes[0].restLocal.reset();a.nodes[0].restMatrix=identity();
  check("identity_mixed_equal_values_still_unproven",reason(validate(a,b,map),Verdict::Insufficient,Reason::MissingEvidence));
  b.sha256=std::string(64,'b');
  check("cross_file_mixed_rest_still_unproven",reason(validate(a,b,map),Verdict::Insufficient,Reason::MissingEvidence));
  b.sha256=a.sha256;
  b.nodes[0].restLocal->translation[0]=10;
  check("identity_mixed_rest_representation_not_proven",reason(validate(a,b,map),Verdict::Insufficient,Reason::MissingEvidence));
  b.nodes[1].restLocal->translation[0]=2;
  check("identity_mixed_rest_keeps_known_conflict",reason(validate(a,b,map),Verdict::Incompatible,Reason::RestConflict));
  a=fixture();b=fixture('b');a.format=b.format=Format::Gltf;a.bindings.reset();b.bindings.reset();
  a.nativeSkinBindings=std::vector<NativeSkinBinding>{{0,1,identity()}};b.nativeSkinBindings=a.nativeSkinBindings;
  check("gltf_native_operator_proof",validate(a,b,map).verdict==Verdict::Compatible);
  (*b.nativeSkinBindings)[0].inverseBind[12]=1;
  check("gltf_native_operator_conflict",reason(validate(a,b,map),Verdict::Incompatible,Reason::BindingConflict));
  {const auto d=validate(a,b,map).bindingFault;
   check("diag_native_operator_located",d&&d->fault==BindingFault::OperatorResidual&&d->native&&d->matrix==BindingMatrix::InverseBind&&d->element==12&&d->sourceInstance==0&&d->sourceJoint==1&&d->targetJoint==1);}
  b.nativeSkinBindings=a.nativeSkinBindings;b.nativeSkinBindings->push_back(b.nativeSkinBindings->front());
  check("gltf_duplicate_binding",validate(a,b,map).verdict==Verdict::Insufficient);
  // Invalid evidence must identify the offending array-domain node; a
  // same-SHA self comparison must not bypass quaternion validity.
  a=fixture();b=fixture('b');a.format=b.format=Format::Gltf;
  a.nodes[1].restLocal->rotation={0,0,0,1.000001};
  auto badSource=validate(a,b,map);
  check("invalid_source_node_diagnostic",badSource.verdict==Verdict::Insufficient&&badSource.reason==Reason::InvalidInput&&badSource.sourceNode==1&&!badSource.targetNode);
  a=fixture();a.format=Format::Gltf;b.nodes[0].restLocal->rotation={0,0,0,1.000001};
  auto badTarget=validate(a,b,map);
  check("invalid_target_node_diagnostic",badTarget.verdict==Verdict::Insufficient&&badTarget.reason==Reason::InvalidInput&&!badTarget.sourceNode&&badTarget.targetNode==0);
  check("nonunit_identity_still_rejected",validate(b,b,map).verdict==Verdict::Insufficient);
  // Both epochs close independently, but the source local transform acts
  // through different geometry-to-bone operators. Default pose is unchanged.
  a=fixture();b=fixture('b');
  (*b.bindings)[0].bindWorld[12]=2;
  (*b.bindings)[0].geometryToBone[12]=-2;
  check("cross_file_different_valid_bind_epoch",reason(validate(a,b,map),Verdict::Incompatible,Reason::BindingConflict));
  {const auto d=validate(a,b,map).bindingFault;
   check("diag_cross_file_operator_located",d&&d->fault==BindingFault::OperatorResidual&&!d->native&&d->closureSide==0&&d->matrix==BindingMatrix::BindWorld&&d->element==12&&d->witness==0&&d->sourceInstance==0&&d->targetInstance==0);}
  b=fixture('b');b.bindings->clear();
  check("cross_file_missing_binding_entry",validate(a,b,map).verdict!=Verdict::Compatible);
  a=fixture();b=fixture('b');(*b.bindings)[0].instance=99;
  check("fbx_binding_instance_bounds",reason(validate(a,b,map),Verdict::Insufficient,Reason::InvalidInput));
  b=fixture('b');(*b.bindings)[0].instance=1;
  check("fbx_binding_instance_mapping",reason(validate(a,b,map),Verdict::Incompatible,Reason::BindingConflict));
  {const auto d=validate(a,b,map).bindingFault;
   check("diag_instance_missing_peer",d&&d->fault==BindingFault::MissingPeer&&!d->native&&d->sourceInstance==0&&d->sourceJoint==0&&d->targetInstance==0&&d->targetJoint==0&&d->witness==0);}
  b=fixture('b');b.bindings->push_back((*b.bindings)[0]);b.bindings->back().witness=1;
  check("diag_witness_count_mismatch",[&]{const auto r=validate(a,b,map);return reason(r,Verdict::Incompatible,Reason::BindingConflict)&&r.bindingFault&&r.bindingFault->fault==BindingFault::CountMismatch&&r.bindingFault->sourceCount==1&&r.bindingFault->targetCount==2;}());
  b=fixture('b');b.bindings->push_back(b.bindings->front());
  check("fbx_binding_duplicate_witness",validate(a,b,map).verdict!=Verdict::Compatible);
  b=fixture();(*b.bindings)[0].bindWorld[12]=2;(*b.bindings)[0].geometryToBone[12]=-2;
  check("same_sha_different_bind_epoch",validate(a,b,map).verdict!=Verdict::Compatible);
  b=fixture('b');(*b.bindings)[0].witness=2;
  check("fbx_binding_unknown_witness",reason(validate(a,b,map),Verdict::Insufficient,Reason::InvalidInput));
  a=fixture();check("fbx_same_object_native_binding",validate(a,a,map).verdict==Verdict::Compatible);
  a.format=Format::Gltf;a.bindings.reset();a.nativeSkinBindings=std::vector<NativeSkinBinding>{{0,1,identity()}};
  check("gltf_same_object_native_binding",validate(a,a,map).verdict==Verdict::Compatible);
  a=fixture();b=fixture('b');b.bindings->clear();
  check("pure_animation_binding_unknown_not_conflict",reason(validate(a,b,map),Verdict::Insufficient,Reason::MissingEvidence));
  std::cout<<"checks "<<checks<<" failures "<<failures<<'\n';return failures?1:0;
}
