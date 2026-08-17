#!/usr/bin/env python3
"""Build a self-contained offline dashboard from hbm_sim validation artifacts.

The simulator deliberately remains independent of browser/UI frameworks.  This
post-processing tool consumes its existing command/DFI CSV traces, textual
statistics, performance-curve JSON, and thermal map, and writes one portable
HTML file that can be opened with any modern browser.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command-trace", type=Path, required=True,
                        help="CSV produced by --cmd-trace")
    parser.add_argument("--dfi-trace", type=Path,
                        help="optional CSV produced by --dfi-trace")
    parser.add_argument("--stats", type=Path,
                        help="optional stdout capture from hbm_sim")
    parser.add_argument("--performance-json", type=Path,
                        help="optional JSON produced by tools/performance_curve.py")
    parser.add_argument("--thermal-map", type=Path,
                        help="optional text map produced by --dump-thermal-map")
    parser.add_argument("--out", type=Path, required=True, help="output standalone HTML")
    parser.add_argument("--title", default="hbm_sim validation dashboard")
    parser.add_argument("--max-events", type=int, default=100_000,
                        help="maximum command events embedded in the dashboard")
    return parser.parse_args()


def integer(row: dict[str, str], name: str) -> int:
    try:
        return int(row.get(name, "0") or 0)
    except ValueError:
        return 0


def read_command_trace(path: Path, maximum: int) -> tuple[list[dict[str, Any]], int]:
    if not path.is_file():
        raise SystemExit(f"command trace not found: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        return [], 0
    original_count = len(rows)
    if maximum < 1:
        raise SystemExit("--max-events must be positive")
    if len(rows) > maximum:
        # Deterministic downsampling retains the first/last event and covers the
        # complete execution interval; aggregate command counts still use all rows.
        step = (len(rows) - 1) / (maximum - 1) if maximum > 1 else len(rows)
        rows = [rows[round(index * step)] for index in range(maximum)]
    result = []
    for row in rows:
        result.append({
            "cycle": integer(row, "cycle"),
            "command": row.get("command", "UNKNOWN"),
            "bus": row.get("bus", "unknown"),
            "stack": integer(row, "stack_id"),
            "channel": integer(row, "channel"),
            "pc": integer(row, "pseudo_channel"),
            "sid": integer(row, "sid"),
            "bg": integer(row, "bank_group"),
            "bank": integer(row, "bank"),
            "row": integer(row, "row"),
            "request": integer(row, "request_id"),
        })
    return result, original_count


def read_dfi_trace(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {}
    if not path.is_file():
        raise SystemExit(f"DFI trace not found: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    kinds = Counter(row.get("kind", "UNKNOWN") for row in rows)
    payload_bytes = sum(integer(row, "beat_bytes") for row in rows)
    return {
        "events": len(rows),
        "kinds": dict(sorted(kinds.items())),
        "payload_bytes": payload_bytes,
        "first_cycle": min((integer(row, "cycle") for row in rows), default=0),
        "last_cycle": max((integer(row, "cycle") for row in rows), default=0),
    }


def read_stats(path: Path | None) -> dict[str, str]:
    if path is None:
        return {}
    if not path.is_file():
        raise SystemExit(f"stats file not found: {path}")
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key, value = key.strip(), value.strip()
        if key and value:
            result[key] = value
    # Keep the dashboard readable even when stdout contains a full configuration dump.
    wanted = (
        "standard", "mem_phy_mode", "phy_protocol", "cycles", "system_cycles",
        "reads", "writes", "completed_reads", "completed_writes", "avg_read_latency",
        "achieved_bw_GBps", "peak_bandwidth_GBps", "bandwidth_util_pct",
        "data_mismatches", "phy_command_backpressure", "phy_data_backpressure",
        "thermal_peak_temp_C", "power_energy_pJ", "cmd_validation", "dfi_validation",
    )
    return {key: result[key] for key in wanted if key in result}


def read_performance(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        return []
    if not path.is_file():
        raise SystemExit(f"performance JSON not found: {path}")
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or not isinstance(payload.get("rows"), list):
        raise SystemExit(f"performance JSON has no rows array: {path}")
    fields = ("standard", "read_ratio_pct", "offered_requests_per_tick",
              "avg_read_latency_ticks", "achieved_bw_GBps", "bandwidth_util_pct")
    return [{field: row.get(field) for field in fields} for row in payload["rows"]
            if isinstance(row, dict)]


def read_thermal(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        return []
    if not path.is_file():
        raise SystemExit(f"thermal map not found: {path}")
    result = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) < 13:
            continue
        try:
            result.append({
                "stack": int(fields[0]), "layer": int(fields[1]),
                "x": int(fields[2]), "y": int(fields[3]),
                "temperature": float(fields[10]), "energy": float(fields[11]),
                "events": int(fields[12]),
            })
        except ValueError:
            continue
    return result


def safe_json(value: Any) -> str:
    # Prevent a trace field from terminating the script tag in the generated HTML.
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")


HTML_TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
:root{color-scheme:dark;--bg:#0b1020;--card:#131b31;--edge:#263453;--ink:#e6edf7;--muted:#99a9c5;--blue:#60a5fa;--green:#4ade80;--orange:#fb923c;--red:#f87171;--purple:#c084fc}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}.wrap{max-width:1500px;margin:auto;padding:28px}h1{margin:0 0 4px;font-size:26px}.sub{color:var(--muted);margin:0 0 22px}.card{background:var(--card);border:1px solid var(--edge);border-radius:10px;padding:16px;margin:14px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}.metric{border:1px solid var(--edge);border-radius:7px;padding:10px;min-height:70px}.metric b{display:block;font-size:11px;color:var(--muted);font-weight:600;overflow-wrap:anywhere}.metric span{font-size:18px;font-variant-numeric:tabular-nums}.controls{display:flex;flex-wrap:wrap;gap:10px;align-items:end;margin-bottom:12px}label{display:grid;gap:4px;color:var(--muted);font-size:12px}input,select,button{background:#0b1328;border:1px solid var(--edge);color:var(--ink);border-radius:6px;padding:7px 8px;font:inherit}button{cursor:pointer;color:white;background:#1d4ed8;border-color:#2563eb}button.ghost{background:#17213b;border-color:#33456d}canvas{width:100%;height:530px;background:#0b1328;border:1px solid var(--edge);border-radius:7px}.overview{height:76px;margin:4px 0 10px;cursor:crosshair}.hint,.warning{color:var(--muted);font-size:12px}.warning{color:#fbbf24}.legend{display:flex;gap:12px;flex-wrap:wrap;margin:10px 0}.legend span::before{content:"";display:inline-block;width:10px;height:10px;border-radius:2px;background:var(--c);margin-right:5px}.bars{display:grid;gap:7px}.bar{display:grid;grid-template-columns:86px 1fr 60px;gap:8px;align-items:center}.bar i{display:block;height:12px;border-radius:3px;background:linear-gradient(90deg,#60a5fa,#818cf8)}.bar em{font-style:normal;color:var(--muted);text-align:right}svg{width:100%;height:290px;background:#0b1328;border:1px solid var(--edge);border-radius:7px}.thermal{display:grid;grid-template-columns:repeat(auto-fill,minmax(38px,1fr));gap:3px}.tile{aspect-ratio:1;border-radius:3px;display:grid;place-items:center;font-size:10px;color:#06101e;font-weight:700}.empty{color:var(--muted);padding:12px 0}code{color:#c4b5fd}.tabs{display:flex;gap:6px;margin-bottom:12px}.tabs button.active{background:#2563eb;border-color:#60a5fa}.range{display:flex;flex-wrap:wrap;gap:13px;padding:9px 10px;margin-bottom:10px;border:1px solid var(--edge);border-radius:7px;color:var(--muted);font-size:12px}.range b{color:var(--ink);font-variant-numeric:tabular-nums}.details{min-height:48px;margin-top:10px;padding:10px;border:1px solid var(--edge);border-radius:7px;color:var(--muted);font:12px/1.55 ui-monospace,SFMono-Regular,Menlo,monospace}.details strong{color:var(--ink)}
</style>
</head>
<body><main class="wrap">
<h1>__TITLE__</h1><p class="sub">Offline validation dashboard • generated from hbm_sim artifacts • no browser/server dependency</p>
<section class="card"><h2>Run summary</h2><div id="metrics" class="grid"></div></section>
<section class="card"><h2>Trace explorer</h2><p id="timelineHint" class="hint"></p><div class="tabs"><button id="bankView" class="active">Command / bank view</button><button id="requestView" class="ghost">Request swimlanes</button><label class="ghost" style="padding:7px 8px;cursor:pointer">Open command CSV<input id="traceUpload" type="file" accept=".csv,text/csv" hidden></label></div><div id="rangeStats" class="range"></div><canvas id="overview" class="overview" width="1400" height="76"></canvas><div class="controls"><label>Start cycle<input id="start" type="number"></label><label>End cycle<input id="end" type="number"></label><label>Stack<select id="stack"></select></label><label>Channel<select id="channel"></select></label><label>Command<select id="command"></select></label><label>Request<input id="requestFilter" type="number" placeholder="all"></label><label>Lanes<select id="lanes"><option value="12">12</option><option value="24" selected>24</option><option value="48">48</option></select></label><button id="reset">Full range</button></div><div class="legend"><span style="--c:#4ade80">ACT/open</span><span style="--c:#60a5fa">read</span><span style="--c:#fb923c">write</span><span style="--c:#f87171">PRE/close</span><span style="--c:#c084fc">refresh/RFM</span><span style="--c:#94a3b8">other</span></div><canvas id="timeline" width="1400" height="530"></canvas><div id="eventDetails" class="details">Click an event to inspect its cycle, request and decoded DRAM location.</div><p class="hint">Overview click recentres the view. Command view uses bank lanes; Request view groups commands by request lifetime. Both are offline equivalents of Ramulator trace exploration.</p></section>
<section class="card"><h2>Command mix</h2><div id="mix" class="bars"></div></section>
<section class="card"><h2>DFI activity</h2><div id="dfi" class="grid"></div></section>
<section class="card"><h2>Injection-rate / latency / throughput</h2><p class="hint">Shown when <code>tools/performance_curve.py --json-out</code> is supplied.</p><svg id="curve" viewBox="0 0 1200 290" preserveAspectRatio="none"></svg><p id="curveHint" class="hint"></p></section>
<section class="card"><h2>Thermal map</h2><p class="hint">Shown when <code>--dump-thermal-map</code> is supplied. Tiles are coloured by absolute temperature.</p><div class="controls"><label>Layer<select id="layer"></select></label></div><div id="thermal" class="thermal"></div></section>
</main><script>
const DATA=__DATA__;
const COLORS={act:'#4ade80',read:'#60a5fa',write:'#fb923c',pre:'#f87171',refresh:'#c084fc',other:'#94a3b8'};
const el=id=>document.getElementById(id); const fmt=n=>Number(n).toLocaleString();
function category(c){c=(c||'').toUpperCase();if(c.startsWith('ACT'))return'act';if(c==='RD'||c==='RDA'||c==='CAS_RD')return'read';if(c==='WR'||c==='WRA'||c==='CAS_WR')return'write';if(c.startsWith('PRE'))return'pre';if(c.startsWith('REF')||c.startsWith('RFM')||c.startsWith('SREF'))return'refresh';return'other'}
function option(select,values){select.innerHTML='';for(const v of values){const o=document.createElement('option');o.value=v;o.textContent=v;select.append(o)}}
function metrics(){const defaults={commands:DATA.totalCommands,embedded_events:DATA.commands.length,first_cycle:DATA.range[0],last_cycle:DATA.range[1]};const all={...defaults,...DATA.stats};el('metrics').innerHTML=Object.entries(all).map(([k,v])=>`<div class="metric"><b>${k}</b><span>${v}</span></div>`).join('')}
function setupFilters(){const vals=(field)=>['all',...Array.from(new Set(DATA.commands.map(x=>String(x[field])))).sort((a,b)=>a.localeCompare(b,undefined,{numeric:true}))];option(el('stack'),vals('stack'));option(el('channel'),vals('channel'));option(el('command'),['all',...Array.from(new Set(DATA.commands.map(x=>x.command))).sort()]);el('start').value=DATA.range[0];el('end').value=DATA.range[1];for(const id of ['start','end','stack','channel','command','lanes'])el(id).addEventListener('input',drawTimeline);el('reset').onclick=()=>{el('start').value=DATA.range[0];el('end').value=DATA.range[1];drawTimeline()}}
function visible(){let start=Number(el('start').value),end=Number(el('end').value);if(!Number.isFinite(start))start=DATA.range[0];if(!Number.isFinite(end)||end<=start)end=start+1;const stack=el('stack').value,channel=el('channel').value,command=el('command').value;return {start,end,rows:DATA.commands.filter(x=>x.cycle>=start&&x.cycle<=end&&(stack==='all'||String(x.stack)===stack)&&(channel==='all'||String(x.channel)===channel)&&(command==='all'||x.command===command))}}
function lane(x){return `S${x.stack}/C${x.channel}/PC${x.pc}/SID${x.sid}/BG${x.bg}/B${x.bank}`}
function drawTimeline(){const canvas=el('timeline'),ctx=canvas.getContext('2d'),box=canvas.getBoundingClientRect(),dpr=devicePixelRatio||1,w=Math.floor(box.width*dpr),h=Math.floor(box.height*dpr);canvas.width=w;canvas.height=h;ctx.scale(dpr,dpr);const W=box.width,H=box.height,{start,end,rows}=visible();const counts=new Map;for(const x of rows)counts.set(lane(x),(counts.get(lane(x))||0)+1);const max=Number(el('lanes').value),lanes=[...counts].sort((a,b)=>counts.get(b[0])-counts.get(a[0])||a[0].localeCompare(b[0])).slice(0,max).map(x=>x[0]);const index=new Map(lanes.map((x,i)=>[x,i]));const left=176,top=30,rowH=Math.max(15,Math.floor((H-top-24)/Math.max(1,lanes.length)));ctx.fillStyle='#0b1328';ctx.fillRect(0,0,W,H);ctx.font='11px system-ui';ctx.fillStyle='#99a9c5';ctx.fillText(`visible ${fmt(rows.length)} commands • ${fmt(start)}–${fmt(end)} cycles • top ${lanes.length} active lanes`,8,17);for(let i=0;i<lanes.length;i++){const y=top+i*rowH;ctx.fillStyle=i%2?'#0e1830':'#0b1328';ctx.fillRect(left,y,W-left,rowH);ctx.fillStyle='#99a9c5';ctx.fillText(lanes[i],8,y+Math.max(11,rowH-4));ctx.strokeStyle='#1f2b47';ctx.beginPath();ctx.moveTo(left,y+rowH-.5);ctx.lineTo(W,y+rowH-.5);ctx.stroke()}const points=[];for(const x of rows){const i=index.get(lane(x));if(i===undefined)continue;const px=left+(x.cycle-start)/(end-start)*(W-left),py=top+i*rowH+2;ctx.fillStyle=COLORS[category(x.command)];ctx.fillRect(px,py,Math.max(2,Math.min(9,(W-left)/(end-start)*2)),Math.max(3,rowH-4));points.push({x:px,y:py,w:10,h:rowH,ev:x})}canvas.onmousemove=e=>{const r=canvas.getBoundingClientRect(),mx=e.clientX-r.left,my=e.clientY-r.top;let p;for(let i=points.length-1;i>=0;i--){const q=points[i];if(mx>=q.x-2&&mx<=q.x+q.w&&my>=q.y&&my<=q.y+q.h){p=q;break}}canvas.title=p?`${p.ev.command} @ ${p.ev.cycle} • ${lane(p.ev)} • row ${p.ev.row} • req ${p.ev.request}`:''};el('timelineHint').textContent=DATA.sampled?`Timeline embeds a deterministic ${fmt(DATA.commands.length)}-event sample of ${fmt(DATA.totalCommands)} total commands.`:`Timeline contains all ${fmt(DATA.totalCommands)} commands.`}
function drawMix(){const count=DATA.commandCounts||{};const max=Math.max(1,...Object.values(count));el('mix').innerHTML=Object.entries(count).sort((a,b)=>b[1]-a[1]).map(([c,n])=>`<div class="bar"><span>${c}</span><i style="width:${100*n/max}%"></i><em>${fmt(n)}</em></div>`).join('')||'<p class="empty">No command events.</p>'}
function drawDfi(){const d=DATA.dfi;if(!Object.keys(d).length){el('dfi').innerHTML='<p class="empty">No DFI CSV supplied.</p>';return}const values={events:d.events,payload_bytes:d.payload_bytes,first_cycle:d.first_cycle,last_cycle:d.last_cycle,...Object.fromEntries(Object.entries(d.kinds||{}).map(([k,v])=>['kind_'+k,v]))};el('dfi').innerHTML=Object.entries(values).map(([k,v])=>`<div class="metric"><b>${k}</b><span>${fmt(v)}</span></div>`).join('')}
function drawCurve(){const svg=el('curve'),rows=DATA.performance;if(!rows.length){svg.innerHTML='';el('curveHint').textContent='No performance JSON supplied.';return}const W=1200,H=290,pad={l:70,r:24,t:24,b:40};const xvals=rows.map(x=>Number(x.offered_requests_per_tick)||0),yvals=rows.map(x=>Number(x.achieved_bw_GBps)||0),lat=rows.map(x=>Number(x.avg_read_latency_ticks)||0);const xmax=Math.max(...xvals,1),ymax=Math.max(...yvals,1),lmax=Math.max(...lat,1);const X=x=>pad.l+x/xmax*(W-pad.l-pad.r),Y=y=>H-pad.b-y/ymax*(H-pad.t-pad.b),YL=y=>H-pad.b-y/lmax*(H-pad.t-pad.b);const groups={};for(const r of rows){const key=`${r.standard} R${r.read_ratio_pct}`;(groups[key]??=[]).push(r)}let out=`<path d="M${pad.l} ${pad.t}V${H-pad.b}H${W-pad.r}" stroke="#52617f" fill="none"/><text x="${pad.l}" y="16" fill="#99a9c5" font-size="12">Throughput GB/s (solid) • Read latency ticks (dashed)</text>`;let i=0;for(const [name,g] of Object.entries(groups)){const color=['#60a5fa','#4ade80','#fb923c','#c084fc','#f87171','#facc15'][i++%6];g.sort((a,b)=>a.offered_requests_per_tick-b.offered_requests_per_tick);const path=f=>g.map((r,j)=>`${j?'L':'M'}${X(Number(r.offered_requests_per_tick))} ${f(Number(r))}`).join(' ');out+=`<path d="${path(r=>Y(r.achieved_bw_GBps))}" stroke="${color}" stroke-width="2" fill="none"/><path d="${path(r=>YL(r.avg_read_latency_ticks))}" stroke="${color}" stroke-width="1.5" stroke-dasharray="5 4" fill="none"/><text x="${W-180}" y="${28+i*16}" fill="${color}" font-size="12">${name}</text>`}out+=`<text x="${pad.l}" y="${H-10}" fill="#99a9c5" font-size="11">offered requests / tick</text>`;svg.innerHTML=out;el('curveHint').textContent=`${rows.length} deterministic sweep points. Dashed latency has its own right-hand scale (max ${lmax.toFixed(1)} ticks).`}
function thermalColor(t,min,max){const f=max===min?.5:(t-min)/(max-min);return `hsl(${220-220*f} 78% ${78-30*f}%)`}
function drawThermal(){const tiles=DATA.thermal,select=el('layer');if(!tiles.length){select.innerHTML='';el('thermal').innerHTML='<p class="empty">No thermal map supplied.</p>';return}const layers=[...new Set(tiles.map(x=>x.layer))].sort((a,b)=>a-b);option(select,layers);const render=()=>{const rows=tiles.filter(x=>String(x.layer)===select.value),temps=rows.map(x=>x.temperature),min=Math.min(...temps),max=Math.max(...temps);el('thermal').innerHTML=rows.sort((a,b)=>a.y-b.y||a.x-b.x).map(x=>`<div class="tile" title="stack ${x.stack}, layer ${x.layer}, (${x.x},${x.y}) • ${x.temperature.toFixed(2)} °C • ${x.energy.toFixed(2)} pJ • ${x.events} events" style="background:${thermalColor(x.temperature,min,max)}">${x.temperature.toFixed(1)}</div>`).join('')};select.onchange=render;render()}
metrics();setupFilters();drawTimeline();drawMix();drawDfi();drawCurve();drawThermal();
</script><script>
// Enhanced explorer: mirrors the useful offline portions of Ramulator's visualizer
// while retaining the project-specific DFI, payload and thermal panels above.
let explorerMode='bank', explorerPoints=[];
const explorerCanvas=el('timeline'), overviewCanvas=el('overview');
function explorerLane(x){return explorerMode==='request'?`REQ ${x.request}`:lane(x)}
function explorerRows(){let start=Number(el('start').value),end=Number(el('end').value);if(!Number.isFinite(start))start=DATA.range[0];if(!Number.isFinite(end)||end<=start)end=start+1;const stack=el('stack').value,channel=el('channel').value,command=el('command').value,request=el('requestFilter').value;return{start,end,rows:DATA.commands.filter(x=>x.cycle>=start&&x.cycle<=end&&(stack==='all'||String(x.stack)===stack)&&(channel==='all'||String(x.channel)===channel)&&(command==='all'||x.command===command)&&(!request||String(x.request)===request))}}
function updateExplorerFilters(){const vals=field=>['all',...Array.from(new Set(DATA.commands.map(x=>String(x[field])))).sort((a,b)=>a.localeCompare(b,undefined,{numeric:true}));option(el('stack'),vals('stack'));option(el('channel'),vals('channel'));option(el('command'),['all',...Array.from(new Set(DATA.commands.map(x=>x.command))).sort()])}
function rangeSummary(rows,start,end){let rd=0,wr=0;for(const x of rows){const c=category(x.command);if(c==='read')rd++;else if(c==='write')wr++}const dur=Math.max(1,end-start);el('rangeStats').innerHTML=`<span>Range <b>${fmt(end-start)}</b> cyc</span><span>Commands <b>${fmt(rows.length)}</b></span><span>RD <b>${fmt(rd)}</b></span><span>WR <b>${fmt(wr)}</b></span><span>Other <b>${fmt(rows.length-rd-wr)}</b></span><span>CMD bus <b>${(rows.length/dur).toFixed(3)}</b> cmd/cyc</span>`}
function drawOverview(){const c=overviewCanvas,b=c.getBoundingClientRect(),d=devicePixelRatio||1,w=Math.floor(b.width*d),h=Math.floor(b.height*d),ctx=c.getContext('2d');c.width=w;c.height=h;ctx.setTransform(d,0,0,d,0,0);const W=b.width,H=b.height,[lo,hi]=DATA.range,span=Math.max(1,hi-lo),bins=180,count=Array(bins).fill(0);for(const x of DATA.commands){const i=Math.max(0,Math.min(bins-1,Math.floor((x.cycle-lo)/span*bins)));count[i]++}const max=Math.max(1,...count);ctx.fillStyle='#0b1328';ctx.fillRect(0,0,W,H);ctx.fillStyle='#365b91';for(let i=0;i<bins;i++){const bh=count[i]/max*(H-14);ctx.fillRect(i/bins*W,H-bh,(W/bins)+1,bh)}const start=Number(el('start').value),end=Number(el('end').value),x=(start-lo)/span*W,width=Math.max(2,(end-start)/span*W);ctx.fillStyle='rgba(96,165,250,.16)';ctx.fillRect(x,0,width,H);ctx.strokeStyle='#60a5fa';ctx.strokeRect(x+.5,.5,width-1,H-1);ctx.fillStyle='#99a9c5';ctx.font='10px system-ui';ctx.fillText(`${fmt(lo)} cyc`,6,12);ctx.fillText(`${fmt(hi)} cyc`,Math.max(6,W-72),12)}
function drawExplorer(){const c=explorerCanvas,b=c.getBoundingClientRect(),d=devicePixelRatio||1,w=Math.floor(b.width*d),h=Math.floor(b.height*d),ctx=c.getContext('2d');c.width=w;c.height=h;ctx.setTransform(d,0,0,d,0,0);const W=b.width,H=b.height,{start,end,rows}=explorerRows(),counts=new Map;for(const x of rows)counts.set(explorerLane(x),(counts.get(explorerLane(x))||0)+1);const limit=Number(el('lanes').value),lanes=[...counts].sort((a,b)=>counts.get(b[0])-counts.get(a[0])||a[0].localeCompare(b[0])).slice(0,limit).map(x=>x[0]),index=new Map(lanes.map((x,i)=>[x,i]));const left=176,top=30,rowH=Math.max(15,Math.floor((H-top-24)/Math.max(1,lanes.length)));ctx.fillStyle='#0b1328';ctx.fillRect(0,0,W,H);ctx.font='11px system-ui';ctx.fillStyle='#99a9c5';ctx.fillText(`${explorerMode==='bank'?'Command / bank':'Request swimlane'} view • ${fmt(rows.length)} visible events • ${fmt(start)}–${fmt(end)} cycles`,8,17);for(let i=0;i<lanes.length;i++){const y=top+i*rowH;ctx.fillStyle=i%2?'#0e1830':'#0b1328';ctx.fillRect(left,y,W-left,rowH);ctx.fillStyle='#99a9c5';ctx.fillText(lanes[i],8,y+Math.max(11,rowH-4));ctx.strokeStyle='#1f2b47';ctx.beginPath();ctx.moveTo(left,y+rowH-.5);ctx.lineTo(W,y+rowH-.5);ctx.stroke()}if(explorerMode==='request'){const spanByReq=new Map;for(const x of rows){const k=explorerLane(x),s=spanByReq.get(k)||{a:x.cycle,b:x.cycle};s.a=Math.min(s.a,x.cycle);s.b=Math.max(s.b,x.cycle);spanByReq.set(k,s)}ctx.fillStyle='#21385e';for(const [k,s] of spanByReq){const i=index.get(k);if(i===undefined)continue;const x=left+(s.a-start)/(end-start)*(W-left),right=left+(s.b-start)/(end-start)*(W-left);ctx.fillRect(x,top+i*rowH+4,Math.max(2,right-x),Math.max(2,rowH-8))}}explorerPoints=[];for(const x of rows){const i=index.get(explorerLane(x));if(i===undefined)continue;const px=left+(x.cycle-start)/(end-start)*(W-left),py=top+i*rowH+2,pw=Math.max(2,Math.min(10,(W-left)/(end-start)*2));ctx.fillStyle=COLORS[category(x.command)];ctx.fillRect(px,py,pw,Math.max(3,rowH-4));explorerPoints.push({x:px,y:py,w:pw,h:Math.max(3,rowH-4),ev:x})}rangeSummary(rows,start,end);drawOverview();el('timelineHint').textContent=DATA.sampled?`Timeline is a deterministic ${fmt(DATA.commands.length)}-event sample of ${fmt(DATA.totalCommands)} total commands.`:`Timeline contains all ${fmt(DATA.totalCommands)} command events.`}
function selectPoint(p){if(!p){el('eventDetails').textContent='Click an event to inspect its cycle, request and decoded DRAM location.';return}const x=p.ev;el('eventDetails').innerHTML=`<strong>${x.command}</strong> • cycle ${fmt(x.cycle)} • request ${x.request}<br>stack ${x.stack}, channel ${x.channel}, PC ${x.pc}, SID ${x.sid}, BG ${x.bg}, bank ${x.bank}, row ${x.row} • ${x.bus} bus`}
explorerCanvas.onmousemove=e=>{const r=explorerCanvas.getBoundingClientRect(),mx=e.clientX-r.left,my=e.clientY-r.top;let hit;for(let i=explorerPoints.length-1;i>=0;i--){const p=explorerPoints[i];if(mx>=p.x-3&&mx<=p.x+p.w+3&&my>=p.y&&my<=p.y+p.h){hit=p;break}}explorerCanvas.title=hit?`${hit.ev.command} @ ${hit.ev.cycle} • ${explorerLane(hit.ev)}`:''};explorerCanvas.onclick=e=>{const r=explorerCanvas.getBoundingClientRect(),mx=e.clientX-r.left,my=e.clientY-r.top;let hit;for(let i=explorerPoints.length-1;i>=0;i--){const p=explorerPoints[i];if(mx>=p.x-3&&mx<=p.x+p.w+3&&my>=p.y&&my<=p.y+p.h){hit=p;break}}selectPoint(hit)};
overviewCanvas.onclick=e=>{const r=overviewCanvas.getBoundingClientRect(),ratio=(e.clientX-r.left)/Math.max(1,r.width),center=DATA.range[0]+ratio*(DATA.range[1]-DATA.range[0]),duration=Math.max(1,Number(el('end').value)-Number(el('start').value));el('start').value=Math.max(DATA.range[0],Math.round(center-duration/2));el('end').value=Math.min(DATA.range[1],Math.round(center+duration/2));if(Number(el('end').value)<=Number(el('start').value))el('end').value=Number(el('start').value)+1;drawExplorer()};
function bindExplorer(){for(const id of ['start','end','stack','channel','command','requestFilter','lanes'])el(id).oninput=drawExplorer;el('reset').onclick=()=>{el('start').value=DATA.range[0];el('end').value=DATA.range[1];drawExplorer()};el('bankView').onclick=()=>{explorerMode='bank';el('bankView').className='active';el('requestView').className='ghost';drawExplorer()};el('requestView').onclick=()=>{explorerMode='request';el('requestView').className='active';el('bankView').className='ghost';drawExplorer()};el('traceUpload').onchange=async e=>{const f=e.target.files?.[0];if(!f)return;const lines=(await f.text()).trim().split(/\r?\n/),head=lines.shift().split(',');const ix=k=>head.indexOf(k),num=(a,k)=>Number(a[ix(k)]||0);DATA.commands=lines.filter(Boolean).map(line=>{const a=line.split(',');return{cycle:num(a,'cycle'),command:a[ix('command')]||'UNKNOWN',bus:a[ix('bus')]||'unknown',stack:num(a,'stack_id'),channel:num(a,'channel'),pc:num(a,'pseudo_channel'),sid:num(a,'sid'),bg:num(a,'bank_group'),bank:num(a,'bank'),row:num(a,'row'),request:num(a,'request_id')}}).sort((a,b)=>a.cycle-b.cycle);DATA.totalCommands=DATA.commands.length;DATA.sampled=false;DATA.range=DATA.commands.length?[DATA.commands[0].cycle,DATA.commands.at(-1).cycle]:[0,1];DATA.commandCounts={};for(const x of DATA.commands)DATA.commandCounts[x.command]=(DATA.commandCounts[x.command]||0)+1;DATA.dfi={};updateExplorerFilters();el('start').value=DATA.range[0];el('end').value=DATA.range[1];metrics();drawMix();drawDfi();drawExplorer();selectPoint()}};
window.addEventListener('keydown',e=>{if(e.key!=='+'&&e.key!=='-')return;const a=Number(el('start').value),b=Number(el('end').value),mid=(a+b)/2,scale=e.key==='+'?.5:2,span=Math.max(1,(b-a)*scale);el('start').value=Math.max(DATA.range[0],Math.round(mid-span/2));el('end').value=Math.min(DATA.range[1],Math.round(mid+span/2));drawExplorer()});
updateExplorerFilters();bindExplorer();drawExplorer();
</script></body></html>"""


def main() -> int:
    args = parse_args()
    commands, total_commands = read_command_trace(args.command_trace, args.max_events)
    command_counts = Counter()
    # Use the source CSV for aggregate count even if timeline events were sampled.
    with args.command_trace.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            command_counts[row.get("command", "UNKNOWN")] += 1
    commands.sort(key=lambda event: event["cycle"])
    data = {
        "commands": commands,
        "totalCommands": total_commands,
        "sampled": total_commands != len(commands),
        "range": [commands[0]["cycle"], commands[-1]["cycle"]] if commands else [0, 1],
        "commandCounts": dict(command_counts),
        "dfi": read_dfi_trace(args.dfi_trace),
        "stats": read_stats(args.stats),
        "performance": read_performance(args.performance_json),
        "thermal": read_thermal(args.thermal_map),
    }
    html = HTML_TEMPLATE.replace("__TITLE__", args.title.replace("<", "&lt;").replace(">", "&gt;"))
    html = html.replace("__DATA__", safe_json(data))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(html, encoding="utf-8")
    print(f"visualization written: {args.out} (timeline={len(commands)}/{total_commands} commands)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
