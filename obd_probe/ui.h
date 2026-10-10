#pragma once
// Web UI served at "/". All OBD decoding happens here in the browser; the firmware only moves bytes.
static const char UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OBD Probe</title>
<style>
:root{--bg:#f5f6f8;--card:#fff;--fg:#1d2330;--mut:#6b7385;--line:#e2e5ea;--acc:#2563eb;--ok:#15803d;--bad:#b91c1c;--warn:#b45309;--code:#f0f2f5}
@media (prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1b1f27;--fg:#e6e9ef;--mut:#8b93a5;--line:#2b313c;--acc:#60a5fa;--ok:#4ade80;--bad:#f87171;--warn:#fbbf24;--code:#141820}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}
header{padding:12px 16px;border-bottom:1px solid var(--line);background:var(--card);display:flex;gap:16px;flex-wrap:wrap;align-items:center}
header h1{font-size:17px;margin:0}main{max-width:1200px;margin:0 auto;padding:12px 16px;display:grid;gap:12px;grid-template-columns:1fr 1fr}
@media (max-width:900px){main{grid-template-columns:1fr}}
section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;min-width:0}
section.wide{grid-column:1/-1}h2{font-size:14px;margin:0 0 8px;text-transform:uppercase;letter-spacing:.04em;color:var(--mut)}
button{background:var(--acc);color:#fff;border:0;border-radius:5px;padding:5px 10px;font:inherit;cursor:pointer;margin:2px}
button.sec{background:transparent;color:var(--acc);border:1px solid var(--acc)}button:disabled{opacity:.5;cursor:default}
input{background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:5px;padding:5px 8px;font:inherit}
table{border-collapse:collapse;width:100%;font-size:13px}td,th{text-align:left;padding:4px 6px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--mut);font-weight:600}.mono,pre,code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px}
pre{background:var(--code);padding:8px;border-radius:5px;overflow:auto;max-height:420px;margin:6px 0;white-space:pre-wrap;word-break:break-all}
.pill{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;border:1px solid var(--line)}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.mut{color:var(--mut)}
.t{color:var(--acc)}.r{color:var(--ok)}.e{color:var(--bad)}.i{color:var(--mut)}
.scroll{overflow:auto;max-height:420px}.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
</style></head><body>
<header><h1>OBD Probe</h1><span id="st" class="mut">loading…</span></header>
<main>
<section><h2>1 · Find the dongle</h2>
 <div class="row"><button onclick="scan('ble')">BLE scan</button><button id="sppbtn" class="sec" onclick="scan('spp')">Classic BT scan</button>
 <label class="mut"><input type="checkbox" id="onlyNamed" checked> hide unnamed</label></div>
 <div class="scroll"><table id="scan"><tr><td class="mut">Run a scan with the dongle powered.</td></tr></table></div>
 <details><summary class="mut">Manual connect</summary><div class="row">
  <input id="maddr" placeholder="aa:bb:cc:dd:ee:ff" size="17"><input id="matype" placeholder="atype 0/1" size="7">
  <input id="mrx" placeholder="rx uuid" size="8"><input id="mtx" placeholder="tx uuid" size="8"><input id="mpin" placeholder="PIN" size="5" value="1234">
  <button onclick="connect(maddr.value,'ble',matype.value||0,'',mrx.value,mtx.value)">BLE</button><button class="sec" onclick="connect(maddr.value,'spp',0,'','','',mpin.value)">SPP</button></div></details>
</section>
<section><h2>2 · Adapter</h2><div id="adapter" class="mut">Not connected.</div>
 <div class="row"><button id="recbtn" onclick="reconnectNow()">Reconnect now</button><button class="sec" onclick="api('/api/disconnect')">Disconnect</button><button class="sec" onclick="api('/api/forget')">Forget saved</button></div>
 <div id="gatt"></div></section>
