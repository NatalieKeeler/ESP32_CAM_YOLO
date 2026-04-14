#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "FS.h"
#include "SD_MMC.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include <vector>
#include <algorithm>

// ============================================================
//  WiFi Configuration
// ============================================================
const char* ssid     = "NETGEAR95";
const char* password = "grandbanana337";

// ============================================================
//  SD Card Pins
// ============================================================
#define SD_MMC_CMD 15
#define SD_MMC_CLK 14
#define SD_MMC_D0   2
#define SD_MMC_D1   4
#define SD_MMC_D2  12
#define SD_MMC_D3  13

// ============================================================
//  Camera Pins – AI Thinker ESP32-CAM
// ============================================================
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22
#define LED_GPIO_NUM     4

// ============================================================
//  Streaming constants
// ============================================================
#define PART_BOUNDARY   "frame"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY
#define STREAM_BOUNDARY "\r\n--" PART_BOUNDARY "\r\n"
#define STREAM_PART     "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

// ============================================================
//  Global state
// ============================================================
WebServer server(80);

bool     sdCardAvailable   = false;
bool     ledState          = false;
bool     isRecording       = false;
bool     streamActive      = false;

// Cached counts – updated only on write/delete, not every stats call
int      cachedPhotoCount  = 0;
int      cachedVideoCount  = 0;

unsigned long lastStatsUpdate    = 0;
unsigned long recordingStartTime = 0;
unsigned long lastCaptureTime    = 0;
unsigned long lastReconnectTime  = 0;

const int RECORDING_FPS   = 5;
String    currentVideoFolder = "";
camera_fb_t* lastPhoto    = NULL;

// ============================================================
//  Forward declarations
// ============================================================
bool   initSDCard();
bool   savePhotoToSD(camera_fb_t* fb, const char* filename);
void   findJPGFiles(File dir, std::vector<String>& files);
int    countPhotosOnSD();
int    countVideoFolders();
String createVideoFolder();
void   setupCamera();
void   setupWiFi();

// ============================================================
//  VR Page  (minimal, pure stream)
// ============================================================
const char VR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>ESP32Cam VR</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-user-select:none;user-select:none}
html,body{width:100%;height:100%;overflow:hidden;background:#000;position:fixed;touch-action:none}
.vr-container{display:flex;width:100%;height:100%}
.vr-eye{flex:1;overflow:hidden;display:flex;align-items:center;justify-content:center}
.vr-stream{width:100%;height:100%;object-fit:cover}
@media(orientation:landscape){.vr-container{flex-direction:row}}
@media(orientation:portrait){.vr-container{flex-direction:column}}
</style>
</head>
<body>
<div class="vr-container">
  <div class="vr-eye"><img id="L" class="vr-stream"></div>
  <div class="vr-eye"><img id="R" class="vr-stream"></div>
</div>
<script>
  // Both eyes share the same persistent MJPEG stream
  document.getElementById('L').src = '/stream';
  document.getElementById('R').src = '/stream';

  document.addEventListener('keydown', e => {
    if (e.key === 'p' || e.key === 'P') fetch('/capture');
    if (e.key === 'Escape' || e.key === 'h' || e.key === 'H') location.href = '/';
  });

  let t0 = 0;
  document.addEventListener('touchstart', () => t0 = Date.now());
  document.addEventListener('touchend',   () => { if (Date.now()-t0>3000) location.href='/'; });

  document.addEventListener('DOMContentLoaded', () => {
    document.documentElement.requestFullscreen && document.documentElement.requestFullscreen().catch(()=>{});
  });
</script>
</body>
</html>
)rawliteral";

