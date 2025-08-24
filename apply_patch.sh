#!/usr/bin/env bash
set -euo pipefail

echo "[*] Applying status-table patch to render /status as a Key:Value table..."

# sanity checks
if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
  echo "[!] Not inside a Git repository."
  exit 1
fi

if [ ! -f "main/http_server.c" ]; then
  echo "[!] main/http_server.c not found. Run this from the project root."
  exit 1
fi

PATCH_FILE=".status-table.patch"

cat > "$PATCH_FILE" <<'PATCH'
diff --git a/main/http_server.c b/main/http_server.c
index 0000000..1111111 100644
--- a/main/http_server.c
+++ b/main/http_server.c
@@ -1,20 +1,26 @@
-    httpd_resp_sendstr_chunk(req,
-        "<!doctype html><html><head>"
+    httpd_resp_sendstr_chunk(req,
+        "<!doctype html><html><head>"
         "<meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
         "<title>");
     httpd_resp_sendstr_chunk(req, titlebuf);
-    httpd_resp_sendstr_chunk(req, "</title>"
-        "<style>"
-        "body{font-family:sans-serif;margin:1rem}"
-        ".row{margin:0.75rem 0}"
-        ".btn{padding:0.6rem 1rem;margin:0.25rem;border:1px solid #ccc;border-radius:10px;cursor:pointer}"
-        ".btn.pushed{background:#ddd}"
-        "#status{width:95%;height:16rem;border:1px solid #ccc;padding:0.5rem;white-space:pre;overflow:auto}"
-        ".group{font-weight:600;margin-right:0.5rem}"
-        "</style></head><body>");
+    httpd_resp_sendstr_chunk(req, "</title>"
+        "<style>"
+        "body{font-family:sans-serif;margin:1rem}"
+        ".row{margin:0.75rem 0}"
+        ".btn{padding:0.6rem 1rem;margin:0.25rem;border:1px solid #ccc;border-radius:10px;cursor:pointer}"
+        ".btn.pushed{background:#ddd}"
+        "#status{width:95%;height:16rem;border:1px solid #ccc;padding:0.5rem;overflow:auto}"
+        "#statTable{width:100%;border-collapse:collapse}"
+        "#statTable th,#statTable td{border:1px solid #ccc;padding:4px 8px;text-align:left;vertical-align:top}"
+        "#statTable th{background:#f5f5f5;width:30%}"
+        ".group{font-weight:600;margin-right:0.5rem}"
+        "</style></head><body>");
@@ -60,12 +66,18 @@
-    httpd_resp_sendstr_chunk(req,
-        "<h3>Status</h3><pre id=\"status\"></pre>"
+    httpd_resp_sendstr_chunk(req,
+        "<h3>Status</h3><div id=\"status\"><table id=\"statTable\"></table></div>"
         "<script>"
-        "const statusBox=document.getElementById('status');"
-        "async function refresh(){try{const r=await fetch('/status');const t=await r.text();statusBox.textContent=t;}catch(e){statusBox.textContent='(error fetching /status)'}}"
+        "const statTable=document.getElementById('statTable');"
+        "function renderStatus(d){let html=\"\";const ks=Object.keys(d).sort();for(let i=0;i<ks.length;i++){const k=ks[i];let v=d[k];if(v===null||v===undefined){v=\"\";}else if(typeof v===\"object\"){try{v=JSON.stringify(v);}catch(_){v=String(v);}}else{v=String(v);}v=v.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\");html+=\"<tr><th>\"+k+\"</th><td>\"+v+\"</td></tr>\";}statTable.innerHTML=html;}"
+        "async function refresh(){try{const r=await fetch('/status');const j=await r.json();renderStatus(j);}catch(e){statTable.innerHTML='<tr><td>(error fetching /status)</td></tr>';}}"
         "function pushMark(btn){btn.classList.add('pushed');setTimeout(()=>btn.classList.remove('pushed'),2000)}"
         "async function fire(uri,btn){pushMark(btn);try{await fetch(uri,{method:'POST'});}catch(e){} setTimeout(refresh,1000);}"
         "document.querySelectorAll('.btn').forEach(b=>b.addEventListener('click',()=>fire(b.dataset.uri,b)));"
         "setInterval(refresh,10000);"
         "refresh();"
         "</script>"
         "</body></html>"
     );
PATCH

echo "[*] Creating a safety commit of current state..."
git add -A
git commit -m "wip: pre-patch snapshot" || true

echo "[*] Applying patch..."
if git apply --index "$PATCH_FILE"; then
  echo "[+] Patch applied and staged."
else
  echo "[!] Patch did not apply cleanly. Trying with --reject..."
  git apply --reject "$PATCH_FILE" || {
    echo "[x] Patch failed even with --reject. See *.rej hunks." >&2
    exit 1
  }
  echo "[*] Patch applied with rejects. Please resolve *.rej hunks."
fi

echo "[*] Done. To commit:"
echo "    git commit -m \"root: render /status JSON as a Key:Value table\""