<section class="wide"><h2>3 · Probe</h2>
 <div class="row"><button onclick="fingerprint()">Fingerprint adapter</button><button onclick="probe()">Full vehicle probe</button>
 <button class="sec" onclick="copyReport()">Copy report</button><span id="pstat" class="mut"></span></div>
 <pre id="report" class="mono">Results appear here.</pre></section>
<section class="wide"><h2>4 · Live data</h2>
 <div class="row"><button id="pollbtn" onclick="togglePoll()">Start polling</button><span class="mut">Supported PIDs are found by the full probe.</span><span id="rate" class="mut"></span></div>
 <div class="scroll"><table id="live"></table></div></section>
<section><h2>5 · Terminal</h2>
 <div class="row"><input id="cmd" class="mono" placeholder="ATI / 010C / ..." size="20" onkeydown="if(event.key=='Enter')term()"><input id="tmo" size="5" value="5000" title="timeout ms"><button onclick="term()">Send</button></div>
 <div class="row">ATZ ATI AT@1 ATRV ATDP ATDPN ATH1 ATH0 0100 010C 03 0902 STI</div>
 <pre id="tout" class="mono"></pre></section>
<section class="wide"><h2>7 · Firmware update</h2>
 <div class="row"><span id="fwv" class="mut"></span><input type="file" id="fwfile" accept=".bin"><button onclick="upload()">Upload &amp; reboot</button><span id="fwst" class="mut"></span></div>
 <div class="mut">Use <code>obd_probe.ino.bin</code> (not the merged/bootloader file). The current firmware keeps running until the new image is verified.</div></section>
<section><h2>6 · Raw link log</h2><div class="row"><label class="mut"><input type="checkbox" id="logpause"> pause</label><button class="sec" onclick="logEl.textContent=''">clear</button></div>
 <pre id="log" class="mono"></pre></section>
</main>
<script>
const $=id=>document.getElementById(id);const logEl=$('log');let S={},logSeq=0,supported=[],polling=false,proto='',reportTxt='';
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const vis=s=>s.replace(/\r/g,'\\r').replace(/\n/g,'\\n');
async function api(p){const r=await fetch(p);let j;try{j=await r.json()}catch(e){j={error:'bad json'}}if(!r.ok&&!j.error)j.error='HTTP '+r.status;return j}
document.querySelectorAll('section .row').forEach(r=>{if(/^[A-Z0-9@ ]+$/.test(r.textContent.trim())){r.innerHTML=r.textContent.trim().split(/\s+/).map(c=>`<button class="sec mono" onclick="cmd.value='${c}';term()">${c}</button>`).join('')}});
async function status(){try{S=await api('/api/status')}catch(e){$('st').innerHTML='<span class="bad">ESP unreachable</span>';return}
 $('sppbtn').disabled=!S.spp;$('sppbtn').title=S.spp?'':'This chip ('+S.chip+') has BLE only - Classic SPP needs an original ESP32';$('fwv').textContent='Running '+S.fw+' ·';if(S.spp)$('st').dataset.mode=S.btmode;
 $('st').innerHTML=`${esc(S.chip)} fw ${esc(S.fw)} · ${esc(S.wifi.ip)} (${S.wifi.rssi} dBm) · `+(S.connected?`<span class="ok">● ${esc(S.transport.toUpperCase())} ${esc(S.device.name||S.device.addr)}</span>`:(S.reconnecting?`<span class="warn">● reconnecting to dongle… (page may pause ~10 s)</span>`:`<span class="bad">● dongle not connected</span>`+(S.autoPaused?` <span class="mut">(auto-reconnect paused while this page is open: use Reconnect now)</span>`:S.nextRetry?` <span class="mut">(retry in ${S.nextRetry}s)</span>`:'')))+(S.bridge?` · <span class="warn">TCP bridge: ${esc(S.bridge)}</span>`:'')+(S.spp?` · radio: ${S.btmode=='spp'?'Classic':'BLE'} mode`:'')+` · heap ${(S.heap/1024)|0}k`;
 let a=S.connected?`<table><tr><th>Address</th><td class="mono">${esc(S.device.addr)}</td></tr><tr><th>Link</th><td>${esc(S.transport)} · ${esc(S.layout)}</td></tr>`+(S.rx?`<tr><th>RX / TX</th><td class="mono">${esc(S.rx)} / ${esc(S.tx)} · MTU ${S.mtu} · chunk ${S.chunk} · write ${S.writeResp?'with':'without'} response</td></tr>`:'')+`<tr><th>Traffic</th><td>${S.bytes.tx} B out · ${S.bytes.rx} B in · ${S.bytes.notifies} notifications · up ${S.connectedFor}s</td></tr></table>`:`<span class="mut">Not connected.${S.saved.addr?' Saved: '+esc(S.saved.name||'')+' '+esc(S.saved.addr)+' ('+esc(S.saved.type)+'), auto-reconnect '+(S.autoReconnect?'on':'off'):''}</span>`+(S.lastError?`<div class="bad">${esc(S.lastError)}</div>`:'');
 $('adapter').innerHTML=a}
