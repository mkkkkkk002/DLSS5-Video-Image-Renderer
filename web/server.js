// Zero-dependency HTTP server driving core/dlss5nr.exe.
//
// The C++ core is a plain command line tool that reports progress on stdout as
// "PROGRESS <done>/<total>" lines. This wraps it: static file serving, a start endpoint that
// spawns the process, and a status endpoint the page polls.

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn, execFile } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const MODELS_DIR = path.join(ROOT, 'models');
// Default output directory for all renders. Created on boot so the first run never errors.
// The user may still override per-job via the output path field / folder picker.
const OUTPUTS_DIR = path.join(ROOT, 'outputs');
const PORT = process.env.PORT || 8777;

// Drag-and-drop copies land here (the browser cannot hand us a local path, only file content).
// Everything under it is disposable: swept at service shutdown by server_guard.exe, and stale
// entries are purged whenever a new file is dropped or picked. A source copy is NOT deleted when
// its job finishes, so the compare/preview player can keep using it afterwards. The startup sweep
// below only catches leftovers from an abrupt kill that bypassed the guard. Output files are
// never written here.
const UPLOADS_DIR = path.join(ROOT, '.tmp_uploads');

// Single-frame preview (compare) PNGs live here. Same disposable policy: swept at shutdown by
// server_guard.exe (the startup sweep below is the abrupt-kill fallback).
const FRAME_DIR = path.join(ROOT, '.frame_previews');

function isTempUpload(p) {
    if (!p) return false;
    const dir = path.normalize(UPLOADS_DIR) + path.sep;
    return path.normalize(p).startsWith(dir);
}

// Dropped/pasted files are classified video vs image by extension. Video and image inputs are
// independent features that may be open at the same time, so temp-upload purges only sweep the
// same kind (a new video drop must not destroy a still image being edited, and vice versa).
const VIDEO_EXT = ['.mp4', '.mov', '.mkv', '.avi', '.webm', '.m4v'];
const IMAGE_EXT = ['.png', '.jpg', '.jpeg', '.bmp', '.webp', '.tif', '.tiff'];
function uploadKind(p) {
    const ext = path.extname(p).toLowerCase();
    if (IMAGE_EXT.includes(ext)) return 'img';
    if (VIDEO_EXT.includes(ext)) return 'video';
    return null;
}

function isFramePath(p) {
    if (!p) return false;
    const dir = path.normalize(FRAME_DIR) + path.sep;
    return path.normalize(p).startsWith(dir);
}

function cleanUploadsDir() {
    try {
        for (const f of fs.readdirSync(UPLOADS_DIR)) {
            fs.unlinkSync(path.join(UPLOADS_DIR, f));
        }
    } catch (e) { /* ignore */ }
}

function cleanFrameDir() {
    try {
        for (const f of fs.readdirSync(FRAME_DIR)) {
            fs.unlinkSync(path.join(FRAME_DIR, f));
        }
    } catch (e) { /* ignore */ }
}

// Resolves the engine executable at job-start time (not module load) so a rebuild is picked
// up automatically. Tries the canonical name first, then any dlss5nr*.exe in core/, so the
// one-off rename used to dodge a locked-by-zombie exe never breaks the UI.
function findEngine() {
    const coreDir = path.join(ROOT, 'core');
    const candidates = ['dlss5nr_engine.exe', 'dlss5nr_run.exe', 'dlss5nr_app.exe', 'dlss5nr_core.exe', 'dlss5nr.exe'];
    for (const c of candidates) {
        const p = path.join(coreDir, c);
        if (fs.existsSync(p)) return p;
    }
    try {
        const files = fs.readdirSync(coreDir).filter((f) => /^dlss5nr.*\.exe$/i.test(f));
        if (files.length) return path.join(coreDir, files[0]);
    } catch (e) { /* ignore */ }
    return path.join(coreDir, 'dlss5nr_run.exe'); // reported only if it truly does not exist
}

// Resolves the NR model dll + forwarder pair for a job. cfg.model selects the precision
// ('fp16' | 'fp8' | 'auto'); an explicit cfg.snippet / cfg.forwarder (absolute or ROOT-relative)
// overrides the auto-selection. Throws with a clear message when the requested model is missing
// so the caller can surface it in the UI instead of failing deep inside the engine.
function resolveModelFiles(cfg) {
    const exist = (p) => fs.existsSync(p);
    // Bare filenames resolve against models/ (the current layout); anything containing a path
    // separator stays ROOT-relative / absolute for backward compatibility with explicit configs.
    const abs = (p) => {
        if (path.isAbsolute(p)) return p;
        if (!p.includes('/') && !p.includes('\\')) return path.join(MODELS_DIR, p);
        return path.join(ROOT, p);
    };
    if (cfg.snippet) {
        const snippet = abs(cfg.snippet);
        const forwarder = cfg.forwarder
            ? abs(cfg.forwarder)
            : path.join(path.dirname(snippet), 'nvngx.dll' + path.basename(snippet).slice(6));
        if (!exist(snippet)) throw new Error('模型文件不存在: ' + snippet);
        if (!exist(forwarder)) throw new Error('模型配套 forwarder 不存在: ' + forwarder);
        return { snippet, forwarder };
    }
    const want = String(cfg.model || 'auto').toLowerCase();
    const pairs = want === 'fp16'
        ? [['nvngx_dlssnr_fp16.dll', 'nvngx.dll_dlssnr_fp16.dll'],
           ['nvngx_dlssnr.dll', 'nvngx.dll_dlssnr.dll']]
        : want === 'fp8'
            ? [['nvngx_dlssnr_fp8.dll', 'nvngx.dll_dlssnr_fp8.dll']]
            : [['nvngx_dlssnr.dll', 'nvngx.dll_dlssnr.dll'],
               ['nvngx_dlssnr_fp16.dll', 'nvngx.dll_dlssnr_fp16.dll'],
               ['nvngx_dlssnr_fp8.dll', 'nvngx.dll_dlssnr_fp8.dll']];
    for (const [s, f] of pairs) {
        const sn = path.join(MODELS_DIR, s), fw = path.join(MODELS_DIR, f);
        if (exist(sn) && exist(fw)) return { snippet: sn, forwarder: fw };
    }
    throw new Error('未找到 ' + (want === 'auto' ? '任何' : want.toUpperCase() + ' 精度') +
        ' 模型。请在 models/ 目录放置 nvngx_dlssnr*.dll 及其配套 nvngx.dll_dlssnr*.dll');
}

// Opens the given URL in the default browser. rundll32 url.dll,FileProtocolHandler is the most
// reliable Windows way to hand a URL to the OS; explorer.exe is a fallback if that ever fails.
function openBrowser(url) {
    const { execFile } = require('child_process');
    if (process.platform === 'win32') {
        execFile('rundll32.exe', ['url.dll,FileProtocolHandler', url], { windowsHide: true }, (e) => {
            if (e) execFile('explorer.exe', [url], { windowsHide: true }, () => {});
        });
    } else if (process.platform === 'darwin') {
        execFile('open', [url], () => {});
    } else {
        execFile('xdg-open', [url], () => {});
    }
}

function mimeFor(p) {
    const ext = path.extname(p).toLowerCase();
    return (
        {
            '.mp4': 'video/mp4',
            '.mov': 'video/quicktime',
            '.mkv': 'video/x-matroska',
            '.webm': 'video/webm',
            '.m4v': 'video/mp4',
            '.avi': 'video/x-msvideo',
            '.png': 'image/png',
            '.jpg': 'image/jpeg',
            '.jpeg': 'image/jpeg',
            '.bmp': 'image/bmp',
            '.webp': 'image/webp',
            '.tif': 'image/tiff',
            '.tiff': 'image/tiff',
        }[ext] || 'application/octet-stream'
    );
}

