"""MCP stdio adapter for the shared native image request boundary.

Explicit environment configuration is required. With FORMAVIEW_IMAGE_ENDPOINT
it exposes the durable native batch service. Scope-only mode is the single-file
technical worker endpoint and does not advertise batch operations.
It does not change user MCP configuration or grant filesystem scope.
"""
import asyncio
from contextlib import AsyncExitStack, asynccontextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import re
import time
from typing import Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from mcp.types import LATEST_PROTOCOL_VERSION
from mcp.shared.version import SUPPORTED_PROTOCOL_VERSIONS

# The desktop publishes a fresh session pipe in this per-user file on each enable.
# Resolve it per call so a registered client can survive a host restart.
desktop_link = None
if __name__ == '__main__' and sys.argv[1:2] == ['--connect']:
    if len(sys.argv) != 3 or not Path(sys.argv[2]).is_absolute():
        raise SystemExit('Usage: --connect <absolute desktop connection file>')
    desktop_link = Path(sys.argv[2])
background_scope = None if desktop_link else os.environ.get('FORMAVIEW_DESKTOP_SCOPE')
configured_endpoint = None if desktop_link else os.environ.get('FORMAVIEW_IMAGE_ENDPOINT')
batch_enabled = bool(desktop_link or configured_endpoint or background_scope)
desktop_enabled = bool(desktop_link or background_scope or (configured_endpoint and os.environ.get('FORMAVIEW_DESKTOP') == '1'))
active_endpoint = configured_endpoint
window_endpoint = None
window_process = None
window_asset = None
owned_hosts = None
window_lock = asyncio.Lock()


