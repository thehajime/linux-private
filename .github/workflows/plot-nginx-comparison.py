#!/usr/bin/env python3
"""Compare nginx CPU scaling; boxes use repeated runs, never mixed conditions."""
import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from report_profiles import PROFILES


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
    for workers in (1, 4):
        profiles = [(p, label, color) for p, label, color in PROFILES
                    if any(k[0] == p and k[2] == workers and v for k, v in groups.items())]
        if not profiles: continue
        fig, axes = plt.subplots(1, 3, figsize=(15, 6), sharey=True)
        width = min(.12, .72 / len(profiles))
        cpus = sorted({k[1] for k, v in groups.items() if k[2] == workers and v})
        for ax, connections in zip(axes, (4, 16, 64)):
            for index, (profile, label, color) in enumerate(profiles):
                offset = (index - (len(profiles) - 1) / 2) * width
                for cpu in cpus:
                    values = groups.get((profile, cpu, workers, connections), [])
                    if not values: continue
                    pos = cpu + offset
                    if len(values) >= 2:
                        ax.boxplot([values], positions=[pos], widths=width*.8, patch_artist=True,
                                   manage_ticks=False, boxprops={'facecolor': color, 'alpha': .55},
                                   medianprops={'color': 'black'}, whis=1.5, showfliers=True)
                    else:
                        ax.plot(pos, values[0], marker='D', color=color, ms=5)
                    ax.annotate(f'n={len(values)}', (pos, max(values)), xytext=(0, 5),
                                textcoords='offset points', ha='center', fontsize=7, rotation=90)
            ax.set_title(f'{connections} client connections'); ax.set_xticks(cpus)
            ax.set_xlim(min(cpus)-.55, max(cpus)+.55); ax.set_xlabel('Online CPUs (native: host CPUs)')
            ax.set_yscale('log'); ax.grid(axis='y', alpha=.25); ax.margins(y=.22)
        axes[0].set_ylabel('Requests / second (log scale)')
        fig.suptitle(f'nginx CPU scaling — {workers} worker(s)')
        handles = [Line2D([], [], color=c, marker='s', linestyle='', label=l) for _, l, c in profiles]
        fig.legend(handles=handles, loc='lower center', ncol=4, bbox_to_anchor=(.5, .035), fontsize=9)
        fig.text(.5, .012, 'Boxes: repeated measurements of the same condition; diamond: n=1, no distribution estimate. Native uses a separate host.', ha='center', fontsize=9)
        fig.tight_layout(rect=(0, .17, 1, .94))
        image = out / f'nginx-workers-{workers}.png'; fig.savefig(image, dpi=150); plt.close(fig)
        generated.append(image.name); print(f'Generated {image}')
    (out / 'plot-status.txt').write_text('CPU x-axis; profile colors; panels separate connection counts; figures separate worker counts. Boxplots require repeated same-condition observations. Singleton observations are diamonds. Missing configurations remain absent.\n' if generated else 'No finite measurements.\n')

if __name__ == '__main__': main()
