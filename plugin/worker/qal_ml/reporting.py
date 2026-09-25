from __future__ import annotations

import csv
import html
import json
from pathlib import Path
from typing import Any


def _confusion_svg(metrics: dict[str, Any]) -> str:
    matrix = metrics["confusion_matrix"]
    labels = metrics["labels"]
    cell = 54
    margin = 92
    size = margin + cell * len(labels) + 20
    maximum = max((value for row in matrix for value in row), default=1) or 1
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {size} {size}">',
             '<rect width="100%" height="100%" fill="white"/>',
             '<style>text{font:12px sans-serif}.title{font:bold 15px sans-serif}</style>',
             f'<text class="title" x="{size/2}" y="20" text-anchor="middle">Confusion matrix</text>']
    for index, label in enumerate(labels):
        coordinate = margin + index * cell + cell / 2
        parts.append(f'<text x="{coordinate}" y="45" text-anchor="middle">P {label}</text>')
        parts.append(f'<text x="50" y="{margin + index * cell + cell/2 + 4}" text-anchor="middle">T {label}</text>')
    for row, values in enumerate(matrix):
        for column, value in enumerate(values):
            intensity = int(245 - 175 * value / maximum)
            fill = f'rgb({intensity},{intensity + 8},255)'
            x = margin + column * cell
            y = margin + row * cell
            parts.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" fill="{fill}" stroke="#94a3b8"/>')
            parts.append(f'<text x="{x + cell/2}" y="{y + cell/2 + 4}" text-anchor="middle">{value}</text>')
    parts.append('</svg>')
    return "".join(parts)


def _importance_svg(names: list[str], values: list[float]) -> str:
    if not values:
        return '<svg xmlns="http://www.w3.org/2000/svg" width="700" height="80"><text x="10" y="35" font-family="sans-serif">Feature importance not available for this classifier.</text></svg>'
    ranking = sorted(zip(names, values), key=lambda item: item[1], reverse=True)[:20]
    width = 900
    row_height = 28
    height = 45 + row_height * len(ranking)
    maximum = max(value for _, value in ranking) or 1.0
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
             '<rect width="100%" height="100%" fill="white"/><style>text{font:12px sans-serif}.title{font:bold 15px sans-serif}</style>',
             '<text class="title" x="450" y="20" text-anchor="middle">Feature importance</text>']
    for index, (name, value) in enumerate(ranking):
        y = 35 + index * row_height
        bar = 540 * value / maximum
        parts.append(f'<text x="5" y="{y + 16}">{html.escape(name[:55])}</text>')
        parts.append(f'<rect x="330" y="{y}" width="{bar}" height="20" fill="#2563eb"/>')
        parts.append(f'<text x="{335 + bar}" y="{y + 15}">{value:.5f}</text>')
    parts.append('</svg>')
    return "".join(parts)


def _learning_svg(history: list[dict[str, Any]]) -> str:
    values = [(int(row["epoch"]), float(row["training_loss"])) for row in history
              if "epoch" in row and "training_loss" in row]
    if not values:
        return ('<svg xmlns="http://www.w3.org/2000/svg" width="700" height="80">'
                '<text x="10" y="35" font-family="sans-serif">Learning epochs do not apply to this classifier.</text></svg>')
    width, height, left, top, right, bottom = 900, 360, 72, 38, 24, 54
    plot_width, plot_height = width-left-right, height-top-bottom
    minimum = min(value for _, value in values)
    maximum = max(value for _, value in values)
    span = maximum-minimum or 1.0
    denominator = max(len(values)-1, 1)
    points = " ".join(
        f"{left + index*plot_width/denominator:.1f},{top + (maximum-value)*plot_height/span:.1f}"
        for index, (_, value) in enumerate(values))
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
             '<rect width="100%" height="100%" fill="white"/>',
             '<style>text{font:12px sans-serif}.title{font:bold 15px sans-serif}</style>',
             f'<text class="title" x="{width/2}" y="20" text-anchor="middle">Training loss by epoch</text>',
             f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_height}" stroke="#64748b"/>',
             f'<line x1="{left}" y1="{top+plot_height}" x2="{left+plot_width}" y2="{top+plot_height}" stroke="#64748b"/>',
             f'<polyline points="{points}" fill="none" stroke="#2563eb" stroke-width="3"/>',
             f'<text x="8" y="{top+4}">{maximum:.5f}</text>',
             f'<text x="8" y="{top+plot_height}">{minimum:.5f}</text>',
             f'<text x="{left}" y="{height-20}">epoch {values[0][0]}</text>',
             f'<text x="{left+plot_width}" y="{height-20}" text-anchor="end">epoch {values[-1][0]}</text>',
             '</svg>']
    return "".join(parts)


