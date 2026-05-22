#!/usr/bin/env python3

import struct
import mmap
import os
import sys
import time
import subprocess
import shutil
import ctypes
import ctypes.util
import json
import threading
import signal
from http.server import BaseHTTPRequestHandler, HTTPServer
import webbrowser

SHM_NAME     = "/vidpipe_shm"
MAX_WORKERS  = 16
OUTPUT_FILE  = "output_final.mp4"
PORT         = 8080

STRUCT_FMT   = f"=iii{MAX_WORKERS}i{MAX_WORKERS}i{MAX_WORKERS}i"
STRUCT_SIZE  = struct.calcsize(STRUCT_FMT)
WORKER_STATUS = {0: "IDLE", 1: "ENCODING", 2: "DONE", 3: "CRASHED"}

def handle_sigtstp(signum, frame):
    print("\\n[MON] Ctrl+Z received. Terminating GUI cleanly...")
    os._exit(0)

signal.signal(signal.SIGTSTP, handle_sigtstp)

def _open_shm_fd(name: str) -> int:
    rt = ctypes.CDLL(ctypes.util.find_library("rt") or "librt.so.1", use_errno=True)
    fd = rt.shm_open(name.encode(), 0, 0)
    if fd < 0:
        return -1
    return fd

def read_shm():
    fd = _open_shm_fd(SHM_NAME)
    if fd < 0:
        return None
    try:
        with mmap.mmap(fd, STRUCT_SIZE, access=mmap.ACCESS_READ) as mm:
            raw = mm.read(STRUCT_SIZE)
    finally:
        os.close(fd)

    fields = struct.unpack(STRUCT_FMT, raw)
    workers = []
    nw = min(fields[0], MAX_WORKERS)
    
    worker_frames = fields[3:3+MAX_WORKERS]
    worker_total = fields[3+MAX_WORKERS:3+2*MAX_WORKERS]
    worker_status = fields[3+2*MAX_WORKERS:3+3*MAX_WORKERS]
    
    for i in range(nw):
        workers.append({
            "id": i,
            "frames_done": worker_frames[i],
            "frames_total": worker_total[i],
            "status": WORKER_STATUS.get(worker_status[i], "UNKNOWN")
        })

    return {
        "num_workers": fields[0],
        "total_frames": fields[1],
        "pipeline_done": bool(fields[2]),
        "workers": workers
    }

