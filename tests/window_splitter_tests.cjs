const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/host/Workspace.qml'), 'utf8');
const hitAreaStart = source.indexOf('objectName: "filesPaneSplitterHitArea"');
const hitArea = source.slice(hitAreaStart, source.indexOf('PreviewPane {', hitAreaStart));
assert.match(hitArea, /width: 14/, 'splitter keeps a usable pointer target around its visible divider');
const area = source.slice(source.indexOf('MouseArea { id: splitterMouse;'));
assert.ok(area.length > 0, 'splitter pointer handlers exist');
let writes = [];
const mouse = {pressed: true, dragWidth: 300, start: 0, old: 300};
mouse.mapToItem = (root, x) => ({x});
const context = vm.createContext({splitterMouse: mouse, pressed: true, old: 300, start: 0,
  root: {controller: {preference(key, value) { writes.push([key, value]); }}},
  mapToItem(root, x) { return {x}; }});
function invoke(name, event = {}) {
  const start = area.indexOf(name + ':');
  assert.ok(start >= 0, name + ' handler exists');
  let end = area.indexOf('{', start), depth = 1;
  while (depth && ++end < area.length) {
    if (area[end] === '{') ++depth;
    if (area[end] === '}') --depth;
  }
  const body = area.slice(start + name.length + 1, end + 1).trim();
  if (body.includes('=>')) vm.runInContext('(' + body + ')', context)(event);
  else vm.runInContext(body, context);
}
for (let x = 0; x < 100; ++x) invoke('onPositionChanged', {x});
assert.equal(writes.length, 0, 'Pointer movement must not synchronously persist preferences');
assert.equal(mouse.dragWidth, 399, 'Drag width follows the pointer locally');
invoke('onPositionChanged', {x: 200});
assert.equal(mouse.dragWidth, 400, 'Maximum width is preserved');
invoke('onPositionChanged', {x: -200});
assert.equal(mouse.dragWidth, 220, 'Minimum width is preserved');
invoke('onReleased');
assert.deepEqual(writes, [['fileWidth', 220]], 'Release persists the final width once');
console.log('PASS 102 drag updates without writes, clamped preview and one release write');
