import {DialogButton, Focusable, PanelSection, PanelSectionRow, SliderField, ToggleField, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {CSSProperties, ReactNode, useEffect, useRef, useState} from 'react';
import {LOGO} from './logo';

const VERSION = '1.0.0';

type Summary = {pass?: boolean; p95_ms?: number; p99_ms?: number; missed_pct?: number; rows?: number; checks?: Record<string, boolean>};
type Report = {trace: string; report: string; ok: boolean; log: string; summary?: Summary};
type Capture = {path: string; started: number; label: string; elapsed_s: number} | null;
type Live = {fps: number | null; real: number | null; pace: number | null; gen_share: number | null};
type Status = {
  engine: 'running' | 'stale' | 'absent'; fresh: boolean; message?: string; capture: Capture; live?: Live;
  multiplier?: number; state?: string; refresh_ns?: number; width?: number; height?: number;
  sources?: number; real?: number; gen?: number; repeat?: number; skip?: number; resync?: number; degrade?: number;
  applied?: number; bypass?: number; ring_miss?: number; unsupported?: number;
  gpu_us_last?: number; gpu_us_avg?: number; gpu_us_max?: number; over_budget?: number; timestamps?: boolean; last_problem?: string;
  auto?: boolean; auto_switches?: number; source_late?: number; cap_fps?: number; base_fps?: number;
};
type Mode = 'off' | 'auto' | 'cap';
type View = 'main' | 'setup' | 'pace' | 'log';
type Check = {key: string; ok: boolean; text: string; detail: string};
type Diagnose = {checks: Check[]; ok: boolean};
type Bench = {ok: boolean; correct?: boolean; analysis_ms?: number; two_outputs_ms?: number; device?: string; log: string; json: string};

const rpc = {
  status: callable<[], Status>('get_status'),
  settings: callable<[], {hud: boolean; base_fps: number}>('get_settings'),
  setMode: callable<[Mode, number], {mode: Mode; base_fps: number}>('set_mode'),
  setHud: callable<[boolean], {hud: boolean}>('set_hud'),
  start: callable<[string], Capture>('start_capture'),
  stop: callable<[], Report>('stop_capture'),
  logs: callable<[], string>('get_logs'),
  diagnose: callable<[], Diagnose>('diagnose'),
  report: callable<[], {path: string}>('save_report'),
  install: callable<[], {message: string; needs_reboot: boolean}>('install'),
  uninstall: callable<[], {message: string; needs_reboot: boolean}>('uninstall'),
  reboot: callable<[], unknown>('reboot'),
  bench: callable<[], Bench>('run_bench'),
};

// Green-black palette shared with the HUD (kedavra_hud.py) and the README.
const C = {
  ground: '#050706', card: '#0d1210', line: '#1c261f', track: '#1a231d',
  green: '#52d64a', greenText: '#8ff07a', greenBg: '#13301a',
  yellow: '#e3c04a', red: '#ff8a7a', redBg: '#3a1d1a',
  text: '#f2f6f3', muted: '#a9b5ae', label: '#8e9b93',
};
const card: CSSProperties = {background: C.card, border: `1px solid ${C.line}`, borderRadius: 16, padding: 12, boxSizing: 'border-box'};
const caps: CSSProperties = {fontSize: 10, fontWeight: 700, letterSpacing: '0.16em', color: C.label};

function Ring({size, width, frac, color, children}: {size: number; width: number; frac: number; color: string; children?: ReactNode}) {
  const r = (size - width) / 2;
  const len = 2 * Math.PI * r;
  const f = Math.max(0, Math.min(1, frac));
  return <div style={{position: 'relative', width: size, height: size, flex: 'none'}}>
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
      <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke={C.track} strokeWidth={width}/>
      {f > 0 && <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke={color} strokeWidth={width} strokeLinecap="round"
        strokeDasharray={`${f * len} ${len}`} transform={`rotate(-90 ${size / 2} ${size / 2})`}/>}
    </svg>
    <div style={{position: 'absolute', inset: 0, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center'}}>{children}</div>
  </div>;
}

function Chip({ok, children}: {ok: boolean; children: ReactNode}) {
  return <span style={{alignSelf: 'flex-start', padding: '3px 8px', borderRadius: 6, fontSize: 10, fontWeight: 700, letterSpacing: '0.06em',
    background: ok ? C.greenBg : C.redBg, color: ok ? C.greenText : C.red}}>{children}</span>;
}

function Tile({selected, onClick, disabled, big, label}: {selected: boolean; onClick: () => void; disabled?: boolean; big: string; label: string}) {
  return <DialogButton onClick={onClick} disabled={disabled} style={{
    minWidth: 0, flex: 1, padding: '8px 2px', borderRadius: 14, display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 2,
    background: selected ? C.text : C.card, color: selected ? '#07110a' : C.text, border: `1px solid ${selected ? C.text : C.line}`,
    boxShadow: selected ? '0 0 0 2px rgba(82,214,74,0.35)' : 'none'}}>
    <span style={{fontSize: 15, fontWeight: 800}}>{big}</span>
    <span style={{fontSize: 10, fontWeight: 600}}>{label}</span>
  </DialogButton>;
}

function NavButton({onClick, children}: {onClick: () => void; children: ReactNode}) {
  return <DialogButton onClick={onClick} style={{minWidth: 0, flex: 1, padding: '8px 2px', borderRadius: 12, fontSize: 11, fontWeight: 600,
    background: C.card, color: C.text, border: `1px solid ${C.line}`}}>{children}</DialogButton>;
}

function Back({onClick, title}: {onClick: () => void; title: string}) {
  return <PanelSectionRow><Focusable style={{display: 'flex', alignItems: 'center', gap: 10}}>
    <DialogButton onClick={onClick} style={{minWidth: 0, width: 40, height: 36, padding: 0, borderRadius: 10, background: C.card, border: `1px solid ${C.line}`}}>‹</DialogButton>
    <div style={{fontSize: 18, fontWeight: 800}}>{title}</div>
  </Focusable></PanelSectionRow>;
}

const fmt = (v: number | null | undefined, d = 0) => v === null || v === undefined ? '—' : v.toFixed(d);

function Content() {
  const [status, setStatus] = useState<Status>();
  const [view, setView] = useState<View>('main');
  const [hud, setHud] = useState(true);
  const [base, setBase] = useState(30);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [report, setReport] = useState<Report>();
  const [logs, setLogs] = useState('');
  const [diag, setDiag] = useState<Diagnose>();
  const [needsReboot, setNeedsReboot] = useState(false);
  const [benchRes, setBenchRes] = useState<Bench>();
  const lock = useRef(false);
  const baseTimer = useRef<number | undefined>(undefined);

  useEffect(() => {rpc.settings().then(r => {setHud(r.hud); setBase(r.base_fps);}).catch(() => {});}, []);
  const refreshDiag = async () => {try {setDiag(await rpc.diagnose());} catch (e) {setMessage(String(e));}};
  useEffect(() => {void refreshDiag();}, []);
  useEffect(() => {
    let gone = false;
    const refresh = async () => {
      try {const s = await rpc.status(); if (!gone) setStatus(s);}
      catch (e) {if (!gone) setMessage(String(e));}
    };
    void refresh();
    const t = setInterval(refresh, 1000);
    return () => {gone = true; clearInterval(t);};
  }, []);

  const act = async (fn: () => Promise<string>) => {
    if (lock.current) return;
    lock.current = true; setBusy(true);
    try {const text = await fn(); setMessage(text); toaster.toast({title: 'GSF Kedavra', body: text});}
    catch (e) {
      // Errors are shown on the panel and as a toast; a silent failure looks like a dead button.
      const text = 'Ошибка: ' + (e instanceof Error ? e.message : String(e));
      setMessage(text); toaster.toast({title: 'GSF Kedavra', body: text});
    }
    finally {lock.current = false; setBusy(false);}
  };

  const running = status?.engine === 'running';
  const mult = status?.multiplier ?? 1;
  const generating = running && status?.state === 'active';
  const mode: Mode = !running || (mult <= 1 && status?.state !== 'active') ? 'off' : status?.base_fps ? 'cap' : 'auto';
  const hz = status?.refresh_ns ? 1e9 / status.refresh_ns : 90;
  const live = status?.live;
  const pace = live?.pace;
  const paceOk = pace === null || pace === undefined || pace >= 0.995;
  const gameFps = status?.base_fps || status?.cap_fps || 0;
  const ratio = gameFps ? hz / gameFps : mult;
  const installed = diag?.checks.some(c => c.key === 'installed' && c.ok);

  const setMode = (m: Mode, b = base) => void act(async () => {
    await rpc.setMode(m, b);
    return m === 'off' ? 'Генерация выключена' : m === 'auto' ? 'Авто: движок сам держит игру на 30 или 45 и выбирает ×3 или ×2'
      : `Базовый FPS ${b}: игра ограничена до ${b}, генерация доводит до ${Math.round(hz)}`;
  });
  const onBase = (v: number) => {
    setBase(v);
    // The slider fires on every step; apply once it settles.
    window.clearTimeout(baseTimer.current);
    if (mode === 'cap') baseTimer.current = window.setTimeout(() => setMode('cap', v), 500);
  };

  let note: string;
  if (!running) note = status?.message ?? 'Проверяю движок…';
  else if (mode === 'off') note = `Генерация выключена. Игра сейчас: ${fmt(live?.real)} FPS.`;
  else if (status?.state === 'degraded') note = 'Приостановлено: анализ кадров не успевает. Движок попробует снова через 2 с.';
  else if (mode === 'cap') note = `Игра ограничена до ${status?.base_fps} FPS, генерация доводит до ${Math.round(hz)}. ` +
    (live?.real !== null && live?.real !== undefined && live.real < 0.95 * (status?.base_fps ?? 0) ? 'Игра не успевает за этим лимитом: снизьте его.' : 'Игра держит лимит.');
  else note = `Авто держит ×${mult}: игра ограничена до ${status?.cap_fps ?? Math.round(hz / mult)} FPS. ` +
    (mult === 3 ? 'Если сцена станет легче, GSF сам попробует ×2.' : 'Если игра перестанет успевать, GSF перейдёт на ×3.');

  const header = <PanelSectionRow><div style={{fontSize: 10, color: C.label, letterSpacing: '0.08em', textAlign: 'right'}}>
    {running ? `Steam Deck · ${Math.round(hz)} Гц` : 'движок не запущен'}</div></PanelSectionRow>;

  if (view === 'setup') return <PanelSection>
    <Back onClick={() => setView('main')} title="Установка"/>
    <PanelSectionRow><div style={{...card, fontSize: 11, lineHeight: 1.45, color: C.muted}}>
      Всё ставится из плагина. Образ SteamOS не меняется: Gamescope KEDAVRA лежит в /var/lib/kedavra и подключается поверх стокового.
      Если игровой режим не загрузится, следующая перезагрузка сама вернёт стоковый.</div></PanelSectionRow>
    <PanelSectionRow><div style={caps}>ПРОВЕРКА СИСТЕМЫ</div></PanelSectionRow>
    {diag?.checks.map((c, i) => <PanelSectionRow key={c.key + i}><div style={{display: 'flex', gap: 8, fontSize: 12, overflowWrap: 'anywhere'}}>
      <span style={{color: c.ok ? C.greenText : C.red, fontWeight: 800}}>{c.ok ? '✓' : '✗'}</span>
      <div>{c.text}{c.detail && <div style={{fontSize: 10, color: C.muted, whiteSpace: 'pre-wrap'}}>{c.detail}</div>}</div>
    </div></PanelSectionRow>)}
    <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      const r = await rpc.install(); setNeedsReboot(r.needs_reboot); await refreshDiag(); return r.message;
    })} style={{background: C.green, color: '#04120a', fontWeight: 800}}>{busy ? 'Выполняется…' : installed ? 'Переустановить Gamescope KEDAVRA' : 'Установить Gamescope KEDAVRA'}</DialogButton></PanelSectionRow>
    {needsReboot && <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      await rpc.reboot(); return 'Перезагрузка…';
    })}>Перезагрузить Deck сейчас</DialogButton></PanelSectionRow>}
    {message && <PanelSectionRow><div style={{fontSize: 11, overflowWrap: 'anywhere', whiteSpace: 'pre-wrap'}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><Focusable style={{display: 'flex', gap: 6}}>
      <NavButton onClick={() => void act(async () => {await refreshDiag(); return 'Проверка обновлена';})}>Проверить</NavButton>
      <NavButton onClick={() => void act(async () => {
        const b = await rpc.bench(); setBenchRes(b);
        return b.ok ? `Бенчмарк: анализ ${b.analysis_ms?.toFixed(2)} мс, два кадра ${b.two_outputs_ms?.toFixed(2)} мс` : 'Бенчмарк не прошёл';
      })}>Бенчмарк</NavButton>
      <NavButton onClick={() => void act(async () => 'Отчёт сохранён: ' + (await rpc.report()).path)}>Отчёт</NavButton>
    </Focusable></PanelSectionRow>
    {benchRes && <PanelSectionRow><div style={{...card, display: 'flex', justifyContent: 'space-between', fontSize: 11}}>
      <div><b style={{fontSize: 15}}>{benchRes.analysis_ms?.toFixed(1)}</b> мс<div style={{color: C.label}}>анализ</div></div>
      <div><b style={{fontSize: 15}}>{benchRes.two_outputs_ms?.toFixed(1)}</b> мс<div style={{color: C.label}}>два кадра</div></div>
      <div><b style={{fontSize: 15, color: benchRes.correct ? C.greenText : C.red}}>{benchRes.correct ? 'PASS' : 'FAIL'}</b><div style={{color: C.label}}>точность</div></div>
    </div></PanelSectionRow>}
    {installed && <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      const r = await rpc.uninstall(); setNeedsReboot(r.needs_reboot); await refreshDiag(); return r.message;
    })} style={{background: 'transparent', color: C.red, border: `1px solid ${C.redBg}`}}>Удалить и вернуть стоковый Gamescope</DialogButton></PanelSectionRow>}
  </PanelSection>;

  if (view === 'pace') return <PanelSection>
    <Back onClick={() => setView('main')} title="Темп"/>
    <PanelSectionRow><div style={{...card, fontSize: 11, lineHeight: 1.45, color: C.muted}}>
      Трасса пишет время каждого переключения кадра на экране (KMS flip). Это доказательство темпа, а не счётчик FPS. Поиграйте 1–2 минуты и остановите запись.</div></PanelSectionRow>
    <PanelSectionRow>{status?.capture ?
      <DialogButton disabled={busy} onClick={() => void act(async () => {
        const r = await rpc.stop(); setReport(r);
        const s = r.summary;
        return s ? `Трасса: P95 ${s.p95_ms?.toFixed(3)} мс, P99 ${s.p99_ms?.toFixed(3)} мс, пропуски ${s.missed_pct?.toFixed(2)}% → ${s.pass ? 'PASS' : 'FAIL'}` : 'Трасса сохранена, анализ не удался';
      })} style={{background: C.text, color: '#07110a', fontWeight: 800, letterSpacing: '0.1em'}}>■ ОСТАНОВИТЬ ЗАПИСЬ ({status.capture.elapsed_s.toFixed(0)} с)</DialogButton> :
      <DialogButton disabled={busy || !running} onClick={() => void act(async () => {
        const label = mode === 'cap' ? `cap${status?.base_fps}` : `${status?.auto ? 'auto-' : ''}x${mult}`;
        const c = await rpc.start(label); return 'Запись трассы KMS flip: ' + (c?.path ?? '');
      })} style={{background: C.green, color: '#04120a', fontWeight: 800}}>● Записать трассу темпа</DialogButton>}
    </PanelSectionRow>
    {report?.summary && <PanelSectionRow><Focusable style={{display: 'flex', gap: 6}}>
      {[['P95', `${report.summary.p95_ms?.toFixed(2)}`, '≤ 1 мс', (report.summary.p95_ms ?? 9) <= 1],
        ['P99', `${report.summary.p99_ms?.toFixed(2)}`, '≤ 2 мс', (report.summary.p99_ms ?? 9) <= 2],
        ['ПРОПУСКИ', `${report.summary.missed_pct?.toFixed(2)}%`, '< 0.5%', (report.summary.missed_pct ?? 9) < 0.5]].map(([k, v, lim, ok]) =>
        <div key={String(k)} style={{...card, flex: 1, padding: 8, display: 'flex', flexDirection: 'column', gap: 3}}>
          <div style={caps}>{k}</div><div style={{fontSize: 16, fontWeight: 800}}>{v}</div><Chip ok={Boolean(ok)}>{lim}</Chip>
        </div>)}
    </Focusable></PanelSectionRow>}
    {report && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, overflowWrap: 'anywhere'}}>
      {Object.entries(report.summary?.checks ?? {}).map(([k, v]) => <div key={k}>{v ? '✓' : '✗'} {k}</div>)}
      Отчёт: {report.report}</div></PanelSectionRow>}
    {running && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, lineHeight: 1.5}}>
      Кадров игры {status?.sources ?? 0} · реальных {status?.real ?? 0}, сгенерированных {status?.gen ?? 0}, повторов {status?.repeat ?? 0}<br/>
      Опозданий кадров игры {status?.source_late ?? 0} · переключений режима {status?.auto_switches ?? 0} · пересинхронизаций {status?.resync ?? 0}<br/>
      GPU KEDAVRA: {status?.timestamps ? `${((status?.gpu_us_avg ?? 0) / 1000).toFixed(2)} мс в среднем, макс ${((status?.gpu_us_max ?? 0) / 1000).toFixed(2)}` : 'нет таймстемпов'} · проблема: {status?.last_problem ?? 'none'}
    </div></PanelSectionRow>}
  </PanelSection>;

  if (view === 'log') return <PanelSection>
    <Back onClick={() => setView('main')} title="Журнал"/>
    <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {setLogs(await rpc.logs()); return 'Журнал обновлён';})}>Обновить</DialogButton></PanelSectionRow>
    {logs && <PanelSectionRow><pre style={{fontSize: 9, whiteSpace: 'pre-wrap', maxHeight: 360, overflow: 'auto'}}>{logs}</pre></PanelSectionRow>}
  </PanelSection>;

  const fpsColor = !generating ? C.green : (live?.fps ?? 0) >= 0.97 * hz ? C.green : (live?.fps ?? 0) >= 0.85 * hz ? C.yellow : C.red;
  return <PanelSection>
    {header}
    <PanelSectionRow><div style={{...card, display: 'flex', alignItems: 'center', gap: 12}}>
      <Ring size={104} width={9} frac={(live?.fps ?? 0) / hz} color={fpsColor}>
        <div style={{fontSize: 34, fontWeight: 900, lineHeight: 1}}>{fmt(live?.fps)}</div>
        <div style={{...caps, marginTop: 2}}>FPS</div>
      </Ring>
      <div style={{display: 'flex', flexDirection: 'column', gap: 5, minWidth: 0}}>
        <div style={caps}>ИГРА × GSF</div>
        <div style={{fontSize: 24, fontWeight: 800, lineHeight: 1}}>{fmt(live?.real)}{generating && <span style={{color: C.green}}> ×{Number.isInteger(ratio) ? ratio : ratio.toFixed(1)}</span>}</div>
        <div style={caps}>{mode === 'cap' ? `CAP ${status?.base_fps}` : mode === 'auto' ? `АВТО · CAP ${status?.cap_fps ?? '—'}` : 'ВЫКЛ'}</div>
        {generating && pace !== undefined && <Chip ok={paceOk}>{paceOk ? '✓ ТЕМП РОВНЫЙ' : 'ТЕМП ПРОСЕЛ'}</Chip>}
      </div>
    </div></PanelSectionRow>

    <PanelSectionRow><div style={{display: 'flex', gap: 6}}>
      {[['РЕАЛ', fmt(live?.real), (live?.real ?? 0) / hz, C.green],
        ['PACE %', pace === null || pace === undefined ? '—' : Math.min(99.9, 100 * pace).toFixed(1), pace ?? 0, paceOk ? C.green : C.yellow],
        ['GPU МС', status?.timestamps ? ((status?.gpu_us_avg ?? 0) / 1000).toFixed(1) : '—', (status?.gpu_us_avg ?? 0) / 11111, C.yellow]].map(([k, v, f, col]) =>
        <div key={String(k)} style={{...card, flex: 1, padding: 8, display: 'flex', alignItems: 'center', gap: 6}}>
          <Ring size={26} width={3.5} frac={Number(f)} color={String(col)}/>
          <div><div style={{fontSize: 13, fontWeight: 800}}>{v}</div><div style={{...caps, fontSize: 8}}>{k}</div></div>
        </div>)}
    </div></PanelSectionRow>

    <PanelSectionRow><div style={{...card, fontSize: 11, lineHeight: 1.4, color: C.muted}}>{note}</div></PanelSectionRow>

    <PanelSectionRow><Focusable style={{display: 'flex', gap: 6}} flow-children="horizontal">
      <Tile selected={mode === 'off'} disabled={busy || !running} big="—" label="Выкл" onClick={() => setMode('off')}/>
      <Tile selected={mode === 'auto'} disabled={busy || !running} big="A" label="Авто" onClick={() => setMode('auto')}/>
      <Tile selected={mode === 'cap'} disabled={busy || !running} big={String(base)} label="Свой FPS" onClick={() => setMode('cap')}/>
    </Focusable></PanelSectionRow>
    {mode !== 'auto' && <PanelSectionRow><SliderField label="Базовый FPS игры" value={base} min={20} max={90} step={1} showValue
      disabled={busy || !running}
      description="Движок ограничит игру до этого FPS, генерация доведёт до частоты экрана. 30 и 45 ровнее всего при 90 Гц."
      onChange={onBase}/></PanelSectionRow>}

    <PanelSectionRow><ToggleField label="HUD на экране" checked={hud} disabled={busy}
      description="Кольца FPS поверх игры: новые кадры на экране и FPS самой игры"
      onChange={v => void act(async () => {setHud((await rpc.setHud(v)).hud); return v ? 'HUD включён' : 'HUD выключен';})}/></PanelSectionRow>
    <PanelSectionRow><div style={{fontSize: 10, lineHeight: 1.4, color: C.label}}>
      Совет: для самого ровного результата включите лок FPS в настройках игры (30 или 45) и выберите тот же «Свой FPS».</div></PanelSectionRow>

    <PanelSectionRow><Focusable style={{display: 'flex', gap: 6}} flow-children="horizontal">
      <NavButton onClick={() => setView('pace')}>Темп</NavButton>
      <NavButton onClick={() => setView('setup')}>Установка</NavButton>
      <NavButton onClick={() => setView('log')}>Журнал</NavButton>
    </Focusable></PanelSectionRow>
    {message && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, overflowWrap: 'anywhere'}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 10, color: C.label, textAlign: 'center', letterSpacing: '0.08em'}}>GSF {VERSION} · KEDAVRA · экспериментально</div></PanelSectionRow>
  </PanelSection>;
}

export default definePlugin(() => ({
  name: 'KEDAVRA',
  titleView: <div className={staticClasses.Title} style={{display: 'flex', alignItems: 'center', gap: 8}}>
    <img src={LOGO} width={26} height={26} alt=""/>
    <span style={{fontWeight: 800, letterSpacing: '0.18em'}}>GSF<span style={{color: C.green}}>·</span>KEDAVRA</span>
  </div>,
  content: <div style={{background: C.ground}}><Content/></div>,
  icon: <img src={LOGO} width={20} height={20} alt=""/>,
  onDismount() {},
}));
