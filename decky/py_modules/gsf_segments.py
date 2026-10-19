# SPDX-License-Identifier: GPL-3.0-or-later
"""Read writer-owned JSONL segments and export immutable, hashed prefixes."""
from contextlib import contextmanager, ExitStack
import hashlib
from itertools import chain
import json
from pathlib import Path
import time


def telemetry_paths(base):
    base=Path(base)
    return sorted(p for p in ({base} | set(base.parent.glob(base.stem+'.*.jsonl')))
                  if p.is_file() and not p.is_symlink())


@contextmanager
def telemetry_stream(base):
    paths=telemetry_paths(base)
    if not paths:raise FileNotFoundError(base)
    with ExitStack() as stack:
        yield chain.from_iterable(stack.enter_context(p.open(encoding='utf-8',errors='replace')) for p in paths)


def telemetry_origin(base):
    origin=None
    try:
        with telemetry_stream(base) as stream:
            for line in stream:
                if '"pair"' not in line:continue
                try:e=json.loads(line)
                except ValueError:continue
                if not isinstance(e,dict) or e.get('event')!='pair':continue
                n=int(e.get('source1_ns',0) or e.get('worker_start_ns',0))
                if n and (origin is None or n<origin):origin=n
    except OSError:pass
    return origin


def export_session(archive,run):
    """Hash the SAME bytes written into the ZIP, even while a writer appends.

    Completed segments never change. Snapshot an active segment at its observed
    length, ending at a complete line; do not rename/truncate the producer file.
    """
    run=Path(run);started=time.monotonic_ns()
    def read_json(path):
        try:return json.loads(path.read_text())
        except (OSError,ValueError):return {}
    launch=read_json(run/'LAUNCH.json');report=read_json(run/'REPORT.json')
    trace_paths=set(telemetry_paths(run/'frames.jsonl'))
    manifest={'schema':1,'snapshot_started_ns':started,'source_appid':launch.get('appid'),
              'exact_commit':launch.get('build',{}).get('source_commit'),
              'mode':launch.get('native_environment',{}).get('GFG_NATIVE_MODE'),
              'source_hz_target':None,'physical_unique_displayed':None,
              'segments':[],'losses':[],'writers':[],
              'legacy_truncation_reported':bool(report.get('telemetry_truncated')),
              'control_totals':{'generated_wsi_max':report.get('max_generated_wsi_accepted'),
                                'last_live':report.get('last_live',[])}}
    source_intervals=[];output_periods=set();origins=set();observed_pairs=0
    for p in sorted(run.rglob('*')):
        if not p.is_file() or p.is_symlink() or p.name=='TELEMETRY_MANIFEST.json':continue
        rel=p.relative_to(run).as_posix();name=run.name+'/'+rel
        if p in trace_paths:
            with p.open('rb') as f:
                observed_size=p.stat().st_size
                data=f.read(observed_size)
            boundary=data.rfind(b'\n')+1;tail=len(data)-boundary;data=data[:boundary]
            archive.writestr(name,data)
            manifest['segments'].append({'path':rel,'bytes':len(data),
                'sha256':hashlib.sha256(data).hexdigest(),'observed_source_bytes':observed_size,
                'partial_tail_omitted_bytes':tail})
            if tail:manifest['losses'].append({'path':rel,'reason':'incomplete_tail_at_snapshot','bytes':tail})
            for line in data.splitlines():
                try:e=json.loads(line)
                except ValueError:continue
                if not isinstance(e,dict) or e.get('event')!='pair':continue
                observed_pairs+=1
                if e.get('interval_ns',0)>0:source_intervals.append(e['interval_ns'])
                if e.get('refresh_period_ns'):output_periods.add(e['refresh_period_ns'])
                if e.get('refresh_origin'):origins.add(e['refresh_origin'])
        else:
            archive.write(p,name)
        if p.name.startswith('frames.') and p.name.endswith('.state.json'):
            state=read_json(p);state['path']=rel;manifest['writers'].append(state)
            if state.get('stop_reason','none')!='none':
                manifest['losses'].append({'path':rel,'reason':state['stop_reason'],
                                          'dropped_bytes':state.get('dropped_bytes'),
                                          'partial_line_bytes':state.get('partial_line_bytes')})
    import statistics
    manifest.update(snapshot_finished_ns=time.monotonic_ns(),observed_pairs=observed_pairs,
        observed_source_hz_median=1e9/statistics.median(source_intervals) if source_intervals else None,
        observed_output_periods_ns=sorted(output_periods),observed_output_origins=sorted(origins),
        possible_unflushed_tail=any(not s.get('closed') for s in manifest['writers']),
        segments_created_during_snapshot=sorted(p.name for p in set(telemetry_paths(run/'frames.jsonl'))-trace_paths))
    archive.writestr(run.name+'/TELEMETRY_MANIFEST.json',json.dumps(manifest,indent=2)+'\n')
    return manifest
