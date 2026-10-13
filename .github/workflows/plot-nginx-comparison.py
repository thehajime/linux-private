#!/usr/bin/env python3
import json, math, subprocess, sys
from pathlib import Path
PROFILES=(("um-mmu","um [SMP=y]","#27ae60"),("um-mmu-seccomp","um-mmu(s) [SMP=y]","#145a32"),("um-nommu","um-nommu [SMP=n]","#d68910"),("um-nommu-seccomp","um-nommu(s) [SMP=n]","#a04000"),("um-nommu-sas-seccomp","um-nommu(sas-s) [SMP=n]","#0000ff"),("um-nommu-sas-zpoline","um-nommu(sas-z) [SMP=n]","#4169e1"))
WORKERS=(1,4); CONNECTIONS=(4,16,64)
def q(x): return "'"+str(x).replace("\\","\\\\").replace("'","\\'")+"'"
def finite(x): return isinstance(x,(int,float)) and math.isfinite(x)
def main():
 if len(sys.argv)!=3: raise SystemExit("usage: plot-nginx-comparison.py AGGREGATE_JSON OUTPUT_DIR")
 src,out=Path(sys.argv[1]),Path(sys.argv[2]); out.mkdir(parents=True,exist_ok=True)
 matrix=json.loads(src.read_text()).get("matrix",[])
 vals={(r.get("profile"),r.get("workers"),r.get("connections")):r.get("requests_per_second") for r in matrix if r.get("status")=="PASS"}
 sts={(r.get("profile"),r.get("workers"),r.get("connections")):r.get("status","MISSING") for r in matrix}
 n=len(PROFILES)*len(WORKERS); width=.84/n; series={}
 with (out/"throughput-comparison.csv").open("w") as f:
  f.write("profile,workers,connections,status,requests_per_second,plot_x\n")
  for pi,(p,_l,_c) in enumerate(PROFILES):
   for wi,w in enumerate(WORKERS):
    off=(pi*len(WORKERS)+wi-(n-1)/2)*width; rows=[]
    for ci,c in enumerate(CONNECTIONS,1):
     x=ci+off; v=vals.get((p,w,c)); status="PASS" if finite(v) else sts.get((p,w,c),"MISSING")
     f.write(f"{p},{w},{c},{status},{v if finite(v) else 'NaN'},{x:.4f}\n")
     if finite(v): rows.append((x,v))
    series[p,w]=rows
 items=[]
 for p,label,color in PROFILES:
  for w in WORKERS:
   rows=series[p,w]
   if not rows: continue
   dat=out/f"throughput-{p}-workers-{w}.dat"; dat.write_text("".join(f"{x:.4f} {v:.12g}\n" for x,v in rows))
   fill="fs solid 0.75" if w==1 else "fs pattern 2"; title=f"title {q(label)}" if w==1 else "notitle"
   items.append(f"{q(dat)} using 1:2 with boxes {fill} lc rgb {q(color)} lt 1 lw 0.4 {title}")
 status=out/"plot-status.txt"
 if not items: status.write_text("Plot omitted: no finite nginx measurements are available.\n"); return
 gp=out/"throughput-comparison.gnuplot"
 max_value=max(v for v in vals.values() if finite(v))
 y_upper=max(100.0,max_value*100.0)
 gp.write_text("\n".join(("set terminal pngcairo size 1200,720 enhanced font 'Helvetica,14' linewidth 3",f"set output {q(out/'nginx-comparison.png')}","set xlabel 'Client connections'","set ylabel 'Requests per second'","set xtics ('4' 1, '16' 2, '64' 3)","set xrange [0.45:3.55]","set grid ytics","set logscale y",f"set yrange [1:{y_upper:.8g}]",f"set boxwidth {width*.9:.5f} absolute","set size 1.0,1.0","set key top left Left reverse font ',12'","set style fill pattern 2","set tics out","plot "+", ".join(items),"")))
 subprocess.run(["gnuplot",str(gp)],check=True)
 status.write_text("Grouped throughput bars: client connections on x-axis; color is UML profile, solid fill is one worker and pattern fill is four workers. SMP=y means CONFIG_SMP=y; SMP=n means CONFIG_SMP=n. Missing measurements are omitted. The y-axis has additional headroom so the in-plot legend does not cover bars.\n")
 print(f"Generated {out/'nginx-comparison.png'}")
if __name__=="__main__": main()
