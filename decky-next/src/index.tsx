// SPDX-License-Identifier: GPL-3.0-or-later
import {ButtonItem, Dropdown, PanelSection, PanelSectionRow, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {useEffect, useRef, useState} from 'react';
import {copyCommand, withTimeout} from './steam-options.mjs';

type Game={appid:number;name:string};
type Profile={enabled:boolean;armed?:boolean;multiplier:number;quality:string;refresh_hz:number;max_in_flight:number;extra_images:number;present_mode:string;diagnostics?:boolean};
type Live={source_fps:number;generated_fps?:number;output_fps?:number;effective_multiplier?:number;generated:number;accepted_real:number;accepted_synthetic:number;queue_depth:number;queue_delay_ms:number;gpu_pair_ms:number;settings_pending?:boolean;recreate_pending?:boolean;selected_multiplier?:number;enabled?:boolean;refresh_hz?:number;late_pairs?:number;last_present_gap_ms?:number;dropped_synthetic?:number;budget_skipped_synthetic?:number;output_acquire_ms?:number;output_copy_gpu_ms?:number;output_ring_bytes?:number};
type Status={reason?:string;profile:Profile;options:string;live:Live[];report:{stage?:string;error?:string};state:string;restart_required?:boolean};
function backend<A extends unknown[],R>(name:string){const rpc=callable<A,R>(name);return (...a:A)=>withTimeout(rpc(...a));}
const api={games:backend<[],Game[]>('get_games'),selection:backend<[],{appid:number}>('get_selection'),select:backend<[number],unknown>('select_game'),prepare:backend<[number],{options:string}>('prepare_game'),configure:backend<[number,Partial<Profile>],{apply?:string}>('configure_game'),status:backend<[number],Status>('get_status'),export:backend<[number],{path:string}>('export_report')};
const fmt=(n:number|undefined,digits=1)=>n===undefined||n===null||!Number.isFinite(n)||n<0?'—':n.toFixed(digits);
const muted={fontSize:12,color:'#acb6c2',lineHeight:1.45};
const card={padding:'12px 14px',background:'#ffffff08',border:'1px solid #ffffff12',borderRadius:8};
function Detail({label,value}:{label:string;value:string}){return <div style={{display:'flex',justifyContent:'space-between',gap:12,padding:'5px 0',fontSize:13}}><span style={{color:'#acb6c2'}}>{label}</span><span style={{textAlign:'right'}}>{value}</span></div>;}

function Content(){
 const [games,setGames]=useState<Game[]>([]),[appid,setAppid]=useState(0),[status,setStatus]=useState<Status>(),[busy,setBusy]=useState(false),[command,setCommand]=useState(''),[message,setMessage]=useState(''),[error,setError]=useState(false);
 const [showLaunch,setShowLaunch]=useState(false),[advanced,setAdvanced]=useState(false),[diagnostics,setDiagnostics]=useState(false);
 const selected=useRef(appid),lock=useRef(false);selected.current=appid;
 useEffect(()=>{let gone=false;void Promise.all([api.games(),api.selection()]).then(([g,s])=>{if(!gone){setGames(g);if(g.some(x=>x.appid===s.appid))setAppid(s.appid);}}).catch(e=>{if(!gone){setMessage(String(e));setError(true);}});return()=>{gone=true;};},[]);
 useEffect(()=>{
  setStatus(undefined);setCommand('');setMessage('');setError(false);setShowLaunch(false);setAdvanced(false);setDiagnostics(false);
  if(!appid)return;
  let gone=false,pending=false;
  const poll=async()=>{if(pending)return;pending=true;try{const s=await api.status(appid);if(!gone)setStatus(s);}catch(e){if(!gone){setMessage(String(e));setError(true);}}finally{pending=false;}};
  void poll();const timer=setInterval(poll,1000);return()=>{gone=true;clearInterval(timer);};
 },[appid]);
 const act=async(fn:()=>Promise<string>)=>{
  if(lock.current)return;lock.current=true;setBusy(true);setMessage('');setError(false);const id=appid;
  try{const text=await fn();if(selected.current===id){setMessage(text);try{setStatus(await api.status(id));}catch{/* A failed poll must not undo a completed action. */}}toaster.toast({title:'GSF Next',body:text});}
  catch(e){if(selected.current===id){setMessage(String(e));setError(true);}}
  finally{lock.current=false;setBusy(false);}
 };
 const change=(patch:Partial<Profile>)=>void act(async()=>{const r=await api.configure(appid,patch);return r?.apply==='swapchain'?'Сохранено. Игра пересоздаст вывод для применения настройки.':'Сохранено. Применяется во время игры.';});
 const choose=(o:unknown)=>{const id=Number(typeof o==='object'&&o!==null&&'data'in o?o.data:o);setAppid(id);void api.select(id).catch(e=>{setMessage(String(e));setError(true);});};
 const p=status?.profile,live=status?.live.reduce<Live|undefined>((a,b)=>!a||b.accepted_real>a.accepted_real?b:a,undefined);
 const configured=Boolean(p?.armed||p?.enabled),options=command||status?.options||'';
 const pending=Boolean(live?.settings_pending),needsRestart=Boolean(status?.restart_required);
 const state=needsRestart?'Нужен один перезапуск после обновления':live?.recreate_pending?'Перенастраивается вывод':pending?'Применяются настройки':live?live.enabled===false||!p?.enabled?'Генерация выключена':status?.state==='generating'?'Генерация работает':'Сбор истории кадров':status?.reason?'Генерация недоступна':status?.report?.stage==='running'?'Ожидается Vulkan-вывод':configured?'Профиль готов к запуску':'Требуется настройка запуска';
 const color=needsRestart||pending||live?.recreate_pending?'#f2c56b':live&&p?.enabled&&!status?.reason?'#7ed6a6':'#acb6c2';
 const capped=Boolean(p?.enabled&&live&&live.source_fps*p.multiplier>p.refresh_hz);
 return <>
 <PanelSection title="Игра">
  <PanelSectionRow><Dropdown strDefaultLabel="Выберите игру" selectedOption={appid} rgOptions={games.map(g=>({label:g.name,data:g.appid}))} onChange={choose}/></PanelSectionRow>
  {!appid&&<PanelSectionRow><div style={muted}>Выберите игру, затем настройте её запуск один раз. После этого генерацией можно управлять прямо здесь.</div></PanelSectionRow>}
  {p&&<PanelSectionRow><div role="status" style={{...card,borderLeft:'3px solid '+color}}><div style={{color,fontSize:14,fontWeight:600}}>{state}</div>{live&&<div style={{...muted,marginTop:4}}>Сейчас: {live.enabled===false?'Off':'×'+(live.selected_multiplier??p.multiplier)} · экран {live.refresh_hz??p.refresh_hz} Гц</div>}</div></PanelSectionRow>}
  {message&&<PanelSectionRow><div role={error?'alert':'status'} style={{...muted,color:error?'#ffaaaa':'#d5dce4'}}>{message}</div></PanelSectionRow>}
  {appid!==0&&!configured&&<ButtonItem disabled={busy||!p} onClick={()=>void act(async()=>{const id=appid;const r=await api.prepare(id);if(selected.current===id){setCommand(r.options);setShowLaunch(true);}try{await copyCommand(r.options);return 'Профиль готов. Команда скопирована — вставьте её в параметры запуска Steam.';}catch{return 'Профиль готов. Steam запретил копирование — команда доступна ниже.';}})}>Подготовить игру</ButtonItem>}
 </PanelSection>
 {p&&<PanelSection title="Генерация">
  <PanelSectionRow><div style={{fontSize:13,marginBottom:6}}>Режим</div><Dropdown selectedOption={p.enabled?p.multiplier:0} rgOptions={[{label:'Off · выключено',data:0},{label:'×2 · 1 промежуточный',data:2},{label:'×3 · 2 промежуточных',data:3},{label:'×4 · 3 промежуточных',data:4}]} onChange={o=>change({multiplier:Number(o.data),enabled:Number(o.data)!==0})} disabled={busy}/></PanelSectionRow>
  <PanelSectionRow><div style={{fontSize:13,marginBottom:6}}>Качество</div><Dropdown selectedOption={p.quality} rgOptions={[{label:'Быстро · рекомендуется',data:'performance'},{label:'Качественно',data:'quality'}]} onChange={o=>change({quality:String(o.data)})} disabled={busy}/></PanelSectionRow>
  <PanelSectionRow><div style={{fontSize:13,marginBottom:6}}>Частота экрана</div><Dropdown selectedOption={p.refresh_hz} rgOptions={[40,45,60,72,80,90,120,144,165,240].map(hz=>({label:hz+' Гц',data:hz}))} onChange={o=>change({refresh_hz:Number(o.data)})} disabled={busy}/></PanelSectionRow>
  <PanelSectionRow><div style={muted}>{capped?`При ${fmt(live?.source_fps,0)} FPS режим ×${p.multiplier} ограничен экраном ${p.refresh_hz} Гц. Лишние промежуточные кадры не создаются.`:'Укажите частоту из меню Steam. Общий лимит Steam должен пропускать эту частоту; исходные FPS ограничивайте внутри игры. Частота экрана здесь не меняется.'}</div></PanelSectionRow>
 </PanelSection>}
 {p&&<PanelSection title="Показатели">
  <PanelSectionRow><div style={{display:'grid',gridTemplateColumns:'1fr 1fr',gap:8}}>
   <div style={card}><div style={muted}>Игра</div><div style={{fontSize:26,fontWeight:600,marginTop:4}}>{fmt(live?.source_fps,0)} <span style={{fontSize:12,color:'#acb6c2'}}>FPS</span></div></div>
   <div style={card}><div style={muted}>Отправлено WSI</div><div style={{fontSize:26,fontWeight:600,marginTop:4}}>{fmt(live?.output_fps,0)} <span style={{fontSize:12,color:'#acb6c2'}}>кадр/с</span></div></div>
  </div></PanelSectionRow>
  <PanelSectionRow><div style={muted}>WSI — кадры, принятые Vulkan. Фактические FPS экрана здесь не измеряются.</div></PanelSectionRow>
 </PanelSection>}
 {p&&<PanelSection title="Запуск игры">
  <ButtonItem disabled={busy} onClick={()=>setShowLaunch(!showLaunch)}>{showLaunch?'Скрыть команду запуска':'Показать команду запуска'}</ButtonItem>
  {showLaunch&&<>
   <PanelSectionRow><div style={muted}>Steam → Свойства игры → Параметры запуска. Замените прежнюю команду GSF и уберите другие генераторы.</div></PanelSectionRow>
   <PanelSectionRow><textarea aria-label="Команда запуска GSF Next" readOnly value={options} style={{boxSizing:'border-box',width:'100%',minHeight:96,padding:10,borderRadius:6,border:'1px solid #ffffff24',background:'#131b24',color:'#e5eaf0',resize:'vertical',userSelect:'text',fontSize:12,lineHeight:1.5}} onFocus={e=>e.target.select()}/></PanelSectionRow>
   <ButtonItem disabled={busy||!options} onClick={()=>void act(async()=>{try{await copyCommand(options);return 'Команда скопирована.';}catch{return 'Steam запретил копирование. Выделите команду в поле и скопируйте вручную.';}})}>Скопировать команду</ButtonItem>
   <PanelSectionRow><div style={muted}>Один запуск с этой командой загружает слой. Дальше множитель, качество и Off меняются без перезапуска. После обновления плагина игру нужно перезапустить один раз.</div></PanelSectionRow>
  </>}
 </PanelSection>}
 {p&&<PanelSection title="Дополнительно">
  <ButtonItem disabled={busy} onClick={()=>setAdvanced(!advanced)}>{advanced?'Скрыть настройки вывода':'Настройки вывода'}</ButtonItem>
  {advanced&&<>
   <PanelSectionRow><div style={{fontSize:13,marginBottom:6}}>Предел очереди</div><Dropdown selectedOption={p.max_in_flight} rgOptions={[{label:'1 · меньше задержка',data:1},{label:'2 · баланс, рекомендуется',data:2},{label:'3 · больше запас',data:3},{label:'4 · глубокая',data:4},{label:'5 · глубже',data:5},{label:'6 · расширенная',data:6},{label:'7 · большой запас',data:7},{label:'8 · максимум',data:8}]} onChange={o=>change({max_in_flight:Number(o.data)})} disabled={busy}/></PanelSectionRow>
   <PanelSectionRow><div style={muted}>Большая очередь может сгладить перегрузку, но увеличивает задержку управления.</div></PanelSectionRow>
   <PanelSectionRow><div style={{fontSize:13,marginBottom:6}}>Запас изображений</div><Dropdown selectedOption={p.extra_images} rgOptions={[{label:'Стандартный · +3',data:3},{label:'Увеличенный · +5',data:5}]} onChange={o=>change({extra_images:Number(o.data)})} disabled={busy}/></PanelSectionRow>
   <PanelSectionRow><div style={{fontSize:13,marginBottom:6}}>Режим вывода</div><Dropdown selectedOption={p.present_mode} rgOptions={[{label:'Автоматически · рекомендуется',data:'auto'},{label:'Сохранить режим игры',data:'keep'}]} onChange={o=>change({present_mode:String(o.data)})} disabled={busy}/></PanelSectionRow>
   <PanelSectionRow><div style={muted}>Запас изображений и режим вывода применяются через пересоздание swapchain. Если игра его не обрабатывает, меняйте эти параметры до запуска.</div></PanelSectionRow>
  </>}
  <ButtonItem disabled={busy} onClick={()=>setDiagnostics(!diagnostics)}>{diagnostics?'Скрыть диагностику':'Диагностика и отчёт'}</ButtonItem>
  {diagnostics&&<>
   <PanelSectionRow><div style={card}>
    <Detail label="Создаётся" value={fmt(live?.generated_fps)+' кадр/с'}/>
    <Detail label="Множитель WSI" value={fmt(live?.effective_multiplier,2)+'×'}/>
    <Detail label="GPU на пару" value={fmt(live?.gpu_pair_ms,2)+' мс'}/>
    <Detail label="Ожидание GPU и очереди" value={fmt(live?.queue_delay_ms,2)+' мс'}/>
    <Detail label="Заданий в очереди" value={String(live?.queue_depth??'—')}/>
    <Detail label="Ожидание WSI-изображения" value={fmt(live?.output_acquire_ms,2)+' мс'}/>
    <Detail label="GPU-копия на вывод" value={fmt(live?.output_copy_gpu_ms,2)+' мс'}/>
    <Detail label="Отклонено до GPU" value={String(live?.budget_skipped_synthetic??'—')}/>
    <Detail label="Пропущено synthetic" value={String(live?.dropped_synthetic??'—')}/>
    <Detail label="GPU-буфер вывода" value={live?.output_ring_bytes===undefined?'—':fmt(live.output_ring_bytes/1048576,1)+' МиБ'}/>
    <Detail label="Интервал WSI" value={fmt(live?.last_present_gap_ms,2)+' мс'}/>
    <Detail label="Опоздавшие пары" value={String(live?.late_pairs??'—')}/>
    <Detail label="Создано / принято synthetic" value={`${live?.generated??'—'} / ${live?.accepted_synthetic??'—'}`}/>
   </div></PanelSectionRow>
   <ButtonItem disabled={busy} onClick={()=>change({diagnostics:!p.diagnostics})}>Подробная запись: {p.diagnostics?'включена':'выключена'}</ButtonItem>
   <ButtonItem disabled={!appid||busy} onClick={()=>void act(async()=>{const r=await api.export(appid);return 'Отчёт сохранён: '+r.path;})}>Сохранить отчёт</ButtonItem>
   <PanelSectionRow><div style={muted}>GSF Next 0.2.0 Alpha 4 · Vulkan SDR · FidelityFX Optical Flow. Проверка физического вывода новой сборки на Steam Deck ещё требуется.</div></PanelSectionRow>
  </>}
  {status?.reason&&<PanelSectionRow><div role="alert" style={{...muted,color:'#f2c56b'}}>{status.reason}</div></PanelSectionRow>}
  {status?.report?.error&&<PanelSectionRow><div role="alert" style={{...muted,color:'#ffaaaa'}}>{status.report.error}</div></PanelSectionRow>}
 </PanelSection>}
 </>;
}
export default definePlugin(()=>({name:'GSF Next',title:<div className={staticClasses.Title}>GSF Next</div>,content:<Content/>,icon:<span>FG</span>}));
