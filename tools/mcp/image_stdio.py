"""MCP stdio adapter for the shared native image request boundary.

Explicit environment configuration is required. With FORMAVIEW_IMAGE_ENDPOINT
it exposes the durable native batch service. Scope-only mode is the single-file
technical worker endpoint and does not advertise batch operations.
It does not change user MCP configuration or grant filesystem scope.
"""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import re
import time
from typing import Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from mcp.types import LATEST_PROTOCOL_VERSION
from mcp.shared.version import SUPPORTED_PROTOCOL_VERSIONS

server = FastMCP('FormaView images', instructions='Use inspect then preflight. Conversion requires the returned approval digest and explicit acknowledgement of reported losses. Filesystem access is limited by the native host scope configuration.')
workers = asyncio.Semaphore(2)
controls = asyncio.Semaphore(4)


def pipe_request(endpoint: str, message: bytes) -> dict[str, Any]:
    """Bounded native named-pipe client; no per-call process or shell."""
    import pywintypes
    import win32file
    import win32pipe
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', endpoint):
        raise ValueError('Invalid local endpoint name')
    name = '\\\\.\\pipe\\' + endpoint
    deadline = time.monotonic() + 300
    connection_deadline = time.monotonic() + 3
    while True:
        try:
            handle = win32file.CreateFile(name, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None, win32file.OPEN_EXISTING, 0, None)
            break
        except pywintypes.error as error:
            if error.winerror != 231 or time.monotonic() >= connection_deadline:
                raise RuntimeError('Cannot connect to image service') from error
            try: win32pipe.WaitNamedPipe(name, 100)
            except pywintypes.error: pass
    try:
        # Nonblocking pipe mode allows a bounded timeout even when a service
        # stops responding. Polling occurs only on a reserved adapter worker.
        win32pipe.SetNamedPipeHandleState(handle, win32pipe.PIPE_NOWAIT, None, None)
        offset = 0
        while offset < len(message):
            if time.monotonic() >= deadline: raise TimeoutError('Service write timed out')
            try:
                _, written = win32file.WriteFile(handle, message[offset:])
                offset += written
                if not written: time.sleep(.001)
            except pywintypes.error as error:
                if error.winerror != 232: raise
                time.sleep(.001)
        response = bytearray()
        while b'\n' not in response:
            if time.monotonic() >= deadline: raise TimeoutError('Service response timed out')
            try:
                _, block = win32file.ReadFile(handle, 65536)
                response.extend(block)
                if not block: time.sleep(.001)
            except pywintypes.error as error:
                if error.winerror != 232: raise
                time.sleep(.001)
            if len(response) > 4 * 1024 * 1024: raise RuntimeError('Service response exceeds four MiB')
        result = json.loads(response)
        if not isinstance(result, dict): raise RuntimeError('Invalid service response')
        return result
    finally:
        handle.Close()


def single_file_tool(**kwargs):
    return (lambda function: function) if os.environ.get('FORMAVIEW_IMAGE_ENDPOINT') else server.tool(**kwargs)


def batch_tool(**kwargs):
    return server.tool(**kwargs) if os.environ.get('FORMAVIEW_IMAGE_ENDPOINT') else (lambda function: function)