async function pollLog(){if($('logpause').checked)return;const l=await api('/api/log?since='+logSeq);if(!Array.isArray(l))return;for(const e of l){logSeq=e.s;const d=document.createElement('span');d.className=e.d;d.textContent=`${(e.t/1000).toFixed(3).padStart(9)} ${{t:'→',r:'←',i:'·',e:'!'}[e.d]} ${vis(e.x)}\n`;logEl.appendChild(d)}
 while(logEl.childNodes.length>800)logEl.removeChild(logEl.firstChild);if(l.length)logEl.scrollTop=1e9}
const OBDHINT=/obd|elm|vlink|v-link|vgate|icar|obdii|konnwei|scan|car|ble_?spp|veepeak|carista|lelink|kiwi/i,OBDSVC=['fff0','ffe0','18f0','e7810a71-73ae-499d-8c15-faa9aef0c3f2'];
async function reconnectNow(){$('recbtn').disabled=true;const r=await api('/api/reconnect');if(r.error)alert(r.error);setTimeout(()=>{$('recbtn').disabled=false;status()},3000)}
async function waitBack(){for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,1500));try{await api('/api/status');return}catch(e){}}}
async function scan(t){$('scan').innerHTML='<tr><td class="mut">Scanning…</td></tr>';let r=await api(`/api/scan?type=${t}&secs=${t=='spp'?10:6}`);
 if(r.switching){$('scan').innerHTML=`<tr><td class="warn">Switching the radio to ${t=='spp'?'Classic':'BLE'} mode — ESP is rebooting, scan will retry…</td></tr>`;await new Promise(r=>setTimeout(r,3000));await waitBack();return scan(t)}
 if(!Array.isArray(r)){$('scan').innerHTML=`<tr><td class="bad">${esc(r.error)}</td></tr>`;return}
 const score=d=>(OBDHINT.test(d.name)?2:0)+((d.svcs||[]).some(s=>OBDSVC.includes(s))?2:0)+(d.name?1:0);
 r.sort((a,b)=>score(b)-score(a)||b.rssi-a.rssi);const rows=r.filter(d=>!$('onlyNamed').checked||d.name||score(d)>1);
 $('scan').innerHTML='<tr><th>Name</th><th>Address</th><th>RSSI</th><th>'+(t=='ble'?'Services / mfg':'Class')+'</th><th></th></tr>'+rows.map(d=>`<tr><td>${score(d)>=2?'<b class="ok">':''}${esc(d.name||'(no name)')}${score(d)>=2?'</b> <span class="pill">likely OBD</span>':''}</td><td class="mono">${esc(d.addr)}${t=='ble'?' <span class="mut">t'+d.atype+'</span>':''}</td><td>${d.rssi}</td><td class="mono">${t=='ble'?esc((d.svcs||[]).join(' '))+(d.mfg?' <span class="mut">mfg '+esc(d.mfg)+'</span>':''):'0x'+d.cod.toString(16)}</td><td><button onclick="connect('${d.addr}','${t}',${d.atype||0},'${esc(d.name).replace(/'/g,'')}')">Connect</button></td></tr>`).join('')+(rows.length<r.length?`<tr><td colspan=5 class="mut">${r.length-rows.length} unnamed hidden</td></tr>`:'')}
