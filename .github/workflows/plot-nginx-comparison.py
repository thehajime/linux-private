#!/usr/bin/env python3
"""Compare nginx CPU scaling with median bars and observed ranges."""
import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from report_profiles import BENCHMARK_PROFILES


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: plot-nginx-comparison.py AGGREGATE_JSON OUTPUT_DIR')
    source, out = map(Path, sys.argv[1:]); out.mkdir(parents=True, exist_ok=True)
    groups = defaultdict(list)
    for row in json.loads(source.read_text()).get('matrix', []):
        if row.get('status') != 'PASS':
            continue
        profile = row['profile']
        cpu = row.get('host_cpu_count') if profile == 'native' else row.get('ncpus')
        if not isinstance(cpu, int) or cpu < 1:
            continue  # Do not invent a native CPU allocation.
        samples = row.get('requests_per_second_samples', [row.get('requests_per_second')])
        key = (profile, cpu, row['workers'], row['connections'])
        groups[key].extend(x for x in samples if isinstance(x, (int, float)) and math.isfinite(x) and x > 0)
    with (out / 'throughput-comparison.csv').open('w', newline='') as f:
        writer = csv.writer(f); writer.writerow(['profile', 'cpus', 'workers', 'connections', 'sample', 'requests_per_second'])
        for key, values in sorted(groups.items()):
            for i, value in enumerate(values, 1): writer.writerow([*key, i, value])
    generated = []
    for workers in (1, 4, 8, 16):
        profiles = [(p, label, color) for p, label, color in BENCHMARK_PROFILES
                    if any(k[0] == p and k[2] == workers and v for k, v in groups.items())]
        if not profiles: continue
        fig, axes = plt.subplots(1, 4, figsize=(19, 6.5), sharey=True)
        width = min(.18, .84 / len(profiles))
        cpus = sorted({k[1] for k, v in groups.items() if k[2] == workers and v})
        for ax, connections in zip(axes, (1, 4, 8, 16)):
            for index, (profile, label, color) in enumerate(profiles):
                offset = (index - (len(profiles) - 1) / 2) * width
                for cpu in cpus:
                    values = groups.get((profile, cpu, workers, connections), [])
                    if not values: continue
                    pos = cpu + offset
                    median = sorted(values)[len(values) // 2] if len(values) % 2 else sum(sorted(values)[len(values)//2-1:len(values)//2+1]) / 2
                    lower = median - min(values)
                    upper = max(values) - median
                    ax.bar(pos, median, width=width*.94, color=color, alpha=.72,
                           edgecolor=color, linewidth=.8,
                           yerr=[[lower], [upper]], capsize=2.5,
                           error_kw={'elinewidth': .8, 'ecolor': color})
                    ax.annotate(f'n={len(values)}', (pos, max(values)), xytext=(0, 5),
                                textcoords='offset points', ha='center', fontsize=7, rotation=90)
            ax.set_title(f'{connections} client connections'); ax.set_xticks(cpus)
            ax.set_xlim(min(cpus)-.55, max(cpus)+.55); ax.set_xlabel('Online CPUs (native: host CPUs)')
            ax.set_yscale('log'); ax.grid(axis='y', alpha=.25); ax.margins(y=.22)
        axes[0].set_ylabel('Requests / second (log scale)')
        fig.suptitle(f'nginx CPU scaling — {workers} worker(s)')
        handles = [Patch(facecolor=c, edgecolor=c, alpha=.72, label=l) for _, l, c in profiles]
        fig.legend(handles=handles, loc='lower center', ncol=4, bbox_to_anchor=(.5, .035), fontsize=9)
        fig.text(.5, .012, 'Bars: median requests/s; whiskers: observed min-max. Native uses a separate host.', ha='center', fontsize=9)
        fig.tight_layout(rect=(0, .17, 1, .94))
        image = out / f'nginx-workers-{workers}.png'; fig.savefig(image, dpi=150); plt.close(fig)
        generated.append(image.name); print(f'Generated {image}')
    (out / 'plot-status.txt').write_text('CPU x-axis; median bars with observed min-max whiskers; panels separate connection counts; figures separate worker counts. Missing configurations remain absent.\n' if generated else 'No finite measurements.\n')

if __name__ == '__main__': main()