async def native_request(request: dict[str, Any]) -> dict[str, Any]:
    endpoint = os.environ.get('FORMAVIEW_IMAGE_ENDPOINT')
    if endpoint:
        params = server.get_context().session.client_params
        if params is not None:
            request = dict(request, _mcp_client={
                'name': params.clientInfo.name, 'version': params.clientInfo.version,
                'protocol': params.protocolVersion if params.protocolVersion in SUPPORTED_PROTOCOL_VERSIONS else LATEST_PROTOCOL_VERSION})
    message = json.dumps(request, ensure_ascii=False, allow_nan=False).encode('utf-8') + b'\n'
    if len(message) > 1024 * 1024:
        raise ValueError('Request exceeds one MiB')
    lane = controls if request.get('operation') in ('get_plan', 'get_job', 'list_jobs', 'cancel', 'get_discovery') else workers
    async with lane:
        if endpoint:
            communication = asyncio.create_task(asyncio.to_thread(pipe_request, endpoint, message))
            try:
                return await asyncio.shield(communication)
            except asyncio.CancelledError:
                await communication
                raise
        executable = Path(os.environ['FORMAVIEW_IMAGE_CLI'])
        scope = Path(os.environ['FORMAVIEW_IMAGE_SCOPE'])
        if not executable.is_absolute() or not scope.is_absolute():
            raise ValueError('Native executable and scope must be absolute paths')
        arguments = ['--scope', str(scope)]
        process = await asyncio.create_subprocess_exec(str(executable), *arguments,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        communication = asyncio.create_task(process.communicate(message))
        try:
            stdout, stderr = await asyncio.shield(communication)
        except asyncio.CancelledError:
            # Do not kill a writer at an unknown commit boundary merely because
            # its transport disconnected. Endpoint-mode jobs retain outcomes
            # in the service; scope-only calls have no durable job semantics.
            await communication
            raise
    if len(stdout) > 4 * 1024 * 1024:
        raise RuntimeError('Native response exceeds four MiB')
    try:
        result = json.loads(stdout)
    except (ValueError, UnicodeError) as error:
        raise RuntimeError(f'Native host returned invalid JSON (exit {process.returncode})') from error
    if process.returncode and 'error' not in result:
        raise RuntimeError(f'Native host failed (exit {process.returncode})')
    # Keep structured native errors, including out_of_scope/input_changed,
    # inside the result contract for GUI/MCP equivalence.
    return result


@server.tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def get_image_capabilities() -> dict[str, Any]:
    """Query the native host's available readers and conversion targets, shared with the GUI.

    Availability does not guarantee that a particular file, variant or conversion
    is compatible. Inspect actual data and review preflight before submission.
    """
    return await native_request({'version': 1, 'operation': 'capabilities'})


@server.tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def inspect_image(path: str, subresource: int = 0, offset: int = 0, limit: int = 100) -> dict[str, Any]:
    """Inspect an authorized image and page subresources; decode only the selected entry's actual channel layout."""
    return await native_request({'version': 1, 'operation': 'inspect', 'input': path,
                                 'subresource': subresource, 'offset': offset, 'limit': limit})


@single_file_tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def preflight_image(request: dict[str, Any]) -> dict[str, Any]:
    """Validate input/output/options/conflict and return losses plus an approval digest; writes no destination file.

    request: input, output, conflict (skip/rename/overwrite), options. options
    requires target (png/jpeg/bmp/tga/tiff/webp/dds/hdr/exr) and channels
    [{source,destination}]. Use inspect's actual channel names. Source A needs
    alpha_channel='A' and source_alpha='straight' or 'premultiplied'. Explicit
    alpha_action, output_scalar, quantization, transfer, meaning, allow_lossy,
    window and metadata policies control changes; defaults preserve/reject loss.
    """
    return await native_request(dict(request, version=1, operation='preflight'))


@single_file_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False))
async def convert_image(request: dict[str, Any], approval_digest: str, acknowledge_losses: bool = False) -> dict[str, Any]:
    """Execute the same preflighted parameters under the configured scope and atomically commit one output.

    The digest must match current source identity, parameters and scope. Any
    reported loss needs explicit acknowledgement. This single-file endpoint
    does not claim durable batch job IDs, retry or idempotency semantics.
    """
    return await native_request(dict(request, version=1, operation='convert',
        approval_digest=approval_digest, acknowledge_losses=acknowledge_losses))


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def discover_images(directory: str, recursive: bool = False) -> dict[str, Any]:
    """Snapshot authorized image filename membership for paged selection; writes only its discovery journal.

    Skips and reports reparse entries, never follows them. Extensions are candidates,
    not proof of decodability; each selected file is validated by plan preflight.
    Limits: 4096 images, 512 directories and 100000 visited entries. The snapshot
    does not grow when the directory later changes. Submit up to 64 selected items
    per plan with explicit output paths and conversion options.
    """
    return await native_request({'version': 1, 'operation': 'discover', 'directory': directory, 'recursive': recursive})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def get_image_discovery(discovery_id: str, offset: int = 0, limit: int = 100) -> dict[str, Any]:
    """Page a fixed directory-membership snapshot and its excluded reparse points."""
    return await native_request({'version': 1, 'operation': 'get_discovery', 'id': discovery_id, 'offset': offset, 'limit': limit})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def create_image_plan(items: list[dict[str, Any]]) -> dict[str, Any]:
    """Queue immutable batch preflight; writes journal records but no destination images.

    Each item requires input, output, options and optional conflict=skip/rename/overwrite.
    Image options require target and explicit channels [{source,destination}]; preserve/reject-loss
    defaults apply. Mixed image/model plans are supported: model items use target='fbx'
    and the options documented by create_model_export_plan, without image channels.
    Query get_image_plan until ready, review every page and its losses,
    then submit the returned approval_digest. Up to 64 items per plan.
    """
    return await native_request({'version': 1, 'operation': 'create_plan', 'items': items})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def get_image_plan(plan_id: str, offset: int = 0, limit: int = 100) -> dict[str, Any]:
    """Page immutable preflight parameters, source snapshots and losses. Only ready plans can be submitted."""
    return await native_request({'version': 1, 'operation': 'get_plan', 'id': plan_id, 'offset': offset, 'limit': limit})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=True, openWorldHint=False))