def write_technical_report(report: dict[str, Any], output_json: Path) -> dict[str, str]:
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
    metrics = report["training"]["metrics"]
    if output_json.name == "report.json":
        prefix = ""
    elif output_json.name.endswith(".joblib.report.json"):
        prefix = output_json.name.removesuffix(".joblib.report.json") + "."
    else:
        prefix = output_json.name.removesuffix(".json") + "."
    confusion_path = output_json.parent / f"{prefix}confusion_matrix.svg"
    importance_path = output_json.parent / f"{prefix}feature_importance.svg"
    learning_path = output_json.parent / f"{prefix}learning_curve.svg"
    csv_path = output_json.parent / f"{prefix}metrics.csv"
    html_path = output_json.parent / f"{prefix}report.html"
    confusion_path.write_text(_confusion_svg(metrics), encoding="utf-8")
    importance_path.write_text(_importance_svg(
        report["feature_schema"]["names"], report["training"].get("feature_importances", [])
    ), encoding="utf-8")
    learning_path.write_text(_learning_svg(report["training"].get("learning_history", [])), encoding="utf-8")
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=["class", "name", "precision", "recall", "f1", "iou", "support"])
        writer.writeheader()
        writer.writerows(metrics["per_class"])
    summary_rows = "".join(
        f"<tr><td>{html.escape(str(row['class']))}</td><td>{html.escape(row['name'])}</td>"
        f"<td>{row['precision']:.4f}</td><td>{row['recall']:.4f}</td><td>{row['f1']:.4f}</td>"
        f"<td>{row['iou']:.4f}</td><td>{row['support']}</td></tr>" for row in metrics["per_class"]
    )
    cloud = report.get("provenance", {}).get("dataset_metadata", {})
    from .distributions import distribution_svg
    distribution = report.get('feature_distributions', {})
    figures=[]
    for index,name in enumerate(report['feature_schema']['names']):
        rows=[r for r in distribution.get('rows',[]) if r['feature']==name]
        if not rows: continue
        plot_path=output_json.parent/f'{prefix}distribution_{index:03d}.svg'
        plot_path.write_text(distribution_svg(rows,name),encoding='utf-8')
        figures.append(f'<details><summary>{html.escape(name)}</summary><img src="{html.escape(plot_path.name)}"></details>')
    distribution_section = ('<h2>Feature distributions by class</h2><p>Dot: median; rectangle: P25-P75; thin line: P10-P90. '
        'Actual fitting partition only, before imputation. Up to 20,000 reproducibly sampled points per class. '
        'Descriptive distributions are not decision thresholds or independent validation.</p>'+''.join(figures)) if figures else ''
    document = f"""<!doctype html><html><head><meta charset="utf-8"><title>ALiS model report</title>
<style>body{{font:14px system-ui;margin:30px;max-width:1100px;color:#172033}}table{{border-collapse:collapse}}th,td{{padding:6px 10px;border:1px solid #cbd5e1}}code,pre{{background:#f1f5f9;padding:3px}}img{{max-width:100%}}</style></head><body>
<h1>ALiS technical model report</h1>
<p><b>Classifier:</b> {html.escape(report['model_family'])} · <b>Device:</b> {html.escape(report['training']['configuration'].get('resolved_device','cpu'))}</p>
<p><b>Balanced accuracy:</b> {metrics['balanced_accuracy']:.4f} · <b>Accuracy:</b> {metrics['accuracy']:.4f} · <b>Cohen kappa:</b> {metrics.get('cohen_kappa')}</p>
<h2>Per-class spatial holdout metrics</h2><table><tr><th>ASPRS</th><th>Name</th><th>Precision</th><th>Recall</th><th>F1</th><th>IoU</th><th>Support</th></tr>{summary_rows}</table>
<h2>Confusion matrix</h2><img src="{html.escape(confusion_path.name)}"><h2>Feature importance</h2><img src="{html.escape(importance_path.name)}">
<h2>Learning curve</h2><img src="{html.escape(learning_path.name)}"><p>Shown only for iterative neural training. It is training loss, not independent validation loss.</p>
{distribution_section}
<h2>Cloud and provenance</h2><pre>{html.escape(json.dumps(cloud, ensure_ascii=False, indent=2, sort_keys=True))}</pre>
<h2>Configuration</h2><pre>{html.escape(json.dumps(report['training']['configuration'], ensure_ascii=False, indent=2, sort_keys=True))}</pre>
<h2>Validation strategy and exact membership</h2><pre>{html.escape(json.dumps(report['training']['split'], ensure_ascii=False, indent=2, sort_keys=True))}</pre>
<h2>Missing data and preprocessing</h2><pre>{html.escape(json.dumps(report['training'].get('preprocessing', {}), ensure_ascii=False, indent=2, sort_keys=True))}</pre>
<h2>Validation warnings</h2><pre>{html.escape(json.dumps(report.get('warnings', []), ensure_ascii=False, indent=2))}</pre>
<p>This report describes one spatial holdout run; it is not independent scientific validation.</p></body></html>"""
    html_path.write_text(document, encoding="utf-8")
    return {"json": str(output_json), "html": str(html_path), "csv": str(csv_path),
            "confusion_svg": str(confusion_path), "importance_svg": str(importance_path),
            "learning_svg": str(learning_path)}
