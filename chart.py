# Copyright (c) 2026 Peter Fors
# SPDX-License-Identifier: MIT

import argparse
import csv
from itertools import groupby
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch
from matplotlib.ticker import MaxNLocator


INK = "#172434"
MUTED = "#4e5c6b"
SERIES = {"ffutf": "#00776d", "simdutf": "#6e798d"}
CACHE = {"L1D": "#cee9db", "L2": "#d5e5fa", "L3": "#eadcf4", "DRAM": "#f6dfbd"}
OPERATION = {
    "validate_utf8": "Validate UTF-8",
    "utf8_to_utf16le": "UTF-8 → UTF-16LE",
    "utf16le_to_utf8": "UTF-16LE → UTF-8",
    "utf8_to_utf16le_with_errors": "UTF-8 → UTF-16LE · error-reporting API",
}
WORKLOADS = {"ASCII": "ASCII", "Latin": "Latin", "CJK": "CJK", "emoji": "Emoji", "mixed": "Mixed"}
CPUS = {"zen4": ("Ryzen 9 7950X", 32), "zen5": ("Ryzen 9 9950X3D", 48)}
ISAS = {"avx2": "AVX2", "avx512": "AVX-512"}
SIZES = (16, 64, 192, 512, 4096, 32768, 262144, 1048576, 4194304, 16777216, 33554432, 67108864)


def size_label(size):
    if size >= 1048576:
        return f"{size // 1048576} MiB"
    if size >= 1024:
        return f"{size // 1024} KiB"
    return f"{size} B"


def load(path):
    data = {}
    trial_counts = set()
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            trial_counts.add(int(row["trials"]))
            if row["placement"] == "aligned" and row["error_at"] == "-1":
                key = (row["cpu"], row["isa"], row["operation"], row["workload"],
                       int(row["input_bytes"]), row["implementation"])
                data[key] = (float(row["input_gb_s"]), int(row["output_bytes"]))
    if len(trial_counts) != 1:
        raise ValueError("expected one trial count for the chart data")
    return data, trial_counts.pop()


def cache_band(footprint, l1d):
    return next((name for name, cap in (("L1D", l1d << 10), ("L2", 1 << 20), ("L3", 32 << 20))
                 if footprint <= cap), "DRAM")


def frame(cpu, headline, boost, shared_scales, trials):
    model, l1d = CPUS[cpu]
    title = "ffutf vs simdutf · 4 KiB input" if headline else "ffutf vs simdutf · mixed text by input size"
    fig, axes = plt.subplots(4, 2, figsize=(13.2, 10.4), facecolor="white")
    fig.subplots_adjust(left=.071, right=.982, top=.826, bottom=.085, hspace=.61, wspace=.18)
    fig.text(.075, .955, title, fontsize=26, weight="bold", color=INK)
    subtitle = f"{model}  ·  Zen {cpu[-1]}  ·  Boost {boost}  ·  Aligned buffers  ·  Valid input"
    if not headline:
        subtitle += f"  ·  {l1d} KiB L1D / 1 MiB L2 / 32 MiB L3"
    fig.text(.076, .917, subtitle, fontsize=12, color=MUTED)
    legend = [Patch(facecolor=color, label=label)
              for color, label in zip(SERIES.values(), ("ffutf", "simdutf 9.5.0"))]
    fig.legend(handles=legend, loc="upper left", bbox_to_anchor=(.071, .897), ncol=2,
               frameon=False, labelcolor=INK, fontsize=11, handlelength=1.35, handleheight=.75)
    fig.text(.30, .853, "AVX2", ha="center", fontsize=13, weight="bold", color=INK)
    fig.text(.755, .853, "AVX-512", ha="center", fontsize=13, weight="bold", color=INK)
    scale_note = "Per-operation scales shared across both CPUs" if shared_scales else "Per-operation scales shared by ISA"
    footer = (f"Median of {trials} interleaved batches  ·  ffutf Clang 23.1.5 -O2  ·  simdutf GCC 16.2.1 -O3"
              f"  ·  {scale_note}")
    fig.text(.076, .045, footer, fontsize=8.5, color=MUTED)
    if not headline:
        fig.text(.076, .024, "Bands show nominal capacity for input plus produced output.",
                 fontsize=8.5, color=MUTED)
    fig.text(.021, .46, "Input GB/s", rotation="vertical", va="center", ha="center",
             fontsize=11, color=INK)
    return fig, axes


