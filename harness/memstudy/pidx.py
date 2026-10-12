import re,sys
# pidx.py <run> <present_a> <present_b> ... : seconds since first present at which each present index was reached,
# and fps between consecutive indices (scene-matched comparison across runs of the same deterministic route).
R="/Users/aviral/Documents/ChatGPT/PS5/KytyPS5/harness/runs/"
t=open(R+sys.argv[1]+"/guest-printf.log",errors="replace").read()
s={int(a):int(b) for a,b in re.findall(r"FrameTiming: present=(\d+) t_us=(\d+)",t)}
t0=min(s.values()); idx=[int(x) for x in sys.argv[2:]]
def at(p):
    k=min((q for q in s if q>=p),default=None); return None if k is None else (s[k]-t0)/1e6
prev=None
for p in idx:
    ts=at(p); line="p%-5d t=%s"%(p,"%.1fs"%ts if ts is not None else "n/a")
    if prev and ts is not None and prev[1] is not None: line+="  fps(p%d..p%d)=%.2f"%(prev[0],p,(p-prev[0])/(ts-prev[1]))
    print(line); prev=(p,ts)
