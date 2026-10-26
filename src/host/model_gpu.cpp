#include "model_document.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
QMatrix4x4 matrix(const formaview::Matrix4 &m) {
  QMatrix4x4 result;
  for (int r=0;r<4;++r) for (int c=0;c<4;++c)
    result(r,c)=static_cast<float>(m[static_cast<std::size_t>(r*4+c)]);
  return result;
}
void include(ModelGpuGeometry::Bounds &b, QVector3D p) {
  if (!b.valid) { b.low=b.high=p; b.valid=true; }
  else for (int a=0;a<3;++a) { b.low[a]=std::min(b.low[a],p[a]); b.high[a]=std::max(b.high[a],p[a]); }
}
// PSA-first paired preview: the reference PSK has no ModelMesh. Mirror the CPU
// pairMesh frame (grey lit face corners, grid/axis guides) with the same
// ActorAsset influences and bone-rotation normals as the PSK-first actor.
std::shared_ptr<const ModelGpuGeometry> buildPairPreview(const ModelDocument &d, std::uint64_t revision, std::stop_token stop) {
  const auto &asset=*d.pairSource; const auto &mesh=*asset.mesh;
  if (mesh.point_weights.size()!=mesh.points.size()) throw std::runtime_error("ActorX GPU weight domain mismatch");
  auto out=std::make_shared<ModelGpuGeometry>();
  auto base=std::make_shared<formaview::render::Frame>();
  auto skin=std::make_shared<formaview::render::SkinData>();
  QMatrix4x4 normalize; normalize.scale(1/std::max(d.radius,1e-6F)); normalize.translate(-d.center);
  skin->display=normalize*d.displayBasis; skin->boneRotationNormals=true;
  for (std::size_t i=0;i<asset.bones.size();++i) {
    if (!asset.bones[i].bind_world) throw std::runtime_error("Verified bind matrix required");
    out->clusters.push_back({d.pairBoneMap[i],formaview::skeletal::inverse(*asset.bones[i].bind_world)});
    out->rigidMeshes.push_back(-1);
  }
  // Unweighted points keep their bind position: an identity entry.
  const auto identity=static_cast<std::uint32_t>(out->clusters.size());
  out->clusters.push_back({std::numeric_limits<std::uint32_t>::max(),{}});
  out->rigidMeshes.push_back(0); out->restInverse.emplace_back();
  out->bounds.resize(out->clusters.size());
  out->meshBounds.assign(1,std::vector<ModelGpuGeometry::Bounds>(out->clusters.size()));
  std::vector<std::array<std::uint32_t,2>> pointRanges(mesh.points.size());
  for (std::size_t pi=0;pi<mesh.points.size();++pi) {
    if ((pi&4095)==0 && stop.stop_requested()) throw std::runtime_error("GPU geometry cancelled");
    const auto &p=mesh.points[pi]; const QVector3D local(static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z));
    if (!std::isfinite(local.x()) || !std::isfinite(local.y()) || !std::isfinite(local.z())) throw std::runtime_error("Unrepresentable GPU local position");
    double sum=0;
    for (auto wi:mesh.point_weights[pi]) {
      const auto &w=mesh.weights.at(wi);
      if (w.point!=pi || w.bone>=asset.bones.size()) throw std::runtime_error("Invalid ActorX source influence");
      if (!std::isfinite(w.value) || w.value<0) throw std::runtime_error("Invalid GPU skin influence");
      sum+=w.value;
    }
    pointRanges[pi][0]=static_cast<std::uint32_t>(skin->influences.size()/2);
    if (mesh.point_weights[pi].empty()) {
      skin->influences.push_back(static_cast<float>(identity)); skin->influences.push_back(1);
      include(out->bounds[identity],local); include(out->meshBounds[0][identity],local); pointRanges[pi][1]=1;
      continue;
    }
    if (sum<=1e-15) throw std::runtime_error("Cannot normalize zero-sum weights");
    for (auto wi:mesh.point_weights[pi]) {
      const auto &w=mesh.weights[wi]; if (w.value<=0) continue;
      skin->influences.push_back(static_cast<float>(w.bone)); skin->influences.push_back(static_cast<float>(w.value/sum));
      include(out->bounds[w.bone],local); include(out->meshBounds[0][w.bone],local); ++pointRanges[pi][1];
    }
    if (pointRanges[pi][1]>1024) throw std::runtime_error("GPU per-vertex influence loop budget exceeded; full weights retained for CPU preview");
  }
  const bool smooth=mesh.normals.size()==mesh.points.size();
  base->vertices.reserve(mesh.faces.size()*3); skin->ranges.reserve(mesh.faces.size()*3);
  for (const auto &face:mesh.faces) for (auto wi:face.wedges) {
    const auto &w=mesh.wedges.at(wi); const auto &p=mesh.points.at(w.point);
    const auto n=smooth?mesh.normals[w.point]:formaview::skeletal::Vec3{};
    base->vertices.push_back({static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z),static_cast<float>(w.uv.x),static_cast<float>(w.uv.y),0,
        static_cast<float>(n.x),static_cast<float>(n.y),static_cast<float>(n.z)});
    skin->ranges.push_back(pointRanges.at(w.point));
  }
  if (skin->influences.size()%4) skin->influences.resize(skin->influences.size()+2);
  if (base->vertices.size()*sizeof(formaview::render::Vertex)+skin->ranges.size()*8>256ULL*1024*1024 || skin->influences.size()*4>128ULL*1024*1024)
    throw std::runtime_error("GPU geometry/influence memory budget exceeded");
  base->revision=revision; base->lines=false; base->lit=true; base->cacheable=false;
  base->rgba={.62F,.66F,.71F,1.F}; base->sourcePoints=mesh.points.size();
  appendModelGuides(*base,d);
  base->skin=std::move(skin);
  out->frames.model=base; out->frames.uv=base;
  out->view.bounds=out->bounds;out->view.sourcePoints=base->sourcePoints;out->view.sourceTriangles=base->sourceTriangles;
  return out;
}
}
std::atomic<std::uint64_t> gpuGeometryBuilds{0};
std::uint64_t modelGpuGeometryBuildCount() { return gpuGeometryBuilds.load(); }
std::shared_ptr<const ModelGpuGeometry> buildModelGpuGeometry(
    const ModelDocument &d, const ModelRenderState &state, std::uint64_t revision,
    std::stop_token stop) {
  ++gpuGeometryBuilds;
  if (d.pairPreviewDomain()) return buildPairPreview(d,revision,stop);
  if (!d.gpuSkinDomain())
    throw std::runtime_error("GPU skinning requires a mesh-cluster animation domain");
  auto out=std::make_shared<ModelGpuGeometry>();
  // Plain mode-0 view: keep every mesh resident (visibility/isolation/LOD select
  // draws) unless that would not fit the vertex budget; then build as asked.
  // Collision overlay and opaque-only appearances need different alpha values.
  // Keep both in the same immutable payload so toggling them only selects draws.
  std::uint64_t allBytes=0;
  for(std::size_t mi=0;mi<d.meshes.size();++mi) {
    const bool collision=!state.classes.empty() && state.classes.at(mi).collision;
    allBytes+=static_cast<std::uint64_t>(d.meshes[mi].triangles.size())*
              (sizeof(formaview::render::Vertex)+8)*(collision?2:1);
  }
  const bool superset=state.mode==0 && allBytes<=256ULL*1024*1024;
  auto buildState=state;
  if(superset){buildState.visible.assign(d.meshes.size(),true);buildState.isolated=false;buildState.highlightedNode.clear();buildState.collisionMode=1;}
  out->frames=buildModelFrames(d,buildState,revision,stop,nullptr,true);
  // This frame has just been allocated by the builder and has never been
  // published. Convert its positions/normals to local space once, in the worker.
  auto base=std::const_pointer_cast<formaview::render::Frame>(out->frames.model);
  if(superset) {
    const auto originalDraws=base->draws;
    for(const auto &draw:originalDraws) {
      if(draw.mesh<0 || state.classes.empty() || !state.classes.at(static_cast<std::size_t>(draw.mesh)).collision)continue;
      auto opaque=draw;opaque.first=static_cast<std::uint32_t>(base->vertices.size());opaque.blend=false;
      for(std::size_t vi=draw.first;vi<static_cast<std::size_t>(draw.first)+draw.count;++vi) {
        if((vi&4095)==0 && stop.stop_requested())throw std::runtime_error("GPU collision geometry cancelled");
        auto vertex=base->vertices.at(vi);vertex.alpha=1.F;vertex.alphaMode=0.F;
        const auto source=base->sourceCorners.at(vi);
        base->vertices.push_back(vertex);base->sourceCorners.push_back(source);
      }
      base->draws.push_back(opaque);
    }
  }
  auto skin=std::make_shared<formaview::render::SkinData>();
  QMatrix4x4 normalize; normalize.scale(1/std::max(d.radius,1e-6F)); normalize.translate(-d.center);
  skin->display=normalize*d.displayBasis;
  // Paired PSK actor: the ActorAsset bind palette is the cluster list and its
  // raw point weights are the influences, exactly the CPU SkinEvaluator inputs.
  const bool actor=d.actorSkinDomain();
  std::vector<std::vector<ModelSkinCluster>> actorClusters;
  std::vector<std::vector<std::vector<ModelSkinSlot>>> actorSlots;
  if (actor) {
    const auto &asset=*d.pairSource; const auto &source=*asset.mesh;
    if (d.meshes.front().raw.size()!=source.points.size() || source.point_weights.size()!=source.points.size())
      throw std::runtime_error("ActorX GPU weight domain mismatch");
    auto &clusters=actorClusters.emplace_back();
    for (std::size_t i=0;i<asset.bones.size();++i) {
      if (!asset.bones[i].bind_world) throw std::runtime_error("Verified bind matrix required");
      clusters.push_back({d.pairBoneMap[i],formaview::skeletal::inverse(*asset.bones[i].bind_world)});
    }
    auto &pointSlots=actorSlots.emplace_back(source.points.size());
    for (std::size_t pi=0;pi<source.points.size();++pi) for (auto wi:source.point_weights[pi]) {
      const auto &w=source.weights.at(wi);
      if (w.point!=pi || w.bone>=clusters.size()) throw std::runtime_error("Invalid ActorX source influence");
      pointSlots[pi].push_back({w.bone,w.value});
    }
  }
  const auto &skinClusters=actor?actorClusters:d.skinClusters;
  const auto &skinSlots=actor?actorSlots:d.skinSlots;
  const auto rest=actor?std::vector<formaview::Matrix4>{}:chainWorlds(d.animNodeLocals,d.animNodeParents);
  const bool anyMorph = d.morphWeightsAt != nullptr;
  std::uint32_t morphStride = 0;
  for (const auto &mesh : d.meshes)
    morphStride = std::max(morphStride, static_cast<std::uint32_t>(mesh.morphTargets.size()));
  std::vector<std::uint32_t> rigid;
  std::vector<std::vector<std::uint32_t>> mapping(d.meshes.size());
  std::map<std::pair<std::uint32_t,formaview::Matrix4>,std::uint32_t> unique;
  std::vector<std::vector<std::array<std::uint32_t,2>>> ranges(d.meshes.size());
  for (std::size_t mi=0;mi<d.meshes.size();++mi) {
    const auto &mesh=d.meshes[mi];
    if (!mesh.skinDeformationValid)
      throw std::runtime_error("Invalid source skin weights; animation paused, source records preserved");
    for (const auto &c:skinClusters.at(mi)) {
      for(double value:c.geometryToBone)if(!std::isfinite(value))throw std::runtime_error("Non-finite GPU inverse bind matrix");
      const auto key=std::make_pair(c.node,c.geometryToBone);
      const auto [it,inserted]=unique.emplace(key,static_cast<std::uint32_t>(out->clusters.size()));
      if (inserted) { out->clusters.push_back(c); out->rigidMeshes.push_back(-1); }
      mapping[mi].push_back(it->second);
    }
    rigid.push_back(static_cast<std::uint32_t>(out->clusters.size()));
    out->clusters.push_back({actor?0u:d.meshInstanceNodes.at(mi),{}});
    out->rigidMeshes.push_back(static_cast<int>(mi));
    bool invertible=false;
    // ActorX unweighted points keep their bind position: an identity entry.
    // Animated-morph meshes hold unmorphed node-local positions; the rigid
    // entry maps them with the plain animated world (no static-rest delta).
    if (!actor && anyMorph && d.meshes.at(mi).morphAnimated) {
      out->restInverse.emplace_back(); // identity; maps raw node-local points
      invertible = true; // never the singular-rest sentinel
    } else
      out->restInverse.push_back(actor?QMatrix4x4():matrix(rest.at(d.meshInstanceNodes.at(mi))).inverted(&invertible));
    // CPU rigid fallback also leaves decoded points fixed for a singular rest.
    if (!invertible) out->clusters.back().node=std::numeric_limits<std::uint32_t>::max();
  }
  if (out->clusters.size()>= (1u<<24)) throw std::runtime_error("GPU cluster integer precision budget exceeded");
  if(d.meshes.size()>(32ULL*1024*1024/sizeof(ModelGpuGeometry::Bounds))/std::max<std::size_t>(out->clusters.size(),1))throw std::runtime_error("GPU per-mesh focus bound budget exceeded; CPU preview retained");
  out->bounds.resize(out->clusters.size());
  out->meshBounds.resize(d.meshes.size(),std::vector<ModelGpuGeometry::Bounds>(out->clusters.size()));
  const auto invDisplay=d.displayBasis.inverted();
  for (std::size_t mi=0;mi<d.meshes.size();++mi) {
    const auto &mesh=d.meshes[mi]; auto &pointRanges=ranges[mi]; pointRanges.resize(mesh.raw.size());
    const bool shown=(state.visible.empty() || state.visible[mi]) &&
        (!(state.isolated || state.mode==1 || state.mode==2) || mi==static_cast<std::size_t>(state.mesh));
    const bool active=superset || shown;
    for (std::size_t pi=0;pi<mesh.raw.size();++pi) {
      if ((pi&4095)==0 && stop.stop_requested()) throw std::runtime_error("GPU geometry cancelled");
      const auto &influences=skinSlots.at(mi).at(pi);
      double sum=0; for (const auto &slot:influences) {
        if (!std::isfinite(slot.second) || slot.second<0) throw std::runtime_error("Invalid GPU skin influence");
        sum+=slot.second;
      }
      if (!std::isfinite(sum)) throw std::runtime_error("Invalid GPU skin weight sum");
      if (actor && !influences.empty() && sum<=1e-15) throw std::runtime_error("Cannot normalize zero-sum weights");
      const auto start=skin->influences.size()/2;
      if (start>std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("GPU influence range overflow");
      pointRanges[pi][0]=static_cast<std::uint32_t>(start);
      const bool animatedMorph=anyMorph && mesh.morphAnimated;
      const auto &raw=animatedMorph||mesh.animationPositions.empty()?mesh.raw[pi]:mesh.animationPositions[pi];
      const auto local=animatedMorph
          ?QVector3D(static_cast<float>(raw.x),static_cast<float>(raw.y),static_cast<float>(raw.z))
          :sum>1e-15?QVector3D(static_cast<float>(raw.x),static_cast<float>(raw.y),static_cast<float>(raw.z)):invDisplay.map(mesh.points.at(pi));
      if (sum>1e-15) {
        for (const auto &slot:influences) if (slot.second>0) {
          if (slot.first>=skinClusters.at(mi).size()) throw std::runtime_error("GPU cluster index outside binding");
          const auto index=mapping[mi].at(slot.first);
          skin->influences.push_back(static_cast<float>(index));
          skin->influences.push_back(static_cast<float>(slot.second/sum));
          if(active){if(shown)include(out->bounds[index],local);include(out->meshBounds[mi][index],local);}
          if(active&&animatedMorph)for(const auto &target:mesh.morphTargets){if(target.positionDeltas.size()!=mesh.raw.size())continue;
            const QVector3D extreme(static_cast<float>(raw.x+target.positionDeltas[pi].x),static_cast<float>(raw.y+target.positionDeltas[pi].y),static_cast<float>(raw.z+target.positionDeltas[pi].z));
            if(shown)include(out->bounds[index],extreme);include(out->meshBounds[mi][index],extreme);}
          ++pointRanges[pi][1];
        }
      } else {
        skin->influences.push_back(static_cast<float>(rigid[mi])); skin->influences.push_back(1);
        if(active){if(shown)include(out->bounds[rigid[mi]],local);include(out->meshBounds[mi][rigid[mi]],local);}
        if(active&&animatedMorph)for(const auto &target:mesh.morphTargets){if(target.positionDeltas.size()!=mesh.raw.size())continue;
          const QVector3D extreme(static_cast<float>(raw.x+target.positionDeltas[pi].x),static_cast<float>(raw.y+target.positionDeltas[pi].y),static_cast<float>(raw.z+target.positionDeltas[pi].z));
          if(shown)include(out->bounds[rigid[mi]],extreme);include(out->meshBounds[mi][rigid[mi]],extreme);} pointRanges[pi][1]=1;
      }
    }
  }
  skin->ranges.reserve(base->vertices.size());
  for (std::size_t vi=0;vi<base->vertices.size();++vi) {
    if((vi&4095)==0 && stop.stop_requested())throw std::runtime_error("GPU corner preparation cancelled");
    const auto [mi,corner]=base->sourceCorners.at(vi); const auto &mesh=d.meshes.at(mi);
    const auto point=mesh.corners.at(corner); auto &v=base->vertices[vi];
    const auto range=ranges[mi].at(point); skin->ranges.push_back(range);
    const auto &raw=(anyMorph&&mesh.morphAnimated)||mesh.animationPositions.empty()?mesh.raw.at(point):mesh.animationPositions.at(point);
    double sum=0; for (const auto &slot:skinSlots.at(mi).at(point)) sum+=slot.second;
    // Animated-morph meshes keep UNMORPHED node-local positions for rigid
    // points too (the rigid palette entry maps them with the plain animated
    // world); mesh.points would re-apply the static defaults on top.
    const bool cornerMorph=anyMorph && mesh.morphAnimated;
    const auto p=sum>1e-15||cornerMorph?QVector3D(static_cast<float>(raw.x),static_cast<float>(raw.y),static_cast<float>(raw.z)):invDisplay.map(mesh.points.at(point));
    if(!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()))throw std::runtime_error("Unrepresentable GPU local position");
    if(range[1]>1024)throw std::runtime_error("GPU per-vertex influence loop budget exceeded; full weights retained for CPU preview");
    v.x=p.x(); v.y=p.y(); v.z=p.z();
    if (actor) {
      // ActorX smooth normals exist only for a full point domain; otherwise the
      // CPU reference uses flat faces, which fragment derivatives reproduce.
      const auto &normals=d.pairSource->mesh->normals;
      const auto n=normals.size()==mesh.raw.size()?normals[point]:formaview::skeletal::Vec3{};
      v.nx=static_cast<float>(n.x); v.ny=static_cast<float>(n.y); v.nz=static_cast<float>(n.z);
    } else {
      const auto &n=d.pointNormals.at(mi).at(point);
      if (sum>1e-15) { v.nx=static_cast<float>(n.x); v.ny=static_cast<float>(n.y); v.nz=static_cast<float>(n.z); }
      else { v.nx=v.ny=v.nz=0; } // Fragment derivatives supply the exact deformed face normal.
    }
  }
  // Animated morph payload: per-point delta texels and a per-vertex range.
  // Layout: texel 2*(globalPoint*morphStride + t) = dPos, +1 = dNorm.
  // The range attribute is declared for every skin draw: entries exist for
  // every vertex (count 0 without morph) so the buffer is always full size.
  skin->morphRanges.assign(base->vertices.size(), {0, 0});
  if (anyMorph && morphStride) {
    std::vector<std::uint64_t> pointBase(d.meshes.size(), 0);
    for (std::size_t mi = 1; mi < d.meshes.size(); ++mi)
      pointBase[mi] = pointBase[mi - 1] + d.meshes[mi - 1].raw.size();
    const std::uint64_t texels = 2ULL * pointBase.back() * morphStride +
        2ULL * d.meshes.back().raw.size() * morphStride;
    if (texels > 32ULL * 1024 * 1024)
      throw std::runtime_error("GPU morph texture budget exceeded; CPU preview retained");
    skin->morph.assign(static_cast<std::size_t>(texels) * 4, 0.F);
    skin->morphStride = morphStride;
    skin->morphPaletteBase = static_cast<std::uint32_t>(out->clusters.size()) * 4;
    for (std::size_t mi = 0; mi < d.meshes.size(); ++mi) {
      const auto &mesh = d.meshes[mi];
      const auto targets = std::min<std::size_t>(mesh.morphTargets.size(), morphStride);
      if (targets == 0) continue;
      for (std::size_t t = 0; t < targets; ++t) {
        const auto &target = mesh.morphTargets[t];
        const bool positions = target.positionDeltas.size() == mesh.raw.size();
        const bool normals = target.normalDeltas.size() == mesh.raw.size();
        if (!positions && !normals) continue;
        for (std::size_t p = 0; p < mesh.raw.size(); ++p) {
          const auto at = static_cast<std::size_t>(
              (2ULL * (pointBase[mi] + p) * morphStride + 2ULL * t) * 4);
          if (positions) {
            skin->morph[at + 0] = static_cast<float>(target.positionDeltas[p].x);
            skin->morph[at + 1] = static_cast<float>(target.positionDeltas[p].y);
            skin->morph[at + 2] = static_cast<float>(target.positionDeltas[p].z);
          }
          if (normals) {
            skin->morph[at + 4] = static_cast<float>(target.normalDeltas[p].x);
            skin->morph[at + 5] = static_cast<float>(target.normalDeltas[p].y);
            skin->morph[at + 6] = static_cast<float>(target.normalDeltas[p].z);
          }
        }
      }
    }
    for (std::size_t vi = 0; vi < base->vertices.size(); ++vi) {
      const auto [mi, corner] = base->sourceCorners.at(vi);
      const auto &mesh = d.meshes.at(mi);
      const auto targets = std::min<std::size_t>(mesh.morphTargets.size(), morphStride);
      if (targets == 0) continue;
      const auto point = mesh.corners.at(corner);
      skin->morphRanges[vi] = {
          static_cast<std::uint32_t>(2ULL * (pointBase[mi] + point) * morphStride),
          static_cast<std::uint32_t>(targets) |
              (static_cast<std::uint32_t>(mi) << 16)};
    }
    if (skin->morph.size() * 4 > 128ULL * 1024 * 1024)
      throw std::runtime_error("GPU morph memory budget exceeded");
  }
  // Precompute exact transparent draw centroids as weighted cluster moments.
  out->centers.resize(base->draws.size());
  for (std::size_t di=0;di<base->draws.size();++di) {
    const auto &draw=base->draws[di]; if (!draw.blend) continue;
    std::map<std::uint32_t,QVector4D> moments;
    for (std::size_t v=draw.first;v<static_cast<std::size_t>(draw.first)+draw.count;++v) {
      if((v&4095)==0 && stop.stop_requested())throw std::runtime_error("GPU draw centroid preparation cancelled");
      const auto &p=base->vertices.at(v); const auto range=skin->ranges.at(v);
      for (std::uint32_t i=0;i<range[1];++i) {
        const auto at=static_cast<std::size_t>(range[0]+i)*2;
        moments[static_cast<std::uint32_t>(skin->influences.at(at))]+=QVector4D(p.x,p.y,p.z,1)*(skin->influences.at(at+1)/static_cast<float>(draw.count));
      }
    }
    for (const auto &entry:moments) out->centers[di].push_back(entry);
  }
  if (skin->influences.size()%4) skin->influences.resize(skin->influences.size()+2);
  if (base->vertices.size()*sizeof(formaview::render::Vertex)+skin->ranges.size()*8>256ULL*1024*1024 || skin->influences.size()*4>128ULL*1024*1024)
    throw std::runtime_error("GPU geometry/influence memory budget exceeded");
  base->sourceCorners.clear(); base->sourceCorners.shrink_to_fit();
  {const auto h=resolveGpuHighlight(d,state.highlightedNode);out->highlighted=h.node;out->highlightEdges=h.edges;}
  if(base->selectionVertices)base->guides.resize(base->guides.size()-base->selectionVertices);
  base->selectionVertices=0;
  skin->boneRotationNormals=actor;
  base->skin=std::move(skin); base->cacheable=false;
  out->coversAllMeshes=superset;
  if(superset) {
    out->meshPoints.assign(d.meshes.size(),0);out->meshTriangles.assign(d.meshes.size(),0);
    for(std::size_t mi=0;mi<d.meshes.size();++mi) {
      const auto &m=d.meshes[mi];std::vector<bool> used(m.points.size());
      for(std::size_t t=0;t+2<m.triangles.size();t+=3){++out->meshTriangles[mi];for(int e=0;e<3;++e){const auto point=m.corners.at(m.triangles[t+static_cast<std::size_t>(e)]);if(!used.at(point)){used[point]=true;++out->meshPoints[mi];}}}
    }
    out->view=makeGpuView(d,*out,state);
  } else {
    out->view.bounds=out->bounds;out->view.sourcePoints=base->sourcePoints;out->view.sourceTriangles=base->sourceTriangles;
  }
  return out;
}
GpuView makeGpuView(const ModelDocument &d, const ModelGpuGeometry &g, const ModelRenderState &state) {
  if(!g.coversAllMeshes)return g.view;
  GpuView view;view.explicitDraws=true;view.bounds.assign(g.clusters.size(),{});
  std::vector<bool> shown(d.meshes.size());
  for(std::size_t mi=0;mi<d.meshes.size();++mi)shown[mi]=(state.visible.empty() || state.visible[mi]) && (!state.isolated || mi==static_cast<std::size_t>(state.mesh));
  const auto &draws=g.frames.model->draws;
  for(std::size_t di=0;di<draws.size();++di) {
    const auto &draw=draws[di];
    if(draw.mesh<0 || !shown.at(static_cast<std::size_t>(draw.mesh)))continue;
    const bool collision=!state.classes.empty() && state.classes.at(static_cast<std::size_t>(draw.mesh)).collision;
    if(collision && draw.blend!=(state.collisionMode==1))continue;
    view.draws.push_back(static_cast<std::uint32_t>(di));
  }
  for(std::size_t mi=0;mi<d.meshes.size();++mi) {
    if(!shown[mi])continue;
    view.sourcePoints+=g.meshPoints[mi];view.sourceTriangles+=g.meshTriangles[mi];
    for(std::size_t ci=0;ci<g.clusters.size();++ci){const auto &b=g.meshBounds[mi][ci];if(b.valid){include(view.bounds[ci],b.low);include(view.bounds[ci],b.high);}}
  }
  return view;
}
GpuHighlight resolveGpuHighlight(const ModelDocument &d, const QString &node) {
  GpuHighlight h;
  // The PSA-first reference mesh shows no skeleton highlight (as on the CPU path).
  if(node.isEmpty() || d.pairPreviewDomain())return h;
  for (qsizetype i=0;i<d.nodes.size();++i) if (d.nodes[i].toMap().value("id").toString()==node) h.node=static_cast<int>(i);
  // Worlds index the PSK bone list (whose nodes are bone/i) or the full node domain.
  const auto &parents=d.actorSkinDomain()?d.boneParents:d.animNodeParents;
  if(h.node>=static_cast<int>(parents.size()))h.node=-1;
  if(h.node>=0) {
    for(std::size_t i=0;i<parents.size();++i) if(parents[i]==h.node && d.nodes[static_cast<qsizetype>(i)].toMap().value("bone").toBool())h.edges.push_back(static_cast<int>(i));
    if(h.edges.empty() && parents.at(static_cast<std::size_t>(h.node))>=0)h.edges.push_back(parents.at(static_cast<std::size_t>(h.node)));
  }
  return h;
}
ModelFrames buildModelGpuPose(const ModelDocument &d, const ModelGpuGeometry &g,
    std::shared_ptr<const formaview::render::Frame> uv,
    const std::vector<formaview::Matrix4> &worlds, std::uint64_t revision, int clip, double seconds,
    const GpuView *viewIn, const GpuHighlight *highlightIn) {
  // Paired ActorX (PSK-first or PSA-first) poses use the bone list; others the node domain.
  if (worlds.size()!=(d.pairSource?d.boneParents.size():d.animNodeParents.size())) throw std::runtime_error("GPU pose node domain mismatch");
  const auto &base=g.frames.model;
  auto f=std::make_shared<formaview::render::Frame>();
  f->geometry=base; f->worlds=worlds; f->revision=revision; f->clip=clip; f->seconds=seconds;
  const GpuView &view=viewIn?*viewIn:g.view;
  const GpuHighlight highlight=highlightIn?*highlightIn:GpuHighlight{g.highlighted,g.highlightEdges};
  f->sourcePoints=view.sourcePoints; f->sourceTriangles=view.sourceTriangles;
  f->guides=base->guides; f->gridVertices=base->gridVertices;
  // Draw index of every presented draw, to find its resident centroid moments.
  std::vector<std::uint32_t> drawSource;
  if (g.coversAllMeshes) {
    f->explicitDraws=true;f->draws.reserve(view.draws.size());drawSource=view.draws;
    for (auto di:view.draws) f->draws.push_back(base->draws[di]);
  } else {
    f->draws=base->draws;drawSource.resize(f->draws.size());
    for (std::size_t di=0;di<drawSource.size();++di) drawSource[di]=static_cast<std::uint32_t>(di);
  }
  if (highlight.node>=0) {
    const auto position=[&](int index){const auto &w=worlds.at(static_cast<std::size_t>(index));return base->skin->display.map(QVector3D(static_cast<float>(w[3]),static_cast<float>(w[7]),static_cast<float>(w[11])));};
    const auto a=position(highlight.node);
    for(int index:highlight.edges)for(const auto &p:{a,position(index)}) {f->guides.push_back({p.x(),p.y(),p.z(),0,0,0,0,1,0,.95F,.48F,.18F,1,-2,.5F});++f->selectionVertices;}
  }
  ModelGpuGeometry::Bounds bounds;
  std::vector<QMatrix4x4> palette; palette.reserve(g.clusters.size());
  for (std::size_t i=0;i<g.clusters.size();++i) {
    const auto &binding=g.clusters[i]; QMatrix4x4 m;
    if (g.rigidMeshes[i]<0) m=matrix(formaview::skeletal::multiply(worlds.at(binding.node),binding.geometryToBone));
    else if (binding.node!=std::numeric_limits<std::uint32_t>::max()) m=matrix(worlds.at(binding.node))*g.restInverse.at(static_cast<std::size_t>(g.rigidMeshes[i]));
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
      const auto x=m(r,c); if (!std::isfinite(x)) throw std::runtime_error("Non-finite GPU pose");
      f->palette.push_back(x);
    }
    palette.push_back(m);
    const auto &b=view.bounds.at(i); if (b.valid) for (int mask=0;mask<8;++mask) {
      const QVector3D p((mask&1)?b.high.x():b.low.x(),(mask&2)?b.high.y():b.low.y(),(mask&4)?b.high.z():b.low.z());
      include(bounds,base->skin->display.map(m.map(p)));
    }
  }
  if (d.morphWeightsAt && g.frames.model->skin->morphStride) {
    // Palette rows for animated morph weights: groups x stride texels,
    // always emitted (zeros when a mesh has no channel) so the palette
    // texture size is stable across poses.
    const auto stride = g.frames.model->skin->morphStride;
    const auto weights = d.morphWeightsAt(static_cast<std::size_t>(clip), seconds);
    bool outOfRange = false;
    for (std::size_t mi = 0; mi < d.meshes.size(); ++mi)
      if (mi < weights.size())
        for (double w : weights[mi])
          if (!std::isfinite(w) || std::abs(w) > 1.0 + 1e-6) outOfRange = true;
    if (outOfRange)
      throw std::runtime_error("Animated morph weight outside [-1,1]; animation stopped (GPU conservative bounds cover [-1,1])");
    // One weight per texel (the shader fetches whole texels), so the group
    // rows are groups x stride texels of stable size.
    for (std::size_t mi = 0; mi < d.meshes.size(); ++mi)
      for (std::uint32_t t = 0; t < stride; ++t) {
        f->palette.push_back(mi < weights.size() && t < weights[mi].size()
                                 ? static_cast<float>(weights[mi][t])
                                 : 0.F);
        f->palette.push_back(0.F); f->palette.push_back(0.F);
        f->palette.push_back(0.F);
      }
  }
  for (std::size_t di=0;di<f->draws.size();++di) if (f->draws[di].blend) {
    QVector4D p; for (const auto &[ci,moment]:g.centers[drawSource[di]]) p+=palette.at(ci)*moment;
    f->draws[di].center=base->skin->display.map(p.toVector3D());
  }
  f->hasDisplayBounds=bounds.valid; f->displayLow=bounds.low; f->displayHigh=bounds.high;
  ModelFrames result; result.model=std::move(f); result.uv=std::move(uv); result.mode=g.frames.mode;
  return result;
}
