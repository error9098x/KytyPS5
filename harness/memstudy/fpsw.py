import re,sys,statistics
# fpsw.py <run> [window_s] [ab_min_s]: fps per window + RemapStats per window.
# If the log has ABToggle lines (KYTY_AB_TOGGLE), also splits FPS by toggle phase: per phase window and
# aggregated on vs off over windows starting >= ab_min_s (default 160) after the first present.
R="/Users/aviral/Documents/ChatGPT/PS5/KytyPS5/harness/runs/"
run=sys.argv[1]; W=float(sys.argv[2]) if len(sys.argv)>2 else 30
ABMIN=float(sys.argv[3]) if len(sys.argv)>3 else 160
t=open(R+run+"/guest-printf.log",errors="replace").read()
s=[(int(a),int(b)) for a,b in re.findall(r"FrameTiming: present=(\d+) t_us=(\d+)",t)]
if not s: print("no samples"); sys.exit()
t0=s[0][1]; end=s[-1][1]
k=0
while k*W*1e6<=end-t0:
    w=[x for x in s if k*W*1e6<=x[1]-t0<(k+1)*W*1e6]
    d=[(b[1]-a[1])/1000 for a,b in zip(w,w[1:])]
    print("%4d-%4ds presents %4d fps %5.2f med %7.1fms  p%s"%(k*W,(k+1)*W,len(w),len(w)/W,statistics.median(d) if d else 0, w[0][0] if w else -1))
    k+=1
ab=[(m.group(1),int(m.group(2))) for m in re.finditer(r"ABToggle: (on|off) t_us=(\d+)",t)]
if ab:
    print("ABToggle split (window = one phase; agg over windows starting >= %ds):"%ABMIN)
    agg={"on":[0,0.0],"off":[0,0.0]}
    for i,(ph,ts) in enumerate(ab):
        te=ab[i+1][1] if i+1<len(ab) else end
        if te<=ts: continue
        n=sum(1 for x in s if ts<=x[1]<te); dur=(te-ts)/1e6
        rel=(ts-t0)/1e6
        print("  %-3s %6.0f-%6.0fs presents %4d fps %5.2f"%(ph,rel,(te-t0)/1e6,n,n/dur))
        if rel>=ABMIN and dur>=W*0.5: agg[ph][0]+=n; agg[ph][1]+=dur
    for ph in ("on","off"):
        n,d=agg[ph]; print("  AGG %-3s presents %d over %.0fs fps %.3f"%(ph,n,d,n/d if d else 0))
    if agg["on"][1] and agg["off"][1] and agg["off"][0]:
        print("  AGG on/off = %.3f"%((agg["on"][0]/agg["on"][1])/(agg["off"][0]/agg["off"][1])))
keys=["batch_us","vr_us","gpu_unmap_us","unmap_fast","gpu_unmaps"]
rs=re.findall(r"RemapStats: (.*)",t)
if rs:
    n=len(rs); seg=max(1,n//8)
    for i in range(0,n,seg):
        acc={}
        for line in rs[i:i+seg]:
            for kk,v in re.findall(r"(\w+)=(\d+)",line): acc[kk]=acc.get(kk,0)+int(v)
        print("RS[%d..%d] "%(i,i+seg)+" ".join("%s=%d"%(kk,acc.get(kk,0)) for kk in ["dt_ms"]+keys+["unmap_slow","unmap_slow_us"] if kk in acc))
    print("sample line:",rs[-1][:400])
