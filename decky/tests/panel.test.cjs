// Mount the shipped rollup bundle, not a reimplementation of Content.
// Steam's controls/API are fakes; no physical Steam/CEF claim.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const React = require('react');
const JSX = require('react/jsx-runtime');
const Renderer = require('react-test-renderer');
const {act} = Renderer;

async function flush() {await act(async () => {await Promise.resolve();});}

test('shipped panel: choose, immediate feedback, enable despite status failure, Steam readback, persist across remount', async () => {
  let selected = 0;
  let current = '';
  let expected = '';
  let enabled = false;
  let statusFails = true;
  const listeners = new Set();
  const toasts = [];
  const writes = [];
  const routes = {
    async get_games() {return [{appid: 42, name: 'Test Game'}];},
    async get_selection() {return {appid: selected};},
    async select_game(id) {selected = id;return {appid: id};},
    async get_status() {
      if (statusFails) throw Error('status temporarily unavailable');
      return {enabled, state: 'waiting', live: [], launch_options: expected};
    },
    async prepare_game(id, previous) {
      assert.equal(id, 42);assert.equal(previous, '');
      expected = 'python3 /home/deck/.local/share/gsf-test/launch.py --appid 42 -- %command%';
      return {options: expected};
    },
    async confirm_game(id, actual) {assert.equal(id, 42);assert.equal(actual, current);enabled = true;}
  };
  const context = {
    console, setTimeout, clearTimeout, setInterval, clearInterval,
    SP_REACT: React, SP_JSX: JSX,
    DFL: {staticClasses: {Title: 'title'}},
    SteamClient: {Apps: {
      RegisterForAppDetails(_id, callback) {
        listeners.add(callback);callback({strLaunchOptions: current});
        return {unregister() {listeners.delete(callback);}};
      },
      SetAppLaunchOptions(id, value) {
        writes.push([id, value]);current = value;
        for (const callback of [...listeners]) callback({strLaunchOptions: value});
      }
    }},
    window: {__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit: {
      connect() {return {_version: 2, callable: name => (...args) => routes[name](...args),
        toaster: {toast: data => toasts.push(data)}};}
    }}
  };
  for (const name of ['PanelSection', 'PanelSectionRow', 'Dropdown', 'ButtonItem'])
    context.DFL[name] = props => React.createElement('gsf-'+name, props, props.children);
  const bundle = fs.readFileSync(path.join(__dirname, '../dist/index.js'), 'utf8')
    .replace('export { index as default };', 'globalThis.plugin = index;');
  vm.runInNewContext(bundle, context);
  let tree;
  await act(async () => {tree = Renderer.create(context.plugin().content);});
  try {
    await flush();
    const dropdown = () => tree.root.findAllByType('gsf-Dropdown').find(n => n.props.rgOptions?.some(o => o.data === 42));
    await act(async () => {dropdown().props.onChange({data: 42, label: 'Test Game'});});
    await flush();
    assert.equal(selected, 42);
    assert.match(JSON.stringify(tree.toJSON()), /Test Game/);
    assert.equal(toasts[0].title, 'GSF · игра выбрана');
    const enable = () => tree.root.findAllByType('gsf-ButtonItem').find(n => n.props.children === 'Включить 2×');
    assert.equal(enable().props.disabled, false, 'status RPC must not gate activation');
    statusFails = false;
    await act(async () => {enable().props.onClick();});
    await flush();
    assert.equal(enabled, true);
    assert.equal(writes.length, 1);
    assert.equal(writes[0][0], 42);
    assert.match(tree.root.findByType('code').props.children, /--appid 42 -- %command%/);
    assert.match(JSON.stringify(tree.toJSON()), /Steam подтвердил/);
    assert.equal(listeners.size, 0);
    act(() => tree.unmount());
    await act(async () => {tree = Renderer.create(context.plugin().content);});
    await flush();
    assert.equal(dropdown().props.selectedOption, 42, 'selection survives QAM remount');
    assert.equal(tree.root.findByType('code').props.children, expected);
    assert.equal(writes.length, 1, 'remount must not write launch options');
  } finally {act(() => tree.unmount());}
});
