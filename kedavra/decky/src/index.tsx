import {DialogButton, Focusable, PanelSection, PanelSectionRow, SliderField, ToggleField, staticClasses} from '@decky/ui';
import {callable, definePlugin, toaster} from '@decky/api';
import {CSSProperties, Fragment, ReactNode, useEffect, useRef, useState} from 'react';
import {LOGO} from './logo';

const VERSION = '1.1.0';

type Summary = {pass?: boolean; p95_ms?: number; p99_ms?: number; missed_pct?: number; rows?: number; checks?: Record<string, boolean>};
type Report = {trace: string; report: string; ok: boolean; log: string; summary?: Summary};
type Capture = {path: string; started: number; label: string; elapsed_s: number} | null;
type Live = {fps: number | null; real: number | null; pace: number | null; gen_share: number | null; unsupported?: number};
type Status = {
  engine: 'running' | 'stale' | 'absent'; fresh: boolean; message?: string; capture: Capture; live?: Live;
  multiplier?: number; state?: string; refresh_ns?: number; width?: number; height?: number;
  steam_ui?: boolean; sources?: number; real?: number; gen?: number; repeat?: number; skip?: number; resync?: number; degrade?: number;
  applied?: number; bypass?: number; ring_miss?: number; unsupported?: number;
  gpu_us_last?: number; gpu_us_avg?: number; gpu_us_max?: number; over_budget?: number; timestamps?: boolean; last_problem?: string;
  auto?: boolean; auto_switches?: number; source_late?: number; cap_fps?: number; base_fps?: number;
  unsupported_format?: number; unsupported_colorspace?: number;
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

function Back({onClick, title}: {onClick: () => void; title: string}) {
  return <PanelSectionRow><Focusable style={{display: 'flex', alignItems: 'center', gap: 10}}>
    <DialogButton onClick={onClick} style={{minWidth: 0, width: 40, height: 36, padding: 0, borderRadius: 10, background: C.card, border: `1px solid ${C.line}`}}>‹</DialogButton>
    <div style={{fontSize: 18, fontWeight: 800}}>{title}</div>
  </Focusable></PanelSectionRow>;
}


// Flat buttons: Steam's DialogButton brings its own grey and padding.
const flat: CSSProperties = {minWidth: 0, border: 'none', boxShadow: 'none'};
const primary: CSSProperties = {...flat, background: '#52d64a', color: '#04120a', fontWeight: 800, borderRadius: 12, padding: '11px 12px'};
const secondary: CSSProperties = {...flat, background: '#0d1210', color: '#f2f6f3', fontWeight: 700, borderRadius: 12, padding: '10px 12px', border: '1px solid #1c261f'};

function Segment({selected, onClick, disabled, label}: {selected: boolean; onClick: () => void; disabled?: boolean; label: string}) {
  return <DialogButton onClick={onClick} disabled={disabled} style={{...flat, flex: 1, padding: '8px 2px', borderRadius: 10, fontSize: 12, fontWeight: 700,
    background: selected ? C.green : 'transparent', color: selected ? '#04120a' : C.text}}>{label}</DialogButton>;
}

function Stat({label, value, ok}: {label: string; value: string; ok?: boolean}) {
  return <div style={{flex: 1, minWidth: 0, padding: '7px 8px', borderRadius: 10, background: C.ground}}>
    <div style={{...caps, fontSize: 8}}>{label}</div>
    <div style={{fontSize: 13, fontWeight: 800, marginTop: 2, color: ok ? C.text : C.yellow}}>{value}</div>
  </div>;
}

function ListButton({title, sub, onClick, warn}: {title: string; sub?: string; onClick: () => void; warn?: boolean}) {
  return <DialogButton onClick={onClick} style={{...flat, display: 'flex', alignItems: 'center', gap: 10, textAlign: 'left', padding: '10px 14px',
    borderRadius: 12, background: C.card, border: `1px solid ${C.line}`, color: C.text}}>
    <span style={{flex: 1, minWidth: 0}}>
      <span style={{display: 'block', fontSize: 13, fontWeight: 700}}>{title}</span>
      {sub && <span style={{display: 'block', fontSize: 10, color: warn ? C.red : C.label, marginTop: 2}}>{sub}</span>}
    </span>
    <span style={{color: C.label, fontSize: 16}}>›</span>
  </DialogButton>;
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
  const [showChecks, setShowChecks] = useState(false);
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
      const text = 'Error: ' + (e instanceof Error ? e.message : String(e));
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
    return m === 'off' ? 'Generation off' : m === 'auto' ? 'Auto: the engine holds the game at 30 or 45 and picks ×3 or ×2'
      : `Base FPS ${b}: the game is held at ${b}, generation fills up to ${Math.round(hz)}`;
  });
  const onBase = (v: number) => {
    setBase(v);
    // The slider fires on every step; apply once it settles.
    window.clearTimeout(baseTimer.current);
    if (mode === 'cap') baseTimer.current = window.setTimeout(() => setMode('cap', v), 500);
  };

  // One short line under the hero card; empty when the card already says it all.
  let note = '';
  if (!running) note = status?.message ?? 'Checking the engine…';
  else if ((live?.unsupported ?? 0) > 1) note = `This game's frames are not supported (Vulkan format ${status?.unsupported_format}, colour space ${status?.unsupported_colorspace}). Save a report from Setup and send it.`;
  else if (status?.steam_ui) note = 'Steam is on screen: generation waits for the game.';
  else if (status?.state === 'degraded') note = 'Paused: frame analysis cannot keep up. Retrying in 2 s.';
  else if (mode === 'cap' && live?.real !== null && live?.real !== undefined && live.real < 0.95 * (status?.base_fps ?? 0))
    note = `The game runs below its cap of ${status?.base_fps}. Lower the cap or use Auto for steadier motion.`;
  else if (mode === 'auto') note = mult === 3 ? 'Tries ×2 by itself when the game has headroom.' : 'Drops to ×3 by itself if the game falls behind.';

  const allOk = diag?.checks.every(c => c.ok);
  const failed = diag?.checks.filter(c => !c.ok) ?? [];
  const gap: CSSProperties = {height: 6};

  if (view === 'setup') return <PanelSection>
    <Back onClick={() => setView('main')} title="Setup"/>
    <PanelSectionRow><DialogButton onClick={() => setShowChecks(!showChecks)} style={{...card, ...flat, display: 'flex', alignItems: 'center', gap: 12, textAlign: 'left'}}>
      <span style={{width: 30, height: 30, borderRadius: 15, flex: 'none', display: 'flex', alignItems: 'center', justifyContent: 'center',
        background: allOk ? C.greenBg : C.redBg, color: allOk ? C.greenText : C.red, fontWeight: 900}}>{allOk ? '✓' : '!'}</span>
      <span style={{flex: 1, minWidth: 0}}>
        <span style={{display: 'block', fontSize: 14, fontWeight: 700}}>{!diag ? 'Checking the system…' : allOk ? 'System ready' : `${failed.length} check${failed.length > 1 ? 's' : ''} failed`}</span>
        <span style={{display: 'block', fontSize: 11, color: C.muted, marginTop: 2}}>{diag ? `${diag.checks.length - failed.length} of ${diag.checks.length} checks passed` : ''}</span>
      </span>
      <span style={{color: C.label, fontSize: 12}}>{showChecks ? 'Hide' : 'Details'}</span>
    </DialogButton></PanelSectionRow>
    {(showChecks || !allOk) && diag && <PanelSectionRow><div style={{...card, display: 'flex', flexDirection: 'column', gap: 7}}>
      {diag.checks.filter(c => showChecks || !c.ok).map((c, i) => <div key={c.key + i} style={{display: 'flex', gap: 8, fontSize: 11, lineHeight: 1.35, overflowWrap: 'anywhere'}}>
        <span style={{color: c.ok ? C.greenText : C.red, fontWeight: 800}}>{c.ok ? '✓' : '✗'}</span>
        <div style={{color: c.ok ? C.muted : C.text}}>{c.text}{c.detail && <div style={{fontSize: 10, color: C.muted, whiteSpace: 'pre-wrap'}}>{c.detail}</div>}</div>
      </div>)}
    </div></PanelSectionRow>}
    <div style={gap}/>
    <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      const r = await rpc.install(); setNeedsReboot(r.needs_reboot); await refreshDiag(); return r.message;
    })} style={primary}>{busy ? 'Working…' : installed ? 'Reinstall Gamescope KEDAVRA' : 'Install Gamescope KEDAVRA'}</DialogButton></PanelSectionRow>
    {needsReboot && <PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      await rpc.reboot(); return 'Rebooting…';
    })} style={secondary}>Reboot the Deck now</DialogButton></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 11, lineHeight: 1.45, color: C.label, padding: '0 2px'}}>
      Mounted over the stock Gamescope from /var/lib/kedavra; the SteamOS image is not changed. If Gaming Mode fails to start, the next boot uses the stock one.</div></PanelSectionRow>
    <div style={gap}/>
    <PanelSectionRow><div style={caps}>TOOLS</div></PanelSectionRow>
    <PanelSectionRow><Focusable style={{display: 'flex', flexDirection: 'column', gap: 6}}>
      <ListButton title="Save report to Desktop" sub="One zip: diagnostics, pacing traces, benchmark" onClick={() => void act(async () => 'Report saved: ' + (await rpc.report()).path.replace(/^.*\/Desktop\//, 'Desktop/'))}/>
      <ListButton title="GPU benchmark" sub={benchRes ? (benchRes.ok ? `Analysis ${benchRes.analysis_ms?.toFixed(1)} ms · two frames ${benchRes.two_outputs_ms?.toFixed(1)} ms · ${benchRes.correct ? 'PASS' : 'FAIL'}` : 'Failed') : 'About 20 seconds'}
        onClick={() => void act(async () => {
          const b = await rpc.bench(); setBenchRes(b);
          return b.ok ? `Benchmark: analysis ${b.analysis_ms?.toFixed(2)} ms, two frames ${b.two_outputs_ms?.toFixed(2)} ms` : 'Benchmark failed';
        })}/>
      <ListButton title="Check again" onClick={() => void act(async () => {await refreshDiag(); return 'Check updated';})}/>
    </Focusable></PanelSectionRow>
    {message && <PanelSectionRow><div style={{fontSize: 11, color: C.muted, overflowWrap: 'anywhere', whiteSpace: 'pre-wrap'}}>{message}</div></PanelSectionRow>}
    {installed && <><div style={gap}/><PanelSectionRow><DialogButton disabled={busy} onClick={() => void act(async () => {
      const r = await rpc.uninstall(); setNeedsReboot(r.needs_reboot); await refreshDiag(); return r.message;
    })} style={{...flat, background: 'transparent', color: C.red, fontSize: 12, padding: '8px 0'}}>Remove and restore stock Gamescope</DialogButton></PanelSectionRow></>}
  </PanelSection>;

  if (view === 'log') return <PanelSection>
    <Back onClick={() => setView('main')} title="Log"/>
    <PanelSectionRow><DialogButton disabled={busy} style={secondary} onClick={() => void act(async () => {setLogs(await rpc.logs()); return 'Log updated';})}>Refresh</DialogButton></PanelSectionRow>
    {logs && <PanelSectionRow><pre style={{...card, fontSize: 9, whiteSpace: 'pre-wrap', maxHeight: 360, overflow: 'auto', margin: 0}}>{logs}</pre></PanelSectionRow>}
  </PanelSection>;

  if (view === 'pace') return <PanelSection>
    <Back onClick={() => setView('main')} title="Pace"/>
    <PanelSectionRow><div style={{fontSize: 12, lineHeight: 1.5, color: C.muted, padding: '0 2px'}}>
      Records the on-screen time of every frame (KMS flip). Play for one or two minutes, then stop.</div></PanelSectionRow>
    <PanelSectionRow>{status?.capture ?
      <DialogButton disabled={busy} onClick={() => void act(async () => {
        const r = await rpc.stop(); setReport(r);
        const s = r.summary;
        return s ? `Trace: P95 ${s.p95_ms?.toFixed(3)} ms, P99 ${s.p99_ms?.toFixed(3)} ms, missed ${s.missed_pct?.toFixed(2)}% → ${s.pass ? 'PASS' : 'FAIL'}` : 'Trace saved, analysis failed';
      })} style={{...primary, background: C.text}}>■  Stop recording · {status.capture.elapsed_s.toFixed(0)} s</DialogButton> :
      <DialogButton disabled={busy || !running} onClick={() => void act(async () => {
        const label = mode === 'cap' ? `cap${status?.base_fps}` : `${status?.auto ? 'auto-' : ''}x${mult}`;
        const c = await rpc.start(label); return 'Recording KMS flip trace: ' + (c?.path ?? '');
      })} style={primary}>●  Record pacing trace</DialogButton>}
    </PanelSectionRow>
    {report?.summary && <PanelSectionRow><Focusable style={{display: 'flex', gap: 8}}>
      {[['P95', `${report.summary.p95_ms?.toFixed(2)} ms`, '≤ 1 ms', (report.summary.p95_ms ?? 9) <= 1],
        ['P99', `${report.summary.p99_ms?.toFixed(2)} ms`, '≤ 2 ms', (report.summary.p99_ms ?? 9) <= 2],
        ['MISSED', `${report.summary.missed_pct?.toFixed(2)}%`, '< 0.5%', (report.summary.missed_pct ?? 9) < 0.5]].map(([k, v, lim, ok]) =>
        <div key={String(k)} style={{...card, flex: 1, padding: 10, display: 'flex', flexDirection: 'column', gap: 4}}>
          <div style={{...caps, fontSize: 9}}>{k}</div><div style={{fontSize: 15, fontWeight: 800, color: ok ? C.greenText : C.red}}>{v}</div>
          <div style={{fontSize: 10, color: C.label}}>{lim}</div>
        </div>)}
    </Focusable></PanelSectionRow>}
    {report && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, overflowWrap: 'anywhere'}}>Saved: {report.report}</div></PanelSectionRow>}
    <div style={gap}/>
    {running && <PanelSectionRow><div style={{...card, display: 'grid', gridTemplateColumns: '1fr auto', rowGap: 6, columnGap: 10, fontSize: 11}}>
      {[['Game frames', status?.sources], ['Generated', status?.gen], ['Repeats', status?.repeat], ['Late game frames', status?.source_late],
        ['Mode switches', status?.auto_switches], ['GPU per frame', status?.timestamps ? `${((status?.gpu_us_avg ?? 0) / 1000).toFixed(2)} ms` : '—']].map(([k, v]) =>
        <Fragment key={String(k)}><span style={{color: C.label}}>{k}</span><span style={{textAlign: 'right', fontWeight: 700}}>{v ?? 0}</span></Fragment>)}
    </div></PanelSectionRow>}
  </PanelSection>;

  const fpsColor = !generating ? C.green : (live?.fps ?? 0) >= 0.97 * hz ? C.green : (live?.fps ?? 0) >= 0.85 * hz ? C.yellow : C.red;
  const mult_text = Math.abs(ratio - Math.round(ratio)) < 0.05 ? `×${Math.round(ratio)}` : `×${ratio.toFixed(1)}`;
  return <PanelSection>
    <PanelSectionRow><div style={{...card, padding: 16}}>
      <div style={{display: 'flex', alignItems: 'center', gap: 16}}>
        <Ring size={96} width={8} frac={(live?.fps ?? 0) / hz} color={fpsColor}>
          <div style={{fontSize: 32, fontWeight: 900, lineHeight: 1}}>{fmt(live?.fps)}</div>
          <div style={{...caps, marginTop: 3}}>FPS</div>
        </Ring>
        <div style={{display: 'flex', flexDirection: 'column', gap: 6, minWidth: 0}}>
          <div style={caps}>GAME</div>
          <div style={{fontSize: 26, fontWeight: 800, lineHeight: 1}}>{fmt(live?.real)}{generating && <span style={{color: C.green, fontSize: 20}}> {mult_text}</span>}</div>
          <div style={{fontSize: 11, color: C.muted}}>{mode === 'cap' ? `Custom · cap ${status?.base_fps}` : mode === 'auto' ? `Auto · cap ${status?.cap_fps ?? '—'}` : running ? 'Generation off' : 'Engine not running'}</div>
        </div>
      </div>
      {generating && <div style={{display: 'flex', gap: 8, marginTop: 14}}>
        <Stat label="PACE" value={pace === null || pace === undefined ? '—' : `${Math.min(99.9, 100 * pace).toFixed(1)}%`} ok={paceOk}/>
        <Stat label="GPU" value={status?.timestamps ? `${((status?.gpu_us_avg ?? 0) / 1000).toFixed(1)} ms` : '—'} ok/>
        <Stat label="DISPLAY" value={`${Math.round(hz)} Hz`} ok/>
      </div>}
    </div></PanelSectionRow>

    {note && <PanelSectionRow><div style={{fontSize: 11, lineHeight: 1.45, color: C.muted, padding: '0 2px'}}>{note}</div></PanelSectionRow>}
    <div style={gap}/>
    <PanelSectionRow><div style={caps}>MODE</div></PanelSectionRow>
    <PanelSectionRow><Focusable style={{display: 'flex', gap: 4, padding: 4, borderRadius: 14, background: C.card, border: `1px solid ${C.line}`}} flow-children="horizontal">
      <Segment selected={mode === 'off'} disabled={busy || !running} label="Off" onClick={() => setMode('off')}/>
      <Segment selected={mode === 'auto'} disabled={busy || !running} label="Auto" onClick={() => setMode('auto')}/>
      <Segment selected={mode === 'cap'} disabled={busy || !running} label={mode === 'cap' ? `Custom ${base}` : 'Custom'} onClick={() => setMode('cap')}/>
    </Focusable></PanelSectionRow>
    {mode === 'cap' && <PanelSectionRow><SliderField label="Base game FPS" value={base} min={20} max={90} step={1} showValue
      disabled={busy || !running} onChange={onBase}/></PanelSectionRow>}
    <PanelSectionRow><ToggleField label="In-game HUD" checked={hud} disabled={busy}
      onChange={v => void act(async () => {setHud((await rpc.setHud(v)).hud); return v ? 'HUD on' : 'HUD off';})}/></PanelSectionRow>
    <div style={gap}/>
    <PanelSectionRow><Focusable style={{display: 'flex', flexDirection: 'column', gap: 6}}>
      <ListButton title="Pace" sub="Record and check frame pacing" onClick={() => setView('pace')}/>
      <ListButton title="Setup" sub={!diag ? 'Checking…' : allOk ? 'System ready' : `${failed.length} check${failed.length > 1 ? 's' : ''} failed`} warn={diag && !allOk} onClick={() => setView('setup')}/>
      <ListButton title="Log" onClick={() => setView('log')}/>
    </Focusable></PanelSectionRow>
    {message && <PanelSectionRow><div style={{fontSize: 10, color: C.muted, overflowWrap: 'anywhere'}}>{message}</div></PanelSectionRow>}
    <PanelSectionRow><div style={{fontSize: 10, color: C.label, textAlign: 'center', letterSpacing: '0.08em', paddingTop: 4}}>GSF {VERSION} · KEDAVRA</div></PanelSectionRow>
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
