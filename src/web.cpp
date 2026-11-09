#include "web.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "params.h"

namespace xo {

namespace {

const char* kPage = R"XOHTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>XO//WAVE</title>
<style>
@font-face{font-family:Anton;src:url(/fonts/Anton-Regular.ttf)}
@font-face{font-family:Plex;src:url(/fonts/IBMPlexMono-Medium.ttf)}
@font-face{font-family:Plex;font-weight:600;src:url(/fonts/IBMPlexMono-SemiBold.ttf)}
:root{--black:#0b0b0b;--white:#f2f0eb;--grey:#8a8780;--rule:#2a2a28;--rule2:#3a3a38;--signal:#ff3b1f}
*{box-sizing:border-box}
body{margin:0;background:var(--black);color:var(--white);font:14px/1.5 Plex,ui-monospace,monospace;-webkit-font-smoothing:antialiased}
header{position:sticky;top:0;z-index:2;background:var(--black);border-bottom:2px solid var(--white);padding:10px 14px}
.logo{font-family:Anton,Impact,sans-serif;font-size:30px;letter-spacing:.02em;line-height:1}
.logo i{font-style:normal;color:var(--grey)}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.14em;color:var(--grey)}
button,select{font:600 12px Plex,monospace;text-transform:uppercase;letter-spacing:.12em;color:var(--white);background:var(--black);border:2px solid var(--white);padding:9px 12px;border-radius:0}
button:active,button.on{background:var(--white);color:var(--black)}
select{flex:1;min-width:0}
#preset{font-family:Anton,Impact,sans-serif;font-size:22px;letter-spacing:.02em;text-transform:none;flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
nav{display:flex;overflow-x:auto;gap:0;border-bottom:1px solid var(--rule);position:sticky;top:96px;background:var(--black);z-index:1}
nav button{border:0;border-right:1px solid var(--rule);padding:12px 14px;white-space:nowrap;color:var(--grey)}
nav button.on{background:var(--white);color:var(--black)}
main{padding:8px 14px 40px}
h2{font-family:Anton,Impact,sans-serif;font-weight:400;font-size:22px;letter-spacing:.01em;margin:18px 0 6px;text-transform:uppercase}
.p{display:grid;grid-template-columns:110px 1fr 74px;gap:10px;align-items:center;padding:7px 0;border-bottom:1px solid var(--rule)}
.p .v{text-align:right;font-weight:600}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:28px;background:transparent}
input[type=range]::-webkit-slider-runnable-track{height:4px;background:var(--rule2)}
input[type=range]::-moz-range-track{height:4px;background:var(--rule2)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;margin-top:-9px;background:var(--signal);border:0;border-radius:0}
input[type=range]::-moz-range-thumb{width:22px;height:22px;background:var(--signal);border:0;border-radius:0}
.keys{display:flex;height:140px;margin-top:12px;position:relative;user-select:none;-webkit-user-select:none;touch-action:none}
.k{flex:1;background:var(--white);border:1px solid var(--rule2)}
.k.b{position:absolute;width:9%;height:60%;background:var(--black);z-index:1}
.k.down{background:var(--signal)!important}
.status{color:var(--grey);font-size:11px;letter-spacing:.1em;text-transform:uppercase;margin-top:6px}
footer{color:var(--grey);font-size:11px;letter-spacing:.14em;text-transform:uppercase;padding:20px 14px}
</style></head><body>
<header>
 <div class="row" style="justify-content:space-between"><div class="logo">XO<i>//</i>WAVE</div><div class="status" id="st"></div></div>
 <div class="row" style="margin-top:8px"><button id="prev">&lt;</button><div id="preset">-</div><button id="next">&gt;</button><select id="plist"></select></div>
</header>
<nav id="tabs"></nav>
<main id="main"></main>
<footer>XOXO//AUDIO//LABS &middot; BUILT BY HAND. PLAYED LOUD. &middot; <span id="ver"></span></footer>
<script>
const G=[
 ["OSC",[["OSC A",n=>n.startsWith("a_")],["OSC B",n=>n.startsWith("b_")],["SUB + NOISE",n=>["sub_level","sub_octave","sub_shape","noise","spread"].includes(n)]]],
 ["FILTER",[["FILTER",n=>["filter_mode","cutoff","resonance","filter_env","filter_key","drive","vel_filter"].includes(n)]]],
 ["ENV",[["AMP ENVELOPE",n=>n.startsWith("amp_")],["MOD ENVELOPE",n=>["mod_attack","mod_decay","mod_sustain","mod_release","menv_pos"].includes(n)]]],
 ["MOD",[["LFO 1",n=>["lfo_rate","lfo_shape","lfo_sync","lfo_pos","lfo_cutoff","lfo_pitch"].includes(n)],["LFO 2",n=>n.startsWith("lfo2_")],["MATRIX",n=>/^mod[1-4]_/.test(n)]]],
 ["FX",[["DRIVE",n=>n=="fx_drive"],["CHORUS",n=>n.startsWith("chorus_")],["DELAY",n=>n.startsWith("delay_")],["REVERB",n=>n.startsWith("reverb_")]]],
 ["PLAY",[["ARP + CHORD",n=>n.startsWith("arp_")||n=="chord"],["PERFORM",n=>["vel_amp","glide","bend_range","volume"].includes(n)]]],
 ["MASTER",[]]];
let S=null,tab=+(localStorage.getItem("xw.tab")||0),dragging=null,built=-1;
const $=id=>document.getElementById(id);
const get=u=>fetch(u,{cache:"no-store"}).then(r=>r.json()).catch(()=>null);
let pend={},timer=null;
function send(u){fetch(u,{cache:"no-store"}).catch(()=>{});}
function setP(name,n){pend[name]=n;if(!timer)timer=setTimeout(()=>{for(const k in pend)send(`/api/set?p=${k}&n=${pend[k].toFixed(4)}`);pend={};timer=null;},40);}
function tabs(){$("tabs").innerHTML="";G.forEach((g,i)=>{const b=document.createElement("button");b.textContent=g[0];if(i==tab)b.className="on";b.onclick=()=>{tab=i;localStorage.setItem("xw.tab",i);tabs();build();};$("tabs").append(b);});}
function slider(label,val,text,step,oninput){
 const d=document.createElement("div");d.className="p";
 d.innerHTML=`<div class="label">${label}</div><input type=range min=0 max=1000 step=${step}><div class="v"></div>`;
 const r=d.querySelector("input"),v=d.querySelector(".v");r.value=Math.round(val*1000);v.textContent=text;
 r.addEventListener("pointerdown",()=>dragging=r);r.addEventListener("pointerup",()=>dragging=null);r.addEventListener("change",()=>dragging=null);
 r.oninput=()=>oninput(r.value/1000,v);return [d,r,v];}
let ctl={};
function build(){
 if(!S)return;built=tab;const m=$("main");m.innerHTML="";ctl={};
 if(G[tab][0]=="MASTER"){
  m.innerHTML="<h2>Master</h2>";
  const [d1]=slider("VOLUME",S.master,Math.round(S.master*100)+"%",1,(x,v)=>{v.textContent=Math.round(x*100)+"%";send(`/api/master?v=${x}`);});m.append(d1);
  const [d2,r2,v2]=slider("TEMPO",(S.tempo-30)/270,S.tempo.toFixed(0)+" BPM",1,(x,v)=>{const t=Math.round(30+x*270);v.textContent=t+" BPM";send(`/api/tempo?v=${t}`);});m.append(d2);ctl.tempo=[r2,v2];
  const h=document.createElement("h2");h.textContent="Keys";m.append(h);m.append(keys());return;}
 for(const [title,f] of G[tab][1]){
  const h=document.createElement("h2");h.textContent=title;m.append(h);
  for(const p of S.params){if(!f(p.name))continue;
   const steps=p.int?Math.max(1,p.max-p.min):1000;
   const [d,r,v]=slider(p.name.replace(/_/g," ").toUpperCase(),p.n,p.t,p.int?1000/steps:1,(x,vv)=>{if(p.int){x=Math.round(x*steps)/steps;}setP(p.name,x);vv.textContent="...";});
   if(p.int){r.step=String(1000/steps);}
   m.append(d);ctl[p.name]=[r,v];}}}
function keys(){
 const k=document.createElement("div");k.className="keys";const base=60;const whites=[0,2,4,5,7,9,11,12];const blacks=[[1,0],[3,1],[6,3],[8,4],[10,5]];
 const on=(n,el)=>{el.classList.add("down");send(`/api/note?n=${base+n}&v=100`);},off=(n,el)=>{el.classList.remove("down");send(`/api/noteoff?n=${base+n}`);};
 whites.forEach(n=>{const e=document.createElement("div");e.className="k";e.onpointerdown=ev=>{ev.preventDefault();on(n,e)};e.onpointerup=e.onpointerleave=()=>{if(e.classList.contains("down"))off(n,e)};k.append(e);});
 blacks.forEach(([n,after])=>{const e=document.createElement("div");e.className="k b";e.style.left=((after+1)*12.5-4.5)+"%";e.onpointerdown=ev=>{ev.preventDefault();on(n,e)};e.onpointerup=e.onpointerleave=()=>{if(e.classList.contains("down"))off(n,e)};k.append(e);});
 return k;}
function render(){
 if(!S)return;$("ver").textContent="v"+S.version;
 const cur=S.presets[S.preset]||"-";$("preset").textContent=cur;
 const pl=$("plist");if(pl.options.length!=S.presets.length){pl.innerHTML="";S.presets.forEach((n,i)=>{const o=document.createElement("option");o.value=i;o.textContent=String(i).padStart(2,"0")+"  "+n;pl.append(o);});}
 pl.value=S.preset;
 $("st").textContent=(S.ext?"MIDI CLOCK ":"")+S.bpm.toFixed(1)+" BPM · "+S.voices+" VOICES"+(S.link==2?" · LINKED":"");
 if(built!=tab)build();
 for(const p of S.params){const c=ctl[p.name];if(!c)continue;if(dragging!==c[0]){c[0].value=Math.round(p.n*1000);}c[1].textContent=p.t;}
 if(ctl.tempo&&dragging!==ctl.tempo[0]){ctl.tempo[0].value=Math.round((S.tempo-30)/270*1000);ctl.tempo[1].textContent=S.tempo.toFixed(0)+" BPM";}}
async function poll(){const s=await get("/api/state");if(s){S=s;render();}setTimeout(poll,800);}
$("prev").onclick=()=>S&&send(`/api/preset?i=${(S.preset-1+S.presets.length)%S.presets.length}`);
$("next").onclick=()=>S&&send(`/api/preset?i=${(S.preset+1)%S.presets.length}`);
$("plist").onchange=e=>send(`/api/preset?i=${e.target.value}`);
tabs();poll();
</script></body></html>
)XOHTML";