@asynccontextmanager
async def scoped_host(option):
    scope = Path(background_scope)
    adapter = Path(sys.executable) if getattr(sys, 'frozen', False) else Path(__file__)
    executable = adapter.resolve().parent.parent / 'formaview.exe'
    if not scope.is_absolute() or not scope.is_file() or not executable.is_file():
        raise ValueError('Use the packaged adapter and an existing absolute desktop scope file')
    process = await asyncio.create_subprocess_exec(str(executable), option, str(scope), str(os.getpid()),
        stdout=asyncio.subprocess.PIPE, stdin=asyncio.subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        async with asyncio.timeout(30):
            while line := await process.stdout.readline():
                if line.startswith(b'{'):
                    endpoint = json.loads(line)['endpoint']
                    break
            else:
                raise RuntimeError('FormaView exited before its scoped endpoint was ready')
        yield endpoint, process
    finally:
        try:
            if 'endpoint' in locals() and process.returncode is None:
                import pywintypes
                try:
                    await asyncio.wait_for(asyncio.to_thread(pipe_request, endpoint,
                        b'{"version":1,"operation":"desktop.close"}\n'), 5)
                except pywintypes.error as error:
                    # The owned host can finish before the final pipe read.
                    if error.winerror not in (109, 233):
                        raise
                if await asyncio.wait_for(process.wait(), 10) != 0:
                    raise RuntimeError('Background FormaView exited with an error')
        finally:
            if process.returncode is None:
                process.terminate()
                await process.wait()


@asynccontextmanager
async def host_lifetime(_server):
    global active_endpoint, owned_hosts, window_endpoint, window_process, window_asset
    if not background_scope:
        yield {}
        return
    if configured_endpoint:
        raise ValueError('Choose a background scope or an existing endpoint')
    async with AsyncExitStack() as stack:
        active_endpoint, _ = await stack.enter_async_context(scoped_host('--mcp-background'))
        owned_hosts = stack
        try:
            yield {}
        finally:
            owned_hosts = window_endpoint = window_process = window_asset = active_endpoint = None


server = FastMCP('FormaView', lifespan=host_lifetime, instructions=(
    'Open a scoped image or video to obtain an asset_id. Pause before stepping. '
    'Use read_texture_pixel directly for stored samples. Query get_image_capabilities '
    'with a source extension for export targets. Conversion requires native preflight and approval.'))
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
    return (lambda function: function) if batch_enabled else server.tool(**kwargs)


def batch_tool(**kwargs):
    return server.tool(**kwargs) if batch_enabled else (lambda function: function)


async def native_request(request: dict[str, Any], *, endpoint=None) -> dict[str, Any]:
    global window_asset
    if not endpoint and window_endpoint and request.get('asset_id') and request['asset_id']==window_asset:
        endpoint = window_endpoint
    if desktop_link:
        try:
            endpoint = json.loads(desktop_link.read_text(encoding='utf-8'))['endpoint']
            if not isinstance(endpoint, str) or not endpoint:
                raise ValueError('Missing desktop endpoint')
        except (OSError, KeyError, ValueError, TypeError) as error:
            raise RuntimeError('Open FormaView and turn on MCP in Settings.') from error
    endpoint = endpoint or active_endpoint
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
                result = await asyncio.shield(communication)
                if endpoint == window_endpoint and result.get('asset_id'):
                    window_asset = result['asset_id']
                return result
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
async def get_image_capabilities(source_extension: str | None = None) -> dict[str, Any]:
    """List native readers/targets, or filter by an input extension such as .JPG, glb or abc.

    Availability does not guarantee that a particular file, variant or conversion
    is compatible. Inspect actual data and review preflight before submission.
    """
    caps = await native_request({'version': 1, 'operation': 'capabilities'})
    if source_extension is None or 'error' in caps:
        return caps
    ext = source_extension.strip().lower().removeprefix('.')
    result = {'source_extension': ext, 'kind': 'unsupported', 'output_formats': []}
    if '.' + ext in caps.get('read_extensions', []):
        result.update(kind='image', output_formats=caps['write_targets'])
    else:
        model = caps.get('model_export', {})
        sample = model.get('abc_frame_export', {})
        if ext in model.get('input_formats', []) or ext == sample.get('input'):
            result.update(kind='model', output_formats=model['output_formats'],
                          output_axes=model['output_axes'], output_units=model['output_units'])
            if ext == sample.get('input'):
                result['frame_export'] = sample
    return result


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


def image_target(suffix, capabilities):
    target = {'jpg': 'jpeg', 'jpe': 'jpeg', 'jfif': 'jpeg', 'tif': 'tiff',
              'rgbe': 'hdr'}.get(suffix, suffix)
    if target not in capabilities['write_targets']:
        raise ValueError('Choose an output extension for one of: ' + ', '.join(capabilities['write_targets']))
    return target


def batch_destinations(inputs, output_directory, extension):
    directory = Path(output_directory)
    if not 1 <= len(inputs) <= 64:
        raise ValueError('Select between 1 and 64 input files per plan.')
    if not directory.is_absolute() or not all(Path(path).is_absolute() for path in inputs):
        raise ValueError('Choose absolute input paths and an absolute output directory.')
    outputs = [directory / (Path(path).stem + '.' + extension) for path in inputs]
    if len(set(outputs)) != len(inputs):
        raise ValueError('Inputs map to the same output name. Use create_image_plan with distinct output paths.')
    return outputs


async def image_plan_item(input, output, target, subresource, alpha_action, source_alpha,
                          allow_lossy, quality, output_scalar, quantization, conflict):
    info = await inspect_image(input, subresource, limit=1)
    if 'error' in info:
        return info
    if 'selected_channels' not in info:
        raise ValueError('Expected an image input. Use plan_fbx_export for models.')
    names = [channel['name'] for channel in info['selected_channels']]
    options = {'target': target, 'subresource': subresource,
               'source_alpha': (source_alpha or info['alpha']) if 'A' in names else info['alpha'],
               'channels': [{'source': name, 'destination': name} for name in names
                            if name != 'A' or alpha_action == 'preserve'],
               'alpha_action': alpha_action, 'allow_lossy': allow_lossy,
               'quality': quality, 'quantization': quantization}
    if 'A' in names:
        options['alpha_channel'] = 'A'
    if output_scalar is not None:
        options['output_scalar'] = output_scalar
    return {'input': input, 'output': output, 'options': options, 'conflict': conflict}


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def plan_image_conversion(
    input: str, output: str, subresource: int = 0,
    alpha_action: Literal['preserve', 'drop'] = 'preserve',
    source_alpha: Literal['straight', 'premultiplied'] | None = None,
    allow_lossy: bool = False, quality: int = 90,
    output_scalar: Literal['uint8', 'uint16', 'half', 'float32'] | None = None,
    quantization: Literal['reject_loss', 'clamp_round'] = 'reject_loss',
    conflict: Literal['skip', 'rename', 'overwrite'] = 'skip',
) -> dict[str, Any]:
    """Preflight one image using its real channel names and the output filename's format.

    Preserves alpha and precision by default. RGB JPEG from RGBA needs explicit
    alpha_action='drop' and allow_lossy=true. If the source alpha mode is unknown,
    specify source_alpha from its provenance. For custom channel mappings, DDS
    encoding or compositing use create_image_plan. Review via get_image_plan.
    """
    caps = await get_image_capabilities()
    if 'error' in caps:
        return caps
    target = image_target(Path(output).suffix.lower().removeprefix('.'), caps)
    item = await image_plan_item(input, output, target, subresource,
                                 alpha_action, source_alpha, allow_lossy, quality, output_scalar, quantization, conflict)
    return item if 'error' in item else await create_image_plan([item])


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def plan_image_batch(
    inputs: list[str], output_directory: str, output_extension: str,
    subresource: int = 0, alpha_action: Literal['preserve', 'drop'] = 'preserve',
    source_alpha: Literal['straight', 'premultiplied'] | None = None,
    allow_lossy: bool = False, quality: int = 90,
    output_scalar: Literal['uint8', 'uint16', 'half', 'float32'] | None = None,
    quantization: Literal['reject_loss', 'clamp_round'] = 'reject_loss',
    conflict: Literal['skip', 'rename', 'overwrite'] = 'skip',
) -> dict[str, Any]:
    """Preflight 1–64 selected images in one plan, using each image's actual channels.

    Pass absolute input paths (e.g. selected from get_image_discovery), an existing
    authorized output directory and a target extension such as .tif. Outputs keep
    their input stems. For duplicate stems use create_image_plan with explicit names.
    No outputs are written until the reviewed plan is submitted via submit_image_job.
    """
    caps = await get_image_capabilities()
    if 'error' in caps:
        return caps
    extension = output_extension.strip().lower().removeprefix('.')
    target = image_target(extension, caps)
    destinations = batch_destinations(inputs, output_directory, extension)
    items = []
    for path, destination in zip(inputs, destinations):
        item = await image_plan_item(path, str(destination), target, subresource, alpha_action, source_alpha,
                                     allow_lossy, quality, output_scalar, quantization, conflict)
        if 'error' in item:
            return dict(item, input=path)
        items.append(item)
    return await create_image_plan(items)


FbxAxes = Literal['source', 'maya', 'houdini', 'unreal', '3ds-max', 'cinema4d', 'blender', 'unity']
FbxUnit = Literal['source', 'cm', 'm', 'mm', 'in']


def fbx_options(input, axes, unit, include_mesh, animation, reference):
    options = {'target': 'fbx', 'output_axes': axes, 'output_unit': unit, 'include_mesh': include_mesh}
    if Path(input).suffix.lower() in ('.psk', '.pskx', '.psa'):
        options['coordinates'] = 'actorx-z-up-x-forward-rh-cm'
    if animation is not None:
        options['animation'] = animation
    if reference is not None:
        options['reference'] = reference
    return options


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def plan_fbx_export(
    input: str, output: str, output_axes: FbxAxes = 'source', output_unit: FbxUnit = 'source',
    include_mesh: bool = True, clip_ids: list[str] | None = None,
    animation: str | None = None, reference: str | None = None, abc_sample: int | None = None,
    conflict: Literal['skip', 'rename', 'overwrite'] = 'skip',
) -> dict[str, Any]:
    """Preflight one FBX in a named coordinate system and unit. Source conventions are the default.

    Use inspect_model for stable clip IDs. Supply one or more IDs to export animation stacks.
    Pair PSK/PSKX with animation=PSA, or PSA with reference=PSK/PSKX. For Alembic
    choose a zero-based sample for a static mesh. Review get_image_plan before submit_image_job.
    """
    if Path(output).suffix.lower() != '.fbx':
        raise ValueError('Choose an .fbx output filename.')
    source_ext = Path(input).suffix.lower()
    if abc_sample is not None and source_ext != '.abc':
        raise ValueError('abc_sample selects a frame from an .abc input.')
    options = fbx_options(input, output_axes, output_unit, include_mesh, animation, reference)
    if clip_ids is not None:
        options['clip_ids'] = clip_ids
    if abc_sample is not None:
        options['abc_sample'] = abc_sample
    return await create_model_export_plan([{'input': input, 'output': output, 'options': options, 'conflict': conflict}])


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def plan_fbx_batch(
    inputs: list[str], output_directory: str, output_axes: FbxAxes = 'source', output_unit: FbxUnit = 'source',
    include_animations: bool = True, abc_sample: int | None = None,
    conflict: Literal['skip', 'rename', 'overwrite'] = 'skip',
) -> dict[str, Any]:
    """Preflight one FBX per selected source, using a shared coordinate preset and unit.

    Includes meshes and, by default, every embedded animation as a separate stack.
    For ABC inputs specify the zero-based sample to bake as static geometry.
    Use plan_fbx_export for paired animations, selected clip IDs or animation-only output.
    The native plan reports format-specific losses and dependencies before approval.
    """
    destinations = batch_destinations(inputs, output_directory, 'fbx')
    has_abc = any(Path(path).suffix.lower() == '.abc' for path in inputs)
    if has_abc != (abc_sample is not None) or (has_abc and abc_sample < 0):
        raise ValueError('Specify a non-negative abc_sample only when the selection contains an ABC file.')
    items = []
    for path, output in zip(inputs, destinations):
        options = fbx_options(path, output_axes, output_unit, True, None, None)
        if Path(path).suffix.lower() == '.abc':
            options['abc_sample'] = abc_sample
        elif include_animations:
            inventory = await inspect_model(path)
            if 'error' in inventory:
                return dict(inventory, input=path)
            if 'clips' not in inventory:
                raise ValueError('Expected a model or animation input: ' + path)
            if inventory['clips']:
                options['clip_ids'] = [clip['id'] for clip in inventory['clips']]
        items.append({'input': path, 'output': str(output), 'options': options, 'conflict': conflict})
    return await create_model_export_plan(items)


@batch_tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
async def plan_fbx_clips(
    input: str, output_directory: str, clip_ids: list[str] | None = None,
    output_axes: FbxAxes = 'source', output_unit: FbxUnit = 'source', include_mesh: bool = True,
    animation: str | None = None, reference: str | None = None,
    conflict: Literal['skip', 'rename', 'overwrite'] = 'skip',
) -> dict[str, Any]:
    """Preflight one FBX file per animation clip in an existing authorized output directory.

    Omit clip_ids for all source clips (maximum 64 per plan), or select IDs from
    inspect_model. Filenames include the original clip index so duplicate names
    stay distinct. Inspect get_image_plan for destinations and losses before submitting.
    """
    directory = Path(output_directory)
    if not directory.is_absolute():
        raise ValueError('Choose an absolute output directory.')
    inventory = await inspect_model(animation or input)
    if 'error' in inventory:
        return inventory
    clips = inventory.get('clips', [])
    selected = {c['id'] for c in clips} if clip_ids is None else set(clip_ids)
    if not 1 <= len(selected) <= 64:
        raise ValueError('Select between 1 and 64 source clips per plan.')
    if clip_ids is not None and (len(selected) != len(clip_ids) or selected.difference(c['id'] for c in clips)):
        raise ValueError('Use distinct clip IDs from inspect_model.')
    def filename_part(value):
        return re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', value)[:48].strip(' .') or 'clip'

    source = filename_part(Path(input).stem)
    base = fbx_options(input, output_axes, output_unit, include_mesh, animation, reference)
    items = []
    for index, clip in enumerate(clips, 1):
        if clip['id'] in selected:
            name = f"{source}-{index:02d}-{filename_part(clip['name'])}.fbx"
            items.append({'input': input, 'output': str(directory / name), 'conflict': conflict,
                          'options': dict(base, clip_id=clip['id'])})
    return await create_model_export_plan(items)


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


@server.tool(annotations=ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False))
async def read_texture_pixel(path: str, x: int, y: int, subresource: int = 0) -> dict[str, Any]:
    """Read decoded source channels and interpreted values, before display transforms. Coordinates are zero-based relative to the selected data window. Does not change the active preview."""
    return await native_request(dict(version=1, operation='pixel', input=path, x=x, y=y, subresource=subresource))