def draw(data, cpus, cpu, headline, out, boost, trials):
    fig, axes = frame(cpu, headline, boost, len(cpus) > 1, trials)
    tests = [(text, 4096) for text in WORKLOADS] if headline else [("mixed", n) for n in SIZES]
    ticks = np.arange(len(tests))
    labels = [WORKLOADS[w] for w, _ in tests] if headline else [size_label(n) for _, n in tests]
    width = .35 if headline else .36

    for r, (operation, title) in enumerate(OPERATION.items()):
        vmax = max(data[c, backend, operation, w, n, method][0]
                   for c in cpus for backend in ISAS for w, n in tests for method in SERIES)
        for col, backend in enumerate(ISAS):
            axis = axes[r, col]
            axis.set_title(title, loc="left", fontsize=11, weight="bold", pad=8, color=INK)
            axis.set_ylim(0, vmax * (1.19 if headline else 1.26))
            axis.set_xlim(-.56, len(tests) - .44)

            if not headline:
                bands = [
                    cache_band(n + data[cpu, backend, operation, w, n, "ffutf"][1], CPUS[cpu][1])
                    for w, n in tests
                ]
                for name, entries in groupby(enumerate(bands), key=lambda record: record[1]):
                    indices = [i for i, _ in entries]
                    low, high = indices[0] - .5, indices[-1] + .5
                    axis.axvspan(low, high, color=CACHE[name], zorder=0)
                    if low >= 0:
                        axis.axvline(low, color="#68778a", linewidth=.65, ls="--", zorder=1)
                    axis.text((low + high) / 2, .943, name, ha="center", va="top",
                              transform=axis.get_xaxis_transform(), fontsize=7.4, color=INK)

            for k, (method, color) in enumerate(SERIES.items()):
                values = [data[cpu, backend, operation, w, n, method][0] for w, n in tests]
                bars = axis.bar(ticks + (k - .5) * width, values, width=width, zorder=3, color=color)
                if headline:
                    for bar, value in zip(bars, values):
                        axis.text(bar.get_x() + width / 2, value + vmax * .024,
                                  f"{value:.1f}", ha="center", va="bottom", color=INK, fontsize=7.4)
            axis.set_xticks(ticks, labels, fontsize=9.1 if headline else 7.1)
            axis.tick_params(axis="x", length=0, colors=INK, pad=5)
            axis.tick_params(axis="y", length=0, colors=MUTED, labelsize=8.3)
            axis.yaxis.set_major_locator(MaxNLocator(nbins=4))
            axis.grid(axis="y", color="#a7b1be", alpha=.45, linewidth=.55, zorder=2)
            for spine in ("top", "right", "left"):
                axis.spines[spine].set_visible(False)
            axis.spines["bottom"].set_color("#9faab7")

    name = f"{cpu}-4k.png" if headline else f"{cpu}-size-sweep.png"
    fig.savefig(out / name, dpi=160, facecolor="white")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description="Draw matching ffutf comparison charts.")
    parser.add_argument("results", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--boost", choices=("enabled", "disabled"), default="disabled")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    matplotlib.rcParams["font.family"] = "DejaVu Sans"
    data, trials = load(args.results)
    cpus = [cpu for cpu in CPUS if any(k[0] == cpu for k in data)]

    for cpu in cpus:
        for headline in (True, False):
            draw(data, cpus, cpu, headline, args.output, args.boost, trials)


if __name__ == "__main__":
    main()