async function connect(addr,type,atype,name,rx,tx,pin){$('gatt').innerHTML='<span class="mut">Connecting…</span>';
 let r=await api(`/api/connect?type=${type}&addr=${encodeURIComponent(addr)}&atype=${atype||0}&name=${encodeURIComponent(name||'')}`+(rx?'&rx='+rx:'')+(tx?'&tx='+tx:'')+(pin?'&pin='+pin:''));
 if(r.switching){$('gatt').innerHTML='<span class="warn">Switching radio mode — ESP rebooting, connect will retry…</span>';await new Promise(r=>setTimeout(r,3000));await waitBack();return connect(addr,type,atype,name,rx,tx,pin)}
 showGatt(r.gatt||[]);if(!r.ok)$('gatt').insertAdjacentHTML('afterbegin',`<div class="bad">${esc(r.error)}</div>`);status()}
function showGatt(g){if(!g.length){$('gatt').innerHTML='';return}
 $('gatt').innerHTML='<h2 style="margin-top:10px">GATT table</h2><table><tr><th>Service</th><th>Characteristic</th><th>Handle</th><th>Props</th><th>Value</th></tr>'+g.map(s=>s.chars.map((c,i)=>`<tr><td class="mono">${i?'':esc(s.uuid)}</td><td class="mono">${esc(c.uuid)}</td><td>${c.handle}</td><td class="mono">${esc(c.props)}</td><td class="mono">${c.value?esc(c.value)+' <span class="mut">'+esc(c.hex)+'</span>':''}</td></tr>`).join('')||`<tr><td class="mono">${esc(s.uuid)}</td><td colspan=4 class="mut">(no characteristics)</td></tr>`).join('')+'</table>'}
async function send(c,t){const r=await api(`/api/cmd?c=${encodeURIComponent(c)}&t=${t||5000}`);if(r.error)throw new Error(r.error);return r}
async function term(){const c=$('cmd').value.trim();if(!c)return;try{const r=await send(c,$('tmo').value);$('tout').textContent=`> ${c}   (${r.ms} ms${r.timeout?', TIMEOUT':''})\n${vis(r.raw)}\n${clean(r.raw,c).join('\n')}\n\n`+$('tout').textContent}catch(e){$('tout').textContent=`> ${c}\n! ${e.message}\n\n`+$('tout').textContent}}
// ---- response parsing
function clean(raw,c){const cc=(c||'').replace(/\s/g,'').toUpperCase();return raw.split(/[\r\n]+/).map(s=>s.replace(/>/g,'').trim()).filter(s=>s&&s.replace(/\s/g,'').toUpperCase()!==cc)}
const isHex=l=>/^[0-9A-F]{2}( ?[0-9A-F]{2})*$/i.test(l);const hb=l=>l.replace(/\s/g,'').match(/../g).map(h=>parseInt(h,16));
function msgs(raw,c){const out=[];let mf=null;for(const l of clean(raw,c)){if(/^(SEARCHING|BUS INIT|STOPPED)/i.test(l))continue;
 if(/^[0-9A-F]{3}$/i.test(l)){mf={len:parseInt(l,16),b:[]};out.push(mf);continue}
 const m=l.match(/^([0-9A-F]):\s*(.*)$/i);if(m&&mf){mf.b.push(...hb(m[2]));continue}
 if(isHex(l))out.push({b:hb(l)});else out.push({text:l})}
 for(const m of out)if(m.len)m.b=m.b.slice(0,m.len);return out}
