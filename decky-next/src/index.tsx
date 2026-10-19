import {ButtonItem, Dropdown, PanelSection, PanelSectionRow, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {useEffect, useRef, useState} from 'react';
import {copyCommand, withTimeout} from './steam-options.mjs';

type Game={appid:number;name:string};
type Profile={enabled:boolean;multiplier:number;quality:string;refresh_hz:number;max_in_flight:number;extra_images:number;present_mode:string;diagnostics?:boolean};
type Live={source_fps:number;generated_fps?:number;output_fps?:number;effective_multiplier?:number;generated:number;accepted_real:number;accepted_synthetic:number;queue_depth:number;queue_delay_ms:number;gpu_pair_ms:number};
type Status={reason?:string;profile:Profile;options:string;live:Live[];report:{stage?:string;error?:string};state:string};
function backend<A extends unknown[],R>(name:string){const rpc=callable<A,R>(name);return (...a:A)=>withTimeout(rpc(...a));}
const api={games:backend<[],Game[]>('get_games'),selection:backend<[],{appid:number}>('get_selection'),select:backend<[number],unknown>('select_game'),prepare:backend<[number],{options:string}>('prepare_game'),configure:backend<[number,Partial<Profile>],unknown>('configure_game'),status:backend<[number],Status>('get_status'),export:backend<[number],{path:string}>('export_report')};
const number=(n:number|undefined,digits=1)=>n===undefined||n===null||n<0?'—':n.toFixed(digits);
function Content(){
 const [games,setGames]=useState<Game[]>([]),[appid,setAppid]=useState(0),[status,setStatus]=useState<Status>(),[busy,setBusy]=useState(false),[command,setCommand]=useState(''),[message,setMessage]=useState('Выберите игру. Настройки применяются после её перезапуска.');
 const selected=useRef(appid),lock=useRef(false);selected.current=appid;
 useEffect(()=>{let gone=false;void Promise.all([api.games(),api.selection()]).then(([g,s])=>{if(!gone){setGames(g);if(g.some(x=>x.appid===s.appid))setAppid(s.appid);}}).catch(e=>{if(!gone)setMessage(String(e));});return()=>{gone=true;};},[]);
 useEffect(()=>{setStatus(undefined);setCommand('');if(!appid)return;let gone=false,pending=false;const poll=async()=>{if(pending)return;pending=true;try{const s=await api.status(appid);if(!gone)setStatus(s);}catch(e){if(!gone)setMessage(String(e));}finally{pending=false;}};void poll();const t=setInterval(poll,1500);return()=>{gone=true;clearInterval(t);};},[appid]);
 const act=async(fn:()=>Promise<string>)=>{if(lock.current)return;lock.current=true;setBusy(true);setMessage('Выполняется…');const id=appid;try{const text=await fn();if(selected.current===id){setMessage(text);try{setStatus(await api.status(id));}catch{/* polling must not undo a completed action */}}toaster.toast({title:'GSF Next',body:text});}catch(e){setMessage(String(e));}finally{lock.current=false;setBusy(false);}};
 const change=(patch:Partial<Profile>)=>void act(async()=>{await api.configure(appid,patch);return 'Сохранено. Перезапустите игру.';});
 const choose=(o:unknown)=>{const id=Number(typeof o==='object'&&o!==null&&'data'in o?o.data:o);setAppid(id);void api.select(id).catch(e=>setMessage(String(e)));};
 const p=status?.profile,live=status?.live.reduce<Live|undefined>((a,b)=>!a||b.accepted_synthetic>a.accepted_synthetic?b:a,undefined);
 const options=command||status?.options||'';
 return <>
 <PanelSection title="GSF Next · 0.2.0 Alpha">
 <PanelSectionRow><div style={{fontSize:13}}>Открытый FidelityFX Optical Flow. Проверка Steam Deck ещё требуется. Уберите старую команду GSF Test и другие генераторы из запуска игры.</div></PanelSectionRow>
 <PanelSectionRow><div role="status" style={{whiteSpace:'pre-wrap',fontSize:13}}>{message}</div></PanelSectionRow>
 <PanelSectionRow><Dropdown strDefaultLabel="Выберите игру" selectedOption={appid} rgOptions={games.map(g=>({label:g.name,data:g.appid}))} onChange={choose}/></PanelSectionRow>
 <ButtonItem disabled={!appid||busy} onClick={()=>void act(async()=>{const r=await api.prepare(appid);if(selected.current===appid)setCommand(r.options);try{await copyCommand(r.options);return 'Профиль включён, команда скопирована. Вставьте её в параметры запуска Steam.';}catch{return 'Профиль включён. Команда ниже; Steam запретил копирование.';}})}>Подготовить и скопировать запуск</ButtonItem>
 {options&&<PanelSectionRow><textarea aria-label="Команда запуска GSF Next" readOnly value={options} style={{width:'100%',minHeight:110,userSelect:'text',fontSize:13}} onFocus={e=>e.target.select()}/></PanelSectionRow>}
 <ButtonItem disabled={!appid||busy} onClick={()=>change({enabled:false})}>Выключить для этой игры</ButtonItem>
 <PanelSectionRow><div style={{fontSize:12}}>После выключения команда пропускает игру без слоя. Подготовка профиля не изменяет параметры запуска Steam автоматически.</div></PanelSectionRow>
 </PanelSection>
 {p&&<PanelSection title="Генерация и вывод">
 <PanelSectionRow><div>Генерация</div><Dropdown selectedOption={p.enabled?p.multiplier:0} rgOptions={[{label:'Off',data:0},{label:'×2',data:2},{label:'×3 · Experimental',data:3},{label:'×4 · Experimental',data:4}]} onChange={o=>change({multiplier:Number(o.data),enabled:Number(o.data)!==0})} disabled={busy}/></PanelSectionRow>
 <PanelSectionRow><div>Частота дисплея</div><Dropdown selectedOption={p.refresh_hz} rgOptions={[40,45,60,72,80,90,120,144,165,240].map(hz=>({label:hz+' Гц',data:hz}))} onChange={o=>change({refresh_hz:Number(o.data)})} disabled={busy}/></PanelSectionRow>
 <PanelSectionRow><div style={{fontSize:12}}>Укажите частоту из меню Steam. Это бюджет генерации, частота экрана не меняется. ×4 при 45 FPS / 90 Гц даёт максимум 90 кадров/с.</div></PanelSectionRow>
 <PanelSectionRow><div>Качество</div><Dropdown selectedOption={p.quality} rgOptions={[{label:'Performance · половинный flow',data:'performance'},{label:'Quality · полный flow',data:'quality'}]} onChange={o=>change({quality:String(o.data)})} disabled={busy}/></PanelSectionRow>
 <PanelSectionRow><div>Предел очереди</div><Dropdown selectedOption={p.max_in_flight} rgOptions={[1,2,3].map(n=>({label:n===2?'2 · по умолчанию':String(n)+' задания',data:n}))} onChange={o=>change({max_in_flight:Number(o.data)})} disabled={busy}/></PanelSectionRow>
 <PanelSectionRow><div>Запас изображений</div><Dropdown selectedOption={p.extra_images} rgOptions={[{label:'+3 · стандарт',data:3},{label:'+5 · расширенный',data:5}]} onChange={o=>change({extra_images:Number(o.data)})} disabled={busy}/></PanelSectionRow>
 <PanelSectionRow><div>Режим презентации</div><Dropdown selectedOption={p.present_mode} rgOptions={[{label:'Auto · MAILBOX, если совместим',data:'auto'},{label:'Сохранить режим игры',data:'keep'}]} onChange={o=>change({present_mode:String(o.data)})} disabled={busy}/></PanelSectionRow>
 </PanelSection>}
 <PanelSection title="Диагностика">
 {p&&<ButtonItem disabled={busy} onClick={()=>change({diagnostics:!p.diagnostics})}>Подробный GPU-отчёт: {p.diagnostics?'On':'Off'}</ButtonItem>}
 <PanelSectionRow><div style={{fontSize:13,whiteSpace:'pre-line'}}>{`Состояние: ${status?.state??'ожидание'}\nSource FPS: ${number(live?.source_fps)}\nGenerated FPS: ${number(live?.generated_fps)}\nOutput FPS (WSI): ${number(live?.output_fps)}\nЭффективный множитель (WSI): ${number(live?.effective_multiplier,2)}\nGPU на пару: ${number(live?.gpu_pair_ms,2)} мс\nОчередь: ${live?.queue_depth??'—'}\nОжидание GPU: ${number(live?.queue_delay_ms,2)} мс\nСоздано / принято WSI: ${live?.generated??'—'} / ${live?.accepted_synthetic??'—'}\nDisplayed: не измеряется`}</div></PanelSectionRow>
 {status?.reason&&<PanelSectionRow><div>{status.reason}</div></PanelSectionRow>}
 {status?.report?.error&&<PanelSectionRow><div>{status.report.error}</div></PanelSectionRow>}
 <ButtonItem disabled={!appid||busy} onClick={()=>void act(async()=>{const r=await api.export(appid);return 'Отчёт: '+r.path;})}>Сохранить отчёт</ButtonItem>
 </PanelSection>
 </>;
}
export default definePlugin(()=>({name:'GSF Next',title:<div className={staticClasses.Title}>GSF Next</div>,content:<Content/>,icon:<span>FG</span>}));
