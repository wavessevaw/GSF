// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the shipped bundle with real React; fake only Decky's transport/controls.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const React=require('react'),JSX=require('react/jsx-runtime'),Renderer=require('react-test-renderer'),{act}=Renderer;
const flush=async()=>act(async()=>{await Promise.resolve();});
test('actual panel: visible launch command, clipboard failure, x4 persistence, no Steam API dependency',async()=>{
 let selected=0,copied='',failClipboard=false,failStatus=false;
 const profile={enabled:false,multiplier:2,quality:'performance',refresh_hz:90,max_in_flight:2,extra_images:3,present_mode:'auto'};
 const options='python3 /home/deck/.local/share/gsf-next/launch.py --appid 42 -- %command%';
 const routes={async get_games(){return[{appid:42,name:'Test game'}];},async get_selection(){return{appid:selected};},async select_game(id){selected=id;},async get_status(){if(failStatus)throw Error('status unavailable');return{profile:{...profile},options,live:[],report:{},state:'waiting'};},async prepare_game(id){assert.equal(id,42);profile.enabled=true;return{options};},async configure_game(id,patch){assert.equal(id,42);Object.assign(profile,patch);}};
 const context={console,setTimeout,clearTimeout,setInterval,clearInterval,SP_REACT:React,SP_JSX:JSX,DFL:{staticClasses:{Title:'title'}},navigator:{clipboard:{async writeText(text){if(failClipboard)throw Error('denied');copied=text;}}},window:{__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit:{connect(){return{_version:2,callable:name=>(...args)=>routes[name](...args),toaster:{toast(){}}};}}}};
 for(const name of ['PanelSection','PanelSectionRow','Dropdown','ButtonItem'])context.DFL[name]=props=>React.createElement('gsf-'+name,props,props.children);
 const bundle=fs.readFileSync(path.join(__dirname,'../dist/index.js'),'utf8').replace('export { index as default };','globalThis.plugin = index;');vm.runInNewContext(bundle,context);
 let tree;await act(async()=>{tree=Renderer.create(context.plugin().content);});
 try{
  await flush();const game=()=>tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data===42));
  await act(async()=>{game().props.onChange({data:42});});await flush();assert.equal(selected,42);
  const prepare=()=>tree.root.findAllByType('gsf-ButtonItem').find(n=>String(n.props.children).includes('Подготовить'));
  failStatus=true;await act(async()=>{prepare().props.onClick();});await flush();
  assert.equal(profile.enabled,true);assert.equal(copied,options);assert.equal(tree.root.findByType('textarea').props.value,options);assert.equal(prepare().props.disabled,false);
  failStatus=false;failClipboard=true;await act(async()=>{prepare().props.onClick();});await flush();
  assert.match(JSON.stringify(tree.toJSON()),/Steam запретил копирование/);assert.equal(tree.root.findByType('textarea').props.value,options);
  const mult=()=>tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data===4&&o.label.startsWith('×4')));
  await act(async()=>{mult().props.onChange({data:4});});await flush();assert.equal(profile.multiplier,4);assert.equal(mult().props.selectedOption,4);
  act(()=>tree.unmount());await act(async()=>{tree=Renderer.create(context.plugin().content);});await flush();assert.equal(mult().props.selectedOption,4);assert.equal(game().props.selectedOption,42);
  const off=tree.root.findAllByType('gsf-ButtonItem').find(n=>String(n.props.children).includes('Выключить'));
  await act(async()=>{off.props.onClick();});await flush();assert.equal(profile.enabled,false);assert.equal(mult().props.selectedOption,0);
 }finally{act(()=>tree.unmount());}
});
