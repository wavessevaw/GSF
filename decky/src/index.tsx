import {ButtonItem, Dropdown, PanelSection, PanelSectionRow, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {useEffect, useRef, useState} from 'react';
import {enableGame, disableGame, copyCommand, withTimeout} from './steam-options.mjs';

type Game = {appid: number; name: string};
type Live = {generated_fps?: number; output_fps?: number; effective_multiplier?: number; generated_total?: number; dropped_synthetic?: number; gpu_generation_ms?: number; buffer_depth?: number; buffer_latency_ms?: number; buffered_owner?: number; source_fps: number; generated_wsi_accepted: number; real_wsi_accepted: number; multiplier?: number; output_refresh_hz?: number; refresh_origin?: string};
type Status = {enabled: boolean; configured?: boolean; manual_launch?: boolean; multiplier?: number; buffer_depth?: number; reconstruction_quality?: string; diagnostic_mode?: string; state: string; live: Live[]; gate_rejection?: string; launch_options?: string; packaged_source_commit?: string; active_build_source_commit?: string; build_matches?: boolean; report?: {result: string; exit_code?: number}};
function backend<Args extends unknown[], Result>(name: string) {
  const invoke = callable<Args, Result>(name);
  return (...args: Args) => withTimeout(invoke(...args));
}
const rpc = {
  games: backend<[], Game[]>('get_games'),
  selection: backend<[], {appid: number}>('get_selection'),
  select: backend<[number], {appid: number}>('select_game'),
  prepare: backend<[number, string], {options: string}>('prepare_game'),
  manual: backend<[number], {options: string}>('prepare_manual_game'),
  confirm: backend<[number, string], unknown>('confirm_game'),
  disable: backend<[number, string], {options: string; restore: boolean; message?: string}>('disable_game'),
  forget: backend<[number, string], void>('finish_restore'),
  status: backend<[number], Status>('get_status'),
  export: backend<[number], {path: string}>('export_report'),
  diagnostic: backend<[number, string], {diagnostic_mode: string}>('set_diagnostic_mode'),
  multiplier: backend<[number, number], {multiplier: number}>('set_multiplier'),
  buffer: backend<[number, number], {buffer_depth: number}>('set_buffer_depth'),
  quality: backend<[number, string], {reconstruction_quality: string}>('set_reconstruction_quality')
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
    try {toaster.toast({title: 'GSF Test', body: progress});} catch { /* Panel feedback remains visible. */ }
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
    } catch (e) {
      const text = String(e);
      setMessage(text);
      try {toaster.toast({title: 'GSF · ошибка', body: text});} catch { /* Panel feedback remains visible. */ }
    }
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
    const text = 'Выбрана: '+game.name+'. Выбери ×2, ×3 или ×4 перед запуском GSF.';
    setMessage(text);
    try {await rpc.select(id); toaster.toast({title: 'GSF · игра выбрана', body: game.name});}
    catch (e) {setMessage('Не удалось сохранить выбор: '+String(e));}
    finally {actionLock.current = false; setBusy(false);}
  };
  const game = games.find(g => g.appid === appid);
  const live = status?.live?.[0];
  const launchCommand = command || (status?.enabled ? status.launch_options : '') || '';
  const state = status?.state === 'generating' ? 'Генерация кадров подтверждена' :
    status?.state === 'bridge' ? 'GPU слой подключён; ждём генерацию' :
    status?.state === 'blocked' ? 'Генерация не включилась: '+status.gate_rejection :
    status?.enabled ? (status.manual_launch ? 'Ручной профиль готов. Вставьте команду в параметры запуска Steam и перезапустите игру.' : 'GSF ×'+(status?.multiplier || 2)+' настроен. Запустите или перезапустите игру') : 'GSF отключён для следующего запуска';
  return <PanelSection title="GSF Motion Intelligence · 0.1.13 · Launch fix 1">
    <PanelSectionRow><Dropdown rgOptions={games.map(g => ({data: g.appid, label: g.name}))}
      selectedOption={appid} disabled={busy} strDefaultLabel="Выберите установленную игру"
      onChange={o => void choose(o)} /></PanelSectionRow>
    {message && <PanelSectionRow><div role="status" style={{fontSize: 13, overflowWrap: 'anywhere'}}>{message}</div></PanelSectionRow>}
    {appid > 0 && <>
      <PanelSectionRow><div style={{fontSize: 14}}>Выбрана игра: <strong>{game?.name || appid}</strong></div></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        const prepared = await rpc.manual(appid);
        if (selected.current === appid) setCommand(prepared.options);
        return 'Ручной профиль готов. Скопируйте команду ниже в Steam → Свойства игры → Параметры запуска. Затем перезапустите игру.';
      }, 'Готовлю ручной запуск GSF…')}>Ручной запуск · показать команду</ButtonItem></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy || status?.multiplier === 0} onClick={() => void act(async () => {
        const apps = typeof SteamClient === 'undefined' ? undefined : SteamClient.Apps;
        const written = await enableGame(apps, rpc, appid);
        if (selected.current === appid) setCommand(written);
        return 'Steam подтвердил команду GSF. Перезапустите выбранную игру.';
      }, 'Жду Steam (до 6 секунд на ответ)…')}>Включить GSF ×{status?.multiplier || 2} автоматически</ButtonItem></PanelSectionRow>
      {launchCommand && <>
        <PanelSectionRow><div style={{fontSize: 12, overflowWrap: 'anywhere'}}>
          Команда запуска GSF:<br/><code style={{userSelect: 'text'}}>{launchCommand}</code>
          <div>Если уже есть параметры запуска, сохраните их: вставьте префикс GSF перед единственным %command%. Уберите другой генератор кадров.</div>
        </div></PanelSectionRow>
        <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
          await copyCommand(launchCommand); return 'Команда скопирована. Вставьте её в параметры запуска выбранной игры.';
        }, 'Копирую команду…')}>Скопировать команду запуска</ButtonItem></PanelSectionRow>
      </>}
      <PanelSectionRow><div style={{fontSize: 12}}>Генерация (изменение применяется после перезапуска игры):</div>
        <Dropdown rgOptions={[
          {data: 0, label: 'Off · без генерации'},
          {data: 2, label: '×2 · один G1, 30→60 / 45→90 / 60→90'},
          {data: 3, label: '×3 · до двух G1/G2, 30→90 / 40→120'},
          {data: 4, label: '×4 · до трёх G1/G2/G3, 22,5→90 / 30→120'}
        ]} selectedOption={status?.multiplier ?? 2} disabled={busy || !status?.configured}
          onChange={option => void act(async () => {
            const value = typeof option === 'object' && option && 'data' in option ? Number(option.data) : Number(option);
            await rpc.multiplier(appid, value);
            return value === 0 ? 'Генерация отключена для следующего запуска. Перезапустите игру.' : 'Множитель ×'+value+' сохранён для игры. Перезапустите её.';
          }, 'Сохраняю режим генерации…')} />
      </PanelSectionRow>
      <PanelSectionRow><div style={{fontSize: 12}}>Frame Buffer · независимый Output Owner:</div>
        <Dropdown rgOptions={[{data: 0, label: '0 · Low Latency'}, {data: 1, label: '1 · Balanced'}, {data: 2, label: '2 · Stable'}]}
          selectedOption={status?.buffer_depth ?? 1} disabled={busy || !status?.configured}
          onChange={option => void act(async () => {
            const depth = Number(typeof option === 'object' && option && 'data' in option ? option.data : option);
            await rpc.buffer(appid, depth); return 'Buffer '+depth+' сохранён. Перезапустите игру.';
          }, 'Сохраняю буфер…')} />
        <div style={{fontSize: 11}}>Буфер действует при совместимом независимом выводе. Совместимость Game Mode пока не подтверждена. Buffer 0: без дополнительного периода сверх ожидания пары A/B; Buffer 1/2: ещё один/два периода.</div>
      </PanelSectionRow>
      <PanelSectionRow><div style={{fontSize: 12}}>Reconstruction Quality:</div>
        <Dropdown rgOptions={[{data: 'ultra-lite', label: 'Ultra Lite'}, {data: 'balanced', label: 'Balanced'}, {data: 'quality', label: 'Quality'}, {data: 'adaptive', label: 'Adaptive'}]}
          selectedOption={status?.reconstruction_quality || 'balanced'} disabled={busy || !status?.configured}
          onChange={option => void act(async () => {
            const quality = String(typeof option === 'object' && option && 'data' in option ? option.data : option);
            await rpc.quality(appid, quality); return 'Качество сохранено. Перезапустите игру.';
          }, 'Сохраняю качество…')} />
      </PanelSectionRow>
      {status?.multiplier === 4 && <PanelSectionRow><div style={{fontSize: 12}}>×4: 30 FPS запрашивают 120 FPS, 45 FPS — 180 FPS. При 90 Гц доступны максимум 90 слотов/с; строятся только кадры для свободных слотов. Физический показ требует измерений дисплея.</div></PanelSectionRow>}
      <PanelSectionRow><div style={{fontSize: 12}}>Диагностика (применяется после перезапуска игры):</div>
        <Dropdown rgOptions={[
          {data: 'normal', label: 'Flow Lite 2.0: общие motion/confidence, low-res реконструкция'},
          {data: 'normal-flow-lite1', label: 'Контроль: Flow Lite 1.0'},
          {data: 'normal-eco45', label: 'Контроль: прежний ECO45 / Shared Flow'},
          {data: 'normal-lmc-gpu', label: 'Контроль: прежний тяжёлый GPU LMC'},
          {data: 'normal-no-shadow', label: 'Контроль: без Shadow Admission'},
          {data: 'normal-adaptive-pairs', label: 'Восстановление с фиксированными слотами (×2)'},
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
      <PanelSectionRow><div style={{fontSize: 11, overflowWrap: 'anywhere'}}>Источник пакета: <span style={{fontFamily: 'monospace'}}>{status?.packaged_source_commit?.slice(0, 12) || 'неизвестен'}</span>; активный: <span style={{fontFamily: 'monospace'}}>{status?.active_build_source_commit?.slice(0, 12) || 'неизвестен'}</span>.
        {status && !status.build_matches && <strong> ОШИБКА: установленный GPU слой не соответствует пакету. Генерация заблокирована до переустановки.</strong>}
      </div></PanelSectionRow>
      <PanelSectionRow><div style={{fontSize: 13}}>{status ? state : 'Выбор сохранён. Нажмите «Ручной запуск» или включите GSF автоматически.'}
        {live && <div>Источник: {live.source_fps.toFixed(1)} FPS<br/>
          Generated: {live.generated_fps?.toFixed(1) ?? '—'} FPS; Output (WSI): {live.output_fps?.toFixed(1) ?? '—'} FPS<br/>
          Effective Multiplier: {live.effective_multiplier?.toFixed(2) ?? '—'}<br/>
          GPU Generation: {live.gpu_generation_ms?.toFixed(2) ?? '—'} ms; Dropped Synthetic: {live.dropped_synthetic ?? '—'}<br/>
          Buffer: {live.buffer_depth ?? '—'}; latency estimate: {live.buffer_latency_ms?.toFixed(1) ?? '—'} ms; Owner: {live.buffered_owner ? 'active' : 'ordinary WSI'}<br/>
          Displayed: не измерено<br/>
          Целевой режим: ×{live.multiplier || status?.multiplier || 2}<br/>
          Частота планировщика: {live.output_refresh_hz ? live.output_refresh_hz.toFixed(2) + ' Гц (' + (live.refresh_origin || 'unknown') + ')' : 'не определена'}<br/>
          Принято WSI: исходных {live.real_wsi_accepted}, сгенерированных {live.generated_wsi_accepted}</div>}
        {!live && status?.report && <div>Последний прогон: {status.report.result}</div>}
      </div></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        setCommand('');
        if (status?.manual_launch) return (await rpc.disable(appid, '')).message || 'GSF отключён. Уберите команду запуска вручную.';
        return await disableGame(SteamClient.Apps, rpc, appid) || 'Прежний запуск восстановлен. Перезапустите игру для отключения GSF.';
      }, 'Восстанавливаю прежние параметры запуска…')}>Восстановить запуск без GSF</ButtonItem></PanelSectionRow>
      <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        return 'Отчёт сохранён: '+(await rpc.export(appid)).path;
      }, 'Сохраняю отчёт…')}>Сохранить отчёт</ButtonItem></PanelSectionRow>
    </>}
    <PanelSectionRow><div style={{fontSize: 11, opacity: 0.7}}>EXPERIMENTAL · ×2/×3/×4 используют общий анализ движения пары и реконструкцию в половинном разрешении. Частота игры не фиксируется. WSI Accepted не равен Displayed: физический scanout и 10/15 Вт на Steam Deck ещё не измерены. Независимый буферизованный вывод ограничен поддерживаемым WSI-контрактом; обычный вывод остаётся резервным путём.</div></PanelSectionRow>
  </PanelSection>;
}

export default definePlugin(() => ({name: 'GSF Test', titleView: <div className={staticClasses.Title}>GSF Test</div>,
  content: <Content/>, icon: <span>G</span>, onDismount() {}}));