// Single-shot ffmpeg wrapper used by the image-render helpers. "-y -v error" are always added.
function runFfmpeg(args) {
    return new Promise((ok, bad) => {
        const p = spawn('ffmpeg', ['-y', '-v', 'error', ...args], { windowsHide: true });
        let err = '';
        p.stderr.on('data', (c) => { err += c.toString('utf8'); });
        p.on('close', (code) => code === 0 ? ok() : bad(new Error('ffmpeg exit ' + code + ' ' + err.trim())));
        p.on('error', bad);
    });
}

// Single-shot engine wrapper for the instant-render helpers (compare-frame & export-image).
// The engine prints diagnostics to STDOUT (printf "ERROR ..."), so a non-zero exit surfaces the
// tail of its stdout -- otherwise the UI would only ever show a bare "engine exit <code>".
function runEngine(exe, args) {
    return new Promise((ok, bad) => {
        const p = spawn(exe, args, { cwd: ROOT, windowsHide: true });
        let out = '', err = '';
        p.stdout.on('data', (c) => {
            out += c.toString('utf8');
            if (out.length > 65536) out = out.slice(-65536);
        });
        p.stderr.on('data', (c) => {
            err += c.toString('utf8');
            if (err.length > 65536) err = err.slice(-65536);
        });
        p.on('close', (code) => {
            if (code === 0) return ok();
            const detail = [...out.split(/\r?\n/), ...err.split(/\r?\n/)]
                .map((s) => s.trim())
                .filter((s) => s && /error|failed|exit|unable|cannot|not found|no |refused|abort/i.test(s))
                .slice(-8)
                .join(' | ');
            bad(new Error('engine exit ' + (code === null ? -1 : code) + (detail ? ' — ' + detail : '')));
        });
        p.on('error', bad);
    });
}

// Runs a PowerShell file-dialog snippet and resolves with the chosen path.
//
// Three things are required to make a dialog actually appear in front of the browser window:
//   1. -STA (file dialogs need a single-threaded apartment).
//   2. A real, shown TopMost owner Form -- otherwise the z-order relationship never gets set
//      up and the dialog sits behind whatever window has focus.
//   3. A timer that calls SetForegroundWindow repeatedly while the dialog is open. The PowerShell
//      process is NOT the foreground process, so a one-shot call would be silently dropped by
//      Windows' foreground lock; repeating the call is the standard workaround.
//
// scriptBody should construct and configure $dlg only; this wrapper owns the owner form, the
// timer, ShowDialog, and stdout output.
function runFileDialog(scriptBody, valueExpr) {
    if (valueExpr === undefined) valueExpr = '$($dlg.FileName)';
    return new Promise((resolve) => {
        const ps =
            "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; " +
            // DPI awareness first: without it WinForms dialogs render small/blurry on scaled displays
            "try { if (-not ('PInvoke.Dpi' -as [type])) { Add-Type -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool SetProcessDPIAware();' -Name Dpi -Namespace PInvoke } } catch { }; " +
            "try { [PInvoke.Dpi]::SetProcessDPIAware() | Out-Null } catch { }; " +
            "try { if (-not ('PInvoke.Mui' -as [type])) { Add-Type -MemberDefinition '[DllImport(\"kernel32.dll\", CharSet = CharSet.Unicode)] public static extern bool SetProcessPreferredUILanguages(uint dwFlags, string pwszLanguagesBuffer, ref uint pulNumLanguages);' -Name Mui -Namespace PInvoke } } catch { }; " +
            "try { $n = [uint32]0; [PInvoke.Mui]::SetProcessPreferredUILanguages(8, \"zh-CN`0\", [ref]$n) | Out-Null } catch { }; " +
            // force Chinese UI strings for the managed dialog parts (buttons/labels of FolderBrowserDialog etc.)
            "try { $c = New-Object System.Globalization.CultureInfo('zh-CN'); " +
            "[System.Threading.Thread]::CurrentThread.CurrentUICulture = $c; " +
            "[System.Threading.Thread]::CurrentThread.CurrentCulture = $c } catch { }; " +
            "Add-Type -AssemblyName System.Windows.Forms; " +
            "try { [System.Windows.Forms.Application]::EnableVisualStyles() } catch { }; " +
            "if (-not ('PInvoke.Win32' -as [type])) { " +
            "Add-Type -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool SetForegroundWindow(IntPtr hWnd);' " +
            "-Name Win32 -Namespace PInvoke " +
            "}; " +
            "$owner = New-Object System.Windows.Forms.Form; " +
            "$owner.TopMost = $true; $owner.WindowState = 'Minimized'; $owner.ShowInTaskbar = $false; " +
            "$owner.Show(); " +
            scriptBody + " " +
            "$timer = New-Object System.Windows.Forms.Timer; $timer.Interval = 80; " +
            "$timer.Add_Tick({ if ($dlg.Handle -ne [IntPtr]::Zero) { [PInvoke.Win32]::SetForegroundWindow($dlg.Handle) | Out-Null } }); " +
            "$timer.Start(); " +
            "$result = $dlg.ShowDialog($owner); " +
            "$timer.Stop(); $owner.Close(); " +
            "if ($result -eq [System.Windows.Forms.DialogResult]::OK) { Write-Host -NoNewline \"__PATH__" + valueExpr + "\" } else { Write-Host -NoNewline '__CANCELLED__' }";
        execFile(
            'powershell.exe',
            ['-NoProfile', '-STA', '-WindowStyle', 'Hidden', '-Command', ps],
            { windowsHide: true, encoding: 'utf8', timeout: 180000 },
            (err, stdout) => {
                if (err) return resolve({ error: err.message });
                const out = (stdout || '').trim();
                if (out === '__CANCELLED__' || out === '') return resolve({ cancelled: true });
                if (!out.startsWith('__PATH__')) return resolve({ error: 'unexpected dialog output: ' + out });
                resolve({ path: out.slice('__PATH__'.length) });
            }
        );
    });
}

let current = null;   // { id, child, done, total, lines, finished, code }
let nextId = 1;

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.json': 'application/json; charset=utf-8',
};

function sendJson(res, code, obj) {    const body = JSON.stringify(obj);
    res.writeHead(code, {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': Buffer.byteLength(body),
        'Cache-Control': 'no-store',
    });
    res.end(body);
}

function readBody(req) {
    return new Promise((resolve) => {
        let data = '';
        req.on('data', (c) => {
            data += c;
            if (data.length > 1e6) req.destroy();
        });
        req.on('end', () => {
            try {
                resolve(data ? JSON.parse(data) : {});
            } catch (e) {
                resolve({});
            }
        });
    });
}

// No file uploads: the UI asks the server to show a native file picker (see /api/pick-file)
// so the selected path stays the original file, and the output defaults to the same folder.

// Returns a path under dir that does not yet exist, appending _1, _2, ... to the stem.
function uniquePath(dir, name) {
    let cand = path.join(dir, name);
    if (!fs.existsSync(cand)) return cand;
    const ext = path.extname(name);
    const stem = path.basename(name, ext);
    let n = 1;
    do {
        cand = path.join(dir, stem + '_' + n + ext);
        n++;
    } while (fs.existsSync(cand));
    return cand;
}

// Resolves the output path: a requested name is honoured but made unique; when nothing is
// requested we derive nr_<stem><ext> under <ROOT>/outputs/. Drag-and-drop temp copies keep
// their original filename (see /api/upload), so the stem is taken from the input either way.
function resolveOutput(input, requested) {
    const dir = OUTPUTS_DIR;
    const ext = path.extname(input) || '.mp4';
    let base;
    if (requested && requested.trim()) {
        base = requested.trim();
    } else {
        base = path.join(dir, 'nr_' + path.basename(input, ext) + ext);
    }
    return uniquePath(path.dirname(base), path.basename(base));
}

