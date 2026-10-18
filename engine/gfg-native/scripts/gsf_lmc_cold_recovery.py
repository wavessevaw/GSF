#!/usr/bin/env python3
"""Native GSF GPU LMC cold-start recovery, NOT strict 0-skip 3x acceptance."""
import argparse
from datetime import datetime,timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[3]
FIELDS=("bridges real synthetic skipped te_busy ring_busy acquire_not_ready holds errors "
        "present_path_host_pixel_reads present_path_cpu_waits teardown_cpu_waits "
        "verify_checks verify_distinct verify_host_reads").split()
PATTERN=re.compile(
    r"GFG native: gpu stats bridges=(\d+).*?real=(\d+) synthetic=(\d+) skipped=(\d+)"
    r".*?te_busy=(\d+) ring_busy=(\d+) acquire_not_ready=(\d+) holds=(\d+) errors=(\d+)"
    r".*?present_path_host_pixel_reads=(\d+) present_path_cpu_waits=(\d+) teardown_cpu_waits=(\d+)"
    r".*?verify_checks=(\d+) verify_distinct=(\d+) verify_host_reads=(\d+)")
FRAMES=12

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--order",required=True,choices=["native-first","validation-first"])
    ap.add_argument("--out",default="gsf-lmc-cold-results")
    ap.add_argument("--timeout",type=int,default=180)
    args=ap.parse_args()
    out=Path(args.out)
    out.mkdir(parents=True,exist_ok=True)
    repo=ROOT/"engine/gfg-native"
    binary=repo/"build-cold/wsi-gpu-smoke"
    so=repo/"build-cold/libVkLayer_gfg_native.so"
    icds=sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
    icds=[x for x in icds if "x86_64" in x.name] or icds
    result={"scope":"ACTUAL_GPU_LMC_cold_recovery_NOT_zero_skips","order":args.order,
            "utc":datetime.now(timezone.utc).isoformat(),
            "source_frames":FRAMES,"source_hz":5,
            "physical_oled_scanout":"UNVERIFIED","strict_zero_skips":"NOT_TESTED",
            "status":"FAIL","checks":{}}
    try:
        if not binary.is_file() or not so.is_file() or not icds:
            raise RuntimeError("missing actual native GPU LMC binary, layer or lavapipe ICD")
        env=dict(os.environ)
        env.update({
            "VK_ICD_FILENAMES":str(icds[0]),
            "VK_LAYER_PATH":str(so.parent)+":/usr/share/vulkan/explicit_layer.d",
            "VK_INSTANCE_LAYERS":("VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation" if args.order=="native-first"
                                  else "VK_LAYER_KHRONOS_validation:VK_LAYER_GFG_native"),
            "GFG_NATIVE_FG":"1","GFG_NATIVE_GPU_BRIDGE":"1","GFG_NATIVE_ASYNC":"1",
            "GFG_NATIVE_GPU_SYNC":"same-device","GFG_NATIVE_TRACE":"1",
            "GFG_NATIVE_TIMING":"1","GFG_TEST_EXPECT_GPU":"1",
            "GFG_TEST_VALIDATION":"1","GFG_TEST_PROTON_CHAIN":"1",
            "GFG_NATIVE_ALGO":"lmc-gpu","GFG_TEST_FRAMES":str(FRAMES),
            "GFG_TEST_SOURCE_HZ":"5","GFG_NATIVE_GPU_BRIDGE_VERIFY":"1",
            "GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY":"2",
            "GFG_TEST_ALLOW_SKIPS":"1"
        })
        start=time.monotonic()
        cmd=["timeout",str(args.timeout)+"s","xvfb-run","-a","-s",
             "-screen 0 1024x768x24",str(binary)]
        try:
            proc=subprocess.run(cmd,cwd=ROOT,env=env,stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,text=True,errors="replace",
                                timeout=args.timeout+10)
            output=proc.stdout
            code=proc.returncode
        except subprocess.TimeoutExpired as e:
            output=e.stdout or ""
            if isinstance(output,bytes):output=output.decode(errors="replace")
            code=124
        (out/"gpu-lmc.log").write_text(output,encoding="utf-8")
        result["duration_ms"]=round((time.monotonic()-start)*1000)
        result["exit_code"]=code
        matches=PATTERN.findall(output)
        if matches:
            result["counters"]=dict(zip(FIELDS,map(int,matches[-1])))
            c=result["counters"]
            result["checks"].update({
                "one_bridge":c["bridges"]==1,
                "real_preservation":c["real"]==FRAMES,
                "slot_accounting":c["synthetic"]+c["skipped"]==2*(FRAMES-1),
                "some_synthetic_wsi":c["synthetic"]>=2,
                "positive_pixel_checks":c["verify_checks"]>=1,
                "verified_pixels_distinct":c["verify_distinct"]==c["verify_checks"],
                "no_bridge_errors":c["errors"]==0,
                "no_present_cpu_wait":c["present_path_cpu_waits"]==0,
                "no_present_cpu_read":c["present_path_host_pixel_reads"]==0
            })
        else:
            result["checks"]["gpu_counters_found"]=False
        result["checks"].update({
            "process_success":code==0,
            "validation_clean":not re.search(r"Validation Error|VUID-|UNASSIGNED-.*ERROR",output),
            "native_gpu_smoke_pass":"GPU bridge WSI smoke PASS" in output
        })
        result["status"]="PASS" if all(result["checks"].values()) else "FAIL"
        print("GSF_NATIVE_GPU_LMC_COLD",result["status"],result.get("counters"),flush=True)
        if result["status"]!="PASS":
            print("\n".join(output.splitlines()[-85:]),file=sys.stderr)
    except Exception as e:
        result["error"]=repr(e)
        print("GSF_COLD_LMC_SETUP_ERROR",repr(e),file=sys.stderr)
    finally:
        (out/"results.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
        summary=["### Real GPU LMC cold-start recovery / software Vulkan",
                 "",
                 "**"+result["status"]+"**. Source is deliberately slow at 5 Hz.",
                 "SKIPS MAY BE PRESENT: this does not pass or replace strict 0-skip CI draft PR #4.",
                 "No Deck RADV / OLED output is inferred.",
                 "",
                 "| Gate | Result |","|---|---|"]
        summary += ["| "+k+" | "+("PASS" if v else "FAIL")+" |"
                    for k,v in result["checks"].items()]
        summary += ["","GPU counters: "+str(result.get("counters","missing"))]
        if "error" in result:summary+=["Failure: "+result["error"]]
        msg="\n".join(summary)+"\n"
        (out/"SUMMARY.md").write_text(msg,encoding="utf-8")
        if os.getenv("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"],"a") as f:f.write(msg)
    return 0 if result["status"]=="PASS" else 1

if __name__=="__main__":
    sys.exit(main())
