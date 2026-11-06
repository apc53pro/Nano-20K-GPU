#!/usr/bin/env python3
"""Verify board glue and the actual GPU at the fixed board clock ratios."""
import argparse,hashlib,importlib.util,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('board_check_hdl',ROOT/'scripts/rtl-check.py')
HDL=importlib.util.module_from_spec(spec);spec.loader.exec_module(HDL)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--build-dir',type=Path,default=ROOT/'build/tang20k');a=ap.parse_args()
 b=a.build_dir.resolve();b.mkdir(parents=True,exist_ok=True);report=b/'functional-checks.json'
 inputs=sorted((ROOT/'rtl').rglob('*.sv'))+sorted((ROOT/'rtl').glob('*.v'))+sorted((ROOT/'rtl/generated').glob('*.hex'))
 inputs+=sorted((ROOT/'reference/include').rglob('*.hpp'))
 inputs+=[ROOT/'reference/src'/n for n in ['setup.cpp','renderer.cpp','demo.cpp','hw_protocol.cpp']]
 inputs+=list((ROOT/'reference/rtl').glob('*.cpp'))+list((ROOT/'reference/rtl').glob('*.hpp'))
 inputs += [Path(__file__).resolve(),ROOT/'scripts/gpu-rtl.py',ROOT/'scripts/rtl-gpu-check.py',ROOT/'scripts/rtl-check.py']
 hashes={p.relative_to(ROOT).as_posix():digest(p) for p in inputs}
 if report.exists():
  old=json.loads(report.read_text())
  if old.get('source_sha256')==hashes and old.get('passed'):
   print('Source-matched Tang20K functional checks already passed.');return
 report.unlink(missing_ok=True)
 print('Testing board clock ratios, SDRAM output phase/registers, bus direction and address latch...',flush=True)
 sources=['rtl/board/tang20k_top.sv','rtl/gpu/nano_tmds.sv','rtl/tests/tang20k_core_stub.sv','rtl/tests/tang20k_board_tb.sv']
 HDL.run(HDL.tool('iverilog')+['-g2012','-s','tang20k_board_tb','-o',str(b/'board-tb.vvp')]+[str(ROOT/p) for p in sources],b/'board-tb-compile.log')
 output=HDL.run(HDL.tool('vvp')+[str(b/'board-tb.vvp')],b/'board-tb.log')
 if 'PASS board wrapper:' not in output:raise RuntimeError('Board wrapper checks incomplete')
 print(output.strip(),flush=True)
 # This executable contains the real GPU, external SDRAM model and SRR. The
 # small wrapper test above uses a stub only to isolate physical pad behavior.
 core_build=ROOT/'build/tang20k-core-sim';exe=ROOT/('build/nano-gpu-tang20k-sim.exe' if sys.platform=='win32' else 'build/nano-gpu-tang20k-sim')
 need_build=True
 if (core_build/'build-report.json').exists() and exe.exists():
  m=json.loads((core_build/'build-report.json').read_text())
  need_build=not (m.get('board_core_profile') and m.get('executable_sha256')==digest(exe) and
   all((ROOT/p).exists() and digest(ROOT/p)==h for p,h in m.get('source_sha256',{}).items()))
 if need_build:
  subprocess.run([sys.executable,str(ROOT/'scripts/gpu-rtl.py'),'--board-core','--headless-only','--build-dir',str(core_build),'--output',str(exe)],cwd=ROOT,check=True)
 tests=ROOT/'build/tang20k-core-tests'
 subprocess.run([sys.executable,str(ROOT/'scripts/rtl-gpu-check.py'),'--executable',str(exe),'--build-dir',str(tests)],cwd=ROOT,check=True)
 results=json.loads((tests/'results.json').read_text())
 if not results['simulation_passed'] or len(results['cases'])!=5:raise RuntimeError('Full GPU regression incomplete')
 changed=[p for p,h in hashes.items() if digest(ROOT/p)!=h]
 if changed:raise RuntimeError('Inputs changed during checks: '+str(changed))
 report.write_text(json.dumps({'schema':'N20K-BOARD-FUNCTIONAL-1','passed':True,'source_sha256':hashes,
  'board_glue_log_sha256':digest(b/'board-tb.log'),'board_glue_scope':'Clock/pad wrapper with isolated core test model',
  'core_build_report_sha256':digest(core_build/'build-report.json'),
  'core_scope':'Real GPU at 6.3MHz core, 3.15MHz virtual host/internal pixel rates, no C++ device backends; physical host limited separately to 10kHz',
  'gpu_regression':results,'hardware_tested':False},indent=2)+'\n',newline='\n')
 print('Board functional evidence: '+str(report))
if __name__=='__main__':
 try:main()
 except Exception as e:print('check-tang20k: '+str(e),file=sys.stderr);sys.exit(1)