// A query number, or def when it's missing, junk, nan or infinite.
float num(const std::string& text, float def) {
    if (text.empty()) return def;
    char* end = nullptr;
    float v = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || !isFiniteBits(v)) return def;
    return v;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

std::string queryValue(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) return part.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

void sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void reply(int fd, int code, const char* type, const std::string& body) {
    std::ostringstream h;
    h << "HTTP/1.0 " << code << (code == 200 ? " OK" : code == 404 ? " Not Found" : " Bad Request") << "\r\n"
      << "Content-Type: " << type << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    sendAll(fd, h.str() + body);
}

}  // namespace

bool WebServer::start(const WebConfig& cfg) {
    cfg_ = cfg;
    for (int port : {cfg.port, 8080}) {   // 80, or 8080 if 80 can't be had
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) continue;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(s, 8) == 0) {
            listen_ = s;
            port_ = port;
            break;
        }
        ::close(s);
    }
    if (listen_ < 0) return false;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void WebServer::stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (listen_ >= 0) ::close(listen_);
    listen_ = -1;
}

void WebServer::run() {
    while (!quit_.load()) {
        pollfd p{listen_, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        int fd = ::accept(listen_, nullptr, nullptr);
        if (fd < 0) continue;
        timeval tv{0, 300000};   // a client that connects and sends nothing can't hold the server up
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle(fd);
        ::close(fd);
    }
}

std::string WebServer::stateJson() const {
    std::ostringstream j;
    EngineStatus* st = cfg_.status;
    const int numTables = cfg_.tables ? static_cast<int>(cfg_.tables->size()) : 0;
    std::vector<const char*> names;
    if (cfg_.tables) for (const auto& t : *cfg_.tables) names.push_back(t.name.c_str());
    j << "{\"version\":\"" << jsonEscape(cfg_.version) << "\",\"params\":[";
    for (int i = 0; i < P_COUNT; ++i) {
        float v = st ? st->param[i].load() : kParams[i].def;
        char text[32];
        paramFormat(i, v, text, sizeof text, names.data(), numTables);
        float hi = (i == P_A_TABLE || i == P_B_TABLE) ? static_cast<float>(std::max(0, numTables - 1)) : kParams[i].max;
        j << (i ? "," : "") << "{\"name\":\"" << kParams[i].name << "\",\"n\":" << paramToNormalized(i, v, numTables)
          << ",\"t\":\"" << jsonEscape(text) << "\",\"min\":" << kParams[i].min << ",\"max\":" << hi
          << ",\"int\":" << (kParams[i].integer ? "true" : "false") << "}";
    }
    j << "],\"presets\":[";
    int n = cfg_.bank ? cfg_.bank->count() : 0;
    for (int i = 0; i < n; ++i) j << (i ? "," : "") << "\"" << jsonEscape(cfg_.bank->name(i)) << "\"";
    j << "],\"preset\":" << (st ? std::max(0, st->preset.load()) : 0) << ",\"master\":" << (st ? st->master.load() : 0.8f)
      << ",\"tempo\":" << (st ? st->tempo.load() : 120.0f) << ",\"bpm\":" << (st ? st->bpmNow.load() : 120.0f)
      << ",\"ext\":" << (st && st->extClock.load() ? "true" : "false") << ",\"voices\":" << (st ? st->voices.load() : 0)
      << ",\"link\":" << (st ? st->linkState.load() : 0) << "}";
    return j.str();
}

void WebServer::handle(int fd) {
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) return;
    buf[n] = 0;
    std::string req(buf);
    std::string method = req.substr(0, req.find(' '));
    size_t a = req.find(' ') + 1, b = req.find(' ', a);
    std::string target = req.substr(a, b - a);
    std::string path = target.substr(0, target.find('?'));
    std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
    if (method != "GET") { reply(fd, 400, "text/plain", "GET only\n"); return; }
    auto push = [this](CmdType t, int a1, float b1) { if (cfg_.cmds) cfg_.cmds->push(Cmd{t, static_cast<int16_t>(a1), b1}); };

    if (path == "/") { reply(fd, 200, "text/html; charset=utf-8", kPage); return; }
    if (path == "/api/state") { reply(fd, 200, "application/json", stateJson()); return; }
    if (path == "/api/set") {
        int id = findParam(queryValue(query, "p"));
        if (id < 0) { reply(fd, 400, "application/json", "{\"error\":\"unknown parameter\"}"); return; }
        const int numTables = cfg_.tables ? static_cast<int>(cfg_.tables->size()) : 0;
        std::string nv = queryValue(query, "n"), vv = queryValue(query, "v");
        if (nv.empty() && vv.empty()) { reply(fd, 400, "application/json", "{\"error\":\"n or v needed\"}"); return; }
        float v = !nv.empty() ? paramFromNormalized(id, num(nv, -1), numTables)
                              : paramClamp(id, num(vv, kParams[id].def), numTables);
        push(CmdType::SetParam, id, v);
        reply(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    if (path == "/api/preset") {
        int i = std::atoi(queryValue(query, "i").c_str());
        if (cfg_.bank && i >= 0 && i < cfg_.bank->count()) push(CmdType::Program, i, 0);
        reply(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    if (path == "/api/master" && cfg_.status) {
        cfg_.status->master.store(std::clamp(num(queryValue(query, "v"), cfg_.status->master.load()), 0.0f, 1.0f));
        reply(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    if (path == "/api/tempo" && cfg_.status) {
        cfg_.status->tempo.store(std::clamp(num(queryValue(query, "v"), cfg_.status->tempo.load()), 30.0f, 300.0f));
        reply(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    if (path == "/api/note" || path == "/api/noteoff") {
        int note = std::clamp(std::atoi(queryValue(query, "n").c_str()), 0, 127);
        int vel = std::clamp(std::atoi(queryValue(query, "v").c_str()), 1, 127);
        push(path == "/api/note" ? CmdType::NoteOn : CmdType::NoteOff, note, static_cast<float>(vel));
        reply(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    if (path.rfind("/fonts/", 0) == 0) {
        std::string file = path.substr(7);
        static const char* allowed[] = {"Anton-Regular.ttf", "IBMPlexMono-Medium.ttf", "IBMPlexMono-SemiBold.ttf"};
        if (std::find_if(std::begin(allowed), std::end(allowed), [&](const char* f) { return file == f; }) != std::end(allowed)) {
            for (const std::string& dir : {cfg_.fontsDir, std::string("fonts"), std::string("/opt/xowave/fonts")}) {
                std::ifstream f(dir + "/" + file, std::ios::binary);
                if (!f) continue;
                std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                reply(fd, 200, "font/ttf", data);
                return;
            }
        }
    }
    reply(fd, 404, "text/plain", "not found\n");
}

}  // namespace xo
