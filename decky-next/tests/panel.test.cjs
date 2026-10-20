// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the shipped bundle with real React; fake only Decky's transport/controls.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const React=require('react'),JSX=require('react/jsx-runtime'),Renderer=require('react-test-renderer'),{act}=Renderer;
const flush=async()=>act(async()=>{await Promise.resolve();});
test('actual panel: visible launch command, clipboard failure, compact settings, x4 persistence and live feedback',async()=>{
 let selected=0,copied='',failClipboard=false,failStatus=false,active=[];
 const profile={enabled:false,multiplier:2,quality:'performance',refresh_hz:90,max_in_flight:2,extra_images:3,present_mode:'auto'};
 const options='python3 /home/deck/.local/share/gsf-next/launch.py --appid 42 -- %command%';
 const routes={async get_games(){return[{appid:42,name:'Test game'}];},async get_selection(){return{appid:selected};},async select_game(id){selected=id;},async get_status(){if(failStatus)throw Error('status unavailable');return{profile:{...profile},options,live:active,report:{},state:active.length?'generating':'waiting'};},async prepare_game(id){assert.equal(id,42);profile.enabled=true;profile.armed=true;return{options};},async configure_game(id,patch){assert.equal(id,42);Object.assign(profile,patch);return {apply:patch.extra_images?"swapchain":"live"};}};
 const context={console,setTimeout,clearTimeout,setInterval,clearInterval,SP_REACT:React,SP_JSX:JSX,DFL:{staticClasses:{Title:'title'}},navigator:{clipboard:{async writeText(text){if(failClipboard)throw Error('denied');copied=text;}}},window:{__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit:{connect(){return{_version:2,callable:name=>(...args)=>routes[name](...args),toaster:{toast(){}}};}}}};
 for(const name of ['PanelSection','PanelSectionRow','Dropdown','ButtonItem'])context.DFL[name]=props=>React.createElement('gsf-'+name,props,props.children);
 const bundle=fs.readFileSync(path.join(__dirname,'../dist/index.js'),'utf8').replace('export { index as default };','globalThis.plugin = index;');vm.runInNewContext(bundle,context);
 let tree;await act(async()=>{tree=Renderer.create(context.plugin().content);});
 try{
  await flush();const game=()=>tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data===42));
  await act(async()=>{game().props.onChange({data:42});});await flush();assert.equal(selected,42);
  assert.equal(tree.root.findAllByType('gsf-Dropdown').length,4,'only game, multiplier, quality and refresh are visible');
  assert.doesNotMatch(JSON.stringify(tree.toJSON()),/GPU на пару|Запас изображений/);
  const prepare=()=>tree.root.findAllByType('gsf-ButtonItem').find(n=>String(n.props.children).includes('Подготовить'));
  failStatus=true;await act(async()=>{prepare().props.onClick();});await flush();
  assert.equal(profile.enabled,true);assert.equal(copied,options);assert.equal(tree.root.findByType('textarea').props.value,options);assert.equal(prepare().props.disabled,false);
  failStatus=false;failClipboard=true;await act(async()=>{prepare().props.onClick();});await flush();
  assert.match(JSON.stringify(tree.toJSON()),/Steam запретил копирование/);assert.equal(tree.root.findByType('textarea').props.value,options);
  const mult=()=>tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data===4&&o.label.startsWith('×4')));
  const quality=tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data==='economy'));
  assert.ok(quality,'budget-first Alpha profile selectable');
  await act(async()=>quality.props.onChange({data:'economy'}));
  await flush();
  await act(async()=>{mult().props.onChange({data:4});});await flush();assert.equal(profile.multiplier,4);assert.equal(mult().props.selectedOption,4);
  assert.match(JSON.stringify(tree.toJSON()),/Применяется во время игры/);
  assert.doesNotMatch(JSON.stringify(tree.toJSON()),/Перезапустите игру/);
  act(()=>tree.unmount());await act(async()=>{tree=Renderer.create(context.plugin().content);});await flush();assert.equal(mult().props.selectedOption,4);assert.equal(game().props.selectedOption,42);
  active=[{source_fps:46,output_fps:89,generated_fps:43,accepted_real:100,accepted_synthetic:95,generated:95,selected_multiplier:4,enabled:true,refresh_hz:90,settings_pending:true}];
  await act(async()=>{mult().props.onChange({data:4});});await flush();
  assert.match(JSON.stringify(tree.toJSON()),/Применяются настройки/);
  assert.match(JSON.stringify(tree.toJSON()),/ограничен экраном 90 Гц/);
  active=[{...active[0],settings_pending:false}];
  await act(async()=>{mult().props.onChange({data:4});});await flush();
  assert.match(JSON.stringify(tree.toJSON()),/Генерация работает/);
  assert.doesNotMatch(JSON.stringify(tree.toJSON()),/GPU на пару/);
  const button=text=>tree.root.findAllByType('gsf-ButtonItem').find(n=>n.props.children===text);
  await act(async()=>{button('Настройки вывода').props.onClick();});
  assert.equal(tree.root.findAllByType('gsf-Dropdown').length,7);
  const queue=tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data===8));
  assert(queue.props.rgOptions.some(o=>o.data===0));
  await act(async()=>{queue.props.onChange({data:0});});await flush();assert.equal(profile.max_in_flight,0);
  await act(async()=>{queue.props.onChange({data:8});});await flush();assert.equal(profile.max_in_flight,8);
  const images=tree.root.findAllByType('gsf-Dropdown').find(n=>n.props.rgOptions.some(o=>o.data===5&&o.label.startsWith('Увеличенный')));
  await act(async()=>{images.props.onChange({data:5});});await flush();assert.equal(profile.extra_images,5);
  assert.match(JSON.stringify(tree.toJSON()),/Игра пересоздаст вывод/);
  await act(async()=>{button('Диагностика и отчёт').props.onClick();});assert.match(JSON.stringify(tree.toJSON()),/GPU на пару/);
  await act(async()=>{mult().props.onChange({data:0});});await flush();assert.equal(profile.enabled,false);assert.equal(mult().props.selectedOption,0);
 }finally{act(()=>tree.unmount());}
});
