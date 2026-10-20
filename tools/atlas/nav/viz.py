"""Self-contained HTML visualisation of an area's navigation graph.

    python viz.py m24_01_00_00 [m24_00_00_00 ...]  ->  OUT/<area>.html

Layers (toggle in the page): floor bands coloured by *region* (strongly connected component of the walk graph in the
selected scenario), collision walls, drop/ledge nodes, gates (green open / red closed / orange one-way), ladders,
lifts, lamps, and computed lamp-to-lamp routes per scenario.
"""
import base64
import colorsys
import io
import json
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy.sparse.csgraph import connected_components, dijkstra
from scipy.spatial import cKDTree

import coverage
import scenario as scen
from graph import DROP, LEDGE, LADDER, LIFT, TYPE_NAMES
from navconf import OUT

PX = 4.0          # pixels per metre
SCEN = ['all_open', 'mech', 'fresh', 'all_closed']


def png64(img):
    b = io.BytesIO(); img.save(b, 'PNG', optimize=True)
    return base64.b64encode(b.getvalue()).decode()


def region_colors(labels, sizes):
    rng = np.random.RandomState(7)
    n = len(sizes)
    cols = np.zeros((n, 3), np.uint8)
    order = np.argsort(sizes)[::-1]
    pal = [(38, 166, 154), (255, 112, 67), (66, 165, 245), (255, 213, 79), (171, 71, 188), (156, 204, 101), (236, 64, 122), (141, 110, 99), (38, 198, 218), (121, 134, 203)]
    cols[:] = (88, 98, 108)                           # everything else: slate grey
    for k, r in enumerate(order[:len(pal)]):
        if sizes[r] >= 300:
            cols[r] = pal[k]
    return cols


def band_layers(g, labels, cols, x0, z0, W, H, nb=6):
    N = g.n_grid
    y = g.y[:N]
    qs = np.quantile(y, np.linspace(0, 1, nb + 1)); qs[-1] += 1
    layers = []
    cs = max(int(round(PX * g.meta['cell'])), 1)
    for b in range(nb):
        m = (y >= qs[b]) & (y < qs[b + 1])
        img = np.zeros((H, W, 4), np.uint8)
        idx = np.nonzero(m)[0]
        idx = idx[np.argsort(y[idx])]
        px = ((g.x[idx] - x0) * PX - cs / 2).astype(int); pz = ((g.z[idx] - z0) * PX - cs / 2).astype(int)
        col = cols[labels[idx]]
        for dx in range(cs):
            for dz in range(cs):
                xx = np.clip(px + dx, 0, W - 1); zz = np.clip(pz + dz, 0, H - 1)
                img[zz, xx, :3] = col; img[zz, xx, 3] = 255
        layers.append(dict(y0=float(qs[b]), y1=float(qs[b + 1]), png=png64(Image.fromarray(img, 'RGBA'))))
    return layers


def draw_walls(area, x0, z0, W, H):
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(img)
    for L in area['layers']:
        for w in L['walls']:
            d.line([((w[0] - x0) * PX, (w[1] - z0) * PX), ((w[2] - x0) * PX, (w[3] - z0) * PX)], fill=(20, 20, 20, 255), width=1)
    return png64(img)


def draw_edge_nodes(g, x0, z0, W, H):
    """Orange: nodes with a drop link leaving them; magenta: ledge (seam repair) links."""
    img = np.zeros((H, W, 4), np.uint8)
    for t, col in ((DROP, (255, 140, 0, 255)), (LEDGE, (200, 0, 180, 255))):
        sel = (g.et == t) & (g.eu < g.n_grid)
        u = g.eu[sel]
        if t == LEDGE:
            u = np.concatenate([u, g.ev[sel]])
        px = ((g.x[u] - x0) * PX).astype(int); pz = ((g.z[u] - z0) * PX).astype(int)
        ok = (px >= 0) & (px < W) & (pz >= 0) & (pz < H)
        img[pz[ok], px[ok]] = col
    return png64(Image.fromarray(img, 'RGBA'))


def simplify(pts, step=3):
    out = pts[::step]
    if pts and (not out or out[-1] != pts[-1]):
        out.append(pts[-1])
    return out