// Runs ffprobe to fill in the resolution and frame rate before anything is started.
function probe(input) {
    return new Promise((resolve) => {
        execFile(
            'ffprobe',
            [
                '-v', 'error',
                '-select_streams', 'v:0',
                '-show_entries', 'stream=width,height,r_frame_rate,duration',
                '-of', 'default=noprint_wrappers=1',
                input,
            ],
            { windowsHide: true },
            (err, stdout) => {
                if (err && !stdout) return resolve({ ok: false, error: 'ffprobe failed' });
                const info = { width: 0, height: 0, fps: 0, frames: 0 };
                for (const line of stdout.split('\n')) {
                    const [k, v] = line.split('=');
                    if (k === 'width') info.width = parseInt(v, 10);
                    else if (k === 'height') info.height = parseInt(v, 10);
                    else if (k === 'r_frame_rate') {
                        if (v.includes('/')) {
                            const [a, b] = v.split('/');
                            info.fps = parseFloat(a) / parseFloat(b);
                        } else info.fps = parseFloat(v);
                    } else if (k === 'duration') {
                        if (info.fps > 0) info.frames = Math.round(parseFloat(v) * info.fps);
                    }
                }
                resolve({ ok: info.width > 0, info });
            }
        );
    });
}

// Builds the engine argument list for one pass. Only the first pass (from the original file)
// honours the crop window; later passes re-render the previous pass's full output. When
// `master` is true the pass writes the lossless 10-bit intermediate (two-stage flow): the user's
// encoder/pixel-format/codec args are deferred to /api/export so encoding never re-runs the model.
function engineArgs(cfg, input, output, useWindow, master) {
    const { snippet, forwarder } = resolveModelFiles(cfg);
    const args = [
        '--input', input,
        '--output', output,
        '--snippet', snippet,
        '--forwarder', forwarder,
        '--encoder', master ? 'hevc10_lossless' : (cfg.encoder || 'h264_nvenc'),
        '--preset', String(cfg.preset ?? 0),
        '--intensity', String(cfg.intensity ?? 1.0),
        '--style', String(cfg.style ?? 0),
        '--local-tone', String(cfg.localTone ?? 1.0),
        '--local-structure', String(cfg.localStructure ?? 1.0),
        '--skin-structure', String(cfg.skinStructure ?? 0.5),
        '--auto-mask', String(cfg.autoMask ?? 1),
        '--ui-correction', String(cfg.uiCorrection ?? 0),
    ];
    if (useWindow) {
        if (cfg.startTime > 0) args.push('--start-time', String(cfg.startTime));
        if (cfg.endTime > 0) args.push('--end-time', String(cfg.endTime));
    }
    if (!master && cfg.codecArgs) args.push('--codec-args', cfg.codecArgs);
    if (!master && cfg.pixFmt) args.push('--pix-fmt', cfg.pixFmt);
    args.push('--residual-mult', String(cfg.residualMult ?? 1.0));
    args.push('--frame-guidance', String(cfg.frameGuidance ?? 3));
    args.push('--depth-interval', String(cfg.depthInterval ?? 0));
    return args;
}

// ffmpeg encoding presets used by /api/export (a fast transcode of the lossless master). Keys
// match the UI encoder ids; *_10bit stay in 10-bit, the rest convert down to 8-bit yuv420p.
const EXPORT_ENCODERS = {
    h264_nvenc: ['-c:v', 'h264_nvenc', '-preset', 'p4', '-rc', 'vbr', '-cq', '20', '-b:v', '0', '-pix_fmt', 'yuv420p'],
    hevc_nvenc: ['-c:v', 'hevc_nvenc', '-preset', 'p4', '-rc', 'vbr', '-cq', '22', '-b:v', '0', '-pix_fmt', 'yuv420p'],
    libx264: ['-c:v', 'libx264', '-crf', '18', '-preset', 'medium', '-pix_fmt', 'yuv420p'],
    libx265: ['-c:v', 'libx265', '-crf', '20', '-preset', 'medium', '-pix_fmt', 'yuv420p'],
    hevc_nvenc_10bit: ['-c:v', 'hevc_nvenc', '-preset', 'p4', '-rc', 'vbr', '-cq', '20', '-b:v', '0', '-vf', 'format=p010le', '-profile:v', 'main10'],
    libx265_10bit: ['-c:v', 'libx265', '-crf', '18', '-preset', 'medium', '-pix_fmt', 'yuv420p10le'],
};

// Live state of the most recent / most current export transcode (UI polls it once a second).
const exporter = { running: false, pct: 0, encoder: null, out: null, error: null };

// Resolves where an export should land: blank -> <ROOT>/outputs/<defaultName>; an existing dir or
// a trailing slash -> that directory + <defaultName>; otherwise treated as an explicit file path.
function resolveExportPath(requested, defaultName) {
    if (!requested || !requested.trim()) return uniquePath(OUTPUTS_DIR, defaultName);
    let p = requested.trim();
    let isDir = false;
    try { isDir = fs.statSync(p).isDirectory(); } catch (e) { isDir = /[\\/]$/.test(p); }
    if (isDir) return uniquePath(p, defaultName);
    const ext = path.extname(p);
    if (!ext) return uniquePath(p, defaultName); // looks like a bare dir that doesn't exist yet
    fs.mkdirSync(path.dirname(p), { recursive: true });
    return uniquePath(path.dirname(p), path.basename(p));
}

function probeDurationSec(file) {
    return new Promise((resolve) => {
        execFile('ffprobe', ['-v', 'error', '-select_streams', 'v:0', '-show_entries',
            'format=duration', '-of', 'csv=p=0', file], { windowsHide: true }, (e, out) => {
            const t = parseFloat((out || '').trim());
            resolve(isFinite(t) && t > 0 ? t : 0);
        });
    });
}

// Spawns ffmpeg with -progress on stdout; parses out_time_ms to update exporter.pct roughly
// once per second. Never touches the DLSS model -- it is a pure transcode of the master.
function runExport(master, encoder, finalOut) {
    return new Promise(async (resolve, reject) => {
        const dur = await probeDurationSec(master);
        const args = ['-y', '-nostats', '-i', master, '-map', '0', '-c:a', 'copy',
            ...EXPORT_ENCODERS[encoder], '-progress', 'pipe:1', '-movflags', '+faststart', finalOut];
        const child = spawn('ffmpeg', args, { windowsHide: true });
        let buf = '';
        child.stdout.on('data', (c) => {
            buf += c.toString('utf8');
            let nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
                const l = buf.slice(0, nl);
                buf = buf.slice(nl + 1);
                if (l.startsWith('out_time_ms=')) {
                    const t = parseInt(l.split('=')[1] || '0', 10) / 1e6;
                    if (dur > 0) exporter.pct = Math.max(0, Math.min(99, Math.round((t / dur) * 100)));
                }
            }
        });
        child.on('error', reject);
        child.on('close', (code) => {
            exporter.pct = code === 0 ? 100 : exporter.pct;
            if (code === 0) resolve(); else reject(new Error('ffmpeg export exit code ' + code));
        });
    });
}

// 8-bit H.264 browser preview of the lossless master (10-bit HEVC masters are not playable in
// some browsers). Lives in the frame dir so it is cleaned when the service shuts down.
function makeBrowserPreview(masterPath) {
    return new Promise((resolve) => {
        try {
            if (!masterPath || !fs.existsSync(masterPath)) return resolve(null);
            fs.mkdirSync(FRAME_DIR, { recursive: true });
            const stem = path.basename(masterPath, path.extname(masterPath));
            const out = path.join(FRAME_DIR, stem + '_preview.mp4');
            runFfmpeg([
                '-i', masterPath,
                '-c:v', 'libx264', '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p',
                '-c:a', 'aac', '-b:a', '192k',
                '-movflags', '+faststart',
                out,
            ]).then(() => resolve(out)).catch(() => resolve(null));
        } catch (e) {
            resolve(null);
        }
    });
}

