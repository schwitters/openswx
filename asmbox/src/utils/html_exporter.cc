// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

#include "utils/html_exporter.h"

#include <fstream>

namespace sw_dumper::utils {

namespace {

const char* kHtmlPart1 = R"html(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <title>SW Report</title>
    <style>
        :root { --primary: #3b5998; --bg: #f4f7f6; --text: #333; --border: #ddd; }
        body { font-family: 'Segoe UI', sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }
        .grid { display: grid; grid-template-columns: 400px 1fr; gap: 20px; max-width: 1600px; margin: auto; }
        .card { background: white; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); margin-bottom: 20px; overflow: hidden; }
        .card-header { background: #f8f9fa; padding: 12px 15px; font-weight: bold; border-bottom: 1px solid var(--border); display:flex; justify-content:space-between; align-items:center; }
        .card-body { padding: 15px; }
        .preview-container { height: 350px; display: flex; align-items: center; justify-content: center; background: #fff; border: 1px solid #eee; cursor: zoom-in; }
        .preview-img { max-width: 100%; max-height: 100%; object-fit: contain; }
        #modal { display: none; position: fixed; z-index: 999; left: 0; top: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.9); justify-content: center; align-items: center; cursor: zoom-out; }
        #modalImg { max-width: 95%; max-height: 95%; border: 2px solid white; }
        .tabs { display: flex; background: #f8f9fa; border-bottom: 1px solid var(--border); }
        .tab { padding: 12px 20px; cursor: pointer; border-bottom: 3px solid transparent; user-select: none; }
        .tab.active { border-bottom-color: var(--primary); font-weight: bold; color: var(--primary); background: #fff; }
        .tab-content { display: none; padding: 0; }
        .tab-content.active { display: block; }
        .table-wrap { overflow-x: auto; max-height: 600px; }
        table { width: 100%; border-collapse: collapse; font-size: 0.9rem; }
        th, td { padding: 10px; text-align: left; border-bottom: 1px solid #eee; vertical-align: top; }
        th { background: #fafafa; position: sticky; top: 0; }
        tr.highlighted { background-color: #fff3cd !important; border-left: 4px solid #ffc107; }
        .val { font-family: monospace; color: #d63384; font-weight: bold; }
        .badge { background: #3b5998; color: white; padding: 2px 8px; border-radius: 10px; font-size: 0.75rem; }
        .prop-tag { display: inline-block; background: #eef2f5; padding: 2px 6px; border-radius: 4px; font-size: 0.8em; margin: 2px; border: 1px solid #dae1e7; }
        .detail-header { padding: 15px; border-bottom: 1px solid #eee; background: #fdfdfd; display: flex; align-items: center; gap: 15px; }
        .btn-back { padding: 6px 12px; cursor: pointer; background: white; border: 1px solid #ccc; border-radius: 4px; font-size: 0.9rem; }
        .btn-back:hover { background: #f0f0f0; border-color: #bbb; }
        .item-title { font-size: 1.1rem; font-weight: bold; color: #3b5998; margin: 0; }
    </style>
</head>
<body>
<div id="modal" onclick="this.style.display='none'"><img id="modalImg" src=""></div>
<div class="grid">
    <aside>
        <div class="card">
            <div class="card-header"><span id="selTitle">Konfiguration</span> <span id="typeBadge" class="badge">DOC</span></div>
            <div class="card-body">
                <select id="mainSelector" style="width:100%; padding:8px; margin-bottom:15px;"></select>
                <div class="preview-container" onclick="openModal()">
                    <img id="mainPreview" class="preview-img" src="">
                    <div id="noPrev" style="display:none; color:#ccc">Keine Vorschau</div>
                </div>
            </div>
        </div>
        <div class="card" id="massCard">
            <div class="card-header">Physikalische Eigenschaften</div>
            <div class="card-body"><table id="massTable"></table></div>
        </div>
    </aside>
    <main>
        <div class="card">
            <div class="tabs">
                <div id="tabProps" class="tab active" onclick="showTab('props')">Eigenschaften</div>
                <div id="tabComps" class="tab" onclick="showTab('comps')" style="display:none">Komponenten</div>
                <div id="tabCut" class="tab" onclick="showTab('cut')" style="display:none">Zuschnittsliste</div>
                <div id="tabViews" class="tab" onclick="showTab('views')" style="display:none">Ansichten</div>
            </div>
            <div id="cnt-props" class="tab-content active"><div class="table-wrap"><table id="propsBody"></table></div></div>
            <div id="cnt-comps" class="tab-content"><div class="table-wrap"><table><thead><tr><th>Name</th><th>Ref</th><th>Infos</th></tr></thead><tbody id="compsBody"></tbody></table></div></div>
            <div id="cnt-cut" class="tab-content">
                <div id="cutListView">
                    <div class="table-wrap"><table><thead><tr><th>Menge</th><th>Name</th><th>Vorschau-Eigenschaften</th></tr></thead><tbody id="cutBody"></tbody></table></div>
                </div>
                <div id="cutDetailView" style="display:none;">
                    <div class="detail-header"><button class="btn-back" onclick="showCutlistMaster()">← Zurück zur Liste</button><h3 id="cutDetailTitle" class="item-title"></h3></div>
                    <div class="table-wrap"><table id="cutDetailTable"></table></div>
                </div>
            </div>
            <div id="cnt-views" class="tab-content"><div class="table-wrap"><table><thead><tr><th>Ansicht</th><th>Modell</th><th>Konfig</th></tr></thead><tbody id="viewsBody"></tbody></table></div></div>
        </div>
    </main>
</div>
<script>
const data = )html";

const char* kHtmlPart2 = R"html(;

function openModal() { const s=document.getElementById('mainPreview').src; if(s&&s.startsWith('data:')){document.getElementById('modalImg').src=s;document.getElementById('modal').style.display='flex';} }
function showTab(n) { document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active')); document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active')); document.getElementById('tab'+n.charAt(0).toUpperCase()+n.slice(1)).classList.add('active'); document.getElementById('cnt-'+n).classList.add('active'); }
function renderProps(p) { return p?Object.entries(p).map(([k,v])=>`<tr><td><b>${k}</b></td><td>${v}</td></tr>`).join(''):""; }

function showCutlistMaster() { document.getElementById('cutListView').style.display = 'block'; document.getElementById('cutDetailView').style.display = 'none'; }
function showCutlistDetail(itemName) {
    showTab('cut'); const idx=getSafeIndex(); const cfg=data.configurations[idx]; if(!cfg||!cfg.cut_list)return;
    const item=cfg.cut_list.find(x=>x.name===itemName); if(!item)return;
    document.getElementById('cutListView').style.display='none'; document.getElementById('cutDetailView').style.display='block';
    document.getElementById('cutDetailTitle').innerText=`${item.name} (${item.quantity}x)`;
    let h=`<tr><td colspan="2" style="background:#f0f0f0;font-weight:bold;">Basisdaten</td></tr><tr><td>Name</td><td>${item.name}</td></tr><tr><td>Menge</td><td>${item.quantity}</td></tr><tr><td colspan="2" style="background:#f0f0f0;font-weight:bold;">Eigenschaften</td></tr>`;
    if(item.properties) h+=renderProps(item.properties); else h+=`<tr><td colspan="2" style="color:#999;font-style:italic">Keine</td></tr>`;
    document.getElementById('cutDetailTable').innerHTML=h;
}

window.addEventListener('message', (event) => {
    const msg = event.data;
    if (msg && msg.type === 'select_cutlist_item') showCutlistDetail(msg.itemName);
});

function getSafeIndex() {
    let idx = parseInt(document.getElementById('mainSelector').value);
    if (isNaN(idx) || idx < 0) idx = 0;
    if (data.configurations && idx >= data.configurations.length) idx = 0;
    if (data.sheets && idx >= data.sheets.length) idx = 0;
    return idx;
}

function updateUI() {
    try {
        const idx = getSafeIndex();
        const isDrw = (data.doc_type === 3);
        document.getElementById('typeBadge').innerText = {1:'Part',2:'Assembly',3:'Drawing'}[data.doc_type] || 'Unknown';

        let payload = null;
        if (isDrw && data.sheets && data.sheets.length > idx) payload = data.sheets[idx];
        else if (!isDrw && data.configurations && data.configurations.length > idx) payload = data.configurations[idx];

        if(window.parent && payload) {
            window.parent.postMessage({
                type: 'sw_report_data',
                docType: data.doc_type,
                fullData: data,
                activePayload: payload
            }, '*');
        }

        ['Comps','Cut','Views'].forEach(x => document.getElementById('tab'+x).style.display='none');
        document.getElementById('massCard').style.display = isDrw ? 'none' : 'block';
        showCutlistMaster();

        if(isDrw) {
            if(!payload) return;
            setImg(payload.preview_png_base64);
            const vb=document.getElementById('viewsBody');
            if(payload.views&&payload.views.length>0) {
                document.getElementById('tabViews').style.display='block';
                vb.innerHTML=payload.views.map(v=>`<tr><td><b>${v.name}</b></td><td>${v.referenced_document}</td><td>${v.referenced_config}</td></tr>`).join('');
            } else vb.innerHTML="<tr><td colspan='3'>Leer</td></tr>";
            let h="";
            if(data.global_properties) h=Object.entries(data.global_properties).map(([k,v])=>`<tr style="color:#666"><td>${k} (G)</td><td>${v}</td></tr>`).join('');
            document.getElementById('propsBody').innerHTML=h||"<tr><td>Keine</td></tr>";
        } else {
            if(!payload) return;
            setImg(payload.preview_png_base64||data.preview_png_base64);
            let h=renderProps(payload.properties);
            if(data.global_properties) h+=Object.entries(data.global_properties).map(([k,v])=>`<tr style="color:#666"><td>${k} (G)</td><td>${v}</td></tr>`).join('');
            document.getElementById('propsBody').innerHTML=h||"<tr><td>Keine</td></tr>";

            const mt=document.getElementById('massTable');
            if(payload.mass_properties) {
                const m=payload.mass_properties;
                mt.innerHTML=`<tr><td>Masse</td><td class="val">${m.mass.toFixed(3)} kg</td></tr><tr><td>Volumen</td><td class="val">${(m.volume*1000).toFixed(4)} L</td></tr>`;
            } else mt.innerHTML="<tr><td>N/A</td></tr>";

            if(data.doc_type===2) {
                document.getElementById('tabComps').style.display='block';
                const cb=document.getElementById('compsBody');
                if(payload.components&&payload.components.length>0) {
                    cb.innerHTML=payload.components.map(cp => {
                        let info = `<div>Config: ${cp.ref_config}</div>`;
                        if(cp.is_suppressed) info += `<span class="badge" style="background:red">Suppressed</span>`;
                        if(cp.component_reference) info += `<div>Ref: <b>${cp.component_reference}</b></div>`;
                        return `<tr><td><b>${cp.name}</b></td><td>${cp.component_reference||'-'}</td><td>${info}</td></tr>`;
                    }).join('');
                } else cb.innerHTML="<tr><td colspan='3'>Leer</td></tr>";
            }

            if(data.doc_type===1 && payload.cut_list && payload.cut_list.length > 0) {
                document.getElementById('tabCut').style.display='block';
                document.getElementById('cutBody').innerHTML = payload.cut_list.map(item => {
                    let propsHtml = "";
                    if(item.properties) {
                        const keys = Object.keys(item.properties);
                        propsHtml = keys.slice(0,3).map(k => `<span class="prop-tag">${k}: ${item.properties[k]}</span>`).join(' ');
                        if(keys.length>3) propsHtml+=" ...";
                    }
                    return `<tr style="cursor:pointer" onclick="showCutlistDetail('${item.name.replace(/'/g, "\\'")}')"><td style="width:50px; text-align:center;"><b>${item.quantity}</b></td><td>${item.name}</td><td>${propsHtml}</td></tr>`;
                }).join('');
            }
        }
    } catch(e){console.error("UI Update Error:", e);}
}
function setImg(b64) {
    const i=document.getElementById('mainPreview'); const n=document.getElementById('noPrev');
    if(b64){i.src="data:image/png;base64,"+b64;i.style.display='block';n.style.display='none';}
    else{i.style.display='none';n.style.display='flex';}
}

const sel=document.getElementById('mainSelector');
const tit=document.getElementById('selTitle');
if(data.doc_type===3) { tit.innerText="Blatt"; if(data.sheets)data.sheets.forEach((s,i)=>sel.add(new Option(s.name,i))); }
else { tit.innerText="Konfiguration"; if(data.configurations)data.configurations.forEach((c,i)=>sel.add(new Option(c.name,i))); }

const urlParams = new URLSearchParams(window.location.search);
const reqConfig = urlParams.get('config');
if(reqConfig && data.configurations) {
    const idx = data.configurations.findIndex(c => c.name === reqConfig);
    if(idx !== -1) sel.value = idx;
}
sel.onchange=updateUI; updateUI();
</script></body></html>)html";

}  // namespace

bool HtmlExporter::Export(const std::filesystem::path& output_path,
                           const nlohmann::json& data) {
  std::ofstream out(output_path);
  if (!out) return false;
  out << kHtmlPart1 << data.dump() << kHtmlPart2;
  return true;
}

}  // namespace sw_dumper::utils