function errText(raw,c){const t=msgs(raw,c).filter(m=>m.text).map(m=>m.text);return t.join(' / ')}
function pidData(raw,mode,pid){const c=(mode).toString(16).padStart(2,'0')+pid.toString(16).padStart(2,'0');return msgs(raw,c).filter(m=>m.b&&m.b[0]==mode+0x40&&m.b[1]==pid).map(m=>m.b.slice(2))}
// ---- PID decoders (SAE J1979)
const pct=a=>(a*100/255).toFixed(1)+' %',trim=a=>((a-128)*100/128).toFixed(1)+' %',tmp=a=>(a-40)+' °C',w=(A,B)=>A*256+B;
const FUEL={1:'Open loop (cold)',2:'Closed loop',4:'Open loop (load/decel)',8:'Open loop (fault)',16:'Closed loop (fault)'};
const STD={1:'OBD-II (CARB)',2:'OBD (EPA)',3:'OBD + OBD-II',4:'OBD-I',5:'Not OBD compliant',6:'EOBD (Europe)',7:'EOBD + OBD-II',8:'EOBD + OBD',9:'EOBD + OBD + OBD-II',10:'JOBD',11:'JOBD + OBD-II',12:'JOBD + EOBD',13:'JOBD + EOBD + OBD-II'};
const FT={1:'Petrol',2:'Methanol',3:'Ethanol',4:'Diesel',5:'LPG',6:'CNG',7:'Propane',8:'Electric',9:'Bifuel petrol',12:'Bifuel LPG',17:'Hybrid petrol'};
const P={0x01:['Monitor status',d=>`MIL ${d[0]&0x80?'ON':'off'}, ${d[0]&0x7f} DTC(s), ${d[1]&8?'diesel':'spark'} ignition`],0x02:['Freeze-frame DTC',d=>dtc(d[0],d[1])],0x03:['Fuel system',d=>FUEL[d[0]]||d[0]],0x04:['Engine load',d=>pct(d[0])],0x05:['Coolant temp',d=>tmp(d[0])],
0x06:['Short fuel trim B1',d=>trim(d[0])],0x07:['Long fuel trim B1',d=>trim(d[0])],0x08:['Short fuel trim B2',d=>trim(d[0])],0x09:['Long fuel trim B2',d=>trim(d[0])],0x0A:['Fuel pressure',d=>d[0]*3+' kPa'],0x0B:['Intake MAP',d=>d[0]+' kPa'],
0x0C:['Engine RPM',d=>(w(d[0],d[1])/4).toFixed(0)+' rpm'],0x0D:['Vehicle speed',d=>d[0]+' km/h'],0x0E:['Timing advance',d=>(d[0]/2-64).toFixed(1)+' °'],0x0F:['Intake air temp',d=>tmp(d[0])],0x10:['MAF air flow',d=>(w(d[0],d[1])/100).toFixed(2)+' g/s'],
0x11:['Throttle position',d=>pct(d[0])],0x12:['Secondary air',d=>d[0]],0x13:['O2 sensors present',d=>'0x'+d[0].toString(16)],0x1C:['OBD standard',d=>STD[d[0]]||d[0]],0x1D:['O2 sensors present (4 bank)',d=>'0x'+d[0].toString(16)],0x1E:['Aux input',d=>d[0]&1?'PTO active':'off'],
0x1F:['Run time since start',d=>w(d[0],d[1])+' s'],0x21:['Distance with MIL on',d=>w(d[0],d[1])+' km'],0x22:['Fuel rail pressure (rel)',d=>(w(d[0],d[1])*0.079).toFixed(1)+' kPa'],0x23:['Fuel rail pressure',d=>w(d[0],d[1])*10+' kPa'],
0x2C:['Commanded EGR',d=>pct(d[0])],0x2D:['EGR error',d=>trim(d[0])],0x2E:['Commanded evap purge',d=>pct(d[0])],0x2F:['Fuel level',d=>pct(d[0])],0x30:['Warm-ups since clear',d=>d[0]],0x31:['Distance since clear',d=>w(d[0],d[1])+' km'],
0x32:['Evap vapour pressure',d=>((w(d[0],d[1])<<16>>16)/4).toFixed(1)+' Pa'],0x33:['Barometric pressure',d=>d[0]+' kPa'],0x41:['Monitors this cycle',d=>d.map(x=>x.toString(16).padStart(2,'0')).join(' ')],0x42:['ECU voltage',d=>(w(d[0],d[1])/1000).toFixed(2)+' V'],
0x43:['Absolute load',d=>(w(d[0],d[1])*100/255).toFixed(1)+' %'],0x44:['Commanded lambda',d=>(w(d[0],d[1])*2/65536).toFixed(3)],0x45:['Relative throttle',d=>pct(d[0])],0x46:['Ambient air temp',d=>tmp(d[0])],0x47:['Abs throttle B',d=>pct(d[0])],0x48:['Abs throttle C',d=>pct(d[0])],
0x49:['Accel pedal D',d=>pct(d[0])],0x4A:['Accel pedal E',d=>pct(d[0])],0x4B:['Accel pedal F',d=>pct(d[0])],0x4C:['Commanded throttle',d=>pct(d[0])],0x4D:['Time with MIL on',d=>w(d[0],d[1])+' min'],0x4E:['Time since clear',d=>w(d[0],d[1])+' min'],
0x51:['Fuel type',d=>FT[d[0]]||d[0]],0x52:['Ethanol %',d=>pct(d[0])],0x59:['Fuel rail abs pressure',d=>w(d[0],d[1])*10+' kPa'],0x5A:['Rel accel pedal',d=>pct(d[0])],0x5B:['Hybrid battery',d=>pct(d[0])],0x5C:['Oil temp',d=>tmp(d[0])],
0x5D:['Injection timing',d=>(w(d[0],d[1])/128-210).toFixed(2)+' °'],0x5E:['Fuel rate',d=>(w(d[0],d[1])/20).toFixed(2)+' L/h']};
for(let i=0;i<8;i++){P[0x14+i]=[`O2 B${1+(i>>2)}S${1+(i&3)} voltage`,d=>(d[0]/200).toFixed(3)+' V'+(d[1]!=255?', trim '+trim(d[1]):'')];P[0x24+i]=[`O2 S${i+1} lambda (wide)`,d=>(w(d[0],d[1])*2/65536).toFixed(3)+' λ, '+(w(d[2],d[3])*8/65536).toFixed(3)+' V'];P[0x34+i]=[`O2 S${i+1} lambda (current)`,d=>(w(d[0],d[1])*2/65536).toFixed(3)+' λ, '+(w(d[2],d[3])/256-128).toFixed(2)+' mA']}
for(let i=0;i<4;i++)P[0x3C+i]=[`Catalyst temp B${1+(i&1)}S${1+(i>>1)}`,d=>(w(d[0],d[1])/10-40).toFixed(1)+' °C'];
[0x55,0x56,0x57,0x58].forEach((p,i)=>P[p]=[['Short','Long','Short','Long'][i]+' sec O2 trim B'+(1+(i>>1)),d=>trim(d[0])]);
function dtc(a,b){return 'PCBU'[a>>6]+((a>>4)&3)+(a&15).toString(16)+(b>>4).toString(16)+(b&15).toString(16)}
const hex2=n=>n.toString(16).toUpperCase().padStart(2,'0');
// ---- probe routines
function rep(s){reportTxt+=s+'\n';$('report').textContent=reportTxt;$('report').scrollTop=1e9}
const ps=s=>$('pstat').textContent=s;
async function step(label,c,t){ps(label+' ('+c+')');try{const r=await send(c,t);const out=clean(r.raw,c).join(' | ');rep(`${c.padEnd(8)} ${String(r.ms).padStart(5)}ms${r.timeout?' TIMEOUT':''}  ${out||'(empty)'}${label?'   — '+label:''}`);return r}catch(e){rep(`${c.padEnd(8)} ERROR ${e.message}`);throw e}}
async function fingerprint(){reportTxt='';rep('=== Adapter fingerprint ===');rep('cmd      time       response   — meaning');
 try{await step('reset, prints ELM version','ATZ',8000);await step('echo off','ATE0');
 for(const [c,l] of [['ATI','version string'],['AT@1','device description'],['AT@2','device identifier (v1.4+)'],['ATRV','supply voltage at OBD pin 16'],['ATDP','current protocol'],['ATDPN','protocol number'],['ATIGN','ignition input (v1.4+)'],['ATCS','CAN status counts (v1.1+)'],['ATKW','ISO key words'],['ATPPS','programmable parameters (v1.1+)'],['ATAMC','activity monitor count (v2.0+)'],['ATFE','forget events (v2.1+)'],['ATRD','read stored byte (v1.4+)'],['STI','STN chip id (OBDLink only)'],['STDI','STN device id'],['VTVERS','vLinker firmware']])await step(l,c,3000);
 rep('\n"?" means the chip rejected the command. Clones that claim v2.1 but reject v1.4/v2.0 commands are v1.x clones with a fake version string.')}catch(e){}ps('done')}
