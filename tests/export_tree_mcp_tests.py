"""Read back same-name image and FBX outputs from the packaged desktop MCP."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import subprocess

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from PIL import Image


async def main():
    parser = argparse.ArgumentParser()
    for name in ("runtime", "probe", "work"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    parser.add_argument("--build-tree", action="store_true", help="Resolve undeployed Qt dependencies from PATH")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=False)
    sources, outputs = args.work / "source", args.work / "output"
    outputs.mkdir()
    branches = [Path("角色") / "材质", Path("道具") / "材质"]
    colours = [(231, 41, 17, 200), (5, 140, 219, 150)]
    for i, folder in enumerate(branches):
        directory = sources / folder
        directory.mkdir(parents=True)
        Image.new("RGBA", (12, 7), colours[i]).save(directory / "asset.png")
        (directory / "asset.obj").write_text(f"v 0 0 0\nv {i+1} 0 0\nv 0 1 0\nf 1 2 3\n", encoding="ascii")
    scope = args.work / "scope.json"
    scope.write_text(json.dumps({"input_roots": [str(sources)], "output_roots": [str(outputs)],
                                 "allow_overwrite": False}), encoding="utf8")
    env = dict(FORMAVIEW_DESKTOP_SCOPE=str(scope), FORMAVIEW_IMAGE_STATE_ROOT=str(args.work / "state"),
               SystemRoot=os.environ["SystemRoot"], PATH=str(Path(os.environ["SystemRoot"]) / "System32"))
    if args.build_tree:
        env["PATH"] = os.environ["PATH"]
    params = StdioServerParameters(command=str(args.runtime / "mcp/formaview-mcp.exe"), env=env)
    async with stdio_client(params) as io, ClientSession(*io) as client:
        await client.initialize()

        async def call(tool, **kwargs):
            response = await client.call_tool(tool, kwargs)
            assert not response.isError, response
            result = response.structuredContent or json.loads(response.content[0].text)
            assert "error" not in result, result
            return result

        async def terminal(kind, key):
            for _ in range(300):
                result = await call("get_image_" + kind, **{kind + "_id": key})
                if result["state"] not in ("queued", "planning", "running"):
                    return result
                await asyncio.sleep(.05)
            raise AssertionError("Timed out waiting for " + kind)

        for kind, extension, tool in (("png", "tif", "plan_image_batch"), ("obj", "fbx", "plan_fbx_batch")):
            out = outputs / extension
            out.mkdir()
            kwargs = dict(inputs=[str(sources / b / ("asset." + kind)) for b in branches], output_directory=str(out),
                          name_template="{name}.{ext}")
            if kind == "png":
                kwargs.update(output_extension="tif", source_alpha="straight")
            else:
                kwargs.update(include_animations=False)
            assert (await client.call_tool(tool, kwargs)).isError, "Flat mode must report the name collision"
            assert (await client.call_tool(tool, dict(kwargs, source_root=str(sources / branches[0])))).isError
            created = await call(tool, **kwargs, source_root=str(sources))
            ready = await terminal("plan", created["plan_id"])
            assert ready["state"] == "ready" and ready["total"] == 2, ready
            assert not list(out.iterdir()), "Preflight created output folders"
            paths = [out / b / ("asset." + extension) for b in branches]
            assert [Path(item["preflight"]["output"]) for item in ready["items"]] == paths
            submitted = await call("submit_image_job", plan_id=ready["id"], approval_digest=ready["approval_digest"],
                                   acknowledge_losses=True, idempotency_key="tree-" + kind)
            job = await terminal("job", submitted["job_id"])
            assert job["state"] == "succeeded" and job["total"] == 2, job
            for i, (row, path) in enumerate(zip(job["items"], paths, strict=True)):
                assert Path(row["result"]["committed_output"]) == path
                assert hashlib.sha256(path.read_bytes()).hexdigest() == row["result"]["output_sha256"]
                if kind == "png":
                    with Image.open(path) as im:
                        assert im.size == (12, 7) and im.convert("RGBA").getpixel((5, 3)) == colours[i]
                else:
                    report = args.work / f"mesh-{i}.json"
                    subprocess.run([str(args.probe), str(path), str(report)], check=True)
                    mesh, = json.loads(report.read_text(encoding="utf8"))["meshes"]
                    assert sorted(mesh["points"]) == sorted([[0, 0, 0], [i + 1, 0, 0], [0, 1, 0]]), mesh
            assert not (out / ("asset." + extension)).exists()
            repeated = await call(tool, **kwargs, source_root=str(sources))
            ready = await terminal("plan", repeated["plan_id"])
            review = await call("get_image_plan", plan_id=ready["id"], summary_only=True)
            assert review["planned_outputs"]["skip"]["count"] == 2, review
            # A file directly under the chosen root has no intermediate directory.
            one = dict(kwargs, inputs=kwargs["inputs"][:1], source_root=str(sources / branches[0]))
            one.pop("name_template")
            root_plan = await call(tool, **one)
            root_item = (await terminal("plan", root_plan["plan_id"]))["items"][0]
            assert Path(root_item["preflight"]["output"]) == out / ("asset." + extension)
            print(tool, "verified both paths and decoded contents")

if __name__ == "__main__":
    asyncio.run(main())
