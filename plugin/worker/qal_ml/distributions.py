"""Descriptive per-class interval charts; never infer certification from labels."""
import html
import numpy as np

def class_distributions(x,y,names,limit_per_class=20000,seed=42):
    rng=np.random.default_rng(seed); summaries=[]
    for code in np.unique(y):
        indices=np.flatnonzero(y==code)
        sampled=indices if len(indices)<=limit_per_class else rng.choice(indices,limit_per_class,replace=False)
        for column,name in enumerate(names):
            values=x[sampled,column];finite=values[np.isfinite(values)]
            summaries.append({'feature':name,'class':int(code),'class_population':len(indices),
                'sampled':len(sampled),'valid':len(finite),'quantiles':np.quantile(finite,[.1,.25,.5,.75,.9]).tolist() if len(finite) else None})
    return {'scope':'Actual fit partition only, raw values before imputation; descriptive, not test accuracy',
        'sample_limit_per_class':limit_per_class,'seed':seed,'rows':summaries}

def distribution_svg(rows,title):
    valid=[r for r in rows if r.get('quantiles')]
    width=850;height=max(140,65+len(valid)*42)
    if not valid:
        return '<svg xmlns="http://www.w3.org/2000/svg" width="850" height="80"><text x="15" y="30">No finite observations</text></svg>'
    lo=min(r['quantiles'][0] for r in valid);hi=max(r['quantiles'][-1] for r in valid)
    if hi<=lo:hi=lo+1
    def px(v):return 210+600*(v-lo)/(hi-lo)
    parts=[f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title,quote=True)}">',
        '<rect width="100%" height="100%" fill="#f5f8fb"/>',f'<text x="15" y="24" font-family="sans-serif" font-size="14">{html.escape(title)}</text>']
    colors=['#5686c4','#439663','#ae7145','#885caa','#d9a237']
    for i,row in enumerate(valid):
        y=55+i*42;q=row['quantiles'];c=colors[i%len(colors)]
        parts.extend([f'<text x="15" y="{y+4}" font-family="sans-serif" font-size="12">Class {row["class"]}: n={row["valid"]}</text>',
            f'<line x1="{px(q[0]):.2f}" y1="{y}" x2="{px(q[4]):.2f}" y2="{y}" stroke="{c}" stroke-width="3"/>',
            f'<rect x="{px(q[1]):.2f}" y="{y-7}" width="{max(1,px(q[3])-px(q[1])):.2f}" height="14" fill="{c}"/>',
            f'<circle cx="{px(q[2]):.2f}" cy="{y}" r="4" fill="#172033"/>'])
    parts.append(f'<text x="210" y="{height-8}" font-family="sans-serif" font-size="11">{lo:.5g}</text><text x="810" y="{height-8}" text-anchor="end" font-family="sans-serif" font-size="11">{hi:.5g}</text></svg>')
    return ''.join(parts)
