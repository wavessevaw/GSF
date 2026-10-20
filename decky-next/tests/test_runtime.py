# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

PLUGIN=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PLUGIN/'py_modules'))
from next_runtime import Runtime,atomic,read,environment,validate,verify

class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.home=Path(self.temp.name)/'home';self.home.mkdir()
        self.plugin=Path(self.temp.name)/'plugin';self.plugin.mkdir()
        payload=self.plugin/'payload';payload.mkdir()
        (payload/'libgsf-next.so').write_bytes(b'test-only-library')
        atomic(payload/'gsf-next.json',{'layer':{'name':'VK_LAYER_GSF_next','library_path':'libgsf-next.so'}})
        atomic(payload/'BUILD_INFO.json',{'source_commit':'a'*40,'upstream_commit':'bc56e5bd0e3a6c328aedef664d0d36363fb58b36'})
        atomic(payload/'CHECKSUMS.json',{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in payload.iterdir()})
        shutil.copy2(PLUGIN/'launch.py',self.plugin/'launch.py')
        shutil.copytree(PLUGIN/'py_modules',self.plugin/'py_modules',ignore=shutil.ignore_patterns('__pycache__'))
        self.runtime=Runtime(self.home,self.plugin)
    def test_atomic_install_relocates_and_rechecks(self):
        self.runtime.install();kit=Path(read(self.runtime.root/'current.json')['kit']);verify(kit)
        self.assertEqual(read(kit/'gsf-next.json')['layer']['library_path'],str(kit/'libgsf-next.so'))
        (kit/'libgsf-next.so').write_bytes(b'corrupt')
        sums=read(kit/'CHECKSUMS.json');sums['libgsf-next.so']=hashlib.sha256(b'corrupt').hexdigest();atomic(kit/'CHECKSUMS.json',sums)
        self.runtime.install();self.assertEqual((kit/'libgsf-next.so').read_bytes(),b'test-only-library')
    def test_corrupt_package_never_installs(self):
        (self.plugin/'payload/libgsf-next.so').write_bytes(b'bad')
        with self.assertRaises(ValueError):self.runtime.install()
        self.assertFalse((self.runtime.root/'current.json').exists())
    def test_manual_activation_and_off(self):
        p=self.runtime.prepare(582160);self.assertTrue(p['enabled']);self.assertIn('--appid 582160 -- %command%',p['options'])
        self.runtime.configure(582160,{'multiplier':4,'max_in_flight':3});self.assertEqual(self.runtime.status(582160)['profile']['multiplier'],4)
        self.runtime.configure(582160,{'enabled':False});self.assertFalse(self.runtime.status(582160)['profile']['enabled'])
        self.assertFalse((self.home/'.local/share/gsf-test').exists())
    def test_environment_isolated_for_every_multiplier(self):
        inherited={'KEEP':'yes','GFG_NATIVE_FORCE_FIFO':'1','GSF_NEXT_TEST_FAIL_SUBMIT':'1','AFMF_DUMP_DIR':'private','VK_INSTANCE_LAYERS':'VK_LAYER_MANGOHUD_overlay'}
        for multiplier in (2,3,4):
            env=environment(Path('/kit'),Path('/run'),inherited,dict(multiplier=multiplier))
            self.assertEqual(env['GSF_NEXT_MULTIPLIER'],str(multiplier));self.assertEqual(env['KEEP'],'yes')
            self.assertEqual(env['AFMF_ACQUIRE_TIMEOUT_US'],'0');self.assertEqual(env['AFMF_ASYNC'],'1')
            self.assertNotIn('GFG_NATIVE_FORCE_FIFO',env);self.assertNotIn('AFMF_DUMP_DIR',env)
            self.assertNotIn('GSF_NEXT_TEST_FAIL_SUBMIT',env);self.assertEqual(env['DISABLE_MAKO'],'1')
        self.assertEqual(inherited['GFG_NATIVE_FORCE_FIFO'],'1')
    def test_other_generator_rejected(self):
        for layer in ('VK_LAYER_MAKO_render','VK_LAYER_LSFG','VK_LAYER_GFG_native','VK_LAYER_AFMF'):
            with self.assertRaises(ValueError):environment(Path('/kit'),Path('/run'),{'VK_INSTANCE_LAYERS':layer},{})
    def test_profile_validation(self):
        for patch in ({'multiplier':1},{'multiplier':True},{'refresh_hz':0},{'max_in_flight':999},{'quality':'unknown'}):
            with self.assertRaises(ValueError):validate(patch)
        with self.assertRaises(ValueError):self.runtime.configure(1,{'unknown':1})
    def invoke(self,enabled,corrupt=False):
        self.runtime.prepare(1);self.runtime.configure(1,{'enabled':enabled})
        if corrupt:
            kit=Path(read(self.runtime.root/'current.json')['kit']);(kit/'libgsf-next.so').write_bytes(b'bad')
        child='import json,os; print(json.dumps({"layer":os.environ.get("VK_INSTANCE_LAYERS"),"probe":os.environ.get("PROBE")}))'
        env=os.environ.copy();env.update(HOME=str(self.home),PROBE='retained');env.pop('VK_INSTANCE_LAYERS',None)
        return subprocess.run([sys.executable,str(self.runtime.root/'launch.py'),'--appid','1','--',sys.executable,'-c',child],env=env,capture_output=True,text=True,timeout=10)
    def test_armed_off_launcher_keeps_layer_for_live_enable(self):
        result=self.invoke(False);self.assertEqual(result.returncode,0)
        log=(self.runtime.latest(1)/'game.log').read_text()
        self.assertEqual(json.loads(log),dict(layer='VK_LAYER_GSF_next',probe='retained'))
    def test_uninstalled_launcher_strict_passthrough(self):
        self.runtime.prepare(1);self.runtime.disable_all()
        child='import os; print(os.environ.get("VK_INSTANCE_LAYERS","none"))'
        env=os.environ.copy();env.update(HOME=str(self.home));env.pop('VK_INSTANCE_LAYERS',None)
        result=subprocess.run([sys.executable,str(self.runtime.root/'launch.py'),'--appid','1','--',sys.executable,'-c',child],env=env,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.stdout.strip(),'none');self.assertEqual(result.returncode,0)
    def test_live_control_revision_and_fields(self):
        first=self.runtime.prepare(42);self.assertFalse(first['restart_required'])
        second=self.runtime.configure(42,dict(multiplier=4,quality='quality',extra_images=5))
        self.assertGreater(second['revision'],first['revision']);self.assertEqual(second['apply'],'swapchain')
        fields=(self.runtime.root/'controls/42').read_text().split()
        self.assertEqual(fields[0],'GSF_NEXT_CONTROL_V1')
        self.assertEqual(list(map(int,fields[1:])),[second['revision'],1,4,90,1,2,5,0,0])
    def test_corrupt_runtime_launches_game_once_without_layer(self):
        result=self.invoke(True,True);self.assertEqual(result.returncode,0)
        report=read(self.runtime.latest(1)/'REPORT.json');self.assertEqual(report['result'],'REAL_ONLY_PREFLIGHT_FAILURE')
        log=(self.runtime.latest(1)/'game.log').read_text();self.assertEqual(json.loads(log),dict(layer=None,probe='retained'))
    def test_live_rates_do_not_claim_displayed(self):
        self.runtime.prepare(1);run=self.runtime.results/'game-1-123';(run/'status').mkdir(parents=True)
        atomic(run/'REPORT.json',dict(stage='running'))
        import time
        t=time.monotonic_ns();base=dict(monotonic_ns=t-1_000_000_000,accepted_real=30,accepted_synthetic=60,generated=60,source_fps=30)
        atomic(run/'status/1.json',base);self.runtime.status(1)
        atomic(run/'status/1.json',{**base,'monotonic_ns':t,'accepted_real':60,'accepted_synthetic':120,'generated':120})
        s=self.runtime.status(1)['live'][0];self.assertEqual(s['output_fps'],90);self.assertEqual(s['generated_fps'],60);self.assertNotIn('displayed_fps',s)
        self.assertEqual(self.runtime.status(1)['live'][0]['output_fps'],90)

    def test_live_ack_and_retired_context_filter(self):
        import time
        p=self.runtime.prepare(1);run=self.runtime.results/'game-1-123';(run/'status').mkdir(parents=True)
        atomic(run/'REPORT.json',dict(stage='running'))
        row=dict(monotonic_ns=time.monotonic_ns(),accepted_real=30,accepted_synthetic=30,generated=30,source_fps=30,control_revision=p['revision']-1)
        atomic(run/'status/1.json',row)
        self.assertTrue(self.runtime.status(1)['live'][0]['settings_pending'])
        atomic(run/'status/1.json',{**row,'control_revision':p['revision']})
        self.assertFalse(self.runtime.status(1)['live'][0]['settings_pending'])
        atomic(run/'status/2.json',{**row,'retired':True,'accepted_synthetic':9999})
        self.assertEqual(len(self.runtime.status(1)['live']),1)
    def test_finished_report_keeps_real_wsi_evidence(self):
        self.runtime.prepare(1)
        child='import json,os,pathlib; p=pathlib.Path(os.environ["GSF_NEXT_STATUS_DIR"])/"1.json"; p.write_text(json.dumps(dict(generated=5,accepted_real=6,accepted_synthetic=5,retired=True)))'
        env=os.environ.copy();env.update(HOME=str(self.home));env.pop('VK_INSTANCE_LAYERS',None)
        result=subprocess.run([sys.executable,str(self.runtime.root/'launch.py'),'--appid','1','--',sys.executable,'-c',child],env=env,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0)
        report=read(self.runtime.latest(1)/'REPORT.json')
        self.assertEqual(report['result'],'WSI_ACCEPTED_SYNTHETIC_DISPLAY_UNVERIFIED')
        self.assertEqual(report['counters']['accepted_synthetic'],5)
        self.assertIsNone(report['displayed'])

if __name__=='__main__': unittest.main()
