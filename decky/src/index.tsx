import {ButtonItem, Dropdown, PanelSection, PanelSectionRow, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {useEffect, useRef, useState} from 'react';
import {enableGame, disableGame} from './steam-options.mjs';

type Game = {appid: number; name: string};
type Live = {source_fps: number; generated_wsi_accepted: number; real_wsi_accepted: number};
type Status = {enabled: boolean; configured?: boolean; manual_unverified?: boolean; package_build?: string; active_build?: string; active_matches_package?: boolean; diagnostic_mode?: string; state: string; live: Live[]; gate_rejection?: string; launch_options?: string; report?: {result: string; exit_code?: number}};
const rpc = {
  games: callable<[], Game[]>('get_games'),
  selection: callable<[], {appid: number}>('get_selection'),
  select: callable<[number], {appid: number}>('select_game'),
  prepare: callable<[number, string], {options: string}>('prepare_game'),
  confirm: callable<[number, string], unknown>('confirm_game'),
  disable: callable<[number, string], {options: string; restore: boolean; message?: string}>('disable_game'),
  forget: callable<[number, string], void>('finish_restore'),
  status: callable<[number], Status>('get_status'),
  export: callable<[number], {path: string}>('export_report'),
  diagnostic: callable<[number, string], {diagnostic_mode: string}>('set_diagnostic_mode'),
  restore: callable<[number], {build: string; enabled: boolean; has_launch_options: boolean}>('restore_known_good'),
  rearm: callable<[number, string], {enabled: boolean; steam_verified: boolean}>('rearm_existing')
};

function Content() {
  const [games, setGames] = useState<Game[]>([]);
  const [appid, setAppid] = useState(0);
  const [status, setStatus] = useState<Status>();
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [command, setCommand] = useState('');
  const selected = useRef(appid);
  selected.current = appid;
  const actionLock = useRef(false);
  useEffect(() => {
    let gone = false;
    void Promise.all([rpc.games(), rpc.selection()]).then(([list, saved]) => {
      if (gone) return;
      setGames(list);
      if (list.some(g => g.appid === saved.appid)) setAppid(saved.appid);
    }).catch(e => {if (!gone) setMessage(String(e));});
    return () => {gone = true;};
  }, []);
  useEffect(() => {
    setStatus(undefined);
    setCommand('');
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
  const act = async (fn: () => Promise<string>, progress: string) => {
    if (actionLock.current) return;
    actionLock.current = true;
    setBusy(true); setMessage(progress);
    const id = appid;
    try {
      const text = await fn();
      if (selected.current === id) {
        setMessage(text);
        toaster.toast({title: 'GSF Test', body: text});
        // A separate status query must not turn a confirmed Steam write into
        // an apparent failure or keep the activation button disabled forever.
        try {setStatus(await rpc.status(id));} catch { /* poll will retry */ }
      }
    } catch (e) {setMessage(String(e));}
    finally {actionLock.current = false; setBusy(false);}
  };
  const choose = async (option: unknown) => {
    const value = typeof option === 'object' && option !== null && 'data' in option ? option.data : option;
    const id = Number(value);
    const game = games.find(g => g.appid === id);
    if (!game) {setMessage('Steam не передал выбранную игру. Попробуйте выбрать ещё раз.'); return;}
    if (actionLock.current) return;
    actionLock.current = true; setBusy(true);
    setAppid(id);
    const text = 'Выбрана: '+game.name+'. Теперь нажмите «Включить 2×».';
    setMessage(text);
    try {await rpc.select(id); toaster.toast({title: 'GSF · игра выбрана', body: game.name});}
    catch (e) {setMessage('Не удалось сохранить выбор: '+String(e));}
    finally {actionLock.current = false; setBusy(false);}
  };
  const game = games.find(g => g.appid === appid);
  const live = status?.live?.[0];
  const state = status?.state === 'generating' ? 'Генерация кадров подтверждена' :
    status?.state === 'bridge' ? 'GPU слой подключён; ждём генерацию' :
    status?.state === 'blocked' ? 'Генерация не включилась: '+status.gate_rejection :
    status?.enabled ? '2× настроен. Запустите или перезапустите игру' : 'GSF отключён для следующего запуска';
  return <PanelSection title="GSF · игровой тест 2×">
    <PanelSectionRow><Dropdown rgOptions={games.map(g => ({data: g.appid, label: g.name}))}
      selectedOption={appid} disabled={busy} strDefaultLabel="Выберите установленную игру"
      onChange={o => void choose(o)} /></PanelSectionRow>
    {appid > 0 && <>
      <PanelSectionRow><div style={{fontSize: 14}}>Выбрана игра: <strong>{game?.name || appid}</strong></div></PanelSectionRow>
      <PanelSectionRow>
        <div style={{fontSize: 12}}>
          Движок в пакете: <strong>{status?.package_build?.slice(0, 10) || 'проверка…'}</strong><br/>
          Движок на Deck: <strong>{status?.active_build?.slice(0, 10) || 'не установлен'}</strong><br/>
          {status?.active_matches_package ? 'Версия движка совпадает с пакетом.' :
            'Версии различаются. Нажмите восстановление ниже перед запуском игры.'}
        </div>
      </PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        const r = await rpc.restore(appid);
        if (!r.build) throw Error('Движок не восстановлен');
        return 'Проверенный движок 0.1.10 восстановлен: '+r.build.slice(0, 10)+'. Steam параметры не менялись. Перезапустите игру.';
      }, 'Проверяю и возвращаю исходный движок GSF 0.1.10…')}>Восстановить движок GSF 0.1.10</ButtonItem></PanelSectionRow>
      {!status?.enabled && !!status?.launch_options && <PanelSectionRow>
        <ButtonItem layout="below" disabled={busy || !status?.active_matches_package}
          onClick={() => void act(async () => {
            await rpc.rearm(appid, status!.launch_options!);
            return 'Ручной запуск ×2 включён без проверки Steam API. Команда должна уже быть вставлена в свойствах игры. Перезапустите её.';
          }, 'Активирую существующую команду GSF ×2…')}>
          Команда уже в Steam: включить ×2 вручную
        </ButtonItem>
      </PanelSectionRow>}
      <PanelSectionRow><div style={{fontSize: 12}}>Диагностика (применяется после перезапуска игры):</div>
        <Dropdown rgOptions={[
          {data: 'normal', label: 'Обычный 2× + Shadow Admission 0.1.10'},
          {data: 'normal-no-shadow', label: 'Контроль: без Shadow Admission'},
          {data: 'normal-adaptive-pairs', label: 'Эксперимент 0.1.8: адаптивная пара G1/B'},
          {data: 'normal-queue-observe', label: 'Диагностика: задания игры перед генерацией'},
          {data: 'normal-no-watch', label: 'Контроль: 2× без Timeline Watch'},
          {data: 'normal-no-latency-admission', label: 'Контроль: прежний допуск, калибровка включена'},
          {data: 'normal-no-calibration', label: 'Контроль: 2× без калибровки'},
          {data: 'normal-calibration-every-frame', label: 'Тест стоимости: калибровать каждый кадр'},
          {data: 'source-only', label: 'Контроль: только исходные кадры'},
          {data: 'compute-only', label: 'Стресс-тест: только вычисления'},
          {data: 'matched-compute', label: 'Тот же допуск, без показа synthetic'}
        ]} selectedOption={status?.diagnostic_mode || 'normal'} disabled={busy || !status?.configured}
          onChange={option => void act(async () => {
            const value = typeof option === 'object' && option && 'data' in option ? String(option.data) : String(option);
            await rpc.diagnostic(appid, value);
            return 'Диагностика сохранена. Перезапустите игру.';
          }, 'Записываю диагностический режим…')} />
      </PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        const written = await enableGame(SteamClient.Apps, rpc, appid);
        setCommand(written);
        return 'Steam подтвердил команду GSF. Перезапустите выбранную игру.';
      }, 'Устанавливаю GSF и записываю команду запуска…')}>Включить 2×</ButtonItem></PanelSectionRow>
      <PanelSectionRow><div style={{fontSize: 13}}>{status ? state : 'Выбор сохранён. Нажмите «Включить 2×» для настройки запуска.'}
        {live && <div>Источник: {live.source_fps.toFixed(1)} FPS<br/>
          Принято WSI: исходных {live.real_wsi_accepted}, сгенерированных {live.generated_wsi_accepted}</div>}
        {!live && status?.report && <div>Последний прогон: {status.report.result}</div>}
      </div></PanelSectionRow>
      {(command || status?.launch_options) && <PanelSectionRow><div style={{fontSize: 12, overflowWrap: 'anywhere'}}>
        Команда запуска GSF:<br/><code>{command || status?.launch_options}</code>
      </div></PanelSectionRow>}
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        setCommand('');
        return await disableGame(SteamClient.Apps, rpc, appid) || 'Прежний запуск восстановлен. Перезапустите игру для отключения GSF.';
      }, 'Восстанавливаю прежние параметры запуска…')}>Восстановить запуск без GSF</ButtonItem></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        return 'Отчёт сохранён: '+(await rpc.export(appid)).path;
      }, 'Сохраняю отчёт…')}>Сохранить отчёт</ButtonItem></PanelSectionRow>
    </>}
    {message && <PanelSectionRow><div style={{fontSize: 13, overflowWrap: 'anywhere'}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 11, opacity: 0.7}}>0.1.10.1 ROLLBACK · Движок и генератор кадров точно из 0.1.10. Кнопка восстановления проверяет активную копию и не изменяет параметры Steam.  Исправлено сохранение timing B от предыдущей пары. Shadow диагностирует ранние отказы GPU-budget, настоящий pacing/давление очереди и причину нулевого timing; пробы восстановления только виртуальные. 0.1.8 Adaptive по-прежнему экспериментален. Допуск по полному измеренному времени готовности GPU, ограниченные повторные попытки. Это ещё не стабильные 2×. Экспериментальный GPU LMC · Vulkan / DXVK / VKD3D, x64. Для отдельного теста отключите GFG/LSFG. Исходный FPS не фиксируется. Счётчики WSI не доказывают физический показ каждого кадра.</div></PanelSectionRow>
  </PanelSection>;
}

export default definePlugin(() => ({name: 'GSF Test', titleView: <div className={staticClasses.Title}>GSF Test</div>,
  content: <Content/>, icon: <span>G</span>, onDismount() {}}));
