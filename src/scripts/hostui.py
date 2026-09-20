#!/usr/bin/env python3
# hostui.py — BioOS host UI (Step 15).
#
#   python3 scripts/hostui.py          -> http://localhost:8471
#
# A single-file web app: the Python side proxies the guest's agent-gate
# protocol over the virtio-console unix socket; the browser renders the
# object browser, lineage graph, genomic range view and byte heatmap.
#
# Endpoints (all proxy `act query ...` / `act lineage ...`):
#   /api/uri?u=<uri>       raw uri_read result (text or base64 blob)
#   /api/lineage/<hex>     lineage text
#   /api/obj/<hex>         object bytes as base64 + type
import base64
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

SOCK = "build/host.sock"
PORT = 8471

_lock = threading.Lock()          # the console socket is half-duplex


def act(line: str, timeout: float = 10.0) -> bytes:
    """Send one `act` line, read the reply up to the '.' sentinel."""
    with _lock:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                s.connect(SOCK)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise ConnectionError("no guest channel")
        s.settimeout(timeout)
        s.sendall(line.encode() + b"\n")
        data = b""
        try:
            while not data.endswith(b"\n.\n"):
                chunk = s.recv(8192)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        s.close()
    if data.endswith(b"\n.\n"):
        data = data[:-3]
    return data


PAGE = """<!doctype html><meta charset=utf-8><title>BioOS</title>
<style>
body{background:#101418;color:#dde;font:13px monospace;margin:0}
h1{color:#80e0ff;font-size:14px;margin:8px 12px}
#cols{display:flex;gap:12px;padding:0 12px}
#objs{min-width:560px}canvas{border:1px solid #345;background:#0a0d10}
.obj{cursor:pointer;padding:1px 4px}.obj:hover{background:#203040}
input{background:#0a0d10;border:1px solid #345;color:#dde;font:inherit;
      width:640px;padding:2px}
pre{white-space:pre-wrap;margin:4px 0}
</style>
<h1>BioOS</h1>
<div id=cols>
 <div id=objs><h1>objects</h1><div id=list></div></div>
 <div><h1>lineage</h1><canvas id=dag width=560 height=420></canvas>
      <h1>heatmap</h1><canvas id=heat width=560 height=200></canvas></div>
 <div><h1>query</h1>
  <input id=q placeholder='seq://HASH/0-64 or var://HASH/chr1:0-300 or sys://...'>
  <pre id=out></pre></div>
</div>
<script>
const $=id=>document.getElementById(id);
async function api(p){const r=await fetch(p);return r.json()}

async function objects(){
  const j=await api('/api/uri?u='+encodeURIComponent('sys://objects'));
  const el=$('list');el.innerHTML='';
  for(const l of j.text.split('\\n')){
    if(!l.startsWith('count')&&l.length>64){
      const h=l.slice(0,64);
      const d=document.createElement('div');
      d.className='obj';
      d.textContent=h.slice(0,16)+'… '+l.slice(65);
      d.onclick=()=>{select(h)};
      el.appendChild(d);
    }else{const d=document.createElement('pre');d.textContent=l;el.appendChild(d)}
  }
}

async function select(h){
  const lin=await api('/api/lineage/'+h);
  drawDag(lin.text);
  const obj=await api('/api/obj/'+h);
  drawHeat(atob(obj.b64||''));
}

function drawDag(text){
  const c=$('dag'),g=c.getContext('2d');
  g.fillStyle='#0a0d10';g.fillRect(0,0,c.width,c.height);
  const lines=text.split('\\n').filter(l=>l.trim());
  let pos=[];
  for(const l of lines){
    const m=l.match(/^(\\s*)([0-9a-f]{64})/);
    if(m){const d=m[1].length/2;pos.push({x:40+d*60,y:0,h:m[2]})}
  }
  let yi=30,py={};
  const nodes=[];
  for(const p of pos){
    p.y=yi;yi+=60;nodes.push(p);
    py[p.h]=p;
  }
  g.strokeStyle='#345';g.fillStyle='#203040';
  let prev={};
  for(const p of nodes){
    // edge to parent = nearest shallower node above
    g.beginPath();g.rect(p.x,p.y,120,24);g.stroke();
    g.fillStyle='#80e0ff';
    g.fillText(p.h.slice(0,12),p.x+4,p.y+16);
  }
  let stack=[];
  for(const p of nodes){
    stack=stack.filter(q=>q.x<p.x);
    if(stack.length){const q=stack[stack.length-1];
      g.strokeStyle='#607080';g.beginPath();
      g.moveTo(q.x+60,q.y+24);g.lineTo(p.x+60,p.y);g.stroke();}
    stack.push(p);
  }
}

function drawHeat(s){
  const c=$('heat'),g=c.getContext('2d');
  g.fillStyle='#0a0d10';g.fillRect(0,0,c.width,c.height);
  const cols=64,cs=8;
  for(let i=0;i<s.length&&i<cols*24;i++){
    const v=s.charCodeAt(i);
    g.fillStyle=`rgb(${v},${64-(v>>3)},${255-v})`;
    g.fillRect(8+(i%cols)*cs,8+((i/cols)|0)*cs,cs-1,cs-1);
  }
}

$('q').addEventListener('keydown',async e=>{
  if(e.key!=='Enter')return;
  const j=await api('/api/uri?u='+encodeURIComponent(e.target.value));
  $('out').textContent=j.text||j.error||'';
});
objects();
</script>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        try:
            if u.path == "/" or u.path == "/index.html":
                body = PAGE.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif u.path == "/api/uri":
                uri = parse_qs(u.query).get("u", [""])[0]
                r = act("act query " + uri)
                try:
                    self._json({"text": r.decode()})
                except UnicodeDecodeError:
                    self._json({"b64": base64.b64encode(r).decode()})
            elif u.path.startswith("/api/lineage/"):
                h = u.path.rsplit("/", 1)[1]
                r = act("act lineage " + h)
                self._json({"text": r.decode(errors="replace")})
            elif u.path.startswith("/api/obj/"):
                h = u.path.rsplit("/", 1)[1]
                r = act("act query obj://" + h)
                try:
                    r.decode()
                    self._json({"text": r.decode()})
                except UnicodeDecodeError:
                    self._json({"b64": base64.b64encode(r).decode()})
            else:
                self.send_error(404)
        except Exception as e:
            self._json({"error": str(e)})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    print(f"BioOS host UI -> http://localhost:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
