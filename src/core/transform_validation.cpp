#include "formaview/transform_validation.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>
#include <tuple>

namespace formaview::skeletal::transform {
namespace {
template<std::size_t N> bool finite(const std::array<double,N> &a) {
  return std::all_of(a.begin(),a.end(),[](double x){return std::isfinite(x);});
}
bool quaternion(const Quat &q) {
  if(!finite(q)) return false;
  double sum=0;for(double x:q) sum+=x*x;
  return std::isfinite(sum) && std::abs(sum-1)<=1e-10;
}
double angle(const Quat &a,const Quat &b) {
  // atan2 of chord lengths avoids acos cancellation near identical rotations.
  double dot=0;for(std::size_t i=0;i<4;++i) dot+=a[i]*b[i];
  double minus=0,plus=0;
  for(std::size_t i=0;i<4;++i) {const double x=dot<0?-b[i]:b[i];minus+=(a[i]-x)*(a[i]-x);plus+=(a[i]+x)*(a[i]+x);}
  return 4*std::atan2(std::sqrt(minus),std::sqrt(plus));
}
bool valid(const Trs &t) {
  return finite(t.translation)&&quaternion(t.rotation)&&finite(t.scale)&&
    std::all_of(t.scale.begin(),t.scale.end(),[](double x){return x!=0;});
}
bool valid(const Evaluation &e) {
  return e.rotationOrder<=5&&e.inheritance<=2&&quaternion(e.preRotation)&&
    quaternion(e.postRotation)&&finite(e.rotationPivot)&&finite(e.scalingPivot)&&
    finite(e.rotationOffset)&&finite(e.scalingOffset);
}
bool valid(const Coordinates &c) {
  if(!std::isfinite(c.metresPerUnit)||c.metresPerUnit<=0)return false;
  unsigned used=0;
  for(int axis:c.axes){if(axis==0||axis < -3||axis>3)return false;const unsigned bit=1u<<static_cast<unsigned>(axis<0?-axis:axis);if(used&bit)return false;used|=bit;}
  return true;
}
bool affine(const Matrix &m) {
  if(!finite(m)||m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
  // Scale each column before determinant evaluation; reject ill-conditioned bases.
  std::array<Vec3,3> c{};
  for(std::size_t j=0;j<3;++j){double n=0;for(std::size_t i=0;i<3;++i)n=std::max(n,std::abs(m[j*4+i]));if(n==0)return false;for(std::size_t i=0;i<3;++i)c[j][i]=m[j*4+i]/n;}
  const double det=c[0][0]*(c[1][1]*c[2][2]-c[1][2]*c[2][1])-c[1][0]*(c[0][1]*c[2][2]-c[0][2]*c[2][1])+c[2][0]*(c[0][1]*c[1][2]-c[0][2]*c[1][1]);
  return std::abs(det)>1e-12;
}
bool hash(const std::string &s) {
  return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');});
}
std::string canonicalHash(std::string s) {for(char &c:s)if(c>='A'&&c<='F')c=static_cast<char>(c-'A'+'a');return s;}
bool graph(const Evidence &e,std::stop_token stop,std::optional<std::size_t> &invalidNode) {
  std::unordered_set<std::uint32_t> ids;
  for(std::size_t i=0;i<e.nodes.size();++i){if(stop.stop_requested())return false;invalidNode=i;const auto &n=e.nodes[i];if(!ids.insert(n.fileIndex).second||((!n.restLocal&&!n.restMatrix)||(n.restLocal&&n.restMatrix)||(n.restLocal&&!valid(*n.restLocal))||(n.restMatrix&&(e.format!=Format::Gltf||!affine(*n.restMatrix))))||(n.parent&&*n.parent>=e.nodes.size())||(n.inheritScaleParent&&*n.inheritScaleParent>=e.nodes.size())||(n.evaluation&&!valid(*n.evaluation)))return false;}
  invalidNode.reset();
  std::vector<unsigned char> state(e.nodes.size());
  for(std::size_t start=0;start<e.nodes.size();++start){
    if(stop.stop_requested())return false;
    std::optional<std::size_t> at=start;
    while(at&&state[*at]==0){if(stop.stop_requested())return false;state[*at]=1;at=e.nodes[*at].parent;}
    if(at&&state[*at]==1){invalidNode=*at;return false;}
    at=start;while(at&&state[*at]==1){state[*at]=2;at=e.nodes[*at].parent;}
  }
  return true;
}
}
Report validate(const Evidence &a,const Evidence &b,std::span<const Pair> mapping,
                const Policy &policy,std::stop_token stop) {
  Report out;out.sourceSha=a.sha256;out.targetSha=b.sha256;
  out.sourceAdapter=a.adapterVersion;out.targetAdapter=b.adapterVersion;out.policy=policy;
  auto result=[&](Verdict v,Reason why){out.verdict=v;out.reason=why;return out;};
  auto cancelled=[&]{return result(Verdict::Cancelled,Reason::Cancelled);};
  auto invalid=[&]{return result(Verdict::Insufficient,Reason::InvalidInput);};
  if(stop.stop_requested())return cancelled();
  if(a.nodes.size()>policy.maxNodes||b.nodes.size()>policy.maxNodes||mapping.size()>policy.maxNodes||
     (a.bindings&&a.bindings->size()>policy.maxBindings)||(b.bindings&&b.bindings->size()>policy.maxBindings)||
     (a.nativeSkinBindings&&a.nativeSkinBindings->size()>policy.maxBindings)||(b.nativeSkinBindings&&b.nativeSkinBindings->size()>policy.maxBindings))
    return result(Verdict::Insufficient,Reason::BudgetExceeded);
  for(double tolerance:{policy.translationMetres,policy.rotationRadians,policy.relativeScale,policy.bindingResidual})
    if(!std::isfinite(tolerance)||tolerance<0)return invalid();
  if(!hash(a.sha256)||!hash(b.sha256)||a.nodes.empty()||b.nodes.empty())return invalid();
  auto supported=[](Format f){return f==Format::Fbx||f==Format::Gltf||f==Format::Actor;};
  if(!supported(a.format)||!supported(b.format))return invalid();
  if(a.format!=b.format)return result(Verdict::Insufficient,Reason::UnsupportedFormat);
  if(!a.fullNodeDomain||!b.fullNodeDomain||a.adapterVersion.empty()||b.adapterVersion.empty())return result(Verdict::Insufficient,Reason::MissingEvidence);
  // Keep the first invalid file-domain location, including before identity
  // checks. This is a diagnostic only; no validity predicate is relaxed.
  if(!graph(a,stop,out.sourceNode)||!graph(b,stop,out.targetNode))return stop.stop_requested()?cancelled():invalid();
  if((a.coordinates&&!valid(*a.coordinates))||(b.coordinates&&!valid(*b.coordinates)))return invalid();
  if(mapping.size()!=a.nodes.size()||mapping.size()!=b.nodes.size())return result(Verdict::Insufficient,Reason::MappingConflict);
  const auto absent=std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> mapped(a.nodes.size(),absent);
  std::vector<bool> taken(b.nodes.size());
  for(const auto &p:mapping){
    if(stop.stop_requested())return cancelled();
    if(p.source>=a.nodes.size()||p.target>=b.nodes.size()||mapped[p.source]!=absent||taken[p.target])return invalid();
    mapped[p.source]=p.target;taken[p.target]=true;
  }
  const bool identical=canonicalHash(a.sha256)==canonicalHash(b.sha256);
  if(a.format==Format::Actor&&!identical)return result(Verdict::Insufficient,Reason::UnsupportedFormat);
  out.mapping.assign(mapping.begin(),mapping.end());
  for(const auto &p:mapping){
    if(stop.stop_requested())return cancelled();
    out.sourceNode=p.source;out.targetNode=p.target;
    const auto &an=a.nodes[p.source],&bn=b.nodes[p.target];
    if(an.parent.has_value()!=bn.parent.has_value()||(an.parent&&mapped[*an.parent]!=*bn.parent))return result(Verdict::Incompatible,Reason::MappingConflict);
    if(an.syntheticRoot!=bn.syntheticRoot || an.inheritScaleParent.has_value()!=bn.inheritScaleParent.has_value() ||
       (an.inheritScaleParent&&mapped[*an.inheritScaleParent]!=*bn.inheritScaleParent))
      return result(Verdict::Incompatible,Reason::EvaluationConflict);
    // Identity proof is about the same file-domain node, not arbitrary bijections.
    if(identical&&an.fileIndex!=bn.fileIndex)return invalid();
  }
  const bool nativeBindings=a.format==Format::Gltf&&a.nativeSkinBindings&&b.nativeSkinBindings;
  bool missing=!a.coordinates||!b.coordinates||(!nativeBindings&&(!a.bindings||!b.bindings));
  const double metres=a.coordinates?a.coordinates->metresPerUnit:1;
  if(a.coordinates&&b.coordinates&&(a.coordinates->axes!=b.coordinates->axes||a.coordinates->metresPerUnit!=b.coordinates->metresPerUnit))
    return result(Verdict::Incompatible,Reason::CoordinateConflict);
  for(const auto &p:mapping){
    if(stop.stop_requested())return cancelled();
    out.sourceNode=p.source;out.targetNode=p.target;
    const auto &an=a.nodes[p.source],&bn=b.nodes[p.target];
    if (an.restMatrix || bn.restMatrix) {
      if (!an.restMatrix || !bn.restMatrix) {missing=true;continue;}
      // Static matrix nodes are not animation targets in glTF. Keep their
      // authored basis exactly; never promote a lossy TRS decomposition.
      for(std::size_t i=0;i<16;++i) {
        const double delta=std::abs((*an.restMatrix)[i]-(*bn.restMatrix)[i]);
        if(!std::isfinite(delta))return invalid();
        if(i>=12&&i<15)out.maxTranslationMetres=std::max(out.maxTranslationMetres,delta*metres);
        else if(delta!=0)return result(Verdict::Incompatible,Reason::RestConflict);
      }
      if(out.maxTranslationMetres>policy.translationMetres)return result(Verdict::Incompatible,Reason::RestConflict);
      // glTF static matrices have no FBX pivot/inheritance metadata.
      continue;
    }
    const auto &ar=*an.restLocal,&br=*bn.restLocal;
    for(std::size_t i=0;i<3;++i){
      const double translation=std::abs(ar.translation[i]-br.translation[i])*metres;
      const double scale=std::abs(ar.scale[i]-br.scale[i])/std::max(std::abs(ar.scale[i]),std::abs(br.scale[i]));
      if(!std::isfinite(translation)||!std::isfinite(scale))return invalid();
      out.maxTranslationMetres=std::max(out.maxTranslationMetres,translation);out.maxScaleResidual=std::max(out.maxScaleResidual,scale);
    }
    out.maxRotationRadians=std::max(out.maxRotationRadians,angle(ar.rotation,br.rotation));
    if((identical&&ar.translation!=br.translation)||(a.coordinates&&b.coordinates&&out.maxTranslationMetres>policy.translationMetres)||out.maxRotationRadians>policy.rotationRadians||out.maxScaleResidual>policy.relativeScale)
      return result(Verdict::Incompatible,Reason::RestConflict);
    if(!an.evaluation||!bn.evaluation){missing=true;continue;}
    const auto &ae=*an.evaluation,&be=*bn.evaluation;
    // Metadata pivots/offsets must match exactly: no implicit conversion or repair.
    if(ae.rotationSpace!=be.rotationSpace||ae.rotationOrder!=be.rotationOrder||ae.inheritance!=be.inheritance||
       angle(ae.preRotation,be.preRotation)>policy.rotationRadians||angle(ae.postRotation,be.postRotation)>policy.rotationRadians||
       ae.rotationPivot!=be.rotationPivot||ae.scalingPivot!=be.scalingPivot||ae.rotationOffset!=be.rotationOffset||ae.scalingOffset!=be.scalingOffset)
      return result(Verdict::Incompatible,Reason::EvaluationConflict);
  }
  for(unsigned side=0;side<2;++side){
    const auto *e=side==0?&a:&b;
    if(!e->bindings)continue;
    for(const auto &binding:*e->bindings){
      if(stop.stop_requested())return cancelled();
      if(binding.joint>=e->nodes.size()||binding.instance>=e->nodes.size()||binding.witness>1||!affine(binding.bindWorld)||!affine(binding.geometryToBone)||!affine(binding.geometryWorld))return invalid();
      double local=0;std::size_t at=0;
      for(std::size_t c=0;c<4;++c)for(std::size_t r=0;r<4;++r){
        double value=0;for(std::size_t k=0;k<4;++k)value+=binding.bindWorld[k*4+r]*binding.geometryToBone[c*4+k];
        const double expected=binding.geometryWorld[c*4+r];
        const double residual=std::abs(value-expected)/std::max(1.,std::abs(expected));
        if(!std::isfinite(residual))return invalid();
        if(residual>local){local=residual;at=c*4+r;}
        out.maxBindingResidual=std::max(out.maxBindingResidual,residual);
      }
      if(out.maxBindingResidual>policy.bindingResidual) {
        BindingDiagnostic d;d.fault=BindingFault::ClosureResidual;d.matrix=BindingMatrix::GeometryWorld;d.closureSide=static_cast<std::uint8_t>(side);
        d.witness=binding.witness;d.element=at;d.residual=local;
        (side==0?d.sourceInstance:d.targetInstance)=binding.instance;(side==0?d.sourceJoint:d.targetJoint)=binding.joint;
        out.bindingFault=d;
        return result(Verdict::Incompatible,Reason::BindingConflict);
      }
    }
  }
  // A valid closure in EACH file is necessary, not sufficient. Compare the
  // actual local-to-skin operator under BOTH instance and joint mappings.
  // Keep the two independent FBX witnesses distinct; duplicate keys are not
  // evidence of completeness. No SHA shortcut may bypass this check.
  if(a.format==Format::Fbx&&a.bindings&&b.bindings) {
    using FbxKey=std::tuple<std::size_t,std::size_t,std::uint8_t>;
    std::map<FbxKey,const Binding*> bindA,bindB;
    for(unsigned side=0;side<2;++side)for(const auto &binding:*(side==0?a.bindings:b.bindings)) {
      if(stop.stop_requested())return cancelled();
      auto &entries=side==0?bindA:bindB;
      const FbxKey key=side==0?FbxKey{mapped[binding.instance],mapped[binding.joint],binding.witness}:
          FbxKey{binding.instance,binding.joint,binding.witness};
      if(!entries.emplace(key,&binding).second)return invalid();
    }
    // A pure-animation source has no declared mesh binding epoch. This is
    // insufficient evidence, not evidence that a target skin is incompatible.
    if(bindA.empty()!=bindB.empty()) {
      if(identical)return invalid();
      missing=true;
    } else {
    if(bindA.size()!=bindB.size()) {
      BindingDiagnostic d;d.fault=BindingFault::CountMismatch;d.sourceCount=bindA.size();d.targetCount=bindB.size();out.bindingFault=d;
      return result(Verdict::Incompatible,Reason::BindingConflict);
    }
    for(const auto &[key,binding]:bindA) {
      if(stop.stop_requested())return cancelled();
      const auto it=bindB.find(key);
      if(it==bindB.end()) {
        BindingDiagnostic d;d.fault=BindingFault::MissingPeer;d.witness=std::get<2>(key);
        d.sourceInstance=binding->instance;d.sourceJoint=binding->joint;d.targetInstance=std::get<0>(key);d.targetJoint=std::get<1>(key);
        out.bindingFault=d;
        return result(Verdict::Incompatible,Reason::BindingConflict);
      }
      const auto &other=*it->second;
      // Joint bind, instance geometry bind and geometry-to-bone operators all
      // retain their independently declared spaces. Do not construct a witness.
      double local=0;std::size_t at=0;BindingMatrix kind=BindingMatrix::None;
      const std::array<std::tuple<BindingMatrix,const Matrix*,const Matrix*>,3> operators{{
          {BindingMatrix::BindWorld,&binding->bindWorld,&other.bindWorld},
          {BindingMatrix::GeometryToBone,&binding->geometryToBone,&other.geometryToBone},
          {BindingMatrix::GeometryWorld,&binding->geometryWorld,&other.geometryWorld}}};
      for(const auto &[which,first,second]:operators)
        for(std::size_t i=0;i<16;++i) {
          const double residual=std::abs((*first)[i]-(*second)[i])/
              std::max({1.,std::abs((*first)[i]),std::abs((*second)[i])});
          if(!std::isfinite(residual))return invalid();
          if(residual>local){local=residual;at=i;kind=which;}
          out.maxBindingResidual=std::max(out.maxBindingResidual,residual);
        }
      if(out.maxBindingResidual>policy.bindingResidual) {
        BindingDiagnostic d;d.fault=BindingFault::OperatorResidual;d.matrix=kind;d.witness=std::get<2>(key);d.element=at;d.residual=local;
        d.sourceInstance=binding->instance;d.sourceJoint=binding->joint;d.targetInstance=other.instance;d.targetJoint=other.joint;
        out.bindingFault=d;
        return result(Verdict::Incompatible,Reason::BindingConflict);
      }
    }
  }
  }
  // glTF has no independent BindPose record. Its native proof compares the
  // actual joint-space operators under the validated full-node bijection.
  // This does NOT assert that default node transforms are a bind pose.
  using Key=std::pair<std::size_t,std::size_t>;
  std::map<Key,const NativeSkinBinding*> nativeA,nativeB;
  for(unsigned side=0;side<2;++side) {
    const auto *e=side==0?&a:&b;
    if(!e->nativeSkinBindings)continue;
    if(e->format!=Format::Gltf)return invalid();
    auto &entries=side==0?nativeA:nativeB;
    for(const auto &binding:*e->nativeSkinBindings) {
      if(stop.stop_requested())return cancelled();
      if(binding.instance>=e->nodes.size()||binding.joint>=e->nodes.size()||!affine(binding.inverseBind))return invalid();
      const Key key=side==0?Key{mapped[binding.instance],mapped[binding.joint]}:Key{binding.instance,binding.joint};
      if(!entries.emplace(key,&binding).second)return invalid();
    }
  }
  if(nativeBindings) {
    if(nativeA.size()!=nativeB.size()) {
      BindingDiagnostic d;d.fault=BindingFault::CountMismatch;d.native=true;d.sourceCount=nativeA.size();d.targetCount=nativeB.size();out.bindingFault=d;
      return result(Verdict::Incompatible,Reason::BindingConflict);
    }
    for(const auto &[key,binding]:nativeA) {
      if(stop.stop_requested())return cancelled();
      const auto it=nativeB.find(key);
      if(it==nativeB.end()) {
        BindingDiagnostic d;d.fault=BindingFault::MissingPeer;d.native=true;
        d.sourceInstance=binding->instance;d.sourceJoint=binding->joint;d.targetInstance=key.first;d.targetJoint=key.second;
        out.bindingFault=d;
        return result(Verdict::Incompatible,Reason::BindingConflict);
      }
      const auto &matrix=binding->inverseBind,&peer=it->second->inverseBind;
      double local=0;std::size_t at=0;
      for(std::size_t i=0;i<16;++i) {
        const double residual=std::abs(matrix[i]-peer[i])/std::max({1.,std::abs(matrix[i]),std::abs(peer[i])});
        if(!std::isfinite(residual))return invalid();
        if(residual>local){local=residual;at=i;}
        out.maxBindingResidual=std::max(out.maxBindingResidual,residual);
      }
      if(out.maxBindingResidual>policy.bindingResidual) {
        BindingDiagnostic d;d.fault=BindingFault::OperatorResidual;d.native=true;d.matrix=BindingMatrix::InverseBind;d.element=at;d.residual=local;
        d.sourceInstance=binding->instance;d.sourceJoint=binding->joint;d.targetInstance=it->second->instance;d.targetJoint=it->second->joint;
        out.bindingFault=d;
        return result(Verdict::Incompatible,Reason::BindingConflict);
      }
    }
  }
  if(stop.stop_requested())return cancelled();
  out.sourceNode.reset();out.targetNode.reset();
  // Complete default transforms and validated full mapping preserve the legacy
  // identity proof without requiring new format metadata from old adapters.
  if(identical)return result(Verdict::Compatible,Reason::Identity);
  if(a.format!=b.format||a.format==Format::Actor)return result(Verdict::Insufficient,Reason::UnsupportedFormat);
  if(missing)return result(Verdict::Insufficient,Reason::MissingEvidence);
  return result(Verdict::Compatible,Reason::None);
}
} // namespace formaview::skeletal::transform
