import {ButtonItem, DropdownItem, PanelSection, PanelSectionRow, staticClasses} from '@decky/ui';
import {callable, definePlugin} from '@decky/api';
import {useEffect, useRef, useState} from 'react';
import {enableGame, disableGame} from './steam-options.mjs';

type Game = {appid: number; name: string};
type Live = {source_fps: number; generated_wsi_accepted: number; real_wsi_accepted: number};
type Status = {enabled: boolean; state: string; live: Live[]; report?: {result: string; exit_code?: number}};
const rpc = {
  games: callable<[], Game[]>('get_games'),
  prepare: callable<[number, string], {options: string}>('prepare_game'),
  confirm: callable<[number, string], unknown>('confirm_game'),
  disable: callable<[number, string], {options: string; restore: boolean; message?: string}>('disable_game'),
  forget: callable<[number, string], void>('finish_restore'),
  status: callable<[number], Status>('get_status'),
  export: callable<[number], {path: string}>('export_report')
};

function Content() {
  const [games, setGames] = useState<Game[]>([]);
  const [appid, setAppid] = useState(0);
  const [status, setStatus] = useState<Status>();
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const selected = useRef(appid);
  selected.current = appid;
  const actionLock = useRef(false);
  useEffect(() => {rpc.games().then(setGames).catch(e => setMessage(String(e)));}, []);
  useEffect(() => {
    setStatus(undefined);
    if (!appid) return;
    let gone = false;
    let pending = false;
    const refresh = async () => {
      if (pending) return;
      pending = true;
      try {const s = await rpc.status(appid); if (!gone) setStatus(s);}
      catch (e) {if (!gone) setMessage(String(e));}
      finally {pending = false;}
    };
    void refresh();
    const timer = setInterval(refresh, 1500);
    return () => {gone = true; clearInterval(timer);};
  }, [appid]);
  const act = async (fn: () => Promise<string>) => {
    if (actionLock.current) return;
    actionLock.current = true;
    setBusy(true); setMessage('');
    const id = appid;
    try {
      const text = await fn();
      if (selected.current === id) {setMessage(text); setStatus(await rpc.status(id));}
    } catch (e) {setMessage(String(e));}
    finally {actionLock.current = false; setBusy(false);}
  };
  const live = status?.live?.[0];
  const state = status?.state === 'generating' ? 'Генерация кадров подтверждена' :
    status?.state === 'bridge' ? 'GPU слой подключён; ждём генерацию' :
    status?.enabled ? '2× настроен. Запустите или перезапустите игру' : 'GSF отключён для следующего запуска';
  return <PanelSection title="GSF · игровой тест 2×">
    <PanelSectionRow><div style={{fontSize: 13, lineHeight: 1.5}}>
      GPU LMC · экспериментальный · Vulkan / DXVK / VKD3D, x64.<br/>
      Частота игры меняется свободно. Вывод ограничен частотой экрана.<br/>
      Для отдельного теста отключите генерацию GFG/LSFG в этой игре.
    </div></PanelSectionRow>
    <PanelSectionRow><DropdownItem label="Игра" rgOptions={games.map(g => ({data: g.appid, label: g.name}))}
      selectedOption={appid} disabled={busy} strDefaultLabel="Выберите установленную игру"
      onChange={o => {setAppid(Number(o.data)); setMessage('');}} /></PanelSectionRow>
    {appid > 0 && <>
      <PanelSectionRow><div style={{fontSize: 13}}>{state}
        {live && <div>Источник: {live.source_fps.toFixed(1)} FPS<br/>
          Принято WSI: исходных {live.real_wsi_accepted}, сгенерированных {live.generated_wsi_accepted}</div>}
        {!live && status?.report && <div>Последний прогон: {status.report.result}</div>}
      </div></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy || !status} onClick={() => void act(async () => {
        await enableGame(SteamClient.Apps, rpc, appid);
        return '2× включён для следующего запуска. Перезапустите выбранную игру.';
      })}>Включить 2× для игры</ButtonItem></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy || !status} onClick={() => void act(async () => {
        return await disableGame(SteamClient.Apps, rpc, appid) || 'Прежний запуск восстановлен. Перезапустите игру для отключения GSF.';
      })}>Восстановить запуск без GSF</ButtonItem></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        return 'Отчёт сохранён: '+(await rpc.export(appid)).path;
      })}>Сохранить отчёт</ButtonItem></PanelSectionRow>
    </>}
    {message && <PanelSectionRow><div style={{fontSize: 13, overflowWrap: 'anywhere'}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 11, opacity: 0.7}}>0.1.0 · Счётчики WSI не доказывают физический показ каждого кадра. Отчёт сохраняется автоматически после выхода.</div></PanelSectionRow>
  </PanelSection>;
}

export default definePlugin(() => ({name: 'GSF Test', titleView: <div className={staticClasses.Title}>GSF Test</div>,
  content: <Content/>, icon: <span>G</span>, onDismount() {}}));