async function probe(){reportTxt='';supported=[];rep('=== Vehicle probe ===');
 try{await step('reset','ATZ',8000);await step('echo off','ATE0');await step('linefeeds on','ATL1');await step('spaces on','ATS1');await step('headers off','ATH0');await step('auto protocol','ATSP0');
 await step('battery voltage','ATRV');
 const r0=await step('first OBD request; adapter searches protocols','0100',20000);
 const e=errText(r0.raw,'0100');if(!pidData(r0.raw,1,0).length){rep(`\nNo OBD response (${e}). Is the ignition ON? With the engine off some ECUs still answer; with ignition off none will.`);ps('no ECU response');return}
 const dp=await step('protocol in use','ATDP');const dpn=await step('protocol number','ATDPN');proto=clean(dpn.raw,'ATDPN')[0]||'';
 await step('headers on: which ECUs answer','ATH1');await step('ECU addresses (header first)','0100',5000);await step('headers off','ATH0');
 // supported PID discovery
 for(let base=0;base<=0xE0;base+=0x20){const c='01'+hex2(base);const r=base?await step('supported PIDs '+hex2(base+1)+'-'+hex2(base+0x20),c,5000):r0;const ds=pidData(r.raw,1,base);if(!ds.length)break;let more=false;
  for(const d of ds)for(let i=0;i<32;i++)if((d[i>>3]>>(7-(i&7)))&1){const p=base+i+1;if(p%0x20==0){if(p==base+0x20)more=true}else if(!supported.includes(p))supported.push(p)}if(!more)break}
 supported.sort((a,b)=>a-b);rep(`\nSupported mode-01 PIDs (${supported.length}): ${supported.map(hex2).join(' ')}`);
 rep('\n--- Current values ---');for(const p of supported){const r=await send('01'+hex2(p),3000);const d=pidData(r.raw,1,p);rep(`01${hex2(p)}  ${(P[p]?P[p][0]:'PID '+hex2(p)).padEnd(28)} ${d.length?(P[p]?safe(P[p][1],d[0]):d[0].map(hex2).join(' ')):errText(r.raw,'01'+hex2(p))}`)}
 buildLive();
 rep('\n--- Vehicle info (mode 09) ---');await step('supported info types','0900',5000);
 for(const [c,l] of [['0902','VIN'],['0904','Calibration ID'],['0906','Calibration verification'],['090A','ECU name']]){const r=await send(c,6000);const s=infoStr(r.raw,c);rep(`${c}  ${l.padEnd(26)} ${s||errText(r.raw,c)||'(none)'}`)}
 rep('\n--- Trouble codes ---');const can=/^A?[6-9AB]$/i.test(proto);
 for(const [c,l] of [['03','Stored DTCs'],['07','Pending DTCs'],['0A','Permanent DTCs']]){const r=await send(c,6000);rep(`${c}  ${l.padEnd(16)} ${dtcs(r.raw,c,can)}`)}
 rep('\nProbe complete. "Copy report" and paste it to Claude.')}catch(e){rep('Stopped: '+e.message)}ps('done')}
