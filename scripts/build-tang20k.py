#!/usr/bin/env python3
"""Build the actual Tang Nano20K target; emit no .fs on a failed fit/timing gate."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True
def module(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/file)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
HDL=module('board_hdl','rtl-check.py')
PACK=module('board_pack','rtl-core-pack.py')
PNR=module('board_pnr','rtl-core-pnr.py')
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def write(p,j): p.write_text(json.dumps(j,indent=2,allow_nan=False)+'\n',newline='\n')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-dir',type=Path,default=ROOT/'build/tang20k')
    ap.add_argument('--pack-only',action='store_true')
    ap.add_argument('--seed',type=int,default=1)
    args=ap.parse_args();b=args.build_dir.resolve();b.mkdir(parents=True,exist_ok=True)
    image=b/'nano-gpu.fs';image.unlink(missing_ok=True)
    rom=b/'seed_rom.v'
    HDL.run([sys.executable,str(ROOT/'scripts/rtl-generate-rom.py'),str(rom)],b/'rom.log')
    sources=[ROOT/'rtl/uv_mult.v',rom,ROOT/'rtl/uv_pipeline.v']+sorted((ROOT/'rtl/gpu').glob('*.sv'))+[ROOT/'rtl/board/tang20k_top.sv']
    inputs=[p for p in sources if p!=rom]+[ROOT/'rtl/board/tang20k.cst',ROOT/'rtl/board/tang20k-sip.json',Path(__file__).resolve()]
    inputs+=sorted((ROOT/'rtl/generated').glob('*.hex'))
    inputs += [ROOT/'scripts'/n for n in ('rtl-check.py','rtl-core-pack.py','rtl-core-pnr.py','rtl-generate-rom.py')]
    hashes={p.relative_to(ROOT).as_posix():digest(p) for p in inputs}
    report={'schema':'N20K-TANG20K-BOARD-1','device':'GW2AR-LV18QN88C8/I7',
        'top':'tang20k_top','source_sha256':hashes,'seed':args.seed,
        'command_player':False,'hardware_tested':False,'upload_ready':False,
        'generated_rom_sha256':digest(rom),
        'external_io_timing_measured':False,
        'timing_scope':'nextpnr internal-clock setup estimates; external SDRAM/parallel-bus margins require hardware measurement',
        'profile':'slow-core-bus-test',
        'display_scope':'Independent constant-black 640x480/60 raster; GPU-rendered video disconnected',
        'core_mhz':6.3,'pixel_mhz':25.2,'serial_mhz':126.0,'host_max_mhz':0.01,
        'synthesized':False,'packed':False,'placed':False,'routed':False,
        'timing_passed':False,'bitstream_generated':False}
    def save(): write(b/'board-report.json',report)
    save()
    extra={'PYTHONHOME':str(HDL.SUITE)} if HDL.SUITE.is_dir() else {}
    report['yosys_version']=HDL.run(HDL.tool('yosys')+['-V']).strip()
    report['nextpnr_version']=HDL.run(HDL.tool('nextpnr-himbaechel')+['--version'],extra_env=extra).strip()
    netlist=b/'synth.json'
    script='read_verilog -sv -D SYNTHESIS '+' '.join(json.dumps(str(p)) for p in sources)+'; synth_gowin -family gw2a -top tang20k_top -nowidelut -json '+json.dumps(str(netlist))+'; stat'
    print('Synthesizing full board: GPU, PLL/dividers, bus pads, SDRAM pads and TMDS...',flush=True)
    HDL.run(HDL.tool('yosys')+['-Q','-T','-p',script],b/'synth.log')
    design=json.loads(netlist.read_text())['modules']['tang20k_top']
    report.update(synthesized=True,synthesis_cells=dict(sorted(Counter(c['type'] for c in design['cells'].values()).items())),synth_sha256=digest(netlist));save()
    common=HDL.tool('nextpnr-himbaechel')+['--device',report['device'],'-o','family=GW2A-18C','-o','cst='+str(ROOT/'rtl/board/tang20k.cst'),
        '--json',str(netlist),'--threads','2','--seed',str(args.seed)]
    db=HDL.SUITE/'share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin'
    if db.exists(): common+=['--chipdb',str(db)]
    print('Packing full board on exact QN88 part...',flush=True)
    HDL.run(common+['--pack-only','--write',str(b/'packed.json')],b/'pack.log',extra)
    cells=json.loads((b/'packed.json').read_text())['modules']['top']['cells']
    report.update(packed=True,physical_lut_lower_bound=PACK.physical_lut_lower_bound(cells));save()
    print(json.dumps(report['physical_lut_lower_bound']),flush=True)
    if not report['physical_lut_lower_bound']['within_capacity']:raise RuntimeError('Full board exceeds physical LUT lower bound')
    if args.pack_only:return
    functional=b/'functional-checks.json'
    if not functional.exists():raise RuntimeError('Run scripts/check-tang20k.py before full board implementation')
    checks=json.loads(functional.read_text())
    if not checks.get('passed') or any(not (ROOT/n).exists() or digest(ROOT/n)!=h for n,h in checks.get('source_sha256',{}).items()):
        raise RuntimeError('Board functional evidence is missing or stale; run scripts/check-tang20k.py')
    report['functional_report_sha256']=digest(functional)
    # Resolve aliases through input buffers before handing exact net names to
    # nextpnr's embedded Python. PLL/divider outputs are direct scalar nets.
    clocks={'clk27':27.0,'gpu_hclk':0.01,'serial_clk':126.0,'core.clk':6.3,'pixel_clk':25.2}
    constraints=[]
    for hint,freq in clocks.items():
        bits=set(design['netnames'][hint]['bits'])
        for c in design['cells'].values():
            if c['type']=='IBUF' and bits.intersection(c['connections']['I']):bits=set(c['connections']['O'])
        aliases=[n for n,v in design['netnames'].items() if len(v['bits'])==1 and v['bits'][0] in bits]
        constraints.append((aliases,freq))
    clocks_py=b/'clocks.py'
    clocks_py.write_text('constraints='+repr(constraints)+'\nfor aliases,freq in constraints:\n    names=[n for n in aliases if n in ctx.nets]\n    assert names, str(aliases)\n    ctx.addClock(names[0],freq)\n',newline='\n')
    report['clock_constraints_sha256']=digest(clocks_py);save()
    print('Placing/routing the complete board target...',flush=True)
    command=common+['--pre-place',str(clocks_py),'--freq','6.3','--timing-allow-fail',
        '--write',str(b/'routed.json'),'--report',str(b/'timing.json'),'--detailed-timing-report']
    env=HDL.environment();env.update(extra)
    result=PNR.run_pnr(command,env,b/'pnr.log');report['route_returncode']=result.returncode;save()
    if result.returncode:raise RuntimeError('Board placement/routing failed; see '+str(b/'pnr.log'))
    routed=json.loads((b/'routed.json').read_text())['modules']['top']
    report.update(placed=PNR.setting(routed['settings'].get('place')),routed=PNR.setting(routed['settings'].get('route')),
        resources=PNR.physical_resources(routed['cells']),routed_sha256=digest(b/'routed.json'))
    # Fixed embedded SDRAM terminals must never be assigned to external GPIO.
    sip=json.loads((ROOT/'rtl/board/tang20k-sip.json').read_text())['ports']
    for port,expected in sip.items():
        stem=port.split('[')[0];index=int(port.split('[')[1][:-1]) if '[' in port else 0
        bit=routed['ports'][stem]['bits'][index]
        pin='IO' if port.startswith('IO_') else 'O'
        # Generated cell suffixes are enumeration IDs, not bus-bit numbers.
        matches=[c for c in routed['cells'].values() if bit in c.get('connections',{}).get(pin,[])]
        if len(matches)!=1 or matches[0].get('attributes',{}).get('NEXTPNR_BEL')!=expected:
            raise RuntimeError('Fixed SiP pin mismatch '+port+' expected '+expected)
    report['sip_pins_verified']=True
    timing=json.loads((b/'timing.json').read_text());fmax=timing.get('fmax',{})
    report['fmax']=fmax;report['critical_paths']=PNR.critical_paths(timing)
    if not fmax or any(not isinstance(v,dict) or any(type(v.get(k)) not in (int,float) or not math.isfinite(v[k]) or v[k]<=0 for k in ('achieved','constraint')) for v in fmax.values()):
        raise RuntimeError('Missing or invalid board clock timing results')
    required={}
    for hint in ('clk27','gpu_hclk','core.clk','pixel_clk'):
        index=list(clocks).index(hint);aliases,target=constraints[index]
        names=set(aliases).intersection(fmax)
        if len(names)!=1:raise RuntimeError('Missing or ambiguous board timing clock '+hint)
        name=names.pop();v=fmax[name]
        # nextpnr serializes constraints as float32 MHz. At 10 kHz its tiny
        # representation error exceeds the old fixed 1 ps period tolerance.
        # Retain that floor and allow 0.1 ppm for floating-point roundoff.
        period_ps=1e6/target
        if abs(1e6/v['constraint']-period_ps)>max(1.000001,period_ps*1e-7):
            raise RuntimeError('Incorrect board timing constraint '+hint)
        required[hint]={'reported_clock':name,'requested_mhz':target,**v}
    report['required_clock_results']=required
    # This fixed test image has no GPU-core-to-pixel payload crossing. Require
    # its absence rather than reusing the old 50.4 MHz capture-window proof.
    pixel_paths=[p for p in timing.get('critical_paths',[]) if
        p.get('from')=='posedge '+required['core.clk']['reported_clock'] and
        p.get('to')=='posedge '+required['pixel_clk']['reported_clock']]
    if pixel_paths:raise RuntimeError('Unexpected GPU-core-to-pixel path in black-raster test image')
    report['pixel_capture']={'applicable':False,
        'reason':'GPU-rendered payload disconnected; black raster is entirely pixel-clock synchronous',
        'unexpected_core_to_pixel_paths':len(pixel_paths)}
    report['timing_passed']=all(v['achieved']>=v['constraint'] for v in fmax.values()) and all(v['achieved']>=v['requested_mhz'] for v in required.values())
    report['inputs_changed']=[n for n,h in hashes.items() if digest(ROOT/n)!=h];save()
    if report['inputs_changed']:raise RuntimeError('Sources changed during implementation')
    if not report['routed'] or not all(v['within_capacity'] for v in report['resources'].values()):raise RuntimeError('Incomplete route or resource overflow')
    if not report['timing_passed']:raise RuntimeError('Board timing targets not met; no upload image emitted')
    print('Generating board programming image...',flush=True)
    HDL.run(HDL.tool('gowin_pack')+['-d','GW2A-18C','-c','-o',str(image),str(b/'routed.json')],b/'bitstream.log',extra)
    if image.stat().st_size<1000:raise RuntimeError('Invalid short bitstream')
    report.update(bitstream_generated=True,upload_ready=True,bitstream_sha256=digest(image),bitstream_bytes=image.stat().st_size);save()
    print('Upload image: '+str(image),flush=True)

if __name__=='__main__':
    try:main()
    except Exception as e:
        print('build-tang20k: '+str(e),file=sys.stderr);sys.exit(1)
