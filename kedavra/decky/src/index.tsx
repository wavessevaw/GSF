import {ButtonItem, PanelSection, PanelSectionRow, ToggleField, SliderField, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {useEffect, useRef, useState} from 'react';

type Summary = {pass?: boolean; p95_ms?: number; p99_ms?: number; missed_pct?: number; rows?: number; checks?: Record<string, boolean>};
type Report = {trace: string; report: string; ok: boolean; log: string; summary?: Summary};
type Capture = {path: string; started: number; label: string; elapsed_s: number} | null;
type Status = {
  engine: 'running' | 'stale' | 'absent'; fresh: boolean; message?: string; capture: Capture;
  multiplier?: number; state?: string; refresh_ns?: number; width?: number; height?: number;
  sources?: number; real?: number; gen?: number; repeat?: number; skip?: number; resync?: number; degrade?: number;
  applied?: number; bypass?: number; ring_miss?: number; unsupported?: number;
  gpu_us_last?: number; gpu_us_avg?: number; gpu_us_max?: number; over_budget?: number; timestamps?: boolean; last_problem?: string;
};

const rpc = {
  status: callable<[], Status>('get_status'),
  setMultiplier: callable<[number], {multiplier: number}>('set_multiplier'),
  start: callable<[string], Capture>('start_capture'),
  stop: callable<[], Report>('stop_capture'),
  logs: callable<[], string>('get_logs'),
};

const stateText: Record<string, string> = {
  off: 'выключено (кадры игры без изменений)',
  active: 'генерация',
  degraded: 'приостановлено: анализ не успевает',
};

function Content() {
  const [status, setStatus] = useState<Status>();
  const [mult, setMult] = useState(3);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [report, setReport] = useState<Report>();
  const [logs, setLogs] = useState('');
  const lock = useRef(false);

  useEffect(() => {
    let gone = false;
    const refresh = async () => {
      try {
        const s = await rpc.status();
        if (gone) return;
        setStatus(s);
        if (s.multiplier && s.multiplier > 1) setMult(s.multiplier);
      } catch (e) {if (!gone) setMessage(String(e));}
    };
    void refresh();
    const t = setInterval(refresh, 1000);
    return () => {gone = true; clearInterval(t);};
  }, []);

  const act = async (fn: () => Promise<string>) => {
    if (lock.current) return;
    lock.current = true; setBusy(true);
    try {const text = await fn(); setMessage(text); toaster.toast({title: 'KEDAVRA', body: text});}
    catch (e) {setMessage(String(e));}
    finally {lock.current = false; setBusy(false);}
  };

  const running = status?.engine === 'running';
  const on = running && (status?.multiplier ?? 1) > 1;
  const hz = status?.refresh_ns ? 1e9 / status.refresh_ns : 0;
  const total = (status?.real ?? 0) + (status?.gen ?? 0) + (status?.repeat ?? 0);
  const repeatPct = total ? 100 * (status?.repeat ?? 0) / total : 0;

  return <PanelSection title="KEDAVRA · генерация кадров">
    <PanelSectionRow><div style={{fontSize: 13}}>
      {running ? <>Движок: <b>работает</b> в Gamescope{hz ? <> · {hz.toFixed(1)} Гц</> : null}<br/>
        Состояние: {stateText[status?.state ?? 'off'] ?? status?.state}</> :
        <>{status?.message ?? 'Проверяю движок…'}</>}
    </div></PanelSectionRow>

    <PanelSectionRow><ToggleField label="Генерация кадров" disabled={busy || !running} checked={on}
      onChange={v => void act(async () => {
        await rpc.setMultiplier(v ? mult : 0);
        return v ? `Включено x${mult}. Ограничьте FPS игры до ${hz ? Math.round(hz / mult) : '1/' + mult + ' частоты'}.` : 'Выключено';
      })}/></PanelSectionRow>
    <PanelSectionRow><SliderField label="Множитель" value={mult} min={2} max={3} step={1} notchCount={2}
      notchLabels={[{notchIndex: 0, label: 'x2'}, {notchIndex: 1, label: 'x3'}]} disabled={busy || !running}
      onChange={v => {setMult(v); if (on) void act(async () => {await rpc.setMultiplier(v); return `Множитель x${v}`;});}}/></PanelSectionRow>
    <PanelSectionRow><div style={{fontSize: 12, opacity: 0.85}}>
      x3: частота 90 Гц, лимит FPS 30 · x2: 90 Гц/45 FPS, 80 Гц/40 FPS или 60 Гц/30 FPS (лимит FPS и частота в меню «Производительность»).
    </div></PanelSectionRow>

    {running && <PanelSectionRow><div style={{fontSize: 12}}>
      Кадры игры: {status?.sources ?? 0} · показано реальных {status?.real ?? 0}, сгенерированных {status?.gen ?? 0}, повторов {status?.repeat ?? 0} ({repeatPct.toFixed(2)}%)<br/>
      Пересинхронизаций {status?.resync ?? 0}, деградаций {status?.degrade ?? 0}, обходов композиции {status?.bypass ?? 0}, промахов кольца {status?.ring_miss ?? 0}<br/>
      GPU KEDAVRA: {status?.timestamps ? `${(status?.gpu_us_avg ?? 0).toFixed(0)} мкс в среднем, макс ${(status?.gpu_us_max ?? 0).toFixed(0)}, сверх бюджета ${status?.over_budget ?? 0}` : 'нет таймстемпов'}<br/>
      Последняя проблема: {status?.last_problem ?? 'none'}<br/>
      <span style={{opacity: 0.7}}>Счётчики — решения композитора. Темп подтверждает только трасса KMS flip ниже.</span>
    </div></PanelSectionRow>}

    <PanelSectionRow>{status?.capture ?
      <ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
        const r = await rpc.stop(); setReport(r);
        const s = r.summary;
        return s ? `Трасса: P95 ${s.p95_ms?.toFixed(3)} мс, P99 ${s.p99_ms?.toFixed(3)} мс, пропуски ${s.missed_pct?.toFixed(2)}% → ${s.pass ? 'PASS' : 'FAIL'}` : 'Трасса сохранена, анализ не удался';
      })}>Остановить запись ({status.capture.elapsed_s.toFixed(0)} с)</ButtonItem> :
      <ButtonItem layout="below" disabled={busy || !running} onClick={() => void act(async () => {
        const c = await rpc.start(`x${status?.multiplier ?? 0}`); return 'Запись трассы KMS flip: ' + (c?.path ?? '');
      })}>Записать трассу темпа (KMS flip)</ButtonItem>}
    </PanelSectionRow>
    {report?.summary && <PanelSectionRow><div style={{fontSize: 12, overflowWrap: 'anywhere'}}>
      {Object.entries(report.summary.checks ?? {}).map(([k, v]) => <div key={k}>{v ? '✓' : '✗'} {k}</div>)}
      Отчёт: {report.report}
    </div></PanelSectionRow>}

    <PanelSectionRow><ButtonItem layout="below" disabled={busy} onClick={() => void act(async () => {
      setLogs(await rpc.logs()); return 'Журнал обновлён';
    })}>Показать журнал</ButtonItem></PanelSectionRow>
    {logs && <PanelSectionRow><pre style={{fontSize: 10, whiteSpace: 'pre-wrap', maxHeight: 240, overflow: 'auto'}}>{logs}</pre></PanelSectionRow>}
    {message && <PanelSectionRow><div style={{fontSize: 12, overflowWrap: 'anywhere'}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 11, opacity: 0.7}}>0.1.0 · экспериментально. Работает только с Gamescope KEDAVRA (kedavra/deck/install.sh). Результаты на железе не подтверждены, пока нет вашей трассы.</div></PanelSectionRow>
  </PanelSection>;
}

export default definePlugin(() => ({name: 'KEDAVRA', titleView: <div className={staticClasses.Title}>KEDAVRA</div>,
  content: <Content/>, icon: <span>K</span>, onDismount() {}}));
