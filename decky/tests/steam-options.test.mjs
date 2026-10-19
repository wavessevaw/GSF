import test from 'node:test';
import assert from 'node:assert/strict';
import {readOptions, enableGame, disableGame, copyLaunchOptions} from '../src/steam-options.mjs';

function fixture(options = 'ORIGINAL=1 %command% --old') {
  let current = options;
  const subscriptions = new Set();
  const writes = [];
  const apps = {
    RegisterForAppDetails(_id, callback) {
      subscriptions.add(callback);
      callback({strLaunchOptions: current}); // Steam may notify synchronously.
      return {unregister() {subscriptions.delete(callback);}};
    },
    SetAppLaunchOptions(id, value) {
      writes.push([id, value]);current = value;
      for (const cb of [...subscriptions]) cb({strLaunchOptions: value});
    }
  };
  let confirmed = false;
  let disabled = false;
  let forgotten = false;
  const rpc = {
    async prepare(_id, old) {assert.equal(old, options);return {options: 'WRAPPED '+options};},
    async confirm(_id, actual) {assert.equal(actual, current);confirmed = true;},
    async disable(_id, actual) {disabled = true;return {restore: actual === 'WRAPPED '+options, options};},
    async forget(_id, actual) {assert.equal(actual, options);forgotten = true;}
  };
  return {apps, rpc, subscriptions, writes, get confirmed() {return confirmed;},
    get disabled() {return disabled;}, get forgotten() {return forgotten;}};
}

test('Steam API: write/readback before backend enable, exact restore', async () => {
  const f = fixture();
  await enableGame(f.apps, f.rpc, 42);
  assert.equal(f.confirmed, true);
  assert.equal(f.subscriptions.size, 0);
  await disableGame(f.apps, f.rpc, 42);
  assert.equal(f.disabled, true);assert.equal(f.forgotten, true);
  assert.deepEqual(f.writes.map(x => x[0]), [42, 42]);
  assert.equal(f.writes[1][1], 'ORIGINAL=1 %command% --old');
  assert.equal(f.subscriptions.size, 0);
});

test('Steam API: changed user options are never restored over', async () => {
  const f = fixture();
  f.rpc.disable = async () => ({restore: false, options: 'USER EDIT'});
  await disableGame(f.apps, f.rpc, 42);
  assert.equal(f.writes.length, 0);
});

test('Steam API: subscription cleans up on timeout and wrong readback', async () => {
  const f = fixture();
  await assert.rejects(readOptions(f.apps, 42, 'NEVER APPLIED', 5), /не подтвердил/);
  assert.equal(f.subscriptions.size, 0);
});

test('Steam API: backend failure leaves profile disabled', async () => {
  const f = fixture();
  f.rpc.confirm = async () => {throw Error('backend failure');};
  await assert.rejects(enableGame(f.apps, f.rpc, 42), /backend failure/);
  assert.equal(f.disabled, true);assert.equal(f.subscriptions.size, 0);
});

test('manual copy: CEF execCommand path and confirmed bytes', async () => {
  const added = [];
  const removed = [];
  const node = {style: {}, focus(){}, select(){}, value:''};
  const host = {document: {
    createElement: kind => {assert.equal(kind,'textarea');return node;},
    body: {appendChild: x => added.push(x), removeChild: x => removed.push(x)},
    execCommand: action => {assert.equal(action,'copy');return true;}
  }};
  assert.equal(await copyLaunchOptions('python3 launch.py --appid 42 -- %command%', host),true);
  assert.equal(node.value,'python3 launch.py --appid 42 -- %command%');
  assert.deepEqual(added,[node]);
  assert.deepEqual(removed,[node]);
});
test('manual copy does not fabricate clipboard success when API is unavailable', async () => {
  await assert.rejects(copyLaunchOptions('python3 x -- %command%', {}), /Буфер обмена недоступен/);
  await assert.rejects(copyLaunchOptions('not a launch command', {}), /Invalid/);
  const writes=[];
  assert.equal(await copyLaunchOptions('python3 x -- %command%', {
    navigator:{clipboard:{writeText:async text=>writes.push(text)}}
  }),true);
  assert.deepEqual(writes,['python3 x -- %command%']);
});