// ============================================================
//  Main UI Page
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32Cam UMSL</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:Arial,sans-serif;background:linear-gradient(135deg,#0f0f0f,#1a1a1a);padding:10px;padding-bottom:120px;min-height:100vh;color:#e0e0e0}
.header{text-align:center;background:linear-gradient(135deg,#2c3e50,#34495e);color:#fff;padding:15px;border-radius:15px;margin-bottom:15px;box-shadow:0 6px 12px rgba(0,0,0,.3);border:1px solid #3a506b}
h1{font-size:1.8rem;margin-bottom:8px;text-shadow:2px 2px 4px rgba(0,0,0,.5)}
.ip-address{font-size:.95rem;opacity:.9;color:#bdc3c7;font-family:'Courier New',monospace}
.video-container{background:linear-gradient(135deg,#1e1e1e,#2d2d2d);border-radius:15px;overflow:hidden;margin-bottom:20px;text-align:center;padding:15px;box-shadow:0 6px 12px rgba(0,0,0,.3);border:1px solid #3a3a3a}
#videoStream{width:100%;max-width:640px;border-radius:10px;background:#000;box-shadow:0 4px 8px rgba(0,0,0,.5);border:2px solid #3a506b;display:none}
.loading{text-align:center;padding:30px;color:#bdc3c7;font-size:16px}
.stats-container{background:linear-gradient(135deg,#1e1e1e,#2d2d2d);padding:15px;border-radius:15px;margin-bottom:20px;box-shadow:0 6px 12px rgba(0,0,0,.3);border:1px solid #3a3a3a}
.stats-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}
.stat-item{display:flex;justify-content:space-between;align-items:center;padding:12px;background:linear-gradient(135deg,#2c3e50,#34495e);border-radius:10px;transition:transform .2s}
.stat-item:hover{transform:translateY(-3px)}
.stat-label{font-weight:700;color:#bdc3c7;font-size:14px}
.stat-value{color:#1abc9c;font-weight:700;font-size:14px;font-family:'Courier New',monospace}
.bottom-menu{position:fixed;bottom:0;left:0;width:100%;background:linear-gradient(135deg,#2c3e50,#34495e);padding:15px 10px;display:flex;justify-content:space-around;align-items:center;box-shadow:0 -4px 12px rgba(0,0,0,.3);border-top:2px solid #1abc9c;z-index:1000}
.menu-btn{background:none;border:none;color:#fff;padding:12px 8px;border-radius:10px;cursor:pointer;font-weight:700;display:flex;flex-direction:column;align-items:center;justify-content:center;transition:all .3s;min-width:70px}
.menu-btn:hover{background:rgba(255,255,255,.1);transform:translateY(-3px)}
.menu-btn:active{transform:translateY(0)}
.menu-btn.active{background:#1abc9c;box-shadow:0 4px 8px rgba(26,188,156,.3)}
.btn-icon{font-size:22px;margin-bottom:5px}
.btn-text{font-size:11px;text-align:center}
.gallery-modal{display:none;position:fixed;z-index:1002;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,.95);padding:20px;overflow-y:auto}
.gallery-content{background:linear-gradient(135deg,#1e1e1e,#2d2d2d);margin:5% auto;padding:25px;border-radius:15px;width:95%;max-width:900px;max-height:88vh;overflow-y:auto;border:1px solid #3a506b}
.gallery-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:15px;margin-top:20px}
.gallery-item{position:relative;border-radius:10px;overflow:hidden;cursor:pointer;background:#2c3e50;height:150px;display:flex;align-items:center;justify-content:center;transition:transform .3s}
.gallery-item:hover{transform:scale(1.05);box-shadow:0 6px 12px rgba(0,0,0,.3)}
.gallery-item img{width:100%;height:100%;object-fit:cover}
.delete-btn{position:absolute;top:8px;right:8px;background:rgba(231,76,60,.9);color:#fff;border:none;border-radius:50%;width:30px;height:30px;cursor:pointer;font-size:16px;display:flex;align-items:center;justify-content:center}
.toast{position:fixed;top:20px;left:50%;transform:translateX(-50%);padding:14px 22px;border-radius:10px;z-index:1010;font-weight:700;box-shadow:0 6px 12px rgba(0,0,0,.3);min-width:260px;text-align:center;transition:opacity .4s}
.toast.success{background:#1e5c3a;color:#2ecc71;border:1px solid #2ecc71}
.toast.error{background:#5c1e1e;color:#e74c3c;border:1px solid #e74c3c}
.fps-bar{height:4px;background:#1abc9c;border-radius:2px;margin-top:8px;transition:width .5s}
.website-footer{text-align:center;color:#7f8c8d;font-size:14px;margin-top:20px;padding:10px;border-top:1px solid #3a506b}
.copyright{text-align:center;color:#1abc9c;font-size:12px;margin-top:10px;padding:5px;font-family:'Courier New',monospace}
@media(max-width:600px){.menu-btn{padding:10px 5px;min-width:55px}.btn-icon{font-size:19px}.btn-text{font-size:10px}.gallery-grid{grid-template-columns:repeat(auto-fill,minmax(110px,1fr))}.gallery-item{height:110px}}
</style>
</head>
<body>
<div class="header">
  <h1>📹 ESP32Cam UMSL</h1>
  <div class="ip-address">IP: %IP% | SD: <span id="sdStatus">%SD_STATUS%</span> | FPS: <span id="currentFPS">0</span></div>
</div>

<div class="video-container">
  <div id="loading" class="loading">🔄 Starting stream...</div>
  <img id="videoStream" crossorigin="anonymous" alt="Camera stream">
  <div id="photoThumb" style="display:none;margin-top:12px"></div>
</div>

<div class="stats-container">
  <div class="stats-grid">
    <div class="stat-item"><span class="stat-label">WiFi Signal</span><span class="stat-value" id="rssi">— dBm</span></div>
    <div class="stat-item"><span class="stat-label">Free RAM</span><span class="stat-value" id="freeRam">— KB</span></div>
    <div class="stat-item"><span class="stat-label">SD Storage</span><span class="stat-value" id="sdSpace">—</span></div>
    <div class="stat-item"><span class="stat-label">Photos</span><span class="stat-value" id="photoCount">0</span></div>
    <div class="stat-item"><span class="stat-label">Videos</span><span class="stat-value" id="videoCount">0</span></div>
    <div class="stat-item"><span class="stat-label">Recording</span><span class="stat-value" id="recStatus">OFF</span></div>
  </div>
</div>

<div class="bottom-menu">
  <button class="menu-btn" onclick="toggleStream()" id="streamBtn">
    <span class="btn-icon">▶️</span><span class="btn-text">Stream</span>
  </button>
  <button class="menu-btn" onclick="toggleLED()" id="ledBtn">
    <span class="btn-icon">💡</span><span class="btn-text" id="ledText">LED OFF</span>
  </button>
  <button class="menu-btn" onclick="capturePhoto()" id="photoBtn">
    <span class="btn-icon">📸</span><span class="btn-text">Photo</span>
  </button>
  <button class="menu-btn" onclick="showGallery()" id="galleryBtn">
    <span class="btn-icon">🖼️</span><span class="btn-text">Gallery</span>
  </button>
  <button class="menu-btn" onclick="location.href='/vr'" id="vrBtn">
    <span class="btn-icon">🥽</span><span class="btn-text">VR Mode</span>
  </button>
  <button class="menu-btn" onclick="toggleRecording()" id="recordBtn">
    <span class="btn-icon">⏺️</span><span class="btn-text" id="recordText">Record</span>
  </button>
  <button class="menu-btn" onclick="refreshStats()" id="statsBtn">
    <span class="btn-icon">📊</span><span class="btn-text">Info</span>
  </button>
  <button class="menu-btn" onclick="formatSDCard()" id="formatBtn">
    <span class="btn-icon">💾</span><span class="btn-text">Format</span>
  </button>
</div>

<div class="website-footer">www.umsl.edu</div>
<div class="copyright">© UMSL 2026</div>

<!-- Gallery Modal -->
<div id="galleryModal" class="gallery-modal">
  <div class="gallery-content">
    <h2 style="color:#fff;margin-bottom:20px;text-align:center">📷 Gallery</h2>
    <div style="display:flex;gap:10px;justify-content:center;margin-bottom:20px">
      <button onclick="refreshGallery()" style="padding:10px 20px;background:#1abc9c;color:#fff;border:none;border-radius:8px;cursor:pointer">🔄 Refresh</button>
      <button onclick="closeGallery()"   style="padding:10px 20px;background:#e74c3c;color:#fff;border:none;border-radius:8px;cursor:pointer">❌ Close</button>
    </div>
    <div id="galleryLoading" class="loading">Loading images...</div>
    <div id="galleryGrid"    class="gallery-grid" style="display:none"></div>
    <div id="galleryError"   style="display:none;text-align:center;color:#e74c3c;margin-top:20px"></div>
    <div class="copyright" style="margin-top:20px">© UMSL 2026</div>
  </div>
</div>

<script>
// ── State ────────────────────────────────────────────────────
let streaming   = false;
let recording   = false;
let ledOn       = false;
let fpsCounter  = 0;
let fpsInterval = null;

const vs   = document.getElementById('videoStream');
const load = document.getElementById('loading');
const sBtn = document.getElementById('streamBtn');
const rBtn = document.getElementById('recordBtn');

// ── MJPEG Stream ─────────────────────────────────────────────
// We set src ONCE. The browser keeps the connection open forever.
function startStream() {
  if (streaming) return;
  streaming = true;
  vs.src = '/stream';
  vs.style.display = 'block';
  load.style.display = 'none';
  sBtn.innerHTML = '<span class="btn-icon">⏸️</span><span class="btn-text">Stop</span>';
  sBtn.classList.add('active');
  startFPSCounter();
}

function stopStream() {
  if (!streaming) return;
  streaming = false;
  vs.src = '';
  vs.style.display = 'none';
  load.style.display = 'block';
  load.textContent = '⏸ Stream paused';
  sBtn.innerHTML = '<span class="btn-icon">▶️</span><span class="btn-text">Stream</span>';
  sBtn.classList.remove('active');
  stopFPSCounter();
  document.getElementById('currentFPS').textContent = '0';
}

function toggleStream() { streaming ? stopStream() : startStream(); }

// Count frames via img load events (fast enough for display)
vs.addEventListener('load', () => fpsCounter++);

function startFPSCounter() {
  fpsInterval = setInterval(() => {
    document.getElementById('currentFPS').textContent = fpsCounter;
    fpsCounter = 0;
  }, 1000);
}
function stopFPSCounter() { clearInterval(fpsInterval); fpsInterval = null; }

// Reconnect if the stream dies
vs.addEventListener('error', () => {
  if (streaming) {
    setTimeout(() => { if (streaming) vs.src = '/stream?r=' + Date.now(); }, 1000);
  }
});

// ── LED ───────────────────────────────────────────────────────
async function toggleLED() {
  try {
    const r = await fetch('/led');
    if (!r.ok) return;
    const t = await r.text();
    ledOn = t.includes('ON');
    document.getElementById('ledBtn').innerHTML =
      `<span class="btn-icon">💡</span><span class="btn-text" id="ledText">${ledOn?'ON':'OFF'}</span>`;
    ledOn ? document.getElementById('ledBtn').classList.add('active')
          : document.getElementById('ledBtn').classList.remove('active');
  } catch(e) {}
}

// ── Photo ─────────────────────────────────────────────────────
async function capturePhoto() {
  try {
    const r = await fetch('/capture');
    if (!r.ok) return;

    // Increment cached count without re-scanning SD
    const el = document.getElementById('photoCount');
    el.textContent = parseInt(el.textContent||'0') + 1;

    // Show tiny thumbnail
    const thumb = document.getElementById('photoThumb');
    thumb.innerHTML = `<img src="/lastPhoto?t=${Date.now()}"
      style="width:90px;height:68px;border-radius:8px;border:3px solid #1abc9c;
             box-shadow:0 4px 8px rgba(0,0,0,.4);cursor:pointer"
      onclick="window.open(this.src,'_blank')">`;
    thumb.style.display = 'block';
    setTimeout(() => thumb.style.display = 'none', 3500);
  } catch(e) {}
}

// ── Recording ─────────────────────────────────────────────────
async function toggleRecording() {
  try {
    const ep = recording ? '/stopRecording' : '/startRecording';
    const r  = await fetch(ep);
    if (!r.ok) { toast('❌ ' + await r.text(), 'error'); return; }
    recording = !recording;
    rBtn.innerHTML = recording
      ? '<span class="btn-icon">⏹️</span><span class="btn-text">Stop</span>'
      : '<span class="btn-icon">⏺️</span><span class="btn-text">Record</span>';
    recording ? rBtn.classList.add('active') : rBtn.classList.remove('active');
    document.getElementById('recStatus').textContent = recording ? 'ON' : 'OFF';
    if (!recording) refreshStats();
  } catch(e) {}
}

// ── Gallery ───────────────────────────────────────────────────
function showGallery()  { document.getElementById('galleryModal').style.display='block'; refreshGallery(); }
function closeGallery() { document.getElementById('galleryModal').style.display='none'; }

async function refreshGallery() {
  const grid = document.getElementById('galleryGrid');
  const lod  = document.getElementById('galleryLoading');
  const err  = document.getElementById('galleryError');
  grid.style.display='none'; lod.style.display='block'; err.style.display='none';
  grid.innerHTML='';
  try {
    const r = await fetch('/listFiles');
    if (!r.ok) throw new Error();
    const data = await r.json();
    if (data.files && data.files.length) {
      data.files.sort((a,b) => b.name.localeCompare(a.name));
      data.files.forEach(f => {
        const item = document.createElement('div');
        item.className = 'gallery-item';
        const img = document.createElement('img');
        img.src = '/getFile?path=' + encodeURIComponent(f.path);
        img.loading = 'lazy';
        img.alt = f.name;
        img.onclick = () => window.open(img.src,'_blank');
        img.onerror = () => img.alt = 'Load failed';
        const del = document.createElement('button');
        del.className = 'delete-btn'; del.innerHTML = '×';
        del.onclick = e => { e.stopPropagation(); deleteFile(f.path); };
        item.appendChild(img); item.appendChild(del);
        grid.appendChild(item);
      });
    } else {
      grid.innerHTML = '<p style="color:#bdc3c7;text-align:center;grid-column:1/-1;padding:20px">No images found</p>';
    }
    lod.style.display='none'; grid.style.display='grid';
  } catch(e) {
    lod.style.display='none'; err.style.display='block';
    err.innerHTML = '<span style="color:#e74c3c">❌ Failed to load gallery</span>';
  }
}

async function deleteFile(path) {
  if (!confirm('Delete this file?')) return;
  try {
    const r = await fetch('/deleteFile?path='+encodeURIComponent(path));
    if (r.ok) { refreshGallery(); refreshStats(); }
  } catch(e) {}
}

// ── Format SD ─────────────────────────────────────────────────
async function formatSDCard() {
  if (!confirm('⚠️ WARNING: All data on the SD card will be erased!\n\nContinue?')) return;
  try {
    const r = await fetch('/formatSD');
    r.ok ? toast('✅ SD card formatted successfully!','success')
         : toast('❌ Format failed: '+ await r.text(),'error');
    refreshStats();
  } catch(e) { toast('❌ Format error','error'); }
}

// ── Stats ─────────────────────────────────────────────────────
async function refreshStats() {
  try {
    const r = await fetch('/stats');
    if (!r.ok) return;
    const d = await r.json();
    document.getElementById('rssi').textContent      = d.rssi + ' dBm';
    document.getElementById('freeRam').textContent   = Math.round(d.freeRam/1024) + ' KB';
    document.getElementById('sdSpace').textContent   = d.freeSpace + '/' + d.totalSpace + ' MB';
    document.getElementById('photoCount').textContent= d.photoCount;
    document.getElementById('videoCount').textContent= d.videoCount || 0;
    document.getElementById('recStatus').textContent = d.isRecording ? 'ON' : 'OFF';
    document.getElementById('sdStatus').textContent  = d.sdCard ? '✔' : '✘';
    if (d.isRecording && !recording) {
      recording = true;
      rBtn.innerHTML = '<span class="btn-icon">⏹️</span><span class="btn-text">Stop</span>';
      rBtn.classList.add('active');
    }
  } catch(e) {}
}

// ── Toast notification ────────────────────────────────────────
function toast(msg, type='success') {
  const t = document.createElement('div');
  t.className = 'toast ' + type;
  t.textContent = msg;
  document.body.appendChild(t);
  setTimeout(() => { t.style.opacity='0'; setTimeout(()=>t.remove(),400); }, 2800);
}

// ── Init ──────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  startStream();                       // auto-start immediately
  refreshStats();
  setInterval(refreshStats, 8000);     // stats every 8s (was 5s)
});
</script>
</body>
</html>
)rawliteral";

// ============================================================
//  SD Card
// ============================================================
bool initSDCard() {
    Serial.println("📁 Initializing SD card...");

    if (SD_MMC.cardType() != CARD_NONE) { SD_MMC.end(); delay(100); }

    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);

    // Use 20 MHz for faster SD access
    if (!SD_MMC.begin("/sdcard", true, false, 20000000)) {
        Serial.println("❌ SD card mount failed!");
        return false;
    }
    delay(50);

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) { Serial.println("❌ No SD card found!"); return false; }

    Serial.printf("✅ SD card type: %s  Size: %llu MB\n",
        cardType == CARD_MMC  ? "MMC"  :
        cardType == CARD_SD   ? "SDSC" :
        cardType == CARD_SDHC ? "SDHC" : "Unknown",
        SD_MMC.cardSize() / (1024*1024));

    File f = SD_MMC.open("/test.txt", FILE_WRITE);
    if (!f) { Serial.println("⚠️ SD card may be write-protected"); return false; }
    f.println("UMSL 2026"); f.close();
    SD_MMC.remove("/test.txt");
    Serial.println("✅ SD card writable");
    return true;
}

void findJPGFiles(File dir, std::vector<String>& files) {
    while (File entry = dir.openNextFile()) {
        if (!entry) break;
        String name = entry.name();
        if (name.startsWith(".") || name.equals("System Volume Information")) { entry.close(); continue; }
        if (entry.isDirectory()) {
            findJPGFiles(entry, files);
        } else if ((name.endsWith(".jpg") || name.endsWith(".JPG") ||
                    name.endsWith(".jpeg") || name.endsWith(".JPEG")) &&
                   !name.startsWith("frame_") && !name.startsWith("/video_")) {
            files.push_back(name);
        }
        entry.close();
    }
}

int countPhotosOnSD() {
    if (!sdCardAvailable) return 0;
    File root = SD_MMC.open("/");
    if (!root) return 0;
    std::vector<String> files;
    findJPGFiles(root, files);
    root.close();
    return files.size();
}

int countVideoFolders() {
    if (!sdCardAvailable) return 0;
    File root = SD_MMC.open("/");
    if (!root) return 0;
    int count = 0;
    while (File f = root.openNextFile()) {
        if (f.isDirectory() && String(f.name()).startsWith("video_")) count++;
        f.close();
    }
    root.close();
    return count;
}

bool savePhotoToSD(camera_fb_t* fb, const char* filename) {
    if (!sdCardAvailable || !fb) return false;
    File f = SD_MMC.open(filename, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write(fb->buf, fb->len);
    f.flush(); f.close();
    return written == fb->len;
}

String createVideoFolder() {
    if (!sdCardAvailable) return "";
    char name[32];
    sprintf(name, "/video_%lu", millis());
    return SD_MMC.mkdir(name) ? String(name) : "";
}

// ============================================================
//  Camera
// ============================================================
void setupCamera() {
    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;

    // ── Key performance levers ──
    cfg.xclk_freq_hz  = 20000000;       // 20 MHz (was 10 MHz) – faster sensor clock
    cfg.pixel_format  = PIXFORMAT_JPEG;
    cfg.frame_size    = FRAMESIZE_QVGA; // 320×240 – best FPS/quality balance
    cfg.jpeg_quality  = 15;             // 15–20 is sweet-spot for speed vs quality
    cfg.fb_count      = 3;              // triple buffer – minimises wait for next frame
    cfg.grab_mode     = CAMERA_GRAB_LATEST; // always grab freshest frame
    cfg.fb_location   = CAMERA_FB_IN_PSRAM;

    if (esp_camera_init(&cfg) != ESP_OK) {
        // Fallback
        cfg.frame_size   = FRAMESIZE_QQVGA;
        cfg.jpeg_quality = 20;
        cfg.xclk_freq_hz = 10000000;
        cfg.fb_count     = 2;
        if (esp_camera_init(&cfg) != ESP_OK) {
            Serial.println("❌ Camera init failed!");
            return;
        }
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s && s->id.PID == OV2640_PID) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
        s->set_brightness(s, 1);        // +1 brightness for better indoor image
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_special_effect(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);          // auto white-balance gain on
        s->set_gainceiling(s, GAINCEILING_8X);
        s->set_colorbar(s, 0);
        s->set_ae_level(s, 0);
        s->set_aec2(s, 1);              // night-mode AEC
        s->set_dcw(s, 1);              // down-size control on
        s->set_framesize(s, FRAMESIZE_QVGA);
    }
    Serial.println("✅ Camera ready");
}

// ============================================================
//  WiFi
// ============================================================
void setupWiFi() {
    Serial.printf("📶 Connecting to WiFi: %s\n", ssid);

    WiFi.disconnect(true); delay(500);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);      // disable power-saving – critical for stream
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);// max TX power for range

    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500); Serial.print(".");
        digitalWrite(LED_GPIO_NUM, attempts++ % 2);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n✅ WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n⚠️ WiFi failed – starting Access Point");
        WiFi.disconnect(true); delay(100);
        WiFi.mode(WIFI_AP);
        if (WiFi.softAP("ESP32-CAM", "12345678", 1, 0, 4))
            Serial.printf("📡 AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }
}

// ============================================================
//  MJPEG Stream  ← THE most important handler
//  Keeps one TCP connection open and pushes frames continuously.
//  No per-frame HTTP handshake = much faster.
// ============================================================
void handleStream() {
    streamActive = true;
    WiFiClient client = server.client();

    // Send multipart header once
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: multipart/x-mixed-replace;boundary=" PART_BOUNDARY "\r\n");
    client.print("Cache-Control: no-cache, no-store, must-revalidate\r\n");
    client.print("Pragma: no-cache\r\n");
    client.print("Access-Control-Allow-Origin: *\r\n");
    client.print("\r\n");

    char partBuf[64];

    while (client.connected()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { delay(5); continue; }

        // Part header
        client.print("--" PART_BOUNDARY "\r\n");
        client.print("Content-Type: image/jpeg\r\n");
        snprintf(partBuf, sizeof(partBuf), "Content-Length: %u\r\n\r\n", fb->len);
        client.print(partBuf);

        // Frame data – write in chunks for reliability
        const size_t CHUNK = 4096;
        size_t sent = 0;
        while (sent < fb->len) {
            size_t toSend = min(CHUNK, fb->len - sent);
            client.write(fb->buf + sent, toSend);
            sent += toSend;
        }
        client.print("\r\n");

        // If recording, save this frame to SD
        if (isRecording && sdCardAvailable &&
            millis() - lastCaptureTime >= (1000 / RECORDING_FPS)) {
            char fname[72];
            sprintf(fname, "%s/frame_%06lu.jpg",
                    currentVideoFolder.c_str(),
                    (millis() - recordingStartTime) / (1000 / RECORDING_FPS));
            savePhotoToSD(fb, fname);
            lastCaptureTime = millis();
        }

        esp_camera_fb_return(fb);
        // No delay – let the camera+network dictate pace
    }

    streamActive = false;
    Serial.println("📡 Stream client disconnected");
}

// ============================================================
//  VR Page
// ============================================================
void handleVR() {
    server.send_P(200, "text/html", VR_HTML);
}

// ============================================================
//  Single Capture
// ============================================================
void handleCapture() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { server.send(500, "text/plain", "Camera error"); return; }

    if (lastPhoto) esp_camera_fb_return(lastPhoto);
    lastPhoto = fb;

    char fname[32];
    sprintf(fname, "/photo_%lu.jpg", millis());
    if (savePhotoToSD(fb, fname)) cachedPhotoCount++;

    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    WiFiClient client = server.client();
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    client.write(fb->buf, fb->len);
}

void handleLastPhoto() {
    if (!lastPhoto) { server.send(404, "text/plain", "No photo available"); return; }
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    WiFiClient client = server.client();
    server.setContentLength(lastPhoto->len);
    server.send(200, "image/jpeg", "");
    client.write(lastPhoto->buf, lastPhoto->len);
}

// ============================================================
//  Recording
// ============================================================
void handleStartRecording() {
    if (isRecording)       { server.send(200, "text/plain", "Already recording"); return; }
    if (!sdCardAvailable)  { server.send(500, "text/plain", "SD card not available"); return; }
    currentVideoFolder = createVideoFolder();
    if (currentVideoFolder.isEmpty()) { server.send(500, "text/plain", "Failed to create video folder"); return; }
    isRecording = true; recordingStartTime = millis(); lastCaptureTime = 0;
    server.send(200, "text/plain", "Recording started");
}

void handleStopRecording() {
    if (!isRecording) { server.send(200, "text/plain", "No active recording"); return; }
    isRecording = false;
    cachedVideoCount++;
    currentVideoFolder = "";
    server.send(200, "text/plain", "Recording stopped");
}

// ============================================================
//  File listing / serving / deleting
// ============================================================
void handleListFiles() {
    if (!sdCardAvailable) { server.send(500, "application/json", "{\"error\":\"SD card not available\"}"); return; }

    std::vector<String> allFiles;
    File root = SD_MMC.open("/");
    if (root) { findJPGFiles(root, allFiles); root.close(); }

    std::sort(allFiles.begin(), allFiles.end(), [](const String& a, const String& b){ return a > b; });

    String json = "{\"files\":[";
    int limit = min(50, (int)allFiles.size());
    for (int i = 0; i < limit; i++) {
        if (i) json += ",";
        String fname = allFiles[i];
        size_t sz = 0;
        File f = SD_MMC.open(fname);
        if (f) { sz = f.size(); f.close(); }
        json += "{\"name\":\"" + fname.substring(fname.lastIndexOf('/')+1) + "\","
              + "\"path\":\"" + fname + "\","
              + "\"size\":"  + sz + "}";
    }
    json += "]}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

void handleGetFile() {
    if (!sdCardAvailable) { server.send(500, "text/plain", "SD card not available"); return; }
    String path = server.arg("path");
    if (path.isEmpty()) { server.send(400, "text/plain", "Missing path"); return; }

    File file = SD_MMC.open(path);
    if (!file || file.isDirectory()) { server.send(404, "text/plain", "Not found"); if(file)file.close(); return; }
    size_t sz = file.size();
    if (!sz) { file.close(); server.send(404, "text/plain", "Empty file"); return; }

    server.sendHeader("Cache-Control", "max-age=3600");   // cache photos in browser
    server.sendHeader("Access-Control-Allow-Origin", "*");

    WiFiClient client = server.client();
    // Send HTTP headers manually so we can stream the body
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", sz);

    uint8_t buf[4096];
    size_t n;
    while ((n = file.read(buf, sizeof(buf))) > 0) client.write(buf, n);
    file.close();
}

void handleDeleteFile() {
    if (!sdCardAvailable) { server.send(500, "text/plain", "SD card not available"); return; }
    String path = server.arg("path");
    if (path.isEmpty()) { server.send(400, "text/plain", "Missing path"); return; }
    if (SD_MMC.remove(path)) { cachedPhotoCount = max(0, cachedPhotoCount-1); server.send(200, "text/plain", "Deleted"); }
    else                      server.send(500, "text/plain", "Delete failed");
}

void handleFormatSD() {
    if (!sdCardAvailable) { server.send(500, "text/plain", "SD card not available"); return; }
    File root = SD_MMC.open("/");
    if (root) {
        while (File f = root.openNextFile()) {
            String p = f.name();
            if (f.isDirectory()) { if (!p.equals("/System Volume Information") && !p.startsWith("/.")) SD_MMC.rmdir(p); }
            else SD_MMC.remove(p);
            f.close();
        }
        root.close();
    }
    cachedPhotoCount = 0; cachedVideoCount = 0;
    File t = SD_MMC.open("/hdr_format.txt", FILE_WRITE);
    if (t) { t.println("UMSL 2026"); t.close(); SD_MMC.remove("/hdr_format.txt"); server.send(200, "text/plain", "Formatted successfully"); }
    else     server.send(500, "text/plain", "Format failed");
}

// ============================================================
//  LED
// ============================================================
void handleLED() {
    ledState = !ledState;
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, ledState ? HIGH : LOW);
    server.send(200, "text/plain", ledState ? "LED ON" : "LED OFF");
}

// ============================================================
//  Stats  – uses cached counts, no SD scan every call
// ============================================================
void handleStats() {
    uint64_t total = 0, used = 0, free_ = 0;
    if (sdCardAvailable) {
        total  = SD_MMC.totalBytes() / (1024*1024);
        used   = SD_MMC.usedBytes()  / (1024*1024);
        free_  = total - used;
    }

    String json = "{";
    json += "\"rssi\":"        + String(WiFi.RSSI())                      + ",";
    json += "\"freeRam\":"     + String(ESP.getFreeHeap())                + ",";
    json += "\"sdCard\":"      + String(sdCardAvailable ? "true":"false") + ",";
    json += "\"photoCount\":"  + String(cachedPhotoCount)                 + ",";
    json += "\"videoCount\":"  + String(cachedVideoCount)                 + ",";
    json += "\"isRecording\":" + String(isRecording    ? "true":"false")  + ",";
    json += "\"totalSpace\":"  + String(total)                            + ",";
    json += "\"usedSpace\":"   + String(used)                             + ",";
    json += "\"freeSpace\":"   + String(free_)                            + ",";
    json += "\"uptime\":"      + String(millis()/1000);
    json += "}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

// ============================================================
//  Root
// ============================================================
void handleRoot() {
    String html = FPSTR(INDEX_HTML);
    html.replace("%IP%",        WiFi.localIP().toString());
    html.replace("%SD_STATUS%", sdCardAvailable ? "✔" : "✘");
    server.send(200, "text/html", html);
}

// ============================================================
//  Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║  ESP32-CAM Fast Stream  (STA Mode)       ║");
    Serial.println("║  © Advanced Systems Lab UMSL 2026        ║");
    Serial.println("╚══════════════════════════════════════════╝\n");

    esp_bt_controller_deinit();
    setCpuFrequencyMhz(240);

    setupCamera();

    // ── WiFi Station Mode ────────────────────────────────────
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);          // disable power-saving
    esp_wifi_set_max_tx_power(78);          // ~19.5 dBm max TX

    WiFi.begin(ssid, password);
    Serial.printf("🔌 Connecting to \"%s\" ", ssid);

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++attempts > 40) {             // 20-second timeout
            Serial.println("\n❌ WiFi connection failed! Check SSID/password.");
            return;
        }
    }

    Serial.println(" ✅");
    Serial.printf("📡 IP      : %s\n",   WiFi.localIP().toString().c_str());
    Serial.printf("📶 RSSI    : %d dBm\n", WiFi.RSSI());
    Serial.printf("🎥 Stream  : http://%s/stream\n", WiFi.localIP().toString().c_str());
    Serial.printf("🌐 Viewer  : http://%s/\n",       WiFi.localIP().toString().c_str());

    server.on("/",       handleRoot);
    server.on("/stream", handleStream);
    server.onNotFound([]() { server.send(404, "text/plain", "404 Not Found"); });

    server.begin();
    Serial.println("✅ Server ready\n==========================================");
}
// ============================================================
//  Loop  – keep it lean; stream blocks in handleStream()
// ============================================================
void loop() {
    // Auto-reconnect if WiFi drops
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️  WiFi lost – reconnecting...");
        WiFi.reconnect();
        delay(5000);
    }
    server.handleClient();
}