function safe(f,d){try{return f(d)}catch(e){return d.map(hex2).join(' ')}}
function infoStr(raw,c){const ms=msgs(raw,c).filter(m=>m.b&&m.b[0]==0x49);if(!ms.length)return'';let bytes=[];
 if(ms.length==1&&ms[0].b.length>7)bytes=ms[0].b.slice(3);else ms.sort((a,b)=>a.b[2]-b.b[2]).forEach(m=>bytes.push(...m.b.slice(3)));
 return bytes.filter(x=>x>=32&&x<127).map(x=>String.fromCharCode(x)).join('').trim()||bytes.map(hex2).join(' ')}
function dtcs(raw,c,can){const mode=parseInt(c,16)+0x40;const ms=msgs(raw,c).filter(m=>m.b&&m.b[0]==mode);if(!ms.length)return errText(raw,c)||'(no response)';const out=[];
 for(const m of ms){let b=m.b.slice(1);if(can)b=b.slice(1);for(let i=0;i+1<b.length;i+=2)if(b[i]||b[i+1])out.push(dtc(b[i],b[i+1]))}return out.length?out.join(', '):'none'}
// ---- live polling
function buildLive(){$('live').innerHTML='<tr><th>PID</th><th>Name</th><th>Value</th><th>Raw</th><th>ms</th></tr>'+supported.filter(p=>p>2).map(p=>`<tr><td class="mono">01${hex2(p)}</td><td>${P[p]?P[p][0]:''}</td><td id="v${p}"></td><td class="mono mut" id="x${p}"></td><td class="mut" id="m${p}"></td></tr>`).join('')}
async function togglePoll(){polling=!polling;$('pollbtn').textContent=polling?'Stop polling':'Start polling';if(!supported.length){polling=false;$('pollbtn').textContent='Start polling';alert('Run the full vehicle probe first');return}
 while(polling){const t0=Date.now();let n=0;for(const p of supported.filter(p=>p>2)){if(!polling)break;try{const r=await send('01'+hex2(p),2000);const d=pidData(r.raw,1,p);n++;
  $('v'+p).textContent=d.length?(P[p]?safe(P[p][1],d[0]):''):errText(r.raw,'01'+hex2(p));$('x'+p).textContent=d.length?d[0].map(hex2).join(' '):'';$('m'+p).textContent=r.ms}catch(e){polling=false}}
  $('rate').textContent=n?` ${(n*1000/(Date.now()-t0)).toFixed(1)} req/s`:''}$('pollbtn').textContent='Start polling'}
async function upload(){const f=$('fwfile').files[0];if(!f)return;$('fwst').textContent='stopping Bluetooth…';
 for(let i=0;i<15;i++){const p=await api('/api/prepare-update');if(p.ok)break;await new Promise(r=>setTimeout(r,1000))}const fd=new FormData();fd.append('fw',f,f.name);const x=new XMLHttpRequest();
 x.upload.onprogress=e=>$('fwst').textContent='uploading '+((e.loaded*100/e.total)|0)+' %';
 x.onload=()=>{$('fwst').textContent=x.responseText.trim();if(x.status==200)setTimeout(()=>location.reload(),9000)};x.onerror=()=>$('fwst').textContent='upload failed';x.open('POST','/update');x.send(fd)}
function copyReport(){navigator.clipboard?.writeText(reportTxt+'\n\nGATT:'+JSON.stringify(S)).then(()=>ps('copied'),()=>ps('copy failed — select the text manually'))}
api('/api/gatt').then(showGatt);status();setInterval(status,2500);setInterval(pollLog,1000);
</script></body></html>
)HTML";