// Whole-video multi-pass plan ("渲染次数"): pass 1 decodes the original file (honouring the
// crop window); every later pass re-renders the *full* previous pass output as if it were a new
// video, so the final file is the N-th generation. Intermediate outputs are temp files that are
// removed when the job finishes. Each pass is a fresh engine process, i.e. exactly what you would
// get by running the CLI N times by hand.
function buildSteps(cfg, finalOut) {
    const passes = Math.max(1, parseInt(cfg.renderPasses, 10) || 1);
    if (passes === 1) {
        return [{ input: cfg.input, output: finalOut, window: true }];
    }
    const dir = path.dirname(finalOut);
    const ext = path.extname(finalOut) || '.mp4';
    const stem = path.basename(finalOut, ext);
    const steps = [];
    let prevOut = cfg.input;
    for (let i = 1; i <= passes; ++i) {
        const isLast = i === passes;
        const out = isLast ? finalOut : uniquePath(dir, stem + '.pass' + i + '.tmp' + ext);
        steps.push({ input: prevOut, output: out, window: i === 1 });
        prevOut = out;
    }
    return steps;
}

function startJob(cfg) {
    if (current && !current.finished) {
        return { ok: false, error: 'a job is already running' };
    }
    if (!fs.existsSync(findEngine())) {
        return { ok: false, error: 'engine not found: ' + findEngine() };
    }
    // v1.2-style single-stage render: the chosen encoder writes the FINAL file directly (no
    // lossless master, no browser preview step). File name honours the dedicated file-name
    // field (or defaults to nr_<input>.mp4); the output field is a folder (or blank = outputs/).
    const inputStem = path.basename(cfg.input).replace(/\.[^.]+$/, '');
    const name = (cfg.fileName || '').trim();
    const defaultName = name
        ? (/\.[A-Za-z0-9]{1,5}$/.test(name) ? name : name + '.mp4')
        : 'nr_' + inputStem + '.mp4';
    const finalOut = resolveExportPath(cfg.output, defaultName);

    const job = {
        id: nextId++,
        steps: buildSteps(cfg, finalOut),
        passIndex: 0,            // which step is running (0-based)
        done: 0,
        total: 0,
        framesPerPass: 0,        // frame count of one pass, learned from the first PROGRESS line
        lines: [],
        t0: 0,                   // wall-clock ms when the engine delivered its first PROGRESS line
        t1: 0,                   // wall-clock ms when the job finished (lazily set on first status)
        finished: false,
        cancelled: false,
        code: null,
        cfg,
        output: finalOut,
        master: null,
        preview: null,
        export: null,
    };
    current = job;
    if (job.steps.length > 1) {
        job.lines.push(
            `whole-video multi-pass: ${job.steps.length} full renders in series (final = ${finalOut})`);
    }
    runNextPass(job);
    return { ok: true, id: job.id, output: finalOut, passes: job.steps.length };
}

function jobFinish(job, cancelled) {
    cleanupTemps(job);
    if (cancelled) {
        job.lines.push('cancelled by user');
        job.cancelled = true;
        job.finished = true;
        job.code = null;
    } else {
        job.lines.push('DONE after ' + job.steps.length + ' pass(es): ' + job.output);
        job.finished = true;
        job.code = 0;
    }
}

function runNextPass(job) {
    if (job.cancelled || job.finished || !job.steps[job.passIndex]) return;
    const step = job.steps[job.passIndex];
    const cfg = job.cfg;
    job.done = 0;
    job.total = 0;
    job.lines.push(`=== pass ${job.passIndex + 1}/${job.steps.length} ===`);

    // Whole-video multi-pass: only the first pass (from the original file) honours the crop
    // window; later passes re-render the previous pass's complete output. Each pass is a fresh
    // engine process -- exactly what running the CLI N times by hand would do.
    const exe = findEngine();
    if (!fs.existsSync(exe)) {
        cleanupTemps(job);
        job.lines.push('ERROR: engine not found: ' + exe);
        job.finished = true;
        job.code = -1;
        return;
    }
    let args;
    try {
        // The final step of the plan is the lossless master; all passes render to it as such.
        args = engineArgs(cfg, step.input, step.output, step.window, step.output === job.master);
    } catch (e) {
        cleanupTemps(job);
        job.lines.push('ERROR: ' + e.message);
        job.finished = true;
        job.code = -1;
        return;
    }
    const child = spawn(exe, args, { cwd: ROOT, windowsHide: true });
    job.child = child;
    let outBuf = '';
    child.stdout.on('data', (c) => {
        outBuf += c.toString('utf8');
        let nl;
        while ((nl = outBuf.indexOf('\n')) >= 0) {
            const line = outBuf.slice(0, nl).replace(/\r$/, '');
            outBuf = outBuf.slice(nl + 1);
            handleLine(job, line);
        }
    });
    let errBuf = '';
    child.stderr.on('data', (c) => {
        errBuf += c.toString('utf8');
        if (errBuf.length > 8000) errBuf = errBuf.slice(-8000);
    });
    child.on('error', (e) => {
        if (job.child !== child) return;
        job.child = null;
        if (!job.cancelled && !job.finished) {
            cleanupTemps(job);
            job.lines.push('ERROR: engine failed to start: ' + e.message);
            job.finished = true;
            job.code = -1;
        }
    });
    child.on('close', (code) => {
        if (job.child === child) job.child = null;
        onPassDone(job, code);
    });
}

function onPassDone(job, code) {
    if (job !== current || job.finished) return;
    if (job.cancelled) {
        jobFinish(job, true);
        return;
    }
    if (code === 0) {
        job.passIndex++;
        if (job.passIndex < job.steps.length) {
            runNextPass(job);
        } else {
            jobFinish(job, false);
        }
    } else {
        cleanupTemps(job);
        job.lines.push('ERROR: engine exit code ' + (code === null ? -1 : code) + ' (see log above)');
        job.finished = true;
        job.code = code === null ? -1 : code;
    }
}

// Removes every intermediate (non-final) step output. Best effort.
// NOTE: the drag-and-drop source upload is intentionally NOT deleted here. Previously it was
// removed once its job finished, which broke the compare/preview player (the preview streams the
// same temp file, so it died right after a render). Now uploads are only purged at server start
// and when the user drops/picks a *different* file (see purgeUploads below).
function cleanupTemps(job) {
    for (let i = 0; i < job.steps.length - 1; ++i) {
        const p = job.steps[i].output;
        try {
            if (fs.existsSync(p)) fs.unlinkSync(p);
        } catch (e) { /* ignore */ }
    }
}

// Deletes stale drag-and-drop uploads of ONE kind (video vs image), keeping only the file the
// user is currently working on (so the preview players keep working until the user replaces the
// input) and any upload still being read by a running job. The kind is inferred from `keep` when
// given; pass it explicitly (e.g. 'video' after picking a disk video file) when there is no keep,
// so only stale uploads of that kind get swept.
function purgeUploads(keep, kind) {
    const keepSet = new Set();
    if (keep) {
        keepSet.add(path.resolve(keep));
        kind = kind || uploadKind(keep) || null;
    }
    // A running whole-video job may still be reading its source upload: never purge it.
    if (current && !current.finished && current.steps[0] && isTempUpload(current.steps[0].input)) {
        keepSet.add(path.resolve(current.steps[0].input));
    }
    try {
        for (const f of fs.readdirSync(UPLOADS_DIR)) {
            const p = path.join(UPLOADS_DIR, f);
            if (keepSet.has(path.resolve(p))) continue;
            if (kind && uploadKind(p) !== kind) continue;
            try {
                if (fs.existsSync(p)) fs.unlinkSync(p);
            } catch (e) { /* ignore */ }
        }
    } catch (e) { /* ignore */ }
}

function handleLine(job, line) {
    if (!line) return;

    // Progress lines drive the bar/percent and must not accumulate in the log as frame spam.
    const m = /^PROGRESS\s+(\d+)\/(\d+)/.exec(line);
    if (m) {
        job.done = parseInt(m[1], 10);
        job.total = parseInt(m[2], 10);
        if (job.total > 0) job.framesPerPass = job.total;
        if (!job.t0) job.t0 = Date.now();   // first decoded/rendered frame: cold start excluded
        return;
    }

    job.lines.push(line);
    if (job.lines.length > 300) job.lines.shift();
}

// ------------------------------------------------------------------ server

