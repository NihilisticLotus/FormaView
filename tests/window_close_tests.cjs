const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

// Execute the production QML handler, with only its UI/service dependencies faked.
const source = fs.readFileSync(path.join(__dirname, '../src/host/Workspace.qml'), 'utf8');
const start = source.indexOf('function requestWindowClose()');
assert.ok(start >= 0);
let end = source.indexOf('{', start), depth = 1;
while (depth && ++end < source.length) {
  if (source[end] === '{') ++depth;
  if (source[end] === '}') --depth;
}
const handler = source.slice(start, end + 1).replace('(): bool', '()');
function scenario({active = false, busy = false, settling = false, dirty = false, confirmed = false} = {}) {
  let prompts = 0, leaves = 0;
  const root = {exitConfirmed: confirmed};
  const context = vm.createContext({root, settingsVisible: dirty,
    settingsDialog: {dirty, leave() { ++leaves; }},
    controller: {imageJobs: {busy, activeJobs() { return active; }}},
    quickExportLoader: {item: settling ? {settling: true} : null},
    exitDialog: {open() { ++prompts; }}});
  const accepted = vm.runInContext(handler + '\nrequestWindowClose()', context);
  return {accepted, prompts, leaves, confirmed: root.exitConfirmed};
}
assert.equal(scenario().accepted, true, 'idle window can close');
assert.deepEqual(scenario({active: true}), {accepted: false, prompts: 1, leaves: 0, confirmed: false}, 'active conversion requires confirmation');
assert.equal(scenario({busy: true}).accepted, false, 'preparation/direct export requires confirmation');
assert.equal(scenario({settling: true}).accepted, false, 'settling quick export requires confirmation');
assert.equal(scenario({dirty: true, active: true}).leaves, 1, 'unsaved settings are resolved first');
assert.equal(scenario({active: true, confirmed: true}).accepted, true, 'explicit cancellation permits exit');
assert.equal(scenario({active: true, confirmed: true}).confirmed, false, 'confirmation is consumed');
console.log('PASS idle, active, busy, settling, dirty settings and one-shot exit confirmation');
