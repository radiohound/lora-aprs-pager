import gzip, math, sys, collections
from pmtiles.reader import Reader, MmapSource
import mapbox_vector_tile as mvt
T="/Volumes/Terrain/california_r2_full/california_r2"
ARCH={"vector":T+"_vector.pmtiles","osm":T+"_vector_osm.pmtiles"}
rd={}
for k,p in ARCH.items():
    fh=open(p,"rb"); rd[k]=Reader(MmapSource(fh))
SITES=[("SF downtown",37.7879,-122.4074),("San Jose downtown",37.3382,-121.8863),("Sunnyvale suburb",37.3688,-122.0363),
       ("Mt Hamilton (open)",37.3414,-121.6430),("Big Sur coast",36.2704,-121.8081),("Yosemite Valley",37.7456,-119.5936),
       ("High Sierra (open)",37.2000,-118.7500)]
def tile(lat,lon,z):
    n=2**z; x=int((lon+180)/360*n); y=int((1-math.asinh(math.tan(math.radians(lat)))/math.pi)/2*n); return x,y
def nverts(c):
    if not c: return 0
    if isinstance(c[0],(int,float)): return 1
    return sum(nverts(i) for i in c)
def stats(z,x,y):
    out={"gz":0,"raw":0,"feat":0,"line":0,"poly":0,"pt":0,"verts":0,"layers":collections.Counter()}
    for k,r in rd.items():
        d=r.get(z,x,y)
        if not d: continue
        out["gz"]+=len(d)
        raw=gzip.decompress(d) if d[:2]==b"\x1f\x8b" else d
        out["raw"]+=len(raw)
        for ln,layer in mvt.decode(raw).items():
            for f in layer["features"]:
                g=f["geometry"]; t=g["type"]; nv=nverts(g["coordinates"])
                out["feat"]+=1; out["verts"]+=nv; out["layers"][ln]+=1
                if "Line" in t: out["line"]+=1
                elif "Polygon" in t: out["poly"]+=1
                else: out["pt"]+=1
    return out
print(f"{'site':20} {'z':>2} {'tile':>13} {'gzKB':>6} {'rawKB':>6} {'feats':>6} {'lines':>6} {'polys':>6} {'pts':>5} {'verts':>7}  top layers")
for name,lat,lon in SITES:
    for z in (10,12,14):
        x,y=tile(lat,lon,z); s=stats(z,x,y)
        top=", ".join(f"{k}:{v}" for k,v in s["layers"].most_common(4))
        print(f"{name:20} {z:>2} {x:>6}/{y:<6} {s['gz']/1024:6.1f} {s['raw']/1024:6.1f} {s['feat']:6d} {s['line']:6d} {s['poly']:6d} {s['pt']:5d} {s['verts']:7d}  {top}")