def route_set(g, M, lamps, pickups, npcs, limit=14):
    """Routes between lamps and from the first lamp to a few pickups / npcs (all scenarios computed by the caller)."""
    routes = []
    srcs = [l for l in lamps if l['node'] is not None]
    if not srcs:
        return routes
    pairs = [(a, b, 'lamp') for a in srcs for b in srcs if a is not b]
    far = sorted([p for p in pickups + npcs if p['node'] is not None], key=lambda p: -np.hypot(p['pos'][0] - srcs[0]['pos'][0], p['pos'][2] - srcs[0]['pos'][2]))
    for p in far[:3]:
        pairs.append((srcs[0], p, p['kind']))
    for a, b, kind in pairs[:limit]:
        d, pred = dijkstra(M, directed=True, indices=a['node'], return_predecessors=True)
        if not np.isfinite(d[b['node']]):
            routes.append(dict(a=a['name'], b=b['name'], kind=kind, ok=False, pts=[]))
            continue
        n = b['node']; path = [n]
        while n != a['node']:
            n = pred[n]; path.append(n)
        path = path[::-1]
        pts = [[round(float(g.x[k]), 1), round(float(g.z[k]), 1), round(float(g.y[k]), 1)] for k in path]
        routes.append(dict(a=a['name'], b=b['name'], kind=kind, ok=True, cost=round(float(d[b['node']]), 1), pts=simplify(pts, 4)))
    return routes


def build_html(mid, params=None):
    out, g, (lamps, pickups, npcs) = coverage.run_area(mid, scenarios=SCEN, params=params, keep_graph=True)
    area = g.area
    b = area['bounds']
    x0 = float(g.x[:g.n_grid].min()) - 2; x1 = float(g.x[:g.n_grid].max()) + 2
    z0 = float(g.z[:g.n_grid].min()) - 2; z1 = float(g.z[:g.n_grid].max()) + 2
    W = int((x1 - x0) * PX); H = int((z1 - z0) * PX)
    # regions in the 'mech' scenario (walk-connected; one-way drops make components directional -> strong components)
    state = scen.scenario_state(g, 'mech')
    M = g.csr(state)
    nc, lab = connected_components(M, directed=True, connection='strong')
    lab = lab[:g.n_grid]
    sizes = np.bincount(lab, minlength=nc)
    cols = region_colors(lab, sizes)
    layers = band_layers(g, lab, cols, x0, z0, W, H)
    walls = draw_walls(area, x0, z0, W, H)
    edges = draw_edge_nodes(g, x0, z0, W, H)
    # gates / ladders / lifts with per-scenario state
    gates = []
    for i, m in enumerate(g.mechs):
        if m['kind'] in ('door', 'fog') and 'frame' in m:
            fr = m['frame']; sg = fr['segment']
            gates.append(dict(i=i, kind=m['kind'], beh=m.get('behavior'), desc=m.get('desc', ''), entity=m.get('entity'), seg=sg,
                              states={s: scen.state_for(m, s) for s in SCEN},
                              flags=[dict(flag=f['flag'], kind=f['kind'], conf=f['conf']) for f in m.get('flags', [])][:6],
                              note=m.get('behavior_note', '')))
    ladders = []
    lifts = []
    for i, m in enumerate(g.mechs):
        pass
    for k in np.nonzero(g.et == LADDER)[0]:
        u, v = int(g.eu[k]), int(g.ev[k])
        gi = int(g.eg[k])
        ladders.append(dict(a=[round(float(g.x[u]), 1), round(float(g.z[u]), 1), round(float(g.y[u]), 1)], b=[round(float(g.x[v]), 1), round(float(g.z[v]), 1), round(float(g.y[v]), 1)],
                            states={s: (scen.state_for(g.mechs[gi], s) if gi >= 0 else 'open') for s in SCEN}))
    for k in np.nonzero(g.et == LIFT)[0]:
        u, v = int(g.eu[k]), int(g.ev[k]); gi = int(g.eg[k])
        m = g.mechs[gi]
        lifts.append(dict(a=[round(float(g.x[u]), 1), round(float(g.z[u]), 1), round(float(g.y[u]), 1)], b=[round(float(g.x[v]), 1), round(float(g.z[v]), 1), round(float(g.y[v]), 1)],
                          desc=m.get('desc', ''), flags=[f['flag'] for f in m.get('flags', [])],
                          states={s: scen.state_for(m, s) for s in SCEN}))
    routes = {}
    for s in SCEN:
        Ms = g.csr(scen.scenario_state(g, s))
        routes[s] = route_set(g, Ms, lamps, pickups, npcs)
    data = dict(area=mid, name=area['name'], x0=x0, z0=z0, W=W, H=H, px=PX, layers=layers, walls=walls, edges=edges,
                gates=gates, ladders=ladders, lifts=lifts, routes=routes,
                lamps=[dict(name=l['name'], pos=l['pos']) for l in lamps],
                pickups=[dict(name=p['name'], pos=p['pos']) for p in pickups], npcs=[dict(name=p['name'], pos=p['pos']) for p in npcs],
                coverage={s: {k: v for k, v in out['scenarios'][s].items() if k not in ('failures', 'lamp_fail')} for s in SCEN},
                nodes=g.n, edges_n=int(len(g.eu)), regions=int((sizes >= 40).sum()), top_region=int(sizes.max()))
    html = HTML.replace('__DATA__', json.dumps(data, separators=(',', ':'))).replace('__TITLE__', f"{area['name']} ({mid}) navigation graph")
    p = OUT / f'{mid}.html'
    p.write_text(html)
    print(p, round(len(html) / 1e6, 2), 'MB')
    return p


