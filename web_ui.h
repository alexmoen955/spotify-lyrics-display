// The web page served by the device (single page, no external files).
#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lyrics Display</title>
<style>
:root{--bg1:#371955;--bg2:#0a414b;--card:rgba(255,255,255,.07);--line:rgba(255,255,255,.12);
--text:#f4f1f8;--dim:#b9b2c6;--accent:#6ee6aa;--warn:#ffb86b;--bad:#ff7a8a}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
background:linear-gradient(160deg,var(--bg1),var(--bg2)) fixed}
main{max-width:640px;margin:0 auto;padding:20px 16px 48px}
header{display:flex;align-items:baseline;gap:10px;margin:4px 0 18px}
h1{font-size:22px;margin:0}h2{font-size:16px;margin:0 0 10px}
.ver{color:var(--dim);font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin:0 0 14px;
backdrop-filter:blur(8px)}
.hide{display:none!important}
.dim{color:var(--dim)}.small{font-size:13px}.ok{color:var(--accent)}.err{color:var(--bad)}.warn{color:var(--warn)}
button,.btn{font:inherit;border:0;border-radius:10px;padding:9px 14px;cursor:pointer;color:#0c2a22;
background:var(--accent);font-weight:600;text-decoration:none;display:inline-block}
button.ghost,.btn.ghost{background:transparent;color:var(--text);border:1px solid var(--line);font-weight:500}
button.danger{background:transparent;color:var(--bad);border:1px solid rgba(255,122,138,.4);font-weight:500}
button:disabled{opacity:.5;cursor:default}
input{font:inherit;width:100%;padding:9px 11px;border-radius:10px;border:1px solid var(--line);
background:rgba(0,0,0,.25);color:var(--text);margin:4px 0 10px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.list{list-style:none;margin:6px 0 12px;padding:0}
.list li{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:8px 10px;border-radius:10px;
border:1px solid var(--line);margin-bottom:6px}
.list li.pick{cursor:pointer}.list li.pick:hover,.list li.sel{border-color:var(--accent)}
.bars{letter-spacing:-2px;color:var(--accent)}
code{background:rgba(0,0,0,.3);padding:2px 6px;border-radius:6px;font-size:13px;word-break:break-all}
.np{display:flex;gap:14px;align-items:center}
.np img{width:96px;height:96px;border-radius:10px;object-fit:cover;background:rgba(0,0,0,.25);flex:none}
.np .t{font-weight:700;font-size:17px}
.bar{height:4px;background:rgba(255,255,255,.18);border-radius:2px;margin-top:8px;overflow:hidden}
.bar>i{display:block;height:100%;background:var(--accent);width:0}
pre{background:rgba(0,0,0,.35);border-radius:10px;padding:10px;height:260px;overflow:auto;font:12px/1.4 ui-monospace,Menlo,monospace;
white-space:pre-wrap;word-break:break-word;margin:8px 0 0}
ol{padding-left:20px;margin:6px 0 12px}ol li{margin-bottom:6px}
.big{font-size:17px}
</style></head><body><main>
<header><h1>Lyrics Display</h1><span class="ver" id="ver"></span></header>

<section class="card hide" id="welcome">
  <h2>Welcome 👋</h2>
  <p>Let's get your display online. First pick the Wi-Fi network it should use. After that you'll connect your Spotify account.</p>
  <button onclick="startSetup()">Get started</button>
</section>

<section class="card hide" id="joined">
  <h2>Connected to <span id="joinedSsid"></span> 🎉</h2>
  <p>Next step: connect Spotify.</p>
  <ol><li>Switch your phone or computer back to your normal Wi-Fi.</li>
  <li>Open <a class="ok" id="joinedLink" href="#"></a><br><span class="small dim">(or <span id="joinedIp"></span>)</span></li></ol>
  <button onclick="post('/api/setup/done').then(()=>{hide('joined')})">Done</button>
</section>

<section class="card hide" id="nowCard">
  <h2>Now playing</h2>
  <div class="np"><img id="cover" alt="">
    <div style="flex:1;min-width:0"><div class="t" id="npTitle"></div><div class="dim" id="npArtist"></div>
    <div class="bar"><i id="npBar"></i></div><div class="small dim" id="npInfo" style="margin-top:6px"></div></div></div>
</section>

<section class="card hide" id="spotifyCard">
  <h2>Spotify</h2>
  <div id="spNoNet" class="hide"><p class="dim">Connect the display to your Wi-Fi first.</p></div>
  <div id="spClient" class="hide">
    <ol>
      <li>Open <a class="ok" href="https://developer.spotify.com/dashboard" target="_blank" rel="noopener">developer.spotify.com/dashboard</a>, pick your app (or create one with <b>Web API</b> ticked) and click <b>Settings → Edit</b>.</li>
      <li>Add this <b>Redirect URI</b> and save:<br><code id="redir"></code> <button class="ghost small" onclick="copyRedir()">Copy</button></li>
      <li>Paste the app's <b>Client ID</b> here:</li>
    </ol>
    <input id="cid" placeholder="Client ID" autocomplete="off">
    <button onclick="saveCid()">Save</button> <span id="cidMsg" class="small"></span>
  </div>
  <div id="spLogin" class="hide">
    <p>Ready to connect your Spotify account.</p>
    <div class="row"><a class="btn" href="/spotify/login">Connect Spotify</a>
    <button class="ghost" onclick="show('spClient');hide('spLogin')">Change Client ID</button></div>
    <p class="small dim">Make sure <code id="redir2"></code> is listed as a Redirect URI in your Spotify app.</p>
  </div>
  <div id="spBusy" class="hide"><p>Connecting to Spotify…</p></div>
  <div id="spOn" class="hide">
    <p class="big">Connected as <b id="spUser"></b> <span class="ok">●</span></p>
    <button class="ghost" onclick="confirm('Disconnect this Spotify account?')&&post('/api/spotify/logout')">Disconnect</button>
  </div>
  <p id="spMsg" class="err small"></p>
</section>

<section class="card hide" id="wifiCard">
  <h2>Wi-Fi</h2>
  <p id="wifiNow" class="dim"></p>
  <div id="savedBox"><div class="small dim">Saved networks (the display joins the strongest one):</div><ul class="list" id="saved"></ul></div>
  <div class="row" style="justify-content:space-between"><b>Add a network</b>
    <button class="ghost small" onclick="scan(true)">Scan again</button></div>
  <ul class="list" id="scan"><li class="dim">Scanning…</li></ul>
  <input id="ssid" placeholder="Network name" autocomplete="off">
  <input id="pass" type="password" placeholder="Password" autocomplete="off">
  <button id="joinBtn" onclick="join()">Connect</button> <span id="joinMsg" class="small"></span>
  <p class="small dim">Only 2.4 GHz networks work.</p>
</section>

<section class="card hide" id="updCard">
  <h2>Updates</h2>
  <p id="updText" class="dim"></p>
  <div class="bar hide" id="updBarBox"><i id="updBar"></i></div>
  <div class="row" style="margin-top:10px"><button class="ghost" onclick="post('/api/update/check')">Check for updates</button>
  <button id="updInstall" class="hide" onclick="confirm('Install the update? The display restarts when it is done.')&&post('/api/update/install')">Install update</button></div>
</section>

<section class="card hide" id="logCard">
  <div class="row" style="justify-content:space-between"><h2 style="margin:0">Live logs</h2>
  <div class="row"><button class="ghost small" id="pauseBtn" onclick="togglePause()">Pause</button>
  <button class="ghost small" onclick="$('#log').textContent=''">Clear</button>
  <button class="ghost small" onclick="dl()">Download</button></div></div>
  <pre id="log"></pre>
</section>

<section class="card hide" id="resetCard">
  <h2>Device</h2>
  <p class="small dim" id="devInfo"></p>
  <div class="row">
    <button class="ghost" onclick="confirm('Restart the display?')&&post('/api/restart')">Restart</button>
    <button class="danger" onclick="reset('wifi','Forget all Wi-Fi networks? The display starts its setup hotspot again.')">Forget Wi-Fi</button>
    <button class="danger" onclick="reset('spotify','Disconnect Spotify and restart?')">Reset Spotify</button>
    <button class="danger" onclick="reset('all','Erase ALL settings (Wi-Fi and Spotify) and start over?')">Factory reset</button>
  </div>
  <p class="small dim">No access to this page? Hold the BOOT button on the device for 10 seconds to factory-reset it.</p>
</section>
</main>
<script>
const $=s=>document.querySelector(s);
const show=id=>document.getElementById(id).classList.remove('hide');
const hide=id=>document.getElementById(id).classList.add('hide');
const vis=(id,on)=>on?show(id):hide(id);
let st=null,setupStarted=false,logFrom=0,paused=false,artVer=-1,lastScan=0;
async function post(url,data){const r=await fetch(url,{method:'POST',body:new URLSearchParams(data||{})});
 if(!r.ok)throw new Error(await r.text());poll();return r.json()}
const fmt=ms=>{const s=Math.floor(ms/1000);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
const bars=r=>r>-55?'▮▮▮▮':r>-67?'▮▮▮▯':r>-77?'▮▮▯▯':'▮▯▯▯';
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function startSetup(){setupStarted=true;hide('welcome');show('wifiCard');scan(true)}
function reset(what,q){if(confirm(q))post('/api/reset',{what}).then(()=>alert('Done. The display is restarting.'))}
function copyRedir(){navigator.clipboard&&navigator.clipboard.writeText(st.spotify.redirectUri)}
async function saveCid(){const id=$('#cid').value.trim();$('#cidMsg').textContent='';
 try{await post('/api/spotify/client',{id})}catch(e){$('#cidMsg').textContent=e.message;$('#cidMsg').className='small err'}}
async function join(){const ssid=$('#ssid').value.trim(),pass=$('#pass').value;if(!ssid)return;
 $('#joinMsg').textContent='Connecting…';$('#joinMsg').className='small dim';
 try{await post('/api/wifi',{ssid,pass})}catch(e){$('#joinMsg').textContent=e.message;$('#joinMsg').className='small err'}}
function pick(ssid){$('#ssid').value=ssid;$('#pass').value='';$('#pass').focus();
 document.querySelectorAll('#scan li').forEach(li=>li.classList.toggle('sel',li.dataset.s===ssid))}
async function scan(refresh){try{const r=await(await fetch('/api/scan'+(refresh?'?refresh=1':''))).json();
 const ul=$('#scan');ul.innerHTML='';
 if(!r.networks.length)ul.innerHTML='<li class="dim">'+(r.scanning?'Scanning…':'No networks found')+'</li>';
 r.networks.forEach(n=>{const li=document.createElement('li');li.className='pick';li.dataset.s=n.ssid;
  li.innerHTML='<span>'+esc(n.ssid)+(n.lock?' 🔒':'')+'</span><span class="bars">'+bars(n.rssi)+'</span>';
  li.onclick=()=>pick(n.ssid);ul.appendChild(li)});
 if(r.scanning)setTimeout(()=>scan(false),1500)}catch(e){}}
function togglePause(){paused=!paused;$('#pauseBtn').textContent=paused?'Resume':'Pause'}
function dl(){const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([$('#log').textContent]));
 a.download='lyrics-display-log.txt';a.click()}
async function logs(){if(!paused){try{const r=await fetch('/api/logs?from='+logFrom);
 const n=r.headers.get('X-Log-Next');const t=await r.text();
 if(n!==null){if(+n<logFrom)$('#log').textContent+='\n--- device restarted ---\n';logFrom=+n}
 if(t){const p=$('#log'),stick=p.scrollTop+p.clientHeight>=p.scrollHeight-20;p.textContent+=t;
  if(p.textContent.length>60000)p.textContent=p.textContent.slice(-40000);if(stick)p.scrollTop=p.scrollHeight}}catch(e){}}
 setTimeout(logs,1000)}
function render(){const s=st,w=s.wifi,sp=s.spotify,u=s.update,np=s.now;
 $('#ver').textContent=s.version;
 const setupMode=s.hotspot&&!w.connected;
 vis('welcome',setupMode&&!setupStarted&&!w.saved.length);
 vis('wifiCard',!setupMode||setupStarted||w.saved.length);
 vis('joined',s.hotspot&&w.connected);
 if(w.connected){$('#joinedSsid').textContent=w.ssid;$('#joinedLink').textContent='http://'+s.host;
  $('#joinedLink').href='http://'+w.ip;$('#joinedIp').textContent='http://'+w.ip}
 const online=w.connected&&!s.hotspot;
 ['nowCard','updCard','logCard','resetCard'].forEach(id=>vis(id,online));
 vis('spotifyCard',!s.hotspot||w.connected);
 // Wi-Fi
 $('#wifiNow').innerHTML=w.connected?'Connected to <b>'+esc(w.ssid)+'</b> <span class="bars">'+bars(w.rssi)+'</span> · '+w.ip
  :(s.hotspot?'Not connected yet. Pick your network below.':'Looking for a saved network…');
 vis('savedBox',w.saved.length);
 $('#saved').innerHTML=w.saved.map(n=>'<li><span>'+esc(n)+(n===w.ssid?' <span class="ok small">connected</span>':'')+
  '</span><button class="ghost small" data-s="'+esc(n)+'">Remove</button></li>').join('');
 document.querySelectorAll('#saved button').forEach(b=>b.onclick=()=>confirm('Forget '+b.dataset.s+'?')&&post('/api/wifi/remove',{ssid:b.dataset.s}));
 const jm=$('#joinMsg');
 if(w.tryState==='trying'){jm.textContent='Connecting to '+w.trySsid+'…';jm.className='small dim'}
 else if(w.tryState==='ok'){jm.textContent='Connected and saved ✓';jm.className='small ok'}
 else if(w.tryState==='fail'){jm.textContent=w.tryMsg;jm.className='small err'}
 $('#joinBtn').disabled=w.tryState==='trying';
 // Spotify
 $('#redir').textContent=sp.redirectUri;$('#redir2').textContent=sp.redirectUri;
 ['spNoNet','spClient','spLogin','spBusy','spOn'].forEach(hide);
 if(!w.connected)show('spNoNet');
 else if(sp.state==='needs_client')show('spClient');
 else if(sp.state==='needs_login'||sp.state==='error'){if($('#spClient').classList.contains('hide'))show('spLogin')}
 else if(sp.state==='connecting')show('spBusy');
 else if(sp.state==='connected'){show('spOn');$('#spUser').textContent=sp.user||'your account'}
 if(sp.clientId&&!$('#cid').value)$('#cid').value=sp.clientId;
 $('#spMsg').textContent=sp.state==='error'?sp.msg:'';
 // Now playing
 if(np.active){$('#npTitle').textContent=np.title;$('#npArtist').textContent=np.artist;
  $('#npBar').style.width=(np.dur?100*np.pos/np.dur:0)+'%';
  $('#npInfo').textContent=(np.playing?'Playing':'Paused')+' · '+fmt(np.pos)+' / '+fmt(np.dur)+' · lyrics: '+np.lyrics;
  if(np.art!==artVer){artVer=np.art;$('#cover').src='/art.jpg?v='+artVer}}
 else{$('#npTitle').textContent='Nothing playing';$('#npArtist').textContent='';$('#npBar').style.width='0';
  $('#npInfo').textContent=sp.state==='connected'?'Play something on Spotify.':'';$('#cover').removeAttribute('src');artVer=-1}
 // Updates
 const ut={idle:'',checking:'Checking…',latest:'You have the latest version ✓',available:'Version '+u.latest+' is available.',
  installing:'Installing '+u.latest+'… '+u.progress+'% (don\'t unplug the display)',error:u.msg,done:'Update installed. Restarting…'}[u.state]||'';
 $('#updText').textContent='Installed: '+s.version+(ut?' · '+ut:'');
 vis('updInstall',u.state==='available');vis('updBarBox',u.state==='installing');$('#updBar').style.width=u.progress+'%';
 $('#devInfo').textContent='http://'+s.host+' · up '+Math.floor(s.uptime/60)+' min · free memory '+Math.round(s.heap/1024)+' KB';
}
async function poll(){try{st=await(await fetch('/api/status')).json();render()}catch(e){}}
const q=new URLSearchParams(location.search);
if(q.get('spotify_error'))setTimeout(()=>{$('#spMsg').textContent=q.get('spotify_error')},500);
if(q.has('spotify')||q.has('spotify_error'))history.replaceState(null,'','/');
poll();setInterval(poll,2000);logs();scan(false);
</script></body></html>)HTML";
