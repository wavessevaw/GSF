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
  let multiplier = 2;
  let manual = false;
  let copied = '';
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
      return {enabled, configured: enabled, manual_launch: manual, multiplier, state: 'waiting', live: [], launch_options: expected};
    },
    async prepare_game(id, previous) {
      assert.equal(id, 42);assert.equal(previous, '');
      expected = 'python3 /home/deck/.local/share/gsf-test/launch.py --appid 42 -- %command%';
      return {options: expected};
    },
    async confirm_game(id, actual) {assert.equal(id, 42);assert.equal(actual, current);enabled = true;},
    async prepare_manual_game(id) {assert.equal(id, 42);enabled = true;manual = true;return {options: expected};},
    async disable_game(id, actual) {assert.equal(id, 42);assert.equal(actual, '');enabled = false;
      return {restore: false, options: actual, message: 'GSF отключён. Уберите команду вручную.'};},
    async set_multiplier(id, value) {assert.equal(id, 42);assert.ok([0, 2, 3, 4].includes(value)); multiplier = value; return {multiplier: value};}
  };
  const context = {
    console, setTimeout, clearTimeout, setInterval, clearInterval,
    SP_REACT: React, SP_JSX: JSX,
    DFL: {staticClasses: {Title: 'title'}},
    navigator: {clipboard: {async writeText(text) {copied = text;}}},
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
    const enable = () => tree.root.findAllByType('gsf-ButtonItem').find(n => String(n.props.children).includes('Включить GSF ×'));
    const multiplierSelect = () => tree.root.findAllByType('gsf-Dropdown').find(n => n.props.rgOptions?.some(o => o.data === 3 && String(o.label).startsWith('×3')));
    assert.equal(enable().props.disabled, false, 'status RPC must not gate activation');
    statusFails = false;
    await act(async () => {enable().props.onClick();});
    await flush();
    assert.equal(enabled, true);
    assert.equal(writes.length, 1);
    assert.equal(writes[0][0], 42);
    assert.match(JSON.stringify(tree.toJSON()), /Steam подтвердил/);
    assert.ok(multiplierSelect(), 'actual x2/x3 selector mounted');
    assert.equal(multiplierSelect().props.disabled, false);
    await act(async () => {multiplierSelect().props.onChange({data: 3, label: '×3'});});
    await flush();
    assert.equal(multiplier, 3, 'per-game API persists 3x');
    assert.equal(multiplierSelect().props.selectedOption, 3);
    assert.match(tree.root.findByType('code').props.children, /--appid 42 -- %command%/);
    assert.match(JSON.stringify(tree.toJSON()), /Множитель ×3 сохранён/);
    await act(async () => {multiplierSelect().props.onChange({data: 4, label: '×4'});});
    assert.equal(multiplier, 4);
    assert.match(JSON.stringify(tree.toJSON()), /120 FPS/);
    await act(async () => {multiplierSelect().props.onChange({data: 3, label: '×3'});});
    assert.equal(listeners.size, 0);
    act(() => tree.unmount());
    await act(async () => {tree = Renderer.create(context.plugin().content);});
    await flush();
    assert.equal(dropdown().props.selectedOption, 42, 'selection survives QAM remount');
    assert.equal(multiplierSelect().props.selectedOption, 3, '3x persists across remount');
    assert.equal(tree.root.findByType('code').props.children, expected);
    assert.equal(writes.length, 1, 'remount must not write launch options');
    // Reproduce a Steam client where Apps is unavailable: the error must be
    // visible, and the independent manual route must remain usable.
    context.SteamClient = undefined;
    await act(async () => {enable().props.onClick();});
    await flush();
    assert.match(JSON.stringify(tree.toJSON()), /Steam API недоступен/);
    assert.equal(enable().props.disabled, false, 'error must release action lock');
    const manualButton = tree.root.findAllByType('gsf-ButtonItem').find(n => String(n.props.children).includes('Ручной запуск'));
    await act(async () => {manualButton.props.onClick();});
    await flush();
    assert.equal(manual, true);
    assert.match(JSON.stringify(tree.toJSON()), /Ручной профиль готов/);
    assert.equal(tree.root.findByType('code').props.children, expected);
    const copyButton = tree.root.findAllByType('gsf-ButtonItem').find(n => String(n.props.children).includes('Скопировать команду'));
    await act(async () => {copyButton.props.onClick();});
    assert.equal(copied, expected);
    assert.match(JSON.stringify(tree.toJSON()), /Команда скопирована/);
    const disable = tree.root.findAllByType('gsf-ButtonItem').find(n => String(n.props.children).includes('Восстановить запуск'));
    await act(async () => {disable.props.onClick();});
    await flush();
    assert.equal(enabled, false);
    assert.equal(writes.length, 1, 'manual activation, copy and disable must not write Steam options');
  } finally {act(() => tree.unmount());}
});