async def submit_image_job(plan_id: str, approval_digest: str, idempotency_key: str, acknowledge_losses: bool = False) -> dict[str, Any]:
    """Submit an approved immutable plan. Reusing the same key and arguments returns the original job, including after restart; differing arguments conflict.

    Input changes are rejected per item at execution. Explicit loss acknowledgement is
    required if preflight reports any loss. Client disconnect does not cancel jobs.
    """
    return await native_request({'version': 1, 'operation': 'submit', 'plan_id': plan_id,
        'approval_digest': approval_digest, 'idempotency_key': idempotency_key, 'acknowledge_losses': acknowledge_losses})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def get_image_job(job_id: str, offset: int = 0, limit: int = 100) -> dict[str, Any]:
    """Read actual per-item output paths, hashes, failures and cancellation outcomes with bounded pagination."""
    return await native_request({'version': 1, 'operation': 'get_job', 'id': job_id, 'offset': offset, 'limit': limit})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def list_image_jobs(offset: int = 0, limit: int = 100) -> dict[str, Any]:
    """Discover retained jobs after reconnecting; reports journal recovery states."""
    return await native_request({'version': 1, 'operation': 'list_jobs', 'offset': offset, 'limit': limit})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=True, openWorldHint=False))
async def cancel_image_work(work_id: str, kind: str = 'job') -> dict[str, Any]:
    """Request cancellation of a job or plan. Accepted means requested, not stopped; query until terminal. Committed outputs are retained."""
    return await native_request({'version': 1, 'operation': 'cancel', 'id': work_id, 'kind': kind})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def retry_image_plan(job_id: str) -> dict[str, Any]:
    """Create fresh preflight for only unfinished/failed/cancelled items of a terminal job. Never automatically resubmit; review and approve the new plan. Interrupted items may have an uncertain output outcome."""
    return await native_request({'version': 1, 'operation': 'retry_plan', 'id': job_id})


@server.tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def inspect_model(input: str) -> dict[str, Any]:
    """Inspect supported model source structure and stable animation clip IDs for FBX export.

    Does not inherit preview root lock. Query capabilities.model_export for actual formats.
    """
    return await native_request({'version': 1, 'operation': 'inspect', 'input': input})


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def create_model_export_plan(items: list[dict[str, Any]]) -> dict[str, Any]:
    """Preflight FBX export through the shared durable job service, up to 64 explicit outputs.

    Each item: input, output, conflict=skip/rename/overwrite, options.
    options: target='fbx'. PSK/PSKX/PSA additionally require
    coordinates='actorx-z-up-x-forward-rh-cm'; other inputs use source conventions.
    OBJ/glTF/GLB/FBX/PMX/VRM share this interface when listed in capabilities.
    For multi-animation FBX splitting, inspect the source, then create one item
    per clip_id. Each clip keeps its original time interval.
    Optional output_axes: source/maya/houdini/unreal/3ds-max/cinema4d/blender/unity;
    output_unit: source/cm/m/mm/in. Defaults preserve explicit source convention.
    PSK/PSKX alone exports a model. PSA requires clip_id from inspect_model.
    Pair PSA with options.reference=PSK/PSKX, or PSK/PSKX with options.animation=PSA.
    PSA automatically uses a same-directory, same-basename .config (requires
    reference PSK/PSKX). Preflight reports applied rules and file snapshot;
    added/changed/deleted configs invalidate the plan. Optional companion overrides
    discovery explicitly. pairing_policy='strict-records' (default)
    or explicitly 'reference-skeleton'; include_mesh=true by default.
    Use clip_id for one clip, or clip_ids=[stable IDs] for multiple independent
    Animation Stacks in one FBX (no concatenation). No retargeting or viewport root lock.
    Use shared get_image_plan, submit_image_job, get_image_job, cancel_image_work,
    retry_image_plan and list_image_jobs for the same plan/job IDs. Review losses before submit.
    """
    return await native_request({'version': 1, 'operation': 'create_plan', 'items': items})


if __name__ == '__main__':
    server.run(transport='stdio')