HTML = r'''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<style>
:root{--bg:#10151b;--fg:#e8edf2;--mut:#93a1b0;--card:#18212b}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.4 system-ui,sans-serif;display:flex;height:100vh}
#side{width:330px;min-width:330px;overflow:auto;padding:12px;background:var(--card);box-sizing:border-box}
#view{flex:1;overflow:auto;position:relative}
h1{font-size:15px;margin:0 0 6px} h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin:14px 0 4px}
label{display:block;margin:2px 0;cursor:pointer} .sw{display:inline-block;width:10px;height:10px;margin-right:6px;border-radius:2px}
table{border-collapse:collapse;width:100%} td,th{padding:2px 4px;text-align:right;border-bottom:1px solid #2a3643} th:first-child,td:first-child{text-align:left}
#stage{position:relative;transform-origin:0 0} #stage img,#stage svg,#stage canvas{position:absolute;left:0;top:0}
#tip{position:fixed;pointer-events:none;background:#000c;padding:4px 7px;border-radius:4px;font-size:12px;display:none;max-width:360px;z-index:9}
button{background:#26394b;color:var(--fg);border:0;padding:3px 8px;border-radius:3px;cursor:pointer;margin:1px}
button.on{background:#2a7f78} .small{color:var(--mut);font-size:11px}
</style></head><body>
<div id="side"><h1 id="ttl"></h1><div class="small" id="sub"></div>
<h2>Scenario</h2><div id="scn"></div>
<h2>Coverage (this scenario)</h2><table id="cov"></table>
<h2>Floor bands (low to high)</h2><div id="bands"></div>
<h2>Layers</h2><div id="lay"></div>
<h2>Routes</h2><div id="rts"></div>
<h2>Legend</h2><div class="small">Floors are coloured by <b>region</b> (strongly connected component of the walk graph in the chosen scenario; teal = largest, grey = tiny). Gates: <span style="color:#3ddc84">open</span> / <span style="color:#ff5252">closed</span> / <span style="color:#ffab40">one-way (operable from one side)</span>. Ladders blue, lifts violet, lamps yellow, orange pixels = ledge drop sources, magenta = seam-repair ledge links.</div>
</div>
<div id="view"><div id="stage"></div></div><div id="tip"></div>
<script>
const D=__DATA__;let scn='mech';
const stage=document.getElementById('stage');stage.style.width=D.W+'px';stage.style.height=D.H+'px';
document.getElementById('ttl').textContent=D.name;document.getElementById('sub').textContent=D.area+' · '+D.nodes.toLocaleString()+' nodes · '+D.edges_n.toLocaleString()+' edges · '+D.regions+' regions (>=40 nodes), largest '+D.top_region.toLocaleString();
const imgs=[];window._z=Math.min((innerWidth-340)/D.W,innerHeight/D.H);stage.style.transform='scale('+window._z+')';D.layers.forEach((L,i)=>{const im=new Image();im.src='data:image/png;base64,'+L.png;stage.appendChild(im);imgs.push(im)});
const wl=new Image();wl.src='data:image/png;base64,'+D.walls;stage.appendChild(wl);
const el=new Image();el.src='data:image/png;base64,'+D.edges;stage.appendChild(el);
const NS='http://www.w3.org/2000/svg';const svg=document.createElementNS(NS,'svg');svg.setAttribute('width',D.W);svg.setAttribute('height',D.H);stage.appendChild(svg);
const gG=mk('g'),gL=mk('g'),gF=mk('g'),gP=mk('g'),gR=mk('g');[gG,gL,gF,gP,gR].forEach(x=>svg.appendChild(x));
function mk(t,a){const e=document.createElementNS(NS,t);for(const k in a||{})e.setAttribute(k,a[k]);return e}
const X=x=>(x-D.x0)*D.px,Z=z=>(z-D.z0)*D.px;
const tip=document.getElementById('tip');
function hov(e,txt){e.addEventListener('mousemove',ev=>{tip.style.display='block';tip.style.left=ev.clientX+12+'px';tip.style.top=ev.clientY+12+'px';tip.innerHTML=txt});e.addEventListener('mouseleave',()=>tip.style.display='none')}
const SC={open:'#3ddc84',closed:'#ff5252',oneway:'#ffab40'};
function draw(){
 [gG,gL,gF,gR].forEach(g=>{while(g.firstChild)g.removeChild(g.firstChild)});
 D.gates.forEach(g=>{const s=g.states[scn];const l=mk('line',{x1:X(g.seg[0]),y1:Z(g.seg[1]),x2:X(g.seg[2]),y2:Z(g.seg[3]),stroke:SC[s],'stroke-width':5,'stroke-linecap':'round'});
  hov(l,'<b>'+(g.kind=='fog'?'fog wall':'door')+'</b> '+g.desc+'<br>behaviour: '+g.beh+' · state here: '+s+'<br>entity '+g.entity+'<br>'+g.note+'<br>'+g.flags.map(f=>f.flag+' ('+f.kind+', '+f.conf+')').join('<br>'));gG.appendChild(l)});
 D.ladders.forEach(g=>{const s=g.states[scn];const l=mk('line',{x1:X(g.a[0]),y1:Z(g.a[1]),x2:X(g.b[0]),y2:Z(g.b[1]),stroke:'#4fc3f7','stroke-width':3,'stroke-dasharray':s=='open'?'':'4 3',opacity:s=='open'?1:.5});
  hov(l,'ladder '+g.a[2]+' → '+g.b[2]+' m ('+s+')');gL.appendChild(l);gL.appendChild(mk('circle',{cx:X(g.a[0]),cy:Z(g.a[1]),r:3,fill:'#4fc3f7'}));gL.appendChild(mk('circle',{cx:X(g.b[0]),cy:Z(g.b[1]),r:3,fill:'#0288d1'}))});
 D.lifts.forEach(g=>{const s=g.states[scn];const l=mk('line',{x1:X(g.a[0]),y1:Z(g.a[1]),x2:X(g.b[0]),y2:Z(g.b[1]),stroke:'#b388ff','stroke-width':3,'stroke-dasharray':s=='open'?'':'4 3',opacity:s=='open'?1:.5});
  hov(l,'lift '+g.desc+' '+g.a[2]+' → '+g.b[2]+' m ('+s+')<br>unlock flags: '+g.flags.join(', '));gL.appendChild(l);gL.appendChild(mk('rect',{x:X(g.a[0])-3,y:Z(g.a[1])-3,width:6,height:6,fill:'#b388ff'}));gL.appendChild(mk('rect',{x:X(g.b[0])-3,y:Z(g.b[1])-3,width:6,height:6,fill:'#7c4dff'}))});
 D.lamps.forEach(p=>{const c=mk('path',{d:'M0,-8 L2.4,-2.5 8,-2.5 3.6,1.2 5,7 0,3.6 -5,7 -3.6,1.2 -8,-2.5 -2.4,-2.5Z',fill:'#ffd54f',stroke:'#000','stroke-width':1,transform:'translate('+X(p.pos[0])+','+Z(p.pos[2])+')'});hov(c,'<b>lamp</b> '+p.name);gF.appendChild(c)});
 D.pickups.forEach(p=>{const c=mk('circle',{cx:X(p.pos[0]),cy:Z(p.pos[2]),r:2.5,fill:'#f48fb1'});hov(c,'pickup '+p.name);gF.appendChild(c)});
 D.npcs.forEach(p=>{const c=mk('circle',{cx:X(p.pos[0]),cy:Z(p.pos[2]),r:2.5,fill:'#a5d6a7'});hov(c,'npc '+p.name);gF.appendChild(c)});
 const cols=['#ffffff','#ff7043','#26c6da','#d4e157','#ec407a','#8d6e63','#ffee58','#66bb6a','#ab47bc','#29b6f6','#ffa726','#9ccc65','#5c6bc0','#ef5350'];
 const R=D.routes[scn];R.forEach((r,i)=>{if(!r.ok||!document.getElementById('r'+i).checked)return;
  const pl=mk('polyline',{points:r.pts.map(p=>X(p[0])+','+Z(p[1])).join(' '),fill:'none',stroke:cols[i%cols.length],'stroke-width':3,opacity:.9,'stroke-linejoin':'round'});
  hov(pl,r.kind+': '+r.a+' → '+r.b+' (cost '+r.cost+')');gR.appendChild(pl)});
 const c=D.coverage[scn];const row=(n,a,b)=>'<tr><td>'+n+'</td><td>'+a+' / '+b+'</td><td>'+(b?Math.round(100*a/b):0)+'%</td></tr>';
 document.getElementById('cov').innerHTML=row('lamp pairs',c.lamp_pairs_ok,c.lamp_pairs)+row('lamp→pickup',c.pickups_ok,c.pickups)+row('lamp→NPC',c.npcs_ok,c.npcs)+'<tr><td>route / straight (median, p90)</td><td colspan=2>'+(c.ratio_median?c.ratio_median.toFixed(2):'-')+' , '+(c.ratio_p90?c.ratio_p90.toFixed(2):'-')+'</td></tr>';
}
const scnNames={all_open:'all open (upper bound)',mech:'mechanisms active (levers pulled, doors ok, keys missing)',fresh:'fresh game (no lever/lift/ladder)',all_closed:'all closed'};
const sd=document.getElementById('scn');for(const k in scnNames){const b=document.createElement('button');b.textContent=scnNames[k];b.dataset.k=k;b.onclick=()=>{scn=k;[...sd.children].forEach(x=>x.classList.toggle('on',x.dataset.k==k));draw()};if(k==scn)b.classList.add('on');sd.appendChild(b);sd.appendChild(document.createElement('br'))}
const bd=document.getElementById('bands');D.layers.forEach((L,i)=>{const l=document.createElement('label');l.innerHTML='<input type=checkbox checked> y '+L.y0.toFixed(0)+' … '+L.y1.toFixed(0)+' m';l.firstChild.onchange=e=>imgs[i].style.display=e.target.checked?'':'none';bd.appendChild(l)});
const ly=document.getElementById('lay');[['walls',wl,'collision walls'],['edge',el,'drop / ledge nodes']].forEach(([k,im,t])=>{const l=document.createElement('label');l.innerHTML='<input type=checkbox '+(k=='edge'?'':'checked')+'> '+t;l.firstChild.onchange=e=>im.style.display=e.target.checked?'':'none';ly.appendChild(l);if(k=='edge')im.style.display='none'});
[['gates',gG],['ladders / lifts',gL],['lamps / pickups / NPCs',gF]].forEach(([t,g])=>{const l=document.createElement('label');l.innerHTML='<input type=checkbox checked> '+t;l.firstChild.onchange=e=>g.style.display=e.target.checked?'':'none';ly.appendChild(l)});
const rt=document.getElementById('rts');D.routes.mech.forEach((r,i)=>{const l=document.createElement('label');l.innerHTML='<input type=checkbox id=r'+i+' '+(i<3?'checked':'')+'> '+r.kind+': '+r.a+' → '+r.b;l.firstChild.onchange=draw;rt.appendChild(l)});
draw();
document.getElementById('view').addEventListener('wheel',e=>{if(!e.ctrlKey)return;e.preventDefault();const z=(window._z||1)*(e.deltaY<0?1.15:1/1.15);window._z=z;stage.style.transform='scale('+z+')'},{passive:false});
</script></body></html>'''


if __name__ == '__main__':
    for a in sys.argv[1:] or ['m24_01_00_00', 'm24_00_00_00']:
        build_html(a)
