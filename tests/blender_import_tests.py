"""Adapter failure/precision contracts; actual Blender validation is external."""
import importlib.util
import pathlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "formaview_import",
    pathlib.Path(__file__).parents[1] / "tools/blender/formaview_import.py",
)
adapter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(adapter)


def tree(**overrides):
    values = {"FormaViewSourceFPS": 23.976, "FormaViewSourceFrames": 21,
              "FormaViewBakeFPS": 191.808}
    values.update(overrides)
    return types.SimpleNamespace(id=b"Root", elems=[
        types.SimpleNamespace(id=b"P", props=[k.encode(), b"", b"", b"", v], elems=[])
        for k, v in values.items()
    ])


class AdapterTests(unittest.TestCase):
    def test_metadata_rejects_invalid_timing(self):
        for values in ({"FormaViewSourceFPS": float("nan")},
                       {"FormaViewSourceFrames": 2.5},
                       {"FormaViewBakeFPS": 1},
                       {"FormaViewSourceFPS": "30"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                adapter._validated_metadata(tree(**values))

    def run_import(self, fail=False, version=(4, 5, 13), signature_ok=True):
        calls = []

        def original(action, item, cnodes, fps, anim_offset, global_scale,
                     shape_key_deforms, fbx_ktime):
            calls.append(fps)

        importer = types.SimpleNamespace(
            blen_read_animations_action_item=original if signature_ok else lambda: None)
        stock = importer.blen_read_animations_action_item

        class Scene(dict):
            render = types.SimpleNamespace(fps=192, fps_base=1.001001)

        scene = Scene()

        def execute(**kwargs):
            importer.blen_read_animations_action_item(None, None, None, 192, 1, 1, {}, 1)
            if fail:
                raise RuntimeError("Import error")
            return {"FINISHED"}

        bpy = types.SimpleNamespace(app=types.SimpleNamespace(version=version),
            context=types.SimpleNamespace(scene=scene),
            ops=types.SimpleNamespace(import_scene=types.SimpleNamespace(fbx=execute)))
        fbx = types.SimpleNamespace(import_fbx=importer,
            parse_fbx=types.SimpleNamespace(parse=lambda path: (tree(), 7400)))
        with tempfile.NamedTemporaryFile() as source:
            with patch.dict(sys.modules, {"bpy": bpy, "io_scene_fbx": fbx}):
                try:
                    result = adapter.import_verified_fbx(source.name)
                finally:
                    self.assertIs(importer.blen_read_animations_action_item, stock)
        return calls, scene, result

    def test_effective_seconds_and_metadata(self):
        calls, scene, result = self.run_import()
        self.assertEqual(calls, [192 / 1.001001])
        self.assertEqual(scene["FormaViewSourceFPS"], 23.976)
        self.assertEqual(scene["FormaViewSourceFrames"], result["FormaViewSourceFrames"])

    def test_failure_restores_importer(self):
        with self.assertRaisesRegex(RuntimeError, "Import error"):
            self.run_import(fail=True)

    def test_version_guard(self):
        with self.assertRaisesRegex(RuntimeError, "4.5.x"):
            self.run_import(version=(5, 0, 0))

    def test_signature_guard(self):
        with self.assertRaisesRegex(RuntimeError, "signature"):
            self.run_import(signature_ok=False)


if __name__ == "__main__":
    unittest.main()
