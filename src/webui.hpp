// webui.hpp - the phone control page (served from memory, no external files).
#pragma once

namespace imp {

static const char* kWebUiHtml = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Improv DSP</title>
<style>
:root{--bg:#0d0f12;--card:#171a1f;--line:#262b33;--text:#e8eaed;--dim:#9aa3ad;--acc:#4fb3ff;
--ok:#3ecf74;--warn:#ffb020;--bad:#ff4d4d;--track:#2a3039}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--text);
font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:12px 16px 96px}
header{display:flex;align-items:center;gap:10px;margin:4px 0 12px}
h1{font-size:18px;margin:0;flex:1;letter-spacing:.02em}
.dot{width:10px;height:10px;border-radius:50%;background:var(--bad);display:inline-block}
.dot.on{background:var(--ok)}.sys{color:var(--dim);font-size:13px;font-variant-numeric:tabular-nums}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 14px;margin:12px 0}
section h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:0 0 8px;
display:flex;justify-content:space-between;cursor:default}
.mute{width:100%;padding:16px;font-size:20px;font-weight:700;border-radius:12px;border:0;background:#24303d;color:var(--text)}
.mute.on{background:var(--bad);color:#fff}
.meter{margin:6px 0}.meter .lab{display:flex;justify-content:space-between;font-size:13px;color:var(--dim)}
.bar{position:relative;height:12px;background:var(--track);border-radius:6px;overflow:hidden}
.bar i{position:absolute;left:0;top:0;bottom:0;background:var(--ok);border-radius:6px;transition:width .1s}
.bar b{position:absolute;top:0;bottom:0;width:2px;background:#fff8}
.chips{display:flex;gap:6px;flex-wrap:wrap;margin:8px 0 2px}
.chip{font-size:12px;padding:3px 8px;border-radius:999px;background:var(--track);color:var(--dim)}
.chip.ok{background:#1e4d33;color:#c7f5d9}.chip.warn{background:#5a4210;color:#ffe3a3}.chip.bad{background:#5c1d1d;color:#ffd0d0}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:8px}
.kv{background:#11151a;border-radius:8px;padding:6px;text-align:center}
.kv span{display:block;font-size:11px;color:var(--dim)}.kv strong{font-variant-numeric:tabular-nums}
.row{margin:10px 0}.row .top{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.row .top label{font-size:15px}.row .top output{font-variant-numeric:tabular-nums;color:var(--acc);font-weight:600}
.row .help{font-size:12px;color:var(--dim)}
.sl{display:flex;align-items:center;gap:8px;margin-top:4px}
.sl input{flex:1;accent-color:var(--acc);height:28px}
.sl button,.btn{min-width:40px;height:36px;border-radius:8px;border:1px solid var(--line);background:#1f252d;color:var(--text);font-size:16px}
.btn{padding:0 14px;width:100%;margin-top:6px;font-size:15px}.btn.primary{background:var(--acc);color:#04111d;border:0;font-weight:700}
.btn.danger{border-color:#6b2a2a}
.tog{display:flex;justify-content:space-between;align-items:center;margin:10px 0;gap:10px}
.tog button{width:64px;height:34px;border-radius:999px;border:0;background:var(--track);color:var(--dim);font-weight:700}
.tog button.on{background:var(--ok);color:#06210f}
table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}
td,th{padding:3px 4px;text-align:left;border-bottom:1px solid var(--line)}th{color:var(--dim);font-weight:500}
.hint{font-size:13px;color:var(--dim);margin:6px 0}
.ring{border:1px solid var(--warn);border-radius:10px;padding:10px;margin-top:6px}
.big{font-size:40px;font-weight:700;text-align:center;font-variant-numeric:tabular-nums}
.pm{display:flex;gap:8px}.pm .btn{font-size:20px}
footer{position:fixed;left:0;right:0;bottom:0;background:#0d0f12ee;border-top:1px solid var(--line);padding:10px 16px}
footer div{max-width:560px;margin:0 auto;display:flex;gap:10px;align-items:center}
#toast{flex:1;font-size:13px;color:var(--dim)}
details>summary{list-style:none}details>summary::-webkit-details-marker{display:none}
</style></head><body><main>
<header><span class="dot" id="dot"></span><h1>Improv DSP</h1><span class="sys" id="sys">connecting…</span></header>
<button class="mute" id="mute">MUTE</button>

<section><h2>Meters</h2>
<div class="meter"><div class="lab"><span>Mic in (peak) — aim for shouts near the line</span><span id="inv">–</span></div>
<div class="bar"><i id="inb"></i><b style="left:80%"></b></div></div>
<div class="meter"><div class="lab"><span>Output (peak)</span><span id="outv">–</span></div>
<div class="bar"><i id="outb"></i></div></div>
<div class="chips"><span class="chip" id="cSpeech">SPEECH</span><span class="chip" id="cHowl">FEEDBACK?</span>
<span class="chip" id="cLim">LIMIT</span><span class="chip" id="cRing">RING-OUT</span><span class="chip" id="cRec">REC</span></div>
<div class="grid">
<div class="kv"><span>Total gain</span><strong id="kTot">–</strong></div>
<div class="kv"><span>AGC</span><strong id="kAgc">–</strong></div>
<div class="kv"><span>Expander</span><strong id="kExp">–</strong></div>
<div class="kv"><span>Interlock</span><strong id="kInt">–</strong></div>
<div class="kv"><span>Voice lvl</span><strong id="kVoice">–</strong></div>
<div class="kv"><span>Noise</span><strong id="kNoise">–</strong></div>
<div class="kv"><span>Events</span><strong id="kEv">–</strong></div>
<div class="kv"><span>Limiter</span><strong id="kLim">–</strong></div>
</div></section>

<section id="sLevel"><h2>Level</h2></section>
<section id="sRing"><h2>Ring-out</h2><div id="ringBox"></div></section>
<section id="sFb"><h2>Feedback</h2></section>
<section id="sOut"><h2>Speakers</h2></section>
<section id="sRec"><h2>Recording</h2></section>
<section><details><summary><h2>Advanced <span>▾</span></h2></summary><div id="sAdv"></div>
<button class="btn danger" data-act="clear_fixed">Clear fixed (rung-out) notches</button></details></section>
</main>
<footer><div><span id="toast">Changes apply instantly. Save to keep them after a restart.</span>
<button class="btn primary" style="width:auto" data-act="save">Save settings</button></div></footer>
<script>
const $=id=>document.getElementById(id);
let META={},P={},touched=0;
const LAYOUT={
 sLevel:[['s','gain_db','Gain','dB',.5],['s','ceiling_db','Ceiling (ring point − 6)','dB',.5],
  ['t','agc_on','Auto level (AGC)'],['s','agc_max_boost_db','AGC max boost','dB',.5],
  ['s','agc_ref_dbfs','AGC reference voice level','dBFS',1],['a','set_reference','Use the current voice as reference'],
  ['t','exp_on','Room expander (quieter between lines)']],
 sFb:[['t','shift_on','Frequency shifter'],['s','shift_hz','Shift amount','Hz',.5],
  ['t','afs_on','Automatic feedback notches'],['notches'],['a','clear_dynamic','Clear automatic notches']],
 sOut:[['t','front_on','Front speaker (left output)'],['s','front_trim_db','Front trim','dB',.5],['s','front_delay_ms','Front delay','ms',.5],
  ['t','fill_on','Fill speaker (right output)'],['s','fill_trim_db','Fill trim','dB',.5],['s','fill_delay_ms','Fill delay','ms',.5],
  ['h','Fill delay ≈ its distance from the stage in feet ÷ 1.13 (30 ft ≈ 27 ms). The processing delay already adds the few ms that keep voices sounding like they come from the stage. Keep the fill a few dB quieter than the front.']],
 sRec:[['t','record_on','Record mic + output to WAV'],['recstat']]
};
const used=new Set(['mute','ringout_mode']);
for(const k in LAYOUT)for(const it of LAYOUT[k])if(it[0]=='s'||it[0]=='t')used.add(it[1]);

function post(url,body){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.json()).catch(()=>({ok:false,msg:'No connection'}))}
function setP(n,v){P[n]=v;touched=Date.now();post('/api/set',encodeURIComponent(n)+'='+v)}
function act(a){post('/api/action','do='+a).then(r=>{toast(r.msg||'');poll(true)})}
function toast(m){$('toast').textContent=m}
function fmt(v,u){return (Math.round(v*10)/10).toFixed(1)+(u?' '+u:'')}
function dec(step){return step<1?1:0}

const ctl={};
function slider(host,n,label,unit,step){
 const m=META[n];if(!m)return;const d=document.createElement('div');d.className='row';
 d.innerHTML=`<div class="top"><label>${label}</label><output></output></div><div class="sl"><button>−</button><input type="range" min="${m.lo}" max="${m.hi}" step="${step}"><button>+</button></div><div class="help">${m.help}</div>`;
 const inp=d.querySelector('input'),out=d.querySelector('output'),[mi,pl]=d.querySelectorAll('button');
 let t=null;const send=v=>{v=Math.min(m.hi,Math.max(m.lo,v));inp.value=v;out.textContent=fmt(v,unit);clearTimeout(t);t=setTimeout(()=>setP(n,v),60)};
 inp.oninput=()=>{touched=Date.now();send(parseFloat(inp.value))};
 mi.onclick=()=>send(parseFloat(inp.value)-step);pl.onclick=()=>send(parseFloat(inp.value)+step);
 ctl[n]=v=>{inp.value=v;out.textContent=fmt(v,unit)};host.appendChild(d);
}
function toggle(host,n,label){
 const d=document.createElement('div');d.className='tog';d.innerHTML=`<span>${label}</span><button>OFF</button>`;
 const b=d.querySelector('button');b.onclick=()=>{const v=P[n]>=.5?0:1;setP(n,v);ctl[n](v)};
 ctl[n]=v=>{b.classList.toggle('on',v>=.5);b.textContent=v>=.5?'ON':'OFF'};host.appendChild(d);
}
function build(){
 for(const k in LAYOUT){const host=$(k);for(const it of LAYOUT[k]){
  if(it[0]=='s')slider(host,it[1],it[2],it[3],it[4]);
  else if(it[0]=='t')toggle(host,it[1],it[2]);
  else if(it[0]=='a'){const b=document.createElement('button');b.className='btn';b.textContent=it[2];b.onclick=()=>act(it[1]);host.appendChild(b)}
  else if(it[0]=='h'){const p=document.createElement('p');p.className='hint';p.textContent=it[1];host.appendChild(p)}
  else if(it[0]=='notches'){const d=document.createElement('div');d.id='notches';host.appendChild(d)}
  else if(it[0]=='recstat'){const p=document.createElement('p');p.className='hint';p.id='recstat';host.appendChild(p)}
 }}
 const adv=$('sAdv');
 for(const n in META){if(used.has(n))continue;const m=META[n];
  if(m.lo==0&&m.hi==1)toggle(adv,n,n);else{const r=m.hi-m.lo;slider(adv,n,n,'',r<=1?.01:r<=40?.5:1)}}
 document.querySelectorAll('[data-act]').forEach(b=>b.onclick=()=>act(b.dataset.act));
 $('mute').onclick=()=>{const v=P.mute>=.5?0:1;setP('mute',v);paintMute(v)};
}
function paintMute(v){const b=$('mute');b.classList.toggle('on',v>=.5);b.textContent=v>=.5?'MUTED — tap to unmute':'MUTE'}
function bar(id,db){$(id).style.width=Math.max(0,Math.min(100,(db+60)/60*100))+'%';
 $(id).style.background=db>-3?'var(--bad)':db>-12?'var(--warn)':'var(--ok)'}
function chip(id,on,cls){const c=$(id);c.className='chip'+(on?' '+cls:'')}

let ringWas=null;
function ringUI(on){
 if(on===ringWas)return;ringWas=on;const b=$('ringBox');
 if(!on){b.innerHTML=`<p class="hint">Do this at sound check with the mic and speakers where they'll be for the show. It finds the gain where feedback starts and sets a safe ceiling 6 dB below it.</p><button class="btn" data-a="start_ringout">Start ring-out</button>`}
 else{b.innerHTML=`<div class="ring"><p class="hint">1. Have someone talk from the farthest spot on stage.<br>2. Raise the gain in small steps. The first few rings get notched automatically.<br>3. When it rings again and won't settle, press <b>Mark ring point</b>.</p>
 <div class="big" id="ringGain">–</div><div class="pm"><button class="btn" id="rgm">− 0.5</button><button class="btn" id="rgp">+ 0.5</button></div>
 <button class="btn primary" data-a="mark_ring">Mark ring point</button><button class="btn" data-a="cancel_ringout">Cancel</button></div>`;
 $('rgm').onclick=()=>setP('gain_db',P.gain_db-.5);$('rgp').onclick=()=>setP('gain_db',P.gain_db+.5)}
 b.querySelectorAll('[data-a]').forEach(x=>x.onclick=()=>act(x.dataset.a));
}
function render(s){
 $('dot').classList.toggle('on',s.ok==1);
 $('sys').textContent=s.ok==1?`${fmt(s.lat,'ms')} · cpu ${Math.round(s.cpu)}% · xruns ${s.xruns}`:'no audio device';
 bar('inb',s.in_peak);$('inv').textContent=fmt(s.in_peak,'dBFS');bar('outb',s.out_peak);$('outv').textContent=fmt(s.out_peak,'dBFS');
 chip('cSpeech',s.speech,'ok');chip('cHowl',s.suspect,'warn');chip('cLim',s.lim<-0.5,'bad');chip('cRing',s.p.ringout_mode>=.5,'warn');chip('cRec',s.rec,'bad');
 $('kTot').textContent=fmt(s.total);$('kAgc').textContent=fmt(s.agc);$('kExp').textContent=fmt(s.exp);$('kInt').textContent=fmt(-s.interlock);
 $('kVoice').textContent=fmt(s.speech_lvl);$('kNoise').textContent=fmt(s.noise);$('kEv').textContent=s.events;$('kLim').textContent=fmt(s.lim);
 const fresh=Date.now()-touched>1500;
 for(const n in s.p){P[n]=s.p[n];if(fresh&&ctl[n])ctl[n](s.p[n])}
 paintMute(P.mute);ringUI(P.ringout_mode>=.5);if($('ringGain'))$('ringGain').textContent=fmt(P.gain_db,'dB');
 let h='<table><tr><th>Notch</th><th>Hz</th><th>dB</th></tr>';
 for(const f of s.fixed)h+=`<tr><td>fixed</td><td>${f.hz}</td><td>${f.db}</td></tr>`;
 for(const d of s.dyn)h+=`<tr><td>auto ${d.slot+1}</td><td>${d.hz}</td><td>${d.db}</td></tr>`;
 if(!s.fixed.length&&!s.dyn.length)h+='<tr><td colspan=3 style="color:var(--dim)">none yet</td></tr>';
 h+='</table>';if(s.log.length){h+='<p class="hint">Recent catches: '+s.log.slice(-5).reverse().map(e=>`${e.hz} Hz${e.ext==1?' (outside tone, ignored)':e.deep==1?' (deeper)':''}`).join(', ')+'</p>'}
 if($('notches'))$('notches').innerHTML=h;
 if($('recstat'))$('recstat').textContent=s.rec?`Recording ${Math.floor(s.rec_s/60)}:${String(Math.floor(s.rec_s%60)).padStart(2,'0')}${s.rec_drop?' · dropped blocks '+s.rec_drop:''}`:'Not recording. Recordings of real shows let you tune the feedback detector at home.';
}
let timer=null;
function poll(once){fetch('/api/status').then(r=>r.json()).then(render).catch(()=>{$('dot').classList.remove('on');$('sys').textContent='no connection'})
 .finally(()=>{if(!once){clearTimeout(timer);timer=setTimeout(poll,300)}})}
fetch('/api/params').then(r=>r.json()).then(m=>{META=m;build();poll()});
</script></body></html>
)HTML";

}  // namespace imp
