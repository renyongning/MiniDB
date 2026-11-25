#!/usr/bin/env python3
"""Plot real candidate counts; requires matplotlib only, never reruns a benchmark."""
import argparse
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

parser = argparse.ArgumentParser()
parser.add_argument("results", type=Path)
parser.add_argument("output", type=Path, help="output basename without extension")
args = parser.parse_args()
rows = {}
for line in args.results.read_text().splitlines():
    if " integer_join " not in line:
        continue
    fields = line.split()
    values = dict(f.split("=", 1) for f in fields if "=" in f)
    rows[fields[0]] = int(values["candidates"])
methods = ["nested_loop", "sort_merge", "hash", "index"]
if any(m not in rows for m in methods):
    raise SystemExit("Missing integer_join result for one or more methods")
plt.rcParams.update({"font.family": ["Arial", "Helvetica", "DejaVu Sans"],
                     "font.size": 14, "axes.spines.top": False,
                     "axes.spines.right": False, "axes.linewidth": 1.5,
                     "svg.fonttype": "none", "pdf.fonttype": 42})
fig, ax = plt.subplots(figsize=(9, 4.8))
values = [rows[m] for m in methods]
bars = ax.barh(["Nested loop", "Sort-merge", "Hash join", "Index join"], values,
              color=["#CFCECE", "#E9A6A1", "#8BCF8B", "#3775BA"],
              edgecolor="#272727", linewidth=1.0, height=.58)
ax.invert_yaxis()
ax.set_xlim(0, max(values) * 1.27)
ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:,.0f}"))
ax.set_xlabel("Candidate row pairs examined")
ax.set_title("TPC-H supplier ⋈ nation", pad=30, fontsize=18)
ax.text(.5, 1.025, "10,000 suppliers · 25 nations · identical 10,000-row results",
        ha="center", transform=ax.transAxes, fontsize=11, color="#4D4D4D")
for bar, value in zip(bars, values):
    ax.text(value + max(values)*.02, bar.get_y()+bar.get_height()/2,
            f"{value:,}", va="center", fontsize=13)
fig.tight_layout(pad=1.5)
args.output.parent.mkdir(parents=True, exist_ok=True)
for extension in ("png", "svg", "pdf"):
    fig.savefig(args.output.with_suffix("."+extension), dpi=300, facecolor="white")
# Matplotlib includes trailing spaces in SVG path data; normalize for git.
svg = args.output.with_suffix(".svg")
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
plt.close(fig)