def desktop_tool(*, read_only=False):
    annotations = ToolAnnotations(readOnlyHint=read_only, destructiveHint=False, openWorldHint=False)
    return server.tool(annotations=annotations) if desktop_enabled else (lambda function: function)


@desktop_tool()
async def open_asset(path: str, window: bool = False) -> dict[str, Any]:
    """Open an authorized video or image. Background sessions open videos paused. Set window=True for a visible player owned by this connection. Attached clients use their existing window."""
    global window_endpoint, window_process
    if window and owned_hosts:
        async with window_lock:
            if window_process is None or window_process.returncode is not None:
                window_endpoint, window_process = await owned_hosts.enter_async_context(scoped_host('--mcp-window'))
            return await native_request(dict(version=1, operation='desktop.open', path=path), endpoint=window_endpoint)
    return await native_request(dict(version=1, operation='desktop.open', path=path))


@desktop_tool(read_only=True)
async def get_window_state(asset_id: str | None = None) -> dict[str, Any]:
    """List displays and read player mode, screen ID, topmost, client/frame geometry in Qt logical pixels. Reading never opens a window. Omit ID to inspect the connection's visible window if present."""
    endpoint = window_endpoint if not asset_id and window_process and window_process.returncode is None else None
    return await native_request(dict(version=1, operation='desktop.window_state', asset_id=asset_id), endpoint=endpoint)


