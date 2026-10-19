import test from 'node:test';
import assert from 'node:assert/strict';
import {readOptions, enableGame, disableGame, copyCommand, withTimeout} from '../src/steam-options.mjs';

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

test('missing Steam API fails with manual launch instructions', async () => {
  await assert.rejects(enableGame(undefined, {}, 42), /Ручной запуск/);
});

test('unresponsive Decky RPC has a bounded wait', async () => {
  await assert.rejects(withTimeout(new Promise(() => {}), 5), /нет ответа от Decky/);
});

test('Steam subscription cleanup failure never strands a launch operation', async () => {
  let notify;
  const apps = {RegisterForAppDetails(_id, callback) {notify = callback;
    return {unregister() {throw Error('stale Steam subscription');}};}};
  const result = readOptions(apps, 42, undefined, 50);
  notify({strLaunchOptions: 'old options'});
  assert.equal(await withTimeout(result, 100), 'old options');
  await assert.rejects(readOptions(apps, 42, undefined, 5), /Ручной запуск/);
});

test('clipboard reports success only after a write, falls back when CEF denies clipboard', async () => {
  let copied;
  await copyCommand('exact command', {async writeText(text) {copied = text;}});
  assert.equal(copied, 'exact command');
  let removed = false;
  let focused = false;
  const field = {style: {}, select() {}, remove() {removed = true;}};
  const doc = {activeElement: {focus() {focused = true;}}, createElement() {return field;},
    body: {appendChild() {}}, execCommand(op) {assert.equal(op, 'copy');return true;}};
  await copyCommand('fallback command', {async writeText() {throw Error('denied');}}, doc);
  assert.equal(field.value, 'fallback command');
  assert.equal(removed, true);
  assert.equal(focused, true);
  doc.execCommand = () => false;
  await assert.rejects(copyCommand('text', undefined, doc), /не разрешил/);
  await assert.rejects(copyCommand('text', undefined, undefined), /недоступен/);
});
