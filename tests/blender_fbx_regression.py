"""Run inside Blender: audit import compatibility and unchanged FBX animation.

Call verify(new_path, baseline_path, scene_name); existing scenes are preserved.
The baseline is the previously precision-validated export of the same source.
"""
import contextlib
import hashlib
import io
import math

import bpy
from io_scene_fbx import parse_fbx


def elements(root, kind):
    if root.id == kind:
        yield root
    for child in root.elems:
        yield from elements(child, kind)


def child(root, kind):
    return next((e for e in root.elems if e.id == kind), None)


def payload(element):
    """Ignore object IDs; include all curve timing, values and interpolation."""
    def encode(value):
        return value.tobytes() if hasattr(value, 'tobytes') else repr(value).encode()
    digest = hashlib.sha256()
    for e in element.elems:
        digest.update(e.id)
        for p in e.props:
            data = encode(p)
            digest.update(len(data).to_bytes(8, 'little'))
            digest.update(data)
    return digest.hexdigest()


def verify(path, baseline, scene_name):
    root, _ = parse_fbx.parse(str(path))
    old, _ = parse_fbx.parse(str(baseline))
    curves = sorted(payload(e) for e in elements(root, b'AnimationCurve'))
    assert curves == sorted(payload(e) for e in elements(old, b'AnimationCurve')), 'Curve payload changed'
    # Includes local/reference time spans and the original-source/bake metadata.
    assert sorted(payload(e) for e in elements(root, b'AnimationStack')) == sorted(
        payload(e) for e in elements(old, b'AnimationStack')), 'Clip timing changed'
    for geom in elements(root, b'Geometry'):
        for kind, data in [(b'LayerElementNormal', b'Normals'), (b'LayerElementColor', b'Colors')]:
            layers = list(elements(geom, kind))
            assert len(layers) <= 1, 'Empty or duplicate attribute layers'
            for layer in layers:
                assert layer.props[0] == 0 and child(layer, data) is not None, 'Missing first-layer data'
    scene = bpy.data.scenes.new(scene_name)
    bpy.context.window.scene = scene
    log = io.StringIO()
    with contextlib.redirect_stdout(log):
        status = bpy.ops.import_scene.fbx(filepath=str(path))
    assert status == {'FINISHED'}, status
    assert 'missing data' not in log.getvalue(), log.getvalue()
    meshes = [o for o in scene.objects if o.type == 'MESH']
    rigs = [o for o in scene.objects if o.type == 'ARMATURE']
    assert len(rigs) == 1
    for mesh in meshes:
        assert mesh.data.has_custom_normals, 'Custom normals lost'
        assert len(mesh.data.color_attributes) == 1, 'Extra color layers'
        assert any(m.type == 'ARMATURE' and m.object == rigs[0] for m in mesh.modifiers)
    fps = scene.render.fps / scene.render.fps_base
    assert math.isfinite(fps) and 1 <= fps <= 120, 'Bake rate leaked into timeline'
    assert scene.render.fps_base == 1, 'Fractional timeline triggers Blender key placement mismatch'
    action = rigs[0].animation_data.action if rigs[0].animation_data else None
    imported_keys_preserved = True
    if action:
        scene.frame_end = math.ceil(action.frame_range[1])
        # FBX bytes alone cannot detect Blender's 0.01-frame key merging.
        # Compare imported channel key counts with the high-rate baseline too.
        baseline_scene = bpy.data.scenes.new(scene_name + ' baseline')
        bpy.context.window.scene = baseline_scene
        with contextlib.redirect_stdout(io.StringIO()):
            bpy.ops.import_scene.fbx(filepath=str(baseline))
        old_rig = next(o for o in baseline_scene.objects if o.type == 'ARMATURE')
        old_action = old_rig.animation_data.action
        counts = lambda a: {(c.data_path, c.array_index): len(c.keyframe_points) for c in a.fcurves}
        imported_keys_preserved = counts(action) == counts(old_action)
        bpy.context.window.scene = scene
        assert imported_keys_preserved, 'Blender merged subframe keys'
    return {'scene': scene.name, 'mesh': meshes[0].name if meshes else None,
            'rig': rigs[0].name, 'bones': len(rigs[0].data.bones), 'fps': fps,
            'curve_count': len(curves), 'curves_and_time_spans_unchanged': True,
            'blender_channel_key_counts_unchanged': imported_keys_preserved,
            'custom_normals': all(m.data.has_custom_normals for m in meshes),
            'color_layers': [len(m.data.color_attributes) for m in meshes],
            'duration': (action.frame_range[1]-action.frame_range[0])/fps if action else None,
            'import_log': log.getvalue()}