@desktop_tool()
async def control_player_window(asset_id: str, action: Literal['normal', 'fullscreen', 'mini', 'move', 'resize'],
                                screen_id: str | None = None, width: int | None = None, height: int | None = None) -> dict[str, Any]:
    """Set player mode idempotently, move to a screen ID returned by get_window_state, or resize a topmost mini window in logical pixels. Normal restores the pre-mode main window."""
    return await native_request(dict(version=1, operation='desktop.window_control', asset_id=asset_id,
                                    action=action, screen_id=screen_id, width=width, height=height))


@desktop_tool(read_only=True)
async def get_playback_state(asset_id: str) -> dict[str, Any]:
    """Read active video position in seconds, pause, duration, rate and volume. Position is player state, not physical screen presentation."""
    return await native_request(dict(version=1, operation='desktop.state', asset_id=asset_id))


@desktop_tool()
async def control_video(asset_id: str, action: Literal['play', 'pause', 'seek', 'seek_relative', 'speed', 'volume', 'subtitle_delay_ms'],
                        value: float | None = None) -> dict[str, Any]:
    """Play/pause, seek in absolute or signed relative seconds, set speed (0.1–8), volume (0–100), or signed subtitle delay in milliseconds (±60000)."""
    return await native_request(dict(version=1, operation='desktop.control', asset_id=asset_id, action=action, value=value))