const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');

    if (url.pathname === '/api/status' && req.method === 'GET') {
        const j = current;
        if (!j) return sendJson(res, 200, { running: false, lines: [], lineCount: 0 });
        let outputSize = null;
        if (j.output && fs.existsSync(j.output)) {
            try { outputSize = fs.statSync(j.output).size; } catch (e) {}
        }
        // Whole-video multi-pass overall progress. Every pass renders the same frame count, so
        // cumulative frames = finished passes * framesPerPass + current pass progress. A
        // successfully finished job is exactly 100% (passIndex already points past the last pass,
        // which would otherwise double-count its progress).
        const fp = j.framesPerPass || 0;
        const overallTotal = fp * j.steps.length;
        const overallDone = (j.finished && j.code === 0)
            ? overallTotal
            : (fp > 0 ? j.passIndex * fp + j.done : j.done);
        // Throughput + ETA. t0 is the moment the engine produced its first frame (after cold
        // start / NVOF init), so the rate reflects actual render speed. t1 is lazily fixed the
        // first time a finished job is polled.
        const nowMs = Date.now();
        if (j.finished && !j.t1) j.t1 = nowMs;
        const endMs = j.finished ? (j.t1 || nowMs) : nowMs;
        const elapsedSec = Math.max(0, (endMs - (j.t0 || endMs)) / 1000);
        const avgFps = (elapsedSec >= 1 && overallDone > 0) ? overallDone / elapsedSec : 0;
        const etaSec = (!j.finished && avgFps > 0 && overallTotal > overallDone)
            ? Math.round((overallTotal - overallDone) / avgFps)
            : 0;
        // Incremental log lines: client passes the count it already has via ?since= and we slice
        // the fresh tail only (no re-transmission of every line on every poll).
        const since = Math.max(0, parseInt(url.searchParams.get('since') || '0', 10));
        const lineCount = j.lines.length;
        return sendJson(res, 200, {
            running: !j.finished,
            id: j.id,
            done: j.done,
            total: j.total,
            pass: Math.min(j.passIndex + 1, j.steps.length),
            passes: j.steps.length,
            overallDone,
            overallTotal,
            avgFps: Math.round(avgFps * 10) / 10,
            etaSec,
            elapsedSec: Math.round(elapsedSec),
            finished: j.finished,
            code: j.code,
            output: j.output || null,
            master: j.master || null,
            preview: j.preview || null,
            export: {
                running: exporter.running,
                pct: exporter.pct,
                encoder: exporter.encoder,
                out: exporter.out,
                error: exporter.error,
            },
            outputSize,
            lines: j.lines.slice(since),
            lineCount,
        });
    }

    // Lightweight bootstrap endpoint: returns the project root so the page can compose default
    // output paths (<ROOT>/outputs/nr_<stem>.<ext>) without the user typing them.
    if (url.pathname === '/api/info' && req.method === 'GET') {
        return sendJson(res, 200, { ok: true, root: ROOT, outputs: OUTPUTS_DIR });
    }

    if (url.pathname === '/api/probe' && req.method === 'POST') {
        const body = await readBody(req);
        if (!body.input) return sendJson(res, 400, { ok: false, error: 'input is required' });
        return sendJson(res, 200, await probe(body.input));
    }

    // Opens a native file picker on the server machine (Windows) and returns the original file
    // path. This avoids copying the video into uploads/ and keeps output next to the source file.
    if (url.pathname === '/api/pick-file' && req.method === 'GET') {
        if (process.platform !== 'win32') {
            return sendJson(res, 501, { ok: false, error: 'file picker only supported on Windows' });
        }
        const filter = "视频文件 (*.mp4;*.mov;*.mkv;*.avi;*.webm)|*.mp4;*.mov;*.mkv;*.avi;*.webm|所有文件 (*.*)|*.*";
        const body =
            "$dlg = New-Object System.Windows.Forms.OpenFileDialog; " +
            "$dlg.Title = '选择视频文件'; $dlg.Filter = '" + filter + "'; ";
        const r = await runFileDialog(body);
        if (r.error) return sendJson(res, 500, { ok: false, error: r.error });
        if (r.cancelled) return sendJson(res, 200, { ok: false, cancelled: true });
        // The user moved on to a different video: the previous drag-and-drop video copy (if any)
        // is no longer needed. Purge only stale video uploads -- an image being edited for the
        // still-image module may still be open and must survive.
        purgeUploads(null, 'video');
        return sendJson(res, 200, { ok: true, path: r.path, name: path.basename(r.path) });
    }

    // Native open dialog restricted to still images (single-image render).
    if (url.pathname === '/api/pick-image' && req.method === 'GET') {
        if (process.platform !== 'win32') {
            return sendJson(res, 501, { ok: false, error: 'file picker only supported on Windows' });
        }
        const filter = "图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff)|*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff|所有文件 (*.*)|*.*";
        const body =
            "$dlg = New-Object System.Windows.Forms.OpenFileDialog; " +
            "$dlg.Title = '选择图片'; $dlg.Filter = '" + filter + "'; ";
        const r = await runFileDialog(body);
        if (r.error) return sendJson(res, 500, { ok: false, error: r.error });
        if (r.cancelled) return sendJson(res, 200, { ok: false, cancelled: true });
        // Switched to a disk image: drop any stale drag-in/paste image copy (video uploads are
        // independent and stay untouched -- see purgeUploads).
        purgeUploads(null, 'img');
        return sendJson(res, 200, { ok: true, path: r.path, name: path.basename(r.path) });
    }

    // Opens a native file picker for a video to feed the side-by-side video compare module.
    if (url.pathname === '/api/pick-video' && req.method === 'GET') {
        if (process.platform !== 'win32') {
            return sendJson(res, 501, { ok: false, error: 'file picker only supported on Windows' });
        }
        const filter = "视频文件 (*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v)|*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v|所有文件 (*.*)|*.*";
        const body =
            "$dlg = New-Object System.Windows.Forms.OpenFileDialog; " +
            "$dlg.Title = '选择视频'; $dlg.Filter = '" + filter + "'; ";
        const r = await runFileDialog(body);
        if (r.error) return sendJson(res, 500, { ok: false, error: r.error });
        if (r.cancelled) return sendJson(res, 200, { ok: false, cancelled: true });
        return sendJson(res, 200, { ok: true, path: r.path, name: path.basename(r.path) });
    }

    // Opens a native folder picker for the output directory, defaulting to the input's folder.
    // Returns the chosen directory; the page composes the final filename using the input stem
    // so the user only has to pick a folder, not a specific file.
    if (url.pathname === '/api/save-file' && req.method === 'GET') {
        if (process.platform !== 'win32') {
            return sendJson(res, 501, { ok: false, error: 'folder picker only supported on Windows' });
        }
        const input = url.searchParams.get('input') || '';
        let dir = input ? path.dirname(input) : '';
        const initial = dir ? "$dlg.InitialDirectory = '" + dir.replace(/'/g, "''") + "'; " : '';
        // Modern (Vista-style, resizable, DPI-aware) folder picker: an OpenFileDialog in
        // "pick a folder" mode. The old FolderBrowserDialog is a small legacy tree window.
        const body =
            "$dlg = New-Object System.Windows.Forms.OpenFileDialog; " +
            "$dlg.Title = '选择输出文件夹(进入目标文件夹后点“打开”)'; " +
            "$dlg.CheckFileExists = $false; $dlg.CheckPathExists = $true; " +
            "$dlg.ValidateNames = $false; " +
            "$dlg.Filter = '文件夹|*.folder'; " +
            "$dlg.FileName = '选择此文件夹'; " +
            initial;
        const r = await runFileDialog(body,
            "$(if ([System.IO.Directory]::Exists($dlg.FileName)) { $dlg.FileName } else { Split-Path -Parent $dlg.FileName })");
        if (r.error) return sendJson(res, 500, { ok: false, error: r.error });
        if (r.cancelled) return sendJson(res, 200, { ok: false, cancelled: true });
        return sendJson(res, 200, { ok: true, dir: r.path });
    }

    if (url.pathname === '/api/start' && req.method === 'POST') {
        const body = await readBody(req);
        if (!body.input) {
            return sendJson(res, 400, { ok: false, error: 'input is required' });
        }
        return sendJson(res, 200, startJob(body));
    }

    if (url.pathname === '/api/cancel' && req.method === 'POST') {
        if (current && !current.finished) {
            current.cancelled = true;
            if (current.child) {
                try { current.child.kill(); } catch (e) { /* ignore */ }
            } else {
                // Between passes no engine process is alive: close the job right away. (The child
                // close callback handles the running-pass case via onPassDone -> jobFinish.)
                jobFinish(current, true);
            }
        }
        return sendJson(res, 200, { ok: true });
    }

    // Receives a dropped/pasted file's content (video or image) and stores it in the disposable
    // uploads dir. The temp copy keeps the user's ORIGINAL filename (sanitized for characters
    // Windows forbids, deduped with _1/_2 when a same-named file already exists). The UI only
    // ever sees this temporary path; it survives the job so previews keep working, and is purged
    // when the user replaces the input with another file of the same kind.
    // Exports a finished lossless master to a final file using the chosen encoder. Runs as an
    // async job so the UI can show per-second progress (/api/status -> export.*). A pure ffmpeg
    // transcode -- never re-runs the model -- so re-exporting with another encoder is cheap.
    if (url.pathname === '/api/export' && req.method === 'POST') {
        const body = await readBody(req);
        const master = (body.master || '').trim();
        const encoder = (body.encoder || '').trim();
        if (!master || !fs.existsSync(master)) {
            return sendJson(res, 400, { ok: false, error: 'master not found' });
        }
        if (!EXPORT_ENCODERS[encoder]) {
            return sendJson(res, 400, { ok: false, error: 'unknown encoder: ' + encoder });
        }
        if (exporter.running) {
            return sendJson(res, 409, { ok: false, error: 'another export is already running' });
        }
        // Output path + optional custom file name: blank file name keeps the default
        // (nr_<orig>_<encoder>.mp4); a typed name is honoured (auto .mp4 if no extension).
        const stem = path.basename(master).replace(/^master_/, 'nr_').replace(/\.[^.]+$/, '');
        // When a custom file name is given, 「保存的文件名」owns the basename and the output
        // field is a folder only -- even if the user pasted something that looks like a file
        // path (its parent dir is used), so the two controls never fight.
        // Default file name is just `nr_<original>.mp4` (no encoder suffix). If the user exports
        // multiple encoders into the same folder, uniquePath() appends (1)/(2)/... so nothing
        // is silently overwritten.
        const customName = (body.fileName || '').trim();
        let outputRaw = (body.output || '').trim();
        if (customName && outputRaw && /\.[A-Za-z0-9]{1,5}$/.test(path.basename(outputRaw))) {
            outputRaw = path.dirname(outputRaw);
        }
        const defaultName = customName
            ? (/\.[A-Za-z0-9]{1,5}$/.test(customName) ? customName : customName + '.mp4')
            : `${stem}.mp4`;
        const finalOut = resolveExportPath(outputRaw, defaultName);
        fs.mkdirSync(path.dirname(finalOut), { recursive: true });

        exporter.running = true;
        exporter.pct = 0;
        exporter.encoder = encoder;
        exporter.out = finalOut;
        exporter.error = null;
        runExport(master, encoder, finalOut).then(() => {
            exporter.running = false;
            exporter.pct = 100;
            const info = {
                ok: true,
                encoder,
                output: finalOut,
                url: '/api/video?path=' + encodeURIComponent(finalOut),
            };
            if (current && current.master === master) current.export = info;
        }).catch((e) => {
            exporter.running = false;
            exporter.error = e.message || String(e);
        });
        return sendJson(res, 200, { ok: true, started: true, encoder, output: finalOut });
    }

    if (url.pathname === '/api/upload' && req.method === 'POST') {
        try {
            fs.mkdirSync(UPLOADS_DIR, { recursive: true });
        } catch (e) {
            return sendJson(res, 500, { ok: false, error: 'cannot create uploads dir' });
        }
        const rawName = decodeURIComponent(req.headers['x-filename'] || 'video.mp4');
        const rawExt = path.extname(rawName).toLowerCase();
        const ext = [...VIDEO_EXT, ...IMAGE_EXT].includes(rawExt) ? rawExt : '.mp4';
        // Preserve the original name (minus extension); strip only what Windows cannot store.
        let stem = path.basename(rawName, rawExt);
        stem = stem.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_').replace(/[. ]+$/, '');
        if (!stem) stem = IMAGE_EXT.includes(ext) ? 'image' : 'video';
        if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i.test(stem)) stem = '_' + stem;
        const tmpPath = uniquePath(UPLOADS_DIR, stem + ext);
        const ws = fs.createWriteStream(tmpPath);
        try {
            await new Promise((ok, bad) => {
                req.on('error', bad);
                ws.on('error', bad);
                req.pipe(ws);
                ws.on('finish', ok);
            });
        } catch (e) {
            try { fs.unlinkSync(tmpPath); } catch (e2) { /* ignore */ }
            return sendJson(res, 500, { ok: false, error: 'upload failed: ' + e.message });
        }
        let size = 0;
        try { size = fs.statSync(tmpPath).size; } catch (e) { /* ignore */ }
        if (size <= 0) {
            try { fs.unlinkSync(tmpPath); } catch (e) { /* ignore */ }
            return sendJson(res, 400, { ok: false, error: 'empty file received' });
        }
        // A fresh drop/paste replaces the previous one of the SAME kind (video or image): sweep
        // only that kind so the dir does not accumulate old copies while the other module's
        // current input (e.g. a video source next to an image being edited) is left untouched.
        purgeUploads(tmpPath);
        return sendJson(res, 200, { ok: true, path: tmpPath, name: path.basename(tmpPath), tmp: true });
    }

    // Streams a result file back to the browser. The output can be anywhere on disk now (it goes
    // next to the input), so the only checks are that it is an absolute path and exists. The
    // server is bound to 127.0.0.1, so this is not reachable from other machines.
    if (url.pathname === '/api/download' && req.method === 'GET') {
        const p = url.searchParams.get('path');
        if (!p) return sendJson(res, 400, { ok: false, error: 'path required' });
        const abs = path.resolve(p);
        if (!path.isAbsolute(p)) return sendJson(res, 400, { ok: false, error: 'absolute path required' });
        if (!fs.existsSync(abs)) return sendJson(res, 404, { ok: false, error: 'not found' });
        const size = fs.statSync(abs).size;
        res.writeHead(200, {
            'Content-Type': mimeFor(abs),
            'Content-Length': size,
            'Content-Disposition': 'attachment; filename="' + encodeURIComponent(path.basename(abs)) + '"',
            'Cache-Control': 'no-store',
        });
        fs.createReadStream(abs).pipe(res);
        return;
    }

    // Streams the input video for the browser's <video> preview element. The HTML5 player needs
    // HTTP Range support to seek, so this implements bytes= parsing instead of a plain file dump.
    // Paths are absolute and arbitrary (bound to 127.0.0.1 only).
    if (url.pathname === '/api/video' && req.method === 'GET') {
        const p = url.searchParams.get('path');
        if (!p) return sendJson(res, 400, { ok: false, error: 'path required' });
        const abs = path.resolve(p);
        if (!path.isAbsolute(p)) return sendJson(res, 400, { ok: false, error: 'absolute path required' });
        let stat;
        try { stat = fs.statSync(abs); } catch (e) { return sendJson(res, 404, { ok: false, error: 'not found' }); }
        const total = stat.size;
        const type = mimeFor(abs);
        const range = req.headers.range;
        if (range) {
            const m = /^bytes=(\d*)-(\d*)$/.exec(range);
            if (!m) return sendJson(res, 416, { ok: false, error: 'bad range' });
            let start = m[1] ? parseInt(m[1], 10) : 0;
            let end = m[2] ? parseInt(m[2], 10) : total - 1;
            if (isNaN(start) || start < 0) start = 0;
            if (isNaN(end) || end < start) end = total - 1;
            if (start >= total) {
                res.writeHead(416, { 'Content-Range': 'bytes */' + total });
                return res.end();
            }
            res.writeHead(206, {
                'Content-Type': type,
                'Accept-Ranges': 'bytes',
                'Content-Range': `bytes ${start}-${end}/${total}`,
                'Content-Length': end - start + 1,
                'Cache-Control': 'no-store',
            });
            fs.createReadStream(abs, { start, end }).pipe(res);
        } else {
            res.writeHead(200, {
                'Content-Type': type,
                'Accept-Ranges': 'bytes',
                'Content-Length': total,
                'Cache-Control': 'no-store',
            });
            fs.createReadStream(abs).pipe(res);
        }
        return;
    }

    // Renders a single frame at the requested time using the same model parameters as a full job,
    // then extracts both the original frame and the rendered frame as PNGs so the UI can show a
    // side-by-side / split-comparison hover view.
    if (url.pathname === '/api/render-frame' && req.method === 'POST') {
        const body = await readBody(req);
        const input = (body.input || '').trim();
        const frameTime = parseFloat(body.frameTime);
        const cfg = body.cfg || {};
        if (!input) return sendJson(res, 400, { ok: false, error: 'input is required' });
        if (!isFinite(frameTime) || frameTime < 0) {
            return sendJson(res, 400, { ok: false, error: 'frameTime must be a non-negative number' });
        }
        if (!fs.existsSync(input)) {
            return sendJson(res, 400, { ok: false, error: 'input file does not exist' });
        }

        fs.mkdirSync(FRAME_DIR, { recursive: true });
        const ts = Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 6);
        const origPath = path.join(FRAME_DIR, `orig_${ts}.png`);
        const renderedPath = path.join(FRAME_DIR, `rendered_${ts}.png`);
        const tmpMp4 = path.join(FRAME_DIR, `src_${ts}.mp4`);

        const ff = (args) => new Promise((ok, bad) => {
            const p = spawn('ffmpeg', ['-y', '-v', 'error', ...args], { windowsHide: true });
            let err = '';
            p.stderr.on('data', (c) => { err += c.toString('utf8'); });
            p.on('close', (code) => code === 0 ? ok() : bad(new Error('ffmpeg exit ' + code + ' ' + err.trim())));
            p.on('error', bad);
        });

        // Frame rate of the source so the render window can be shrunk to ~2 frames (rendering
        // a whole 0.5s window just to extract one frame is what made previews slow).
        let fps = 0;
        try {
            const pi = await probe(input);
            if (pi.ok && pi.info.fps > 0) fps = pi.info.fps;
        } catch (e) { /* fall back below */ }
        const winDur = fps > 0 ? Math.max(0.05, 2.0 / fps) : 0.5;

        try {
            // 1) Render a tiny window (~2-3 frames) with the engine using the user's model
            //    params. --dump-frame makes the engine also write its raw decoded first frame
            //    as a PPM, which we convert to the "before" image -- an exact same-physical-frame
            //    match with zero extra video decoding (a separate frame-accurate ffmpeg grab
            //    would decode every frame up to frameTime all over again).
            const ppmPath = path.join(FRAME_DIR, `orig_${ts}.ppm`);
            // Preview frames are compared pixel-to-pixel, so the rendered window is encoded
            // losslessly in 4:4:4 -- never through the lossy NVENC/yuv420p path. That keeps
            // smooth gradients intact until the PNG extract, so banding can't creep in here.
            const prevCfg = Object.assign({}, cfg, {
                encoder: 'libx264',
                pixFmt: 'yuv444p',
                codecArgs: '-qp 0 -preset ultrafast',
            });
            const args = engineArgs(prevCfg, input, tmpMp4, true);
            args.push('--start-time', frameTime.toString());
            args.push('--end-time', (frameTime + winDur).toString());
            args.push('--dump-frame', ppmPath);
            // 16-bit first-frame export for the rendered preview: same trick as the image
            // module, so the compare view never shows the 8-bit Bayer dither grid.
            const rendered16 = path.join(FRAME_DIR, `rendered_${ts}_16.png`);
            args.push('--png16', rendered16);
            const exe = findEngine();
            if (!fs.existsSync(exe)) {
                return sendJson(res, 500, { ok: false, error: 'engine not found' });
            }
            await runEngine(exe, args);

            if (fs.existsSync(ppmPath)) {
                // PPM -> PNG is a trivial conversion; no video decoding involved.
                await ff(['-i', ppmPath, '-frames:v', '1', '-f', 'image2', origPath]);
                try { fs.unlinkSync(ppmPath); } catch (e) { /* ignore */ }
            } else {
                // Old engine without --dump-frame: fall back to a separate frame-accurate grab.
                await ff([
                    '-i', input,
                    '-ss', frameTime.toString(),
                    '-frames:v', '1',
                    '-f', 'image2',
                    origPath,
                ]);
            }

            // 2) Extract the first frame of the rendered window (== the frame at frameTime).
            //    Prefer the engine's 16-bit PNG; fall back to decoding the 8-bit clip.
            if (fs.existsSync(rendered16)) {
                renderedPath = rendered16;
            } else {
                await ff([
                    '-i', tmpMp4,
                    '-frames:v', '1',
                    '-f', 'image2',
                    renderedPath,
                ]);
            }

            try { fs.unlinkSync(tmpMp4); } catch (e) { /* ignore */ }

            // Probe PNG dimensions so the UI can render at native size.
            const dim = await new Promise((res) => {
                execFile('ffprobe', ['-v', 'error', '-select_streams', 'v:0',
                    '-show_entries', 'stream=width,height', '-of', 'csv=p=0', renderedPath],
                    { windowsHide: true }, (e, out) => {
                        if (e) return res({ width: 0, height: 0 });
                        const [w, h] = (out || '').split(',');
                        res({ width: parseInt(w, 10) || 0, height: parseInt(h, 10) || 0 });
                    });
            });

            return sendJson(res, 200, {
                ok: true,
                orig: '/api/frame-img?path=' + encodeURIComponent(origPath),
                render: '/api/frame-img?path=' + encodeURIComponent(renderedPath),
                width: dim.width,
                height: dim.height,
                frameTime,
            });
        } catch (e) {
            try { fs.unlinkSync(tmpMp4); } catch (e2) { /* ignore */ }
            return sendJson(res, 500, { ok: false, error: e.message || 'render-frame failed' });
        }
    }

    // Serves one of the disposable compare-frame PNGs. Path must be inside FRAME_DIR (no
    // arbitrary reads).
    if (url.pathname === '/api/frame-img' && req.method === 'GET') {
        const p = url.searchParams.get('path') || '';
        if (!isFramePath(p)) return sendJson(res, 400, { ok: false, error: 'frame path required' });
        if (!fs.existsSync(p)) return sendJson(res, 404, { ok: false, error: 'frame missing' });
        res.writeHead(200, {
            'Content-Type': 'image/png',
            'Content-Length': fs.statSync(p).size,
            'Cache-Control': 'no-store',
        });
        fs.createReadStream(p).pipe(res);
        return;
    }

    // Streams an arbitrary still image (png/jpg/bmp/webp/...) to the browser. The server is bound
    // to 127.0.0.1 so serving absolute paths is safe, and the extension whitelist keeps it honest.
    if (url.pathname === '/api/image' && req.method === 'GET') {
        const p = url.searchParams.get('path') || '';
        if (!path.isAbsolute(p) || !/\.(png|jpe?g|bmp|webp|tif?f)$/i.test(p)) {
            return sendJson(res, 400, { ok: false, error: 'image path required' });
        }
        if (!fs.existsSync(p)) return sendJson(res, 404, { ok: false, error: 'image missing' });
        res.writeHead(200, {
            'Content-Type': mimeFor(p),
            'Content-Length': fs.statSync(p).size,
            'Cache-Control': 'no-store',
        });
        fs.createReadStream(p).pipe(res);
        return;
    }

    // Renders a single still image through the DLSS NR engine and returns the "before"/"after"
    // as PNGs. Internally the image is looped into a tiny lossless clip (NR is a video pipeline:
    // it needs a video reader + encoder even for one frame); the first rendered frame is what a
    // still-image pass produces, equivalent to the engine's first-frame (reset) behaviour.
    if (url.pathname === '/api/render-image' && req.method === 'POST') {
        const body = await readBody(req);
        const input = (body.input || '').trim();
        const cfg = body.cfg || {};
        if (!input) return sendJson(res, 400, { ok: false, error: 'input is required' });
        if (!/\.(png|jpe?g|bmp|webp|tif?f)$/i.test(input)) {
            return sendJson(res, 400, { ok: false, error: 'not a supported image type' });
        }
        if (!fs.existsSync(input)) {
            return sendJson(res, 400, { ok: false, error: 'input file does not exist' });
        }

        fs.mkdirSync(FRAME_DIR, { recursive: true });
        const ts = Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 6);
        const srcMp4 = path.join(FRAME_DIR, `src_${ts}.mp4`);
        const outMp4 = path.join(FRAME_DIR, `out_${ts}.mp4`);
        const rendered16 = path.join(FRAME_DIR, `rendered_${ts}_16.png`);
        let renderedPath = rendered16;

        try {
            // 1) Loop the still into a 3-frame lossless clip. x264 -qp 0 (lossless) + 4:4:4 keeps
            //    the pixels intact through the encode so the rendered frame truly is the image's.
            await runFfmpeg([
                '-loop', '1', '-framerate', '30', '-i', input,
                '-frames:v', '3',
                '-pix_fmt', 'yuv444p',
                '-c:v', 'libx264', '-qp', '0', '-preset', 'ultrafast',
                srcMp4,
            ]);

            // 2) Run the engine over the tiny clip with the user's model parameters. A still has
            //    no motion, so frame guidance is forced to 0 (force-zero) -- skips the NV-OF init
            //    entirely, which is both faster and the correct input for a static image.
            const exe = findEngine();
            if (!fs.existsSync(exe)) {
                return sendJson(res, 500, { ok: false, error: 'engine not found' });
            }
            // Same pixel-exact reasoning as render-frame: the still's rendered clip is written
            // losslessly in 4:4:4 so no encoder/colour-subsampling artifact reaches the PNG.
            const imgCfg = Object.assign({}, cfg, {
                frameGuidance: 0,
                startTime: 0,
                endTime: 0.1,
                encoder: 'libx264',
                pixFmt: 'yuv444p',
                codecArgs: '-qp 0 -preset ultrafast',
            });
            const args = engineArgs(imgCfg, srcMp4, outMp4, true);
            // Ask the engine for a TRUE 16-bit PNG of the first frame (model output at 16-bit
            // float, residual-blended, scaled to 0..65535 -- no 8-bit quantisation, so smooth
            // gradients cannot band). Requires the newer engine; older builds fall back below.
            args.push('--png16', rendered16);
            await runEngine(exe, args);

            // 3) Result PNG: prefer the engine's 16-bit export; fall back to decoding the
            //    lossless clip's first frame for older engines.
            if (fs.existsSync(rendered16)) {
                renderedPath = rendered16;
            } else {
                renderedPath = path.join(FRAME_DIR, `rendered_${ts}.png`);
                await runFfmpeg([
                    '-i', outMp4, '-frames:v', '1', '-f', 'image2', renderedPath,
                ]);
            }

            try { fs.unlinkSync(srcMp4); } catch (e) { /* ignore */ }
            try { fs.unlinkSync(outMp4); } catch (e) { /* ignore */ }

            const dim = await new Promise((res) => {
                execFile('ffprobe', ['-v', 'error', '-select_streams', 'v:0',
                    '-show_entries', 'stream=width,height', '-of', 'csv=p=0', renderedPath],
                    { windowsHide: true }, (e, out) => {
                        if (e) return res({ width: 0, height: 0 });
                        const [w, h] = (out || '').split(',');
                        res({ width: parseInt(w, 10) || 0, height: parseInt(h, 10) || 0 });
                    });
            });

            return sendJson(res, 200, {
                ok: true,
                orig: '/api/image?path=' + encodeURIComponent(input),
                render: '/api/frame-img?path=' + encodeURIComponent(renderedPath),
                renderedAbs: renderedPath,
                width: dim.width,
                height: dim.height,
            });
        } catch (e) {
            try { fs.unlinkSync(srcMp4); } catch (e2) { /* ignore */ }
            try { fs.unlinkSync(outMp4); } catch (e2) { /* ignore */ }
            return sendJson(res, 500, { ok: false, error: e.message || 'render-image failed' });
        }
    }

    // "Save the rendered PNG to a user-chosen location": native save dialog (png/jpg), then
    // copies or re-encodes the frame to the picked file.
    if (url.pathname === '/api/save-image' && req.method === 'GET') {
        const src = url.searchParams.get('src') || '';
        if (!isFramePath(src) || !fs.existsSync(src)) {
            return sendJson(res, 400, { ok: false, error: 'no rendered image available' });
        }
        const filter = "PNG 图片 (*.png)|*.png|JPG 图片 (*.jpg)|*.jpg|所有文件 (*.*)|*.*";
        const body =
            "$dlg = New-Object System.Windows.Forms.SaveFileDialog; " +
            "$dlg.Title = '保存渲染结果'; $dlg.Filter = '" + filter + "'; " +
            "$dlg.DefaultExt = 'png'; $dlg.AddExtension = $true; " +
            "$dlg.FileName = 'nr_render.png'; ";
        const r = await runFileDialog(body);
        if (r.error) return sendJson(res, 500, { ok: false, error: r.error });
        if (r.cancelled) return sendJson(res, 200, { ok: false, cancelled: true });
        const dst = r.path;
        try {
            if (/\.jpe?g$/i.test(dst)) {
                await runFfmpeg(['-i', src, '-q:v', '2', '-f', 'image2', dst]);
            } else {
                fs.copyFileSync(src, dst);
            }
        } catch (e) {
            return sendJson(res, 500, { ok: false, error: 'save failed: ' + e.message });
        }
        return sendJson(res, 200, { ok: true, path: dst });
    }

    // static
    let rel = url.pathname === '/' ? '/index.html' : url.pathname;
    const filePath = path.join(__dirname, path.normalize(rel).replace(/^(\.\.[/\\])+/, ''));
    if (!filePath.startsWith(__dirname)) {
        res.writeHead(403);
        return res.end('forbidden');
    }
    fs.readFile(filePath, (err, data) => {
        if (err) {
            res.writeHead(404);
            return res.end('not found');
        }
        res.writeHead(200, {
            'Content-Type': MIME[path.extname(filePath).toLowerCase()] || 'application/octet-stream',
            'Cache-Control': 'no-store',
        });
        res.end(data);
    });
});

// Bind to localhost only: the download/open endpoints serve arbitrary absolute paths now, and a
// LAN-reachable server must not be able to leak local files.
server.listen(PORT, '127.0.0.1', () => {
    const url = 'http://127.0.0.1:' + PORT + '/';
    console.log('dlss5nr web UI: ' + url);
    fs.mkdirSync(OUTPUTS_DIR, { recursive: true });
    cleanUploadsDir();   // fallback only: normal shutdown cleanup is done by server_guard.exe
    cleanFrameDir();     // fallback only: normal shutdown cleanup is done by server_guard.exe
    // When launched from start_ui.bat (--open), open the browser ourselves once the socket is
    // live. Doing it in-process removes the fragile "wait for port then start" dance from the
    // batch file, which was silently failing and leaving no browser open.
    if (process.argv.includes('--open')) {
        setTimeout(() => openBrowser(url), 500);
    }
});
