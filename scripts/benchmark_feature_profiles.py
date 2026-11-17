"""Bounded native speed-profile and optional CPU/CUDA concurrency experiment.

Uses a preserved real support fixture, never a production manifest/checkpoint.
Reduced profiles are exact column subsets at retained radii, not accuracy claims.
"""
import argparse, concurrent.futures, json, os, shutil, statistics, subprocess, time
from pathlib import Path
import numpy as np

FAST = 'neighbor_count linearity planarity surface_variation verticality roughness z_range z_standard_deviation'.split()
BALANCED = 'neighbor_count linearity planarity sphericity eigenentropy surface_variation verticality roughness z_range z_standard_deviation z_above_minimum z_below_maximum'.split()

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--context',type=Path,required=True);ap.add_argument('--schema',type=Path,required=True);ap.add_argument('--worker',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--gpu',action='store_true');args=ap.parse_args()
    root=args.output;root.mkdir(parents=True,exist_ok=True)
    source=np.fromfile(args.context,dtype='<f8').reshape(-1,3);base=json.loads(args.schema.read_text());original=base['features'];scales=sorted({float(k.rsplit('@',1)[1]) for k in original})
    keys={'complete':original,'balanced':[f'{n}@{r:g}' for n in BALANCED for r in scales[:3]],'fast':[f'{n}@{r:g}' for n in FAST for r in scales[:2]]}
    report={'scope':'Real full-resolution spatial fixtures with complete retained-radius support; no production outputs modified','profiles':keys,'runs':[],'quality':'Feature equivalence only, no classification accuracy certification'}
    env=dict(os.environ);env['PATH']=str(args.worker.parent)+';'+env.get('PATH','');env['QT_QPA_PLATFORM_PLUGIN_PATH']=str(args.worker.parent/'platforms');env['QT_QPA_PLATFORM']='minimal'
    def save(): (root/'results.json').write_text(json.dumps(report,indent=2))
    def prepare(label,xyz,ids,fields,threads=24):
        folder=root/label;folder.mkdir(exist_ok=True);xyz.astype('<f8').tofile(folder/'xyz.f64');ids.astype('<u4').tofile(folder/'query_indices.u32');origin=xyz[ids[0]].tolist()
        cfg={'directory':str(folder),'point_count':len(xyz),'origin':origin,'features':fields,'threads':threads,'memory_bytes':4*1024**3,'cache_bytes':len(xyz)*len(fields)*4+1048576,'query_indices_file':'query_indices.u32'}
        (folder/'request.json').write_text(json.dumps(cfg));return folder,cfg
    def execute(prepared):
        folder,cfg=prepared;start=time.perf_counter();p=subprocess.run([str(args.worker),str(folder/'request.json')],env=env,capture_output=True,text=True,timeout=120);elapsed=time.perf_counter()-start;(folder/'worker.log').write_text(p.stdout+p.stderr)
        if p.returncode: raise RuntimeError(p.stdout+p.stderr)
        ids=np.fromfile(folder/'query_indices.u32',dtype='<u4');v=np.fromfile(folder/'features.f32',dtype='<f4').reshape(len_xyz:=cfg['point_count'],len(cfg['features']))[ids]
        return elapsed,v
    def compare(a,b,fields):
        finite=np.isfinite(a)&np.isfinite(b);diff=np.zeros(a.shape,np.float64);np.subtract(a,b,out=diff,where=finite);diff=np.abs(diff)
        bad=(np.isnan(a)!=np.isnan(b))|np.isinf(b)|(finite&(diff>1e-3*np.maximum(1,np.abs(a))))
        return {'passed':not bool(bad.any()),'failed_values':int(bad.sum()),'values_checked':int(a.size),'max_absolute_error':float(diff.max()),'counts_exact':all(np.array_equal(a[:,j],b[:,j],equal_nan=True) for j,k in enumerate(fields) if k.startswith('neighbor_count@'))}
    lo=source[:,:2].min(0);hi=source[:,:2].max(0);radius=max(scales);fixtures=[]
    for region,fraction in enumerate((.3,.5,.7)):
        center=lo+(hi-lo)*fraction;nearest=np.argmin(((source[:,:2]-center)**2).sum(1));center=source[nearest,:2]
        # Reject rather than silently clip a complete halo at the source edge.
        if np.any(center-2-radius<lo) or np.any(center+2+radius>hi):continue
        mask=(np.abs(source[:,:2]-center)<=2+radius).all(1);xyz=source[mask];own=np.flatnonzero((np.abs(xyz[:,:2]-center)<=2).all(1));ids=own[np.linspace(0,len(own)-1,min(2048,len(own)),dtype=int)]
        if not len(ids) or len(xyz)>250000:continue
        fixture={'region':region,'xyz':xyz,'ids':ids};fixtures.append(fixture);reference=None
        for repeat in range(3):
            for profile in (('complete','balanced','fast') if repeat%2==0 else ('fast','balanced','complete')):
                fields=keys[profile];prepared=prepare(f'r{region}_{profile}_{repeat}',xyz,ids,fields);elapsed,values=execute(prepared)
                if profile=='complete':reference=values
                if reference is None:raise RuntimeError('Missing full-schema reference')
                selected=reference[:,[original.index(k) for k in fields]];parity=compare(selected,values,fields)
                row={'region':region,'profile':profile,'repeat':repeat,'seconds':elapsed,'support_points':len(xyz),'queries':len(ids),'fields':len(fields),'comparison':parity};report['runs'].append(row);save();print(json.dumps(row),flush=True)
                if not parity['passed']:raise RuntimeError('Reduced-profile native equivalence failed')
    if not fixtures:raise RuntimeError('No interior fixture with complete halo')
    report['median_seconds']={p:statistics.median(r['seconds'] for r in report['runs'] if r['profile']==p) for p in keys}
    report['chosen_profile']=min(report['median_seconds'],key=report['median_seconds'].get);save()
    fixture=fixtures[0];xyz,ids=fixture['xyz'],fixture['ids'];fields=keys['fast'];reference=execute(prepare('fast_reference',xyz,ids,fields))[1]
    concurrency=[]
    for repeat in range(3):
        single=execute(prepare(f'cpu24_{repeat}',xyz,ids,fields))[0]
        cpu30=execute(prepare(f'cpu30_{repeat}',xyz,ids,fields,30))[0]
        jobs=[prepare(f'cpu12_{repeat}_{half}',xyz,part,fields,12) for half,part in enumerate(np.array_split(ids,2))]
        start=time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(2) as pool: outputs=list(pool.map(execute,jobs))
        both=time.perf_counter()-start;parity=compare(reference,np.concatenate([v for _,v in outputs]),fields)
        concurrency.append({'cpu24_seconds':single,'cpu30_seconds':cpu30,'two_cpu12_seconds':both,'comparison':parity})
        print(json.dumps(concurrency[-1]),flush=True)
    report['cpu_concurrency']=concurrency;report['cpu_concurrency_note']='Two bounded microbenchmark workers only; not proof of whole-cloud RAM or disk safety.';save()
    if args.gpu:
        try:
            import torch
            torch.set_num_threads(1)
            if not torch.cuda.is_available():raise RuntimeError('CUDA is unavailable')
            status=subprocess.run(['nvidia-smi','--query-gpu=memory.free','--format=csv,noheader,nounits'],capture_output=True,text=True)
            if status.returncode or float(status.stdout.strip().splitlines()[0])<2048:raise RuntimeError('Less than 2 GiB verified free VRAM')
            start=time.perf_counter();torch.cuda.init();torch.cuda.synchronize();cold=time.perf_counter()-start
            origin=xyz[ids[0]];start=time.perf_counter();points=torch.as_tensor((xyz-origin).astype(np.float32),device='cuda');torch.cuda.synchronize();upload=time.perf_counter()-start
            source_ids=torch.arange(len(xyz),device='cuda');radii=scales[:2];eps=np.finfo(np.float64).eps
            def gpu_run(query_ids):
                output=[]
                for offset in range(0,len(query_ids),16):
                    q=torch.as_tensor(query_ids[offset:offset+16].astype(np.int64),device='cuda');delta32=points[None,:,:]-points[q,None,:];distance=delta32.double().square().sum(2);delta=points.double()[None,:,:]-points[q].double()[:,None,:];notself=source_ids[None,:]!=q[:,None];by_radius={}
                    for r in radii:
                        mask=(distance<=r*r)&notself;count=mask.sum(1);safe=count.double().clamp_min(1);w=mask.double();mean=(delta*w[:,:,None]).sum(1)/safe[:,None];centered=delta-mean[:,None,:];cov=torch.bmm((centered*w[:,:,None]).transpose(1,2),centered)/safe[:,None,None];eig,vec=torch.linalg.eigh(cov);eig=eig.flip(1);normal=vec[:,:,0];valid=(count>=3)&(eig[:,-1]>=-eps*torch.maximum(torch.ones_like(safe),eig[:,0].abs())*128);eig=eig.clamp_min(0);l1,l2,l3=eig.unbind(1)
                        def ratio(a,b):return torch.where(b.abs()>eps,a/b,torch.full_like(a,float('nan')))
                        z=delta[:,:,2];maximum=z.masked_fill(~mask,float('-inf')).amax(1);minimum=z.masked_fill(~mask,float('inf')).amin(1)
                        values={'neighbor_count':count.double(),'linearity':ratio(l1-l2,l1),'planarity':ratio(l2-l3,l1),'surface_variation':ratio(l3,l1+l2+l3),'verticality':1-normal[:,2].abs(),'roughness':(mean*normal).sum(1).abs(),'z_range':maximum-minimum,'z_standard_deviation':cov[:,2,2].clamp_min(0).sqrt()}
                        for name in FAST:
                            if name!='neighbor_count': values[name]=torch.where(valid if name not in ('z_range','z_standard_deviation') else count>0,values[name],torch.full_like(safe,float('nan')))
                        by_radius[r]=values
                    output.append(torch.stack([by_radius[r][n] for n in FAST for r in radii],1).float().cpu().numpy())
                return np.concatenate(output)
            gpu_run(ids[:8]);torch.cuda.synchronize();start=time.perf_counter();gpu_values=gpu_run(ids);torch.cuda.synchronize();gpu_seconds=time.perf_counter()-start
            half_cpu,half_gpu=np.array_split(ids,2);job=prepare('hybrid_cpu12',xyz,half_cpu,fields,12);start=time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(1) as pool:
                future=pool.submit(execute,job);gpu_half=gpu_run(half_gpu);cpu_half=future.result()[1]
            hybrid=time.perf_counter()-start
            report['gpu']={'device':torch.cuda.get_device_name(0),'cold_init_seconds':cold,'upload_seconds':upload,'warmed_all_fast_seconds':gpu_seconds,'comparison':compare(reference,gpu_values,fields),'cpu12_plus_gpu_seconds':hybrid,'hybrid_comparison':compare(reference,np.concatenate((cpu_half,gpu_half)),fields),'peak_allocated_bytes':torch.cuda.max_memory_allocated(),'search':'Bounded dense CUDA search, experimental O(support x query); NOT a scalable production index','deployed':False}
        except Exception as exc:report['gpu']={'error':str(exc),'deployed':False}
        save();print(json.dumps(report['gpu']),flush=True)
    print(json.dumps({'median_seconds':report['median_seconds'],'chosen_profile':report['chosen_profile']}),flush=True)

if __name__=='__main__':main()
