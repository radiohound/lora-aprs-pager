// The phone setup page, served by the pager at http://192.168.4.1 while the setup screen is open.
// One self-contained page: no external files except the two symbol sheets (sym0.png, sym1.png, embedded in the firmware).
#pragma once
static const char SETUP_PAGE[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pager setup</title>
<style>
:root{--bg:#10161d;--card:#1a232d;--fg:#e8eef4;--mute:#8da0b2;--ac:#35c46f;--bad:#ff6b5e;--line:#2c3947}
@media (prefers-color-scheme:light){:root{--bg:#f2f5f8;--card:#fff;--fg:#14202b;--mute:#5a6b7b;--ac:#12803f;--bad:#c62828;--line:#d3dbe3}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,sans-serif;padding:16px;max-width:560px;margin:auto}
h1{font-size:1.3rem;margin:.2em 0 .6em}h2{font-size:1rem;margin:0 0 .6em;color:var(--mute);text-transform:uppercase;letter-spacing:.05em}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:14px}
label{display:block;margin:.7em 0 .2em;font-weight:600}small{color:var(--mute);display:block;margin-top:.2em}
input[type=text],input[type=number],select{width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font:inherit}
input[type=range]{width:100%}.row{display:flex;gap:10px}.row>div{flex:1}
.chk{display:flex;align-items:center;gap:10px;margin:.7em 0}.chk input{width:22px;height:22px}.chk label{margin:0}
button{font:inherit;padding:12px 18px;border-radius:10px;border:0;background:var(--ac);color:#fff;font-weight:700;width:100%}
#msg{margin-top:10px;min-height:1.4em;font-weight:600}.ok{color:var(--ac)}.err{color:var(--bad)}
.tabs{display:flex;gap:8px;margin-bottom:8px}.tabs button{background:var(--bg);color:var(--fg);border:1px solid var(--line);padding:8px;width:auto;flex:1;font-weight:600}
.tabs button.on{background:var(--ac);color:#fff;border-color:var(--ac)}
#grid{display:flex;flex-wrap:wrap;gap:3px;max-height:260px;overflow:auto;padding:4px;background:#c9d3dc;border-radius:8px}
#grid i{width:36px;height:36px;display:block;background-size:576px 216px;border-radius:4px;cursor:pointer;outline:2px solid transparent}
#grid i.sel{outline-color:#e53935;background-color:#fff}
#cur{display:flex;align-items:center;gap:10px;margin:8px 0}#cur i{width:36px;height:36px;background-size:576px 216px;background-color:#c9d3dc;border-radius:4px}
</style></head><body>
<h1>T-LoRa Pager setup</h1>
<form id="f" onsubmit="return save(event)">
<div class="card"><h2>Station</h2>
<label for="call">Call sign</label><input type="text" id="call" maxlength="9" autocapitalize="characters" autocomplete="off" spellcheck="false">
<small>With an SSID, like W1AW-7. The pager will not transmit while this is N0CALL.</small>
<label for="comment">Comment</label><input type="text" id="comment" maxlength="43" autocomplete="off"><small>Up to 43 characters, shown next to your position.</small>
<label for="path">Digipeater path</label><input type="text" id="path" maxlength="20" autocapitalize="characters" autocomplete="off" spellcheck="false"><small>Usually WIDE1-1.</small></div>

<div class="card"><h2>Map symbol</h2>
<div id="cur"><i id="curi"></i><span id="curt"></span></div>
<div class="tabs"><button type="button" id="t0" onclick="tab(0)">Primary</button><button type="button" id="t1" onclick="tab(1)">Alternate</button></div>
<div id="grid"></div>
<div id="ovl" style="display:none"><label for="ov">Overlay character (optional)</label><input type="text" id="ov" maxlength="1" autocapitalize="characters" autocomplete="off" oninput="ovInput()"><small>One letter or digit drawn on the symbol, like I or L on a diamond.</small></div></div>

<div class="card"><h2>Beacon</h2>
<div class="row"><div><label for="txdbm">TX power (dBm)</label><input type="number" id="txdbm" min="2" max="22"></div>
<div><label for="movem">Moved (metres)</label><input type="number" id="movem" min="20" max="5000"></div></div>
<div class="row"><div><label for="moving">While moving (s)</label><input type="number" id="moving" min="30" max="3600"></div>
<div><label for="stationary">Standing still (min)</label><input type="number" id="stationary" min="1" max="120"></div></div>
<small>While moving it beacons after the pager has moved the distance above and at least the moving time has passed. Standing still, it beacons at the long interval.</small>
<div class="chk"><input type="checkbox" id="auto"><label for="auto">Start beaconing when switched on</label></div></div>

<div class="card"><h2>Pager</h2>
<label for="dim">Dim the screen after</label><select id="dim"><option value="0">Never</option><option value="15">15 seconds</option><option value="30">30 seconds</option><option value="60">1 minute</option><option value="120">2 minutes</option><option value="300">5 minutes</option></select>
<label for="off">Turn the screen off after</label><select id="off"><option value="0">Never</option><option value="60">1 minute</option><option value="120">2 minutes</option><option value="300">5 minutes</option><option value="600">10 minutes</option><option value="1800">30 minutes</option></select>
<small>Any key or knob turn brings the screen back at once, exactly as it was. There is no lock screen. A new message also wakes it.</small>
<div class="chk"><input type="checkbox" id="baticon"><label for="baticon">Battery shape in the top right corner</label></div>
<div class="chk"><input type="checkbox" id="haptic"><label for="haptic">Haptic feedback</label></div>
<label for="bright">Screen brightness</label><input type="range" id="bright" min="10" max="255"></div>

<div class="card"><h2>Battery, screen off</h2>
<div class="chk"><input type="checkbox" id="gpssl"><label for="gpssl">GPS sleeps while the screen is off</label></div>
<label for="gpsmv">GPS wakes for a fix every, while moving</label><select id="gpsmv"><option value="30">30 seconds</option><option value="60">1 minute</option><option value="120">2 minutes</option><option value="300">5 minutes</option></select>
<label for="gpsst">GPS wakes for a fix every, once standing still</label><select id="gpsst"><option value="2">2 minutes</option><option value="5">5 minutes</option><option value="10">10 minutes</option><option value="15">15 minutes</option><option value="30">30 minutes</option><option value="60">1 hour</option></select>
<div class="chk"><input type="checkbox" id="rxd"><label for="rxd">Radio listens in short bursts while the screen is off (saves about 2 mA; may miss weak packets)</label></div>
<div class="chk"><input type="checkbox" id="slow40"><label for="slow40">Slower processor (40 MHz) while the screen is off (new, being tested)</label></div>
<div class="chk"><input type="checkbox" id="gpsmo"><label for="gpsmo">Wake the GPS when the pager moves (motion sensor)</label></div>
<label for="gpsbk">With the motion sensor: GPS wakes anyway every, while nothing moves</label><select id="gpsbk"><option value="15">15 minutes</option><option value="30">30 minutes</option><option value="60">1 hour</option><option value="120">2 hours</option><option value="240">4 hours</option></select>
<small>With the screen off the pager sleeps and still hears every packet. The GPS is the biggest cost: it wakes briefly for a fix, and once two fixes in a row show you have not moved (or it cannot see the sky) it waits the longer time. The cost: a beacon sent while the screen is off uses the last fix, and a move is noticed at the next wake. Switch GPS sleep off to keep the GPS running (uses much more battery).</small></div>

<div class="card"><h2>Clock</h2>
<label for="tz">Time zone (hours from UTC)</label><input type="number" id="tz" min="-12" max="14"><small>The time comes from GPS in UTC. California is -8 (Pacific standard time).</small>
<div class="chk"><input type="checkbox" id="dst"><label for="dst">Add an hour in US daylight saving time</label></div></div>

<button type="submit">Save</button><div id="msg"></div>
</form>
<script>
const $=id=>document.getElementById(id);let T=0,tbl='/',code='[';
function ic(el,t,c){const i=c.charCodeAt(0)-33;el.style.backgroundImage='url(sym'+t+'.png)';el.style.backgroundPosition='-'+((i%16)*36)+'px -'+(Math.floor(i/16)*36)+'px'}
function show(){ $('curt').textContent='Table '+tbl+'  Symbol '+code; ic($('curi'),tbl=='/'?0:1,code);
 document.querySelectorAll('#grid i').forEach(e=>e.classList.toggle('sel',e.dataset.c==code&&(T==0?tbl=='/':tbl!='/')))}
function tab(t){T=t;$('t0').classList.toggle('on',t==0);$('t1').classList.toggle('on',t==1);$('ovl').style.display=t?'block':'none';
 if(t==0)tbl='/';else if(tbl=='/')tbl='\\';build();show()}
function build(){const g=$('grid');g.innerHTML='';for(let n=0;n<94;n++){const c=String.fromCharCode(33+n),e=document.createElement('i');e.dataset.c=c;e.title=c;ic(e,T,c);
 e.onclick=()=>{code=c;if(T==0)tbl='/';else if(!/[0-9A-Z]/.test(tbl))tbl='\\';show()};g.appendChild(e)}}
function ovInput(){const v=$('ov').value.toUpperCase();$('ov').value=v;tbl=/^[0-9A-Z]$/.test(v)?v:'\\';show()}
fetch('settings.json').then(r=>r.json()).then(s=>{
 $('call').value=s.call;$('comment').value=s.comment;$('path').value=s.path;$('txdbm').value=s.txdbm;$('movem').value=s.movem;
 $('moving').value=s.moving;$('stationary').value=s.stationary;$('auto').checked=!!s.auto;$('haptic').checked=!!s.haptic;$('bright').value=s.bright;$('tz').value=s.tz;$('dst').checked=!!s.dst;$('baticon').checked=!!s.baticon;$('dim').value=s.dim;$('off').value=s.off;$('gpssl').checked=!!s.gpssl;$('gpsmv').value=s.gpsmv;$('gpsst').value=s.gpsst;$('gpsmo').checked=!!s.gpsmo;$('gpsbk').value=s.gpsbk;$('rxd').checked=!!s.rxd;$('slow40').checked=!!s.slow40;
 tbl=s.table;code=s.sym;T=tbl=='/'?0:1;$('ov').value=/^[0-9A-Z]$/.test(tbl)?tbl:'';tab(T)}).catch(()=>{$('msg').textContent='Could not load the settings';$('msg').className='err'});
async function save(ev){ev.preventDefault();const m=$('msg');m.className='';m.textContent='Saving...';
 const p=new URLSearchParams({call:$('call').value,path:$('path').value,table:tbl,sym:code,comment:$('comment').value,txdbm:$('txdbm').value,
  moving:$('moving').value,stationary:$('stationary').value,movem:$('movem').value,auto:$('auto').checked?1:0,haptic:$('haptic').checked?1:0,bright:$('bright').value,tz:$('tz').value,dst:$('dst').checked?1:0,baticon:$('baticon').checked?1:0,dim:$('dim').value,off:$('off').value,gpssl:$('gpssl').checked?1:0,gpsmv:$('gpsmv').value,gpsst:$('gpsst').value,gpsmo:$('gpsmo').checked?1:0,gpsbk:$('gpsbk').value,rxd:$('rxd').checked?1:0,slow40:$('slow40').checked?1:0});
 try{const r=await fetch('save',{method:'POST',body:p});const j=await r.json();m.textContent=j.ok?'Saved. The pager is using the new settings.':j.error;m.className=j.ok?'ok':'err'}
 catch(e){m.textContent='Could not reach the pager';m.className='err'}return false}
</script></body></html>)HTML";