@desktop_tool()
async def set_video_toggle(asset_id: str, name: Literal['mute', 'sub-visibility', 'loop-file'], enabled: bool) -> dict[str, Any]:
    """Mute, show subtitles without changing the selected track, or repeat the whole file. Supply the desired state."""
    return await native_request(dict(version=1, operation='desktop.toggle', asset_id=asset_id, name=name, enabled=enabled))


@desktop_tool()
async def video_chapters(asset_id: str, action: Literal['list', 'select'] = 'list', index: int | None = None) -> dict[str, Any]:
    """List chapter titles and start times, or jump to a zero-based chapter index."""
    return await native_request(dict(version=1, operation='desktop.chapters', asset_id=asset_id, action=action, index=index))


@desktop_tool()
async def step_video_frame(asset_id: str, direction: Literal['previous', 'next']) -> dict[str, Any]:
    """Advance or reverse one frame in a paused video and return the observed player position."""
    return await native_request(dict(version=1, operation='desktop.step', asset_id=asset_id, direction=direction))


@desktop_tool()
async def capture_video_frame(asset_id: str, output: str, subtitles: bool = True) -> dict[str, Any]:
    """Save the decoded video frame as a new PNG in the authorized output root. Pause first for a reproducible capture. Includes subtitles when requested."""
    return await native_request(dict(version=1, operation='desktop.capture', asset_id=asset_id, output=output, subtitles=subtitles))


