"""Import a FormaView baked FBX in Blender 4.5 using its effective frame rate.

Run in Blender's Python console, or with blender --python this_file -- file.fbx.
Uses the installed FBX importer; no installed files are changed. Scene FPS may
be approximated by Blender's float fps_base, while source FPS remains metadata.
This adapter is required for the verified workflow; generic import is unverified.
"""

import inspect
import math
import pathlib
import threading


def _validated_metadata(tree):
    values = {}
    pending = [tree]
    while pending:
        node = pending.pop()
        if node.id == b"P" and len(node.props) > 4:
            key = node.props[0]
            if isinstance(key, bytes) and key.startswith(b"FormaView"):
                name = key.decode("utf-8", errors="strict")
                value = node.props[4]
                if name in values and values[name] != value:
                    raise ValueError("Conflicting FormaView metadata: " + name)
                values[name] = value
        pending.extend(node.elems)
    for name in ("FormaViewSourceFPS", "FormaViewSourceFrames", "FormaViewBakeFPS"):
        value = values.get(name)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError("Missing or non-numeric metadata: " + name)
        if not math.isfinite(value) or value <= 0:
            raise ValueError("Invalid metadata: " + name)
    frames = values["FormaViewSourceFrames"]
    if frames != int(frames):
        raise ValueError("Source frame count must be integral")
    if values["FormaViewBakeFPS"] < values["FormaViewSourceFPS"]:
        raise ValueError("Bake FPS must be at least source FPS")
    return values


def import_verified_fbx(path):
    """Import one explicit FormaView FBX into the current scene; return metadata.

    Supports Blender 4.5.x only and checks the private importer signature before
    modifying it. The temporary override is restored even if import fails.
    Like Blender's importer, failure can leave partially imported scene objects.
    """
    import bpy
    from io_scene_fbx import import_fbx, parse_fbx

    if bpy.app.version[:2] != (4, 5):
        raise RuntimeError("This adapter requires Blender 4.5.x")
    if threading.current_thread() is not threading.main_thread():
        raise RuntimeError("Run this adapter on Blender's main thread")
    original = import_fbx.blen_read_animations_action_item
    expected = (
        "action", "item", "cnodes", "fps", "anim_offset", "global_scale",
        "shape_key_deforms", "fbx_ktime",
    )
    if tuple(inspect.signature(original).parameters) != expected:
        raise RuntimeError("Unsupported installed Blender FBX importer signature")
    path = pathlib.Path(path).resolve(strict=True)
    tree, _ = parse_fbx.parse(str(path))
    values = _validated_metadata(tree)

    def effective_seconds(action, item, cnodes, fps, *args, **kwargs):
        scene = bpy.context.scene
        effective_fps = scene.render.fps / scene.render.fps_base
        if not math.isfinite(effective_fps) or effective_fps <= 0:
            raise ValueError("Invalid Blender effective FPS")
        return original(action, item, cnodes, effective_fps, *args, **kwargs)

    try:
        import_fbx.blen_read_animations_action_item = effective_seconds
        result = bpy.ops.import_scene.fbx(
            filepath=str(path), use_anim=True, anim_offset=1,
            automatic_bone_orientation=False,
        )
    finally:
        import_fbx.blen_read_animations_action_item = original
    if result != {"FINISHED"}:
        raise RuntimeError("Blender FBX import did not complete")
    for name, value in values.items():
        bpy.context.scene[name] = value
    return values


if __name__ == "__main__":
    import sys

    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(args) != 1:
        raise SystemExit("Usage: blender --python formaview_import.py -- file.fbx")
    import_verified_fbx(args[0])