HTML_CONTENT = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Video Pipeline Monitor</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;700&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-color: #0b0f19;
            --card-bg: rgba(17, 24, 39, 0.65);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --accent: #38bdf8;
            --success: #34d399;
            --danger: #f87171;
            --border: rgba(255, 255, 255, 0.08);
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Outfit', sans-serif;
            background: #0b0f19;
            background-image: 
                radial-gradient(circle at 15% 50%, rgba(56, 189, 248, 0.15), transparent 25%),
                radial-gradient(circle at 85% 30%, rgba(139, 92, 246, 0.15), transparent 25%);
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 3rem;
            overflow-x: hidden;
        }
        /* Animated Background Orbs */
        .orb {
            position: fixed; border-radius: 50%; filter: blur(80px); z-index: -1;
            animation: float 10s ease-in-out infinite alternate;
        }
        .orb-1 { width: 300px; height: 300px; background: rgba(56, 189, 248, 0.2); top: 10%; left: 10%; }
        .orb-2 { width: 400px; height: 400px; background: rgba(139, 92, 246, 0.2); bottom: 10%; right: 10%; animation-delay: -5s; }
        
        @keyframes float { 0% { transform: translate(0, 0); } 100% { transform: translate(30px, -50px); } }

        h1 {
            font-weight: 700; font-size: 3rem; letter-spacing: -1px;
            background: linear-gradient(135deg, #e0f2fe, #38bdf8);
            -webkit-background-clip: text; -webkit-text-fill-color: transparent;
            margin-bottom: 0.5rem; text-align: center;
        }
        .subtitle { color: var(--text-muted); font-size: 1.1rem; margin-bottom: 3rem; text-align: center; font-weight: 300; }
        
        .dashboard {
            width: 100%; max-width: 950px;
            background: var(--card-bg);
            backdrop-filter: blur(20px); -webkit-backdrop-filter: blur(20px);
            border: 1px solid var(--border);
            border-radius: 24px; padding: 2.5rem;
            box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5), inset 0 1px 0 rgba(255,255,255,0.1);
        }
        
        .global-status {
            display: flex; justify-content: space-between; align-items: center;
            padding-bottom: 2rem; border-bottom: 1px solid var(--border); margin-bottom: 2rem;
        }
        .stat-group { display: flex; gap: 3rem; }
        .stat-box { display: flex; flex-direction: column; }
        .stat-value { font-size: 2rem; font-weight: 700; color: #fff; }
        .stat-label { font-size: 0.85rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: 1.5px; margin-top: 4px; }
        
        .status-badge {
            padding: 0.6rem 1.2rem; border-radius: 9999px; font-weight: 600; font-size: 0.95rem;
            display: flex; align-items: center; gap: 0.75rem; transition: all 0.4s cubic-bezier(0.4, 0, 0.2, 1);
        }
        .status-running { background: rgba(56, 189, 248, 0.1); color: #38bdf8; border: 1px solid rgba(56, 189, 248, 0.2); box-shadow: 0 0 15px rgba(56, 189, 248, 0.2); }
        .status-done { background: rgba(52, 211, 153, 0.1); color: #34d399; border: 1px solid rgba(52, 211, 153, 0.2); box-shadow: 0 0 15px rgba(52, 211, 153, 0.2); transform: scale(1.05); }
        
        .workers-grid { display: grid; grid-template-columns: 1fr; gap: 1.25rem; }
        .worker-card {
            background: rgba(15, 23, 42, 0.4); border: 1px solid var(--border);
            border-radius: 16px; padding: 1.5rem; display: flex; flex-direction: column; gap: 1.25rem;
            transition: all 0.3s ease; position: relative; overflow: hidden;
        }
        .worker-card:hover { transform: translateY(-3px); border-color: rgba(255,255,255,0.15); box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5); }
        
        .worker-header { display: flex; justify-content: space-between; align-items: center; }
        .worker-title { font-weight: 600; font-size: 1.15rem; display: flex; align-items: center; gap: 0.5rem; }
        .worker-title::before { content: ''; width: 8px; height: 8px; border-radius: 50%; background: var(--text-muted); }
        .active-worker .worker-title::before { background: var(--accent); box-shadow: 0 0 10px var(--accent); animation: pulse 2s infinite; }
        .done-worker .worker-title::before { background: var(--success); }
        
        .worker-state { font-size: 0.8rem; font-weight: 700; padding: 0.3rem 0.8rem; border-radius: 8px; text-transform: uppercase; letter-spacing: 1px; }
        .state-IDLE { background: rgba(148, 163, 184, 0.1); color: #94a3b8; }
        .state-ENCODING { background: rgba(56, 189, 248, 0.1); color: #38bdf8; }
        .state-DONE { background: rgba(52, 211, 153, 0.1); color: #34d399; }
        .state-CRASHED { background: rgba(239, 68, 68, 0.2); color: #f87171; }
        
        .progress-container { width: 100%; height: 10px; background: rgba(0,0,0,0.4); border-radius: 9999px; overflow: hidden; position: relative; box-shadow: inset 0 1px 3px rgba(0,0,0,0.5); }
        .progress-bar {
            height: 100%; border-radius: 9999px; width: 0%; position: relative;
            background: linear-gradient(90deg, #0ea5e9, #6366f1);
            transition: width 0.4s cubic-bezier(0.4, 0, 0.2, 1);
        }
        .progress-bar::after {
            content: ''; position: absolute; top: 0; left: 0; bottom: 0; right: 0;
            background: linear-gradient(90deg, transparent, rgba(255,255,255,0.3), transparent);
            transform: translateX(-100%); animation: shimmer 1.5s infinite;
        }
        .progress-done .progress-bar { background: linear-gradient(90deg, #10b981, #34d399); }
        .progress-done .progress-bar::after { animation: none; }
        
        .worker-stats { display: flex; justify-content: space-between; font-size: 0.95rem; color: var(--text-muted); font-weight: 500; }
        
        /* Completion overlay message */
        #completion-message {
            display: none; flex-direction: column; align-items: center; justify-content: center;
            margin-top: 2rem; padding: 2rem; border-radius: 16px;
            background: linear-gradient(135deg, rgba(16, 185, 129, 0.1), rgba(52, 211, 153, 0.05));
            border: 1px solid rgba(52, 211, 153, 0.3);
            animation: slideUp 0.6s cubic-bezier(0.4, 0, 0.2, 1) forwards;
        }
        #completion-message h2 { color: #34d399; font-size: 2rem; margin-bottom: 0.5rem; text-shadow: 0 0 20px rgba(52, 211, 153, 0.4); }
        #completion-message p { color: var(--text-muted); font-size: 1.1rem; }
        
        .spinner {
            width: 18px; height: 18px; border: 2px solid rgba(255,255,255,0.2);
            border-radius: 50%; border-top-color: #fff; animation: spin 0.8s linear infinite;
        }
        
        @keyframes spin { to { transform: rotate(360deg); } }
        @keyframes shimmer { 100% { transform: translateX(100%); } }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
        @keyframes slideUp { from { opacity: 0; transform: translateY(20px); } to { opacity: 1; transform: translateY(0); } }
    </style>
</head>
<body>
    <div class="orb orb-1"></div>
    <div class="orb orb-2"></div>

    <h1>Video Pipeline Monitor</h1>
    <div class="subtitle">Distributed Processing Command Center</div>
    
    <div class="dashboard">
        <div class="global-status">
            <div class="stat-group">
                <div class="stat-box">
                    <span class="stat-value" id="tot-frames">-</span>
                    <span class="stat-label">Total Frames</span>
                </div>
                <div class="stat-box">
                    <span class="stat-value" id="tot-workers">-</span>
                    <span class="stat-label">Active Workers</span>
                </div>
            </div>
            <div id="main-status" class="status-badge status-running">
                <div class="spinner"></div> <span>Waiting for orchestrator...</span>
            </div>
        </div>
        
        <div class="workers-grid" id="workers-container">
            <!-- Workers will be injected here -->
        </div>
        
        <div id="completion-message">
            <h2>✨ Encoding Completed Successfully! ✨</h2>
            <p>The video player is launching and the dashboard is shutting down cleanly.</p>
        </div>
    </div>

    <script>
        const container = document.getElementById('workers-container');
        const mainStatus = document.getElementById('main-status');
        const totFrames = document.getElementById('tot-frames');
        const totWorkers = document.getElementById('tot-workers');
        const completionMsg = document.getElementById('completion-message');
        
        function updateUI(state) {
            if (!state) return;
            
            totFrames.textContent = state.total_frames;
            totWorkers.textContent = state.num_workers;
            
            if (state.pipeline_done) {
                mainStatus.className = 'status-badge status-done';
                mainStatus.innerHTML = '<span>🚀 Processing Finished</span>';
                completionMsg.style.display = 'flex';
                container.style.opacity = '0.5';
            } else {
                mainStatus.className = 'status-badge status-running';
                mainStatus.innerHTML = '<div class="spinner"></div> <span>Encoding in Progress</span>';
            }
            
            let html = '';
            state.workers.forEach(w => {
                const total = Math.max(w.frames_total, 1);
                const pct = (w.frames_done / total) * 100;
                const isDone = w.status === 'DONE';
                const isEnc = w.status === 'ENCODING';
                const activeClass = isEnc ? 'active-worker' : (isDone ? 'done-worker' : '');
                
                html += `
                    <div class="worker-card ${activeClass}">
                        <div class="worker-header">
                            <div class="worker-title">Worker Node ${String(w.id).padStart(2, '0')}</div>
                            <div class="worker-state state-${w.status}">${w.status}</div>
                        </div>
                        <div class="progress-container ${isDone ? 'progress-done' : ''}">
                            <div class="progress-bar" style="width: ${pct}%"></div>
                        </div>
                        <div class="worker-stats">
                            <span>Processing... ${pct.toFixed(1)}%</span>
                            <span>${w.frames_done} / ${w.frames_total} Frames</span>
                        </div>
                    </div>
                `;
            });
            container.innerHTML = html;
        }

        async function poll() {
            try {
                const res = await fetch('/api/state');
                const data = await res.json();
                updateUI(data);
                if (data && data.pipeline_done) {
                    setTimeout(() => fetch('/api/shutdown', {method: 'POST'}), 1500);
                    return; // Stop polling
                }
            } catch (e) {
                console.log("Waiting for server...");
            }
            setTimeout(poll, 250);
        }
        
        poll();
    </script>
</body>
</html>
"""

class MonitorHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-type', 'text/html')
            self.end_headers()
            self.wfile.write(HTML_CONTENT.encode('utf-8'))
        elif self.path == '/api/state':
            state = read_shm()
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(state).encode('utf-8'))
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path == '/api/shutdown':
            self.send_response(200)
            self.end_headers()
            
            def shutdown_and_exit():
                play_video()
                os._exit(0)
                
            threading.Thread(target=shutdown_and_exit).start()
            
    def log_message(self, format, *args):
        pass # Suppress logs

def play_video():
    print("\\n[MON] Pipeline finished! Launching player...")
    if os.path.isfile(OUTPUT_FILE):
        player = shutil.which("mpv") or shutil.which("vlc")
        if player:
            subprocess.Popen([player, OUTPUT_FILE], 
                             stdout=subprocess.DEVNULL, 
                             stderr=subprocess.DEVNULL, 
                             start_new_session=True)
    else:
        print("[MON] Output file not found.")

def main():
    print(f"\\n[MON] Starting Web Dashboard at http://localhost:{PORT}")
    print("[MON] Opening browser automatically...")
    
    server = HTTPServer(('localhost', PORT), MonitorHandler)
    webbrowser.open(f"http://localhost:{PORT}")
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