@desktop_tool()
async def video_playlist(asset_id: str, action: Literal['list','select','next','previous'] = 'list',
                         index: int | None = None, offset: int = 0) -> dict[str, Any]:
    """Page the video playlist (100 entries per page), select a zero-based index or navigate. Unavailable cross-folder entries omit the path. Switching returns a new asset_id."""
    return await native_request(dict(version=1, operation='desktop.playlist', asset_id=asset_id, action=action, index=index, offset=offset))


@desktop_tool(read_only=True)
async def list_video_tracks(asset_id: str) -> dict[str, Any]:
    """List embedded and external audio/subtitle tracks and their player IDs."""
    return await native_request(dict(version=1, operation='desktop.tracks', asset_id=asset_id))


@desktop_tool()
async def select_video_track(asset_id: str, type: Literal['audio','sub'], id: int) -> dict[str, Any]:
    """Select a listed track of the given type. ID zero disables that track type."""
    return await native_request(dict(version=1, operation='desktop.track', asset_id=asset_id, type=type, id=id))


@desktop_tool()
async def load_video_subtitle(asset_id: str, path: str) -> dict[str, Any]:
    """Load and select an authorized local SRT, ASS, SSA or VTT (up to 16 MiB). Reuses an already loaded track."""
    return await native_request(dict(version=1, operation='desktop.subtitle', asset_id=asset_id, path=path))


@desktop_tool()
async def video_ab_loop(asset_id: str, action: Literal['set','clear'], a: float | None = None, b: float | None = None) -> dict[str, Any]:
    """Set A and B in absolute seconds, or clear both loop markers."""
    return await native_request(dict(version=1, operation='desktop.loop', asset_id=asset_id, action=action, a=a, b=b))


@desktop_tool()
async def set_video_filter(asset_id: str, name: Literal['brightness','contrast','saturation','sharpen'], value: float) -> dict[str, Any]:
    """Set brightness, contrast or saturation (-100..100 integer), or sharpening (0..2). Zero resets one adjustment; saturation -100 is grayscale."""
    return await native_request(dict(version=1, operation='desktop.filter', asset_id=asset_id, name=name, value=value))


if __name__ == '__main__':
    # FastMCP wraps the binary streams in its own TextIOWrapper. Give it
    # duplicate descriptors so finalizing those wrappers cannot close the
    # process streams that the frozen launcher still flushes on exit.
    process_stdin, process_stdout = sys.stdin, sys.stdout
    with os.fdopen(os.dup(process_stdin.fileno()), 'r', encoding='utf-8') as reader, \
            os.fdopen(os.dup(process_stdout.fileno()), 'w', encoding='utf-8') as writer:
        sys.stdin, sys.stdout = reader, writer
        try:
            server.run(transport='stdio')
        finally:
            sys.stdin, sys.stdout = process_stdin, process_stdout
