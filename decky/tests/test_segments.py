# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'decky/py_modules'))
from gsf_segments import export_session,telemetry_paths,telemetry_stream
from gsf_diagnostic import diagnostic_summary


class SegmentTests(unittest.TestCase):
    def test_actual_c_multithread_writer_rotates_without_losing_history(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);exe=root/'writer';run=root/'session'
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                str(ROOT/'engine/gfg-native/tests/telemetry_segments.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe),str(run)],check=True,capture_output=True)
            events=[]
            paths=telemetry_paths(run/'frames.jsonl')
            self.assertGreater(len(paths),15)
            for p in paths:
                raw=p.read_bytes();self.assertTrue(raw.endswith(b'\n'))
                self.assertLessEqual(len(raw),512)
                events.extend(json.loads(line) for line in raw.splitlines())
            self.assertEqual(sum(e['event']=='test' for e in events),300)
            self.assertEqual(sum(e['event']=='final' for e in events),1)
            self.assertEqual(sum(e['event']=='legacy' for e in events),1)
            states=[json.loads(p.read_text()) for p in run.glob('*.state.json')]
            self.assertEqual(len(states),2)
            self.assertTrue(all(s['closed'] for s in states))
            cap=next(s for s in states if s['stop_reason']=='detail_budget_reached')
            self.assertGreater(cap['dropped_bytes'],0)
            self.assertLessEqual(cap['bytes_written'],512)
            archive=root/'export.zip'
            with zipfile.ZipFile(archive,'w') as z:manifest=export_session(z,run)
            with zipfile.ZipFile(archive) as z:
                for s in manifest['segments']:
                    data=z.read('session/'+s['path'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(),s['sha256'])
                    self.assertEqual(len(data),s['bytes'])
                self.assertEqual(json.loads(z.read('session/TELEMETRY_MANIFEST.json')),manifest)
            self.assertTrue(any(loss['reason']=='detail_budget_reached' for loss in manifest['losses']))
            self.assertFalse(manifest['possible_unflushed_tail'])

    def test_export_trims_only_snapshot_tail_and_keeps_writer_file(self):
        with tempfile.TemporaryDirectory() as temp:
            run=Path(temp)/'session';run.mkdir()
            p=run/'frames.123.123.000000.jsonl';data=b'{"event":"complete"}\n{"event":"partial'
            p.write_bytes(data)
            (run/'frames.123.123.state.json').write_text('{"closed":false,"stop_reason":"none"}')
            with zipfile.ZipFile(Path(temp)/'export.zip','w') as z:m=export_session(z,run)
            self.assertEqual(p.read_bytes(),data)
            self.assertEqual(m['segments'][0]['bytes'],data.index(b'\n')+1)
            self.assertGreater(m['losses'][0]['bytes'],0)
            self.assertTrue(m['possible_unflushed_tail'])

    def test_diagnostics_join_across_segments_and_use_earliest_process_origin(self):
        with tempfile.TemporaryDirectory() as temp:
            run=Path(temp);base=run/'frames.jsonl'
            def pair(pid,ns):return dict(event='pair',pid=pid,generation=1,source1_ns=ns,
                te_sequence=1,te_submit_sequence=1,dispatched_outputs=1,planned_mask=1,
                wsi_accepted_ns=[1,0,1],drop_detail=['WSI-accepted','unused'])
            later=pair(10,31_000_000_000);earlier=pair(20,1_000_000_000)
            (run/'frames.10.000000.jsonl').write_text(json.dumps(later)+'\n')
            (run/'frames.20.000000.jsonl').write_text(json.dumps(earlier)+'\n')
            gpu=dict(event='gpu_pair',pid=10,generation=1,te_sequence=1,timed=1,ready_ms=[7,0])
            (run/'frames.10.000001.jsonl').write_text(json.dumps(gpu)+'\n')
            report=diagnostic_summary(base)
            self.assertTrue(report['available'])
            self.assertEqual(report['accepted_outputs'],2)
            self.assertEqual(report['gpu_sample_joins']['gpu_pair']['matched'],1)
            self.assertEqual([w['start_s'] for w in report['windows_10s']],[0,30])
            with telemetry_stream(base) as stream:self.assertEqual(sum(1 for _ in stream),3)

    def test_recovery_distinguishes_no_retry_no_output_and_reentry(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'frames.jsonl'
            states=[('CONGESTED',0,0,0,'CONGESTED_NO_RETRY_OBSERVED'),
                    ('RECOVERY_PROBE',2,0,0,'RETRY_NO_ACCEPTED_GENERATION'),
                    ('CONGESTED',3,1,1,'RECOVERED_THEN_CONGESTED')]
            events=[dict(event='pacing_recovery',pid=i,generation=1,state=state,
                         probe_attempts=probes,stable_recoveries=stable,
                         congestion_reentries=reentries,ready_and_wsi_accepted=0)
                    for i,(state,probes,stable,reentries,_) in enumerate(states)]
            path.write_text(''.join(json.dumps(e)+'\n' for e in events))
            report=diagnostic_summary(path)['pacing_recovery']
            self.assertEqual([e['outcome'] for e in report],[s[-1] for s in states])
            self.assertTrue(all(e['physical_scanout'] is None for e in report))


if __name__=='__main__':unittest.main()
