#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#include <time.h>

#include <IRremoteESP8266.h>
#include <IRsend.h>


struct ScheduleItem {
  String timeStr;   // "22:00"
  String keysStr;   // "25,NIGHT_ON"
};

uint16_t irInterFrameDelayMs = 40;  // default 40ms antar frame IR

const int MAX_SCHEDULES = 10;
ScheduleItem schedules[MAX_SCHEDULES];
int scheduleCount = 0;

int lastCheckedMinute = -1;

// ================== KONFIGURASI WI-FI ==================
const char* ssid     = "Tenda_3E0D00";
const char* password = "arif1234";

// ================== STATIC IP (opsional) ===============
IPAddress local_ip(192, 168, 0, 50);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

// ================== IR SEND =============================
const uint16_t IR_LED_PIN = 23;  // KY-005 -> pin S lewat resistor 220R dari GPIO23, pin - ke GND
IRsend irsend(IR_LED_PIN);

// ================== WEB SERVER ==========================
WebServer server(80);

// ================== STRUKTUR DATA KODE ==================
struct IrCode {
  String key;   // misal "25", "26", "OFF"
  String raw;   // "4436,4344,558,1592,..."
};

const int MAX_CODES = 20;
IrCode codes[MAX_CODES];
int codeCount = 0;

// ================== UTIL: PARSE RAW STRING ==============
/* bool sendRawString(const String &rawStr) {
  const int MAX_PULSES = 200;
  uint16_t buf[MAX_PULSES];
  int len = 0;

  int start = 0;
  while (start < rawStr.length()) {
    int comma = rawStr.indexOf(',', start);
    String token;
    if (comma == -1) {
      token = rawStr.substring(start);
      start = rawStr.length();
    } else {
      token = rawStr.substring(start, comma);
      start = comma + 1;
    }
    token.trim();
    if (token.length() == 0) continue;
    long val = token.toInt();
    if (val <= 0) continue;
    if (len >= MAX_PULSES) break;
    buf[len++] = (uint16_t)val;
  }

  if (len == 0) return false;

  Serial.print(F("Mengirim IR raw, len="));
  Serial.println(len);
  irsend.sendRaw(buf, len, 38); // 38 kHz carrier
  return true;
}
*/
bool sendOneRaw(const String &rawStr) {
  const int MAX_PULSES = 2000;
  uint16_t buf[MAX_PULSES];
  int len = 0;

  int start = 0;
  while (start < rawStr.length()) {
    int comma = rawStr.indexOf(',', start);
    String token;
    if (comma == -1) {
      token = rawStr.substring(start);
      start = rawStr.length();
    } else {
      token = rawStr.substring(start, comma);
      start = comma + 1;
    }
    token.trim();
    if (token.length() == 0) continue;
    long val = token.toInt();
    if (val <= 0) continue;
    if (len >= MAX_PULSES) break;
    buf[len++] = (uint16_t)val;
  }

  if (len == 0) return false;

  Serial.print(F("Mengirim 1 frame IR, len="));
  Serial.println(len);
  irsend.sendRaw(buf, len, 38); // 38 kHz
  return true;
}

bool sendMultiRaw(const String &rawMulti) {
  // rawMulti contoh: "4436,...,580|502,...,570"
  int start = 0;
  bool ok = false;

  while (start < rawMulti.length()) {
    int sep = rawMulti.indexOf('|', start);
    String part;
    if (sep == -1) {
      part = rawMulti.substring(start);
      start = rawMulti.length();
    } else {
      part = rawMulti.substring(start, sep);
      start = sep + 1;
    }
    part.trim();
    if (part.length() == 0) continue;

    if (sendOneRaw(part)) {
      ok = true;
      if (irInterFrameDelayMs > 0) {
        delay(irInterFrameDelayMs);   // <<=== delay antar frame dari setting
      }
    }
  }
  return ok;
}


// ================== FILE: LOAD & SAVE ===================

void loadCodes() {
  codeCount = 0;
  if (!SPIFFS.exists("/codes.txt")) {
    Serial.println(F("codes.txt belum ada, membuat default."));
    File f = SPIFFS.open("/codes.txt", FILE_WRITE);
    if (!f) {
      Serial.println(F("Gagal membuat codes.txt"));
      return;
    }
    // isi default kosong, nanti bisa diisi via web
    f.println("25:");
    f.println("26:");
    f.close();
  }

  File f = SPIFFS.open("/codes.txt", FILE_READ);
  if (!f) {
    Serial.println(F("Gagal membuka codes.txt"));
    return;
  }

  Serial.println(F("Memuat codes.txt:"));
  while (f.available() && codeCount < MAX_CODES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int sep = line.indexOf(':');
    if (sep == -1) continue;
    String key = line.substring(0, sep);
    String raw = line.substring(sep + 1);
    key.trim();
    raw.trim();
    codes[codeCount].key = key;
    codes[codeCount].raw = raw;
    Serial.print("  key=");
    Serial.print(key);
    Serial.print(" raw len=");
    Serial.println(raw.length());
    codeCount++;
  }
  f.close();
}

void saveCodes() {
  File f = SPIFFS.open("/codes.txt", FILE_WRITE);
  if (!f) {
    Serial.println(F("Gagal menulis codes.txt"));
    return;
  }
  for (int i = 0; i < codeCount; i++) {
    if (codes[i].key.length() == 0) continue;
    f.print(codes[i].key);
    f.print(":");
    f.println(codes[i].raw);
  }
  f.close();
  Serial.println(F("codes.txt tersimpan."));
}

// Cari kode berdasarkan key
int findCodeIndex(const String &key) {
  for (int i = 0; i < codeCount; i++) {
    if (codes[i].key == key) return i;
  }
  return -1;
}

// Tambah atau update kode
void upsertCode(const String &key, const String &raw) {
  int idx = findCodeIndex(key);
  if (idx == -1) {
    if (codeCount >= MAX_CODES) {
      Serial.println(F("Slot kode sudah penuh."));
      return;
    }
    idx = codeCount++;
  }
  codes[idx].key = key;
  codes[idx].raw = raw;
  Serial.print(F("Kode disimpan: "));
  Serial.print(key);
  Serial.print(F(" len raw="));
  Serial.println(raw.length());
  saveCodes();
}

// Hapus kode berdasarkan key
bool deleteCode(const String &key) {
  int idx = findCodeIndex(key);
  if (idx == -1) return false;

  for (int i = idx; i < codeCount - 1; i++) {
    codes[i] = codes[i + 1];
  }
  codeCount--;
  if (codeCount < 0) codeCount = 0;

  saveCodes();
  Serial.print(F("Kode dihapus: "));
  Serial.println(key);
  return true;
}

void loadSettings() {
  if (!SPIFFS.exists("/settings.txt")) {
    return; // pakai default
  }

  File f = SPIFFS.open("/settings.txt", FILE_READ);
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    int eq = line.indexOf('=');
    if (eq == -1) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();

    if (key == "ir_delay_ms") {
      int v = val.toInt();
      if (v < 0) v = 0;
      if (v > 500) v = 500; // batasi 0–500 ms
      irInterFrameDelayMs = (uint16_t)v;
      Serial.print(F("Load irInterFrameDelayMs = "));
      Serial.println(irInterFrameDelayMs);
    }
  }

  f.close();
}

void saveSettings() {
  File f = SPIFFS.open("/settings.txt", FILE_WRITE);
  if (!f) {
    Serial.println(F("Gagal buka /settings.txt untuk write"));
    return;
  }
  f.print("ir_delay_ms=");
  f.println(irInterFrameDelayMs);
  f.close();
  Serial.print(F("Settings disimpan. irInterFrameDelayMs = "));
  Serial.println(irInterFrameDelayMs);
}

void loadSchedules() {
  scheduleCount = 0;
  if (!SPIFFS.exists("/schedules.txt")) {
    // bikin default kosong
    File f = SPIFFS.open("/schedules.txt", FILE_WRITE);
    if (f) {
      // Contoh default (bisa dikosongkan)
      // f.println("22:00=25,NIGHT_ON");
      // f.println("05:00=20,NIGHT_OFF");
      f.close();
    }
    return;
  }

  File f = SPIFFS.open("/schedules.txt", FILE_READ);
  if (!f) return;

  while (f.available() && scheduleCount < MAX_SCHEDULES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int eq = line.indexOf('=');
    if (eq == -1) continue;
    schedules[scheduleCount].timeStr = line.substring(0, eq);
    schedules[scheduleCount].keysStr = line.substring(eq + 1);
    schedules[scheduleCount].timeStr.trim();
    schedules[scheduleCount].keysStr.trim();
    scheduleCount++;
  }
  f.close();
}

void saveSchedules() {
  File f = SPIFFS.open("/schedules.txt", FILE_WRITE);
  if (!f) return;
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].timeStr.length() == 0) continue;
    f.print(schedules[i].timeStr);
    f.print("=");
    f.println(schedules[i].keysStr);
  }
  f.close();
}

void runSchedulesIfDue() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int currentMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  if (currentMinute == lastCheckedMinute) {
    return; // sudah dicek menit ini
  }
  lastCheckedMinute = currentMinute;

  char buf[6];
  sprintf(buf, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  String nowStr(buf);

  Serial.print(F("Cek schedule untuk "));
  Serial.println(nowStr);

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].timeStr == nowStr) {
      Serial.print(F("Schedule match: "));
      Serial.print(schedules[i].timeStr);
      Serial.print(" => ");
      Serial.println(schedules[i].keysStr);

      // Pecah keysStr jadi key per key
      String keys = schedules[i].keysStr;
      int start = 0;
      while (start < keys.length()) {
        int comma = keys.indexOf(',', start);
        String key;
        if (comma == -1) {
          key = keys.substring(start);
          start = keys.length();
        } else {
          key = keys.substring(start, comma);
          start = comma + 1;
        }
        key.trim();
        if (key.length() == 0) continue;

        int idx = findCodeIndex(key);
        if (idx == -1 || codes[idx].raw.length() == 0) {
          Serial.print(F("  Key tidak ditemukan / kosong: "));
          Serial.println(key);
          continue;
        }

        Serial.print(F("  Kirim key: "));
        Serial.println(key);
        // gunakan sendMultiRaw supaya dukung 2 frame
        if (!sendMultiRaw(codes[idx].raw)) {
          Serial.println(F("  Gagal kirim raw untuk key ini"));
        }
        delay(40); // jeda antar key
      }
    }
  }
}

// ================== HANDLER HTTP ========================

// Halaman utama: list tombol + form tambah/ubah
void handleRoot() {
  String editKey = "";
  String editRaw = "";
  if (server.hasArg("edit")) {
    editKey = server.arg("edit");
    int idx = findCodeIndex(editKey);
    if (idx != -1) {
      editRaw = codes[idx].raw;
    }
  }

  String html;
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32 AC IR</title>");
  html += F("<script>"
          "function sendIr(key){"
          "  fetch('/ac?key=' + encodeURIComponent(key))"
          "    .then(r => r.text())"
          "    .then(t => {"
          "      console.log('IR sent for key:', key);"
          "      const s = document.getElementById('status');"
          "      if(s){s.textContent = 'IR terkirim untuk ' + key + ' (' + new Date().toLocaleTimeString() + ')';}"
          "    })"
          "    .catch(e => console.error(e));"
          "  return false;"
          "}"
          "</script>");
  html += F("<style>"
            "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
            ".wrap{max-width:900px;margin:0 auto;}"
            ".card{background:#020617;border-radius:18px;padding:20px;box-shadow:0 20px 40px rgba(0,0,0,.6);border:1px solid #1f2937;}"
            "h1{font-size:22px;margin:0 0 4px;}"
            "h2{font-size:18px;margin:16px 0 8px;}"
            "p{font-size:14px;color:#9ca3af;}"
            "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px;}"
            "th,td{text-align:left;padding:6px 4px;border-bottom:1px solid #1f2937;}"
            "th{color:#9ca3af;font-weight:500;font-size:12px;text-transform:uppercase;letter-spacing:.04em;}"
            ".badge{display:inline-block;padding:2px 10px;border-radius:999px;background:#111827;color:#e5e7eb;font-size:11px;}"
            ".btn{display:inline-block;padding:6px 10px;border-radius:999px;text-decoration:none;font-size:12px;margin:0 3px 3px 0;}"
            ".btn-primary{background:#22c55e;color:#022c22;}"
            ".btn-secondary{background:#0ea5e9;color:#e0f2fe;}"
            ".btn-danger{background:#ef4444;color:#fef2f2;}"
            "label{display:block;font-size:12px;color:#9ca3af;margin-top:8px;margin-bottom:2px;}"
            "input,textarea{width:100%;max-width:100%;border-radius:10px;border:none;padding:8px 10px;margin:0 0 8px;background:#020617;color:#e5e7eb;box-shadow:0 0 0 1px #1f2937 inset;font-size:13px;}"
            "input:focus,textarea:focus{outline:none;box-shadow:0 0 0 1px #38bdf8 inset;}"
            "button{border:none;border-radius:999px;padding:8px 14px;background:#22c55e;color:#022c22;font-size:13px;cursor:pointer;margin-top:4px;}"
            "button:hover{filter:brightness(1.1);}"
            ".grid{display:grid;grid-template-columns:minmax(0,2fr) minmax(0,1.5fr);gap:18px;margin-top:18px;}"
            "@media(max-width:768px){.grid{grid-template-columns:1fr;}}"
            "small{color:#6b7280;font-size:11px;}"
            "</style></head><body><div class='wrap'><div class='card'>");

  html += F("<h1>ESP32 AC IR Controller</h1>");
  html += F("<p>Kelola tombol IR (suhu, OFF, mode, dsb.) langsung dari web, tanpa flash ulang.</p>");
  html += F("<p><a class='btn btn-secondary' href='/update'>Firmware Update (OTA)</a></p>");


  html += F("<div class='grid'>");

  // Kolom kiri: daftar tombol
  html += F("<div>");
  html += F("<h2>Daftar Tombol</h2>");
  if (codeCount == 0) {
    html += F("<p><i>Belum ada kode tersimpan.</i></p>");
  } else {
    html += F("<table><thead><tr><th>Key</th><th>Panjang Raw</th><th>Aksi</th></tr></thead><tbody>");
    for (int i = 0; i < codeCount; i++) {
      html += F("<tr><td><span class='badge'>");
      html += codes[i].key;
      html += F("</span></td><td><small>");
      html += String(codes[i].raw.length());
      html += F(" chars</small></td><td>");
      html += F("<button class='btn btn-primary' onclick=\"return sendIr('");
      html += codes[i].key;
      html += F("');\">Kirim</button>");
      html += F("<a class='btn btn-secondary' href='/?edit=");
      html += codes[i].key;
      html += F("'>Edit</a>");
      html += F("<a class='btn btn-danger' href='/delete?key=");
      html += codes[i].key;
      html += F("' onclick=\"return confirm('Hapus key ");
      html += codes[i].key;
      html += F(" ?');\">Hapus</a>");
      html += F("</td></tr>");
    }
    html += F("</tbody></table>");
    html += F("<p id='status'><small>Siap mengirim IR.</small></p>");
  }
  html += F("<p><small>Akses cepat juga tersedia: /ac?temp=25, /ac?temp=26, dll (menggunakan key yang sama).</small></p>");
  html += F("</div>");

  // Kolom kanan: form tambah/ubah
  html += F("<div>");
  html += F("<h2>Tambah / Ubah Kode</h2>");
  html += F("<form method='POST' action='/save'>");
  html += F("<label>Key (misal 25, 26, OFF, COOL):</label>");
  html += F("<input name='key' value='");
  if (editKey.length() > 0) {
    html += editKey;
  }
  html += F("'>");

  html += F("<label>Raw data (angka µs dipisah koma):</label>");
  html += F("<textarea name='raw' rows='8' cols='50'>");
  if (editRaw.length() > 0) {
    String rawDisplay = editRaw;
    rawDisplay.replace('|', '\n');
    html += rawDisplay;
  }
  html += F("</textarea>");

  html += F("<button type='submit'>Simpan Kode</button>");
  html += F("<p><small>Tempel rawData dari Serial Monitor, misalnya nilai di dalam <code>rawData[]</code> tanpa tanda kurung kurawal.</small></p>");
  html += F("</form>");
  html += F("</div>"); // end kolom kanan
  html += F("<hr><h2>Pengaturan IR</h2>");
  html += F("<form method='POST' action='/savesettings'>");
  html += F("<label>Delay antar frame (ms):</label><br>");
  html += F("<input type='number' name='delay' min='0' max='500' value='");
  html += String(irInterFrameDelayMs);
  html += F("' style='width:100px;padding:4px;margin:4px 0;border-radius:8px;border:1px solid #1f2937;background:#020617;color:#e5e7eb;'>");
  html += F("<br><button type='submit' class='btn btn-primary'>Simpan Delay</button>");
  html += F("</form>");

  html += F("</div>"); // end grid

  html += F("<hr><h2>Schedule Otomatis</h2>");

  if (scheduleCount == 0) {
    html += F("<p><i>Belum ada schedule.</i></p>");
  } else {
    html += F("<ul>");
    for (int i = 0; i < scheduleCount; i++) {
      html += F("<li><span class='badge'>");
      html += schedules[i].timeStr;
      html += F("</span> &mdash; ");
      html += schedules[i].keysStr;
      html += F(" <a class='btn btn-danger' href='/delsched?i=");
      html += String(i);
      html += F("'>Hapus</a></li>");
    }
    html += F("</ul>");
  }

  html += F("<h3>Tambah Schedule</h3>");
  html += F("<form method='POST' action='/savesched'>");
  html += F("Jam (HH:MM, 24 jam):<br><input name='time' placeholder='22:00'><br>");
  html += F("Keys (dipisah koma, misal: 25,NIGHT_ON):<br><input name='keys' placeholder='25,NIGHT_ON'><br>");
  html += F("<button type='submit'>Simpan Schedule</button>");
  html += F("</form>");

  html += F("</div></div></body></html>");

  server.send(200, "text/html", html);
}

// Kirim IR berdasarkan key/temp
void handleAc() {
  String key;
  if (server.hasArg("temp")) {
    key = server.arg("temp");   // kompatibel /ac?temp=25
  } else if (server.hasArg("key")) {
    key = server.arg("key");    // /ac?key=OFF
  } else {
    server.send(400, "text/plain", "Parameter 'key' atau 'temp' diperlukan, contoh: /ac?temp=25");
    return;
  }

  key.trim();
  int idx = findCodeIndex(key);
  if (idx == -1 || codes[idx].raw.length() == 0) {
    server.send(404, "text/plain", "Kode untuk key '" + key + "' tidak ditemukan atau kosong.");
    return;
  }

  Serial.print(F("Request kirim key="));
  Serial.println(key);

  // Kirim 3x supaya lebih kuat
  if (!sendMultiRaw(codes[idx].raw)) {
    server.send(500, "text/plain", "Gagal parsing raw data untuk key " + key);
    return;
  }

  server.send(200, "text/plain", "IR terkirim untuk key " + key);
}

// Simpan/Update kode dari form
void handleSave() {
  if (!server.hasArg("key") || !server.hasArg("raw")) {
    server.send(400, "text/plain", "Butuh 'key' dan 'raw'.");
    return;
  }
  String key = server.arg("key");
  String raw = server.arg("raw");
  key.trim();
  raw.trim();
  if (key.length() == 0 || raw.length() == 0) {
    server.send(400, "text/plain", "Key dan raw tidak boleh kosong.");
    return;
  }

  raw.replace("\r\n", "\n");
  raw.replace('\r', '\n');
  while (raw.indexOf("\n\n") != -1) {
    raw.replace("\n\n", "\n");   // hilangkan baris kosong dobel
  }
  raw.replace('\n', '|');
  upsertCode(key, raw);
  server.sendHeader("Location", "/?edit=" + key);
  server.send(303); // redirect
}

// Hapus kode dari query ?key=
void handleDelete() {
  if (!server.hasArg("key")) {
    server.send(400, "text/plain", "Parameter 'key' diperlukan, contoh: /delete?key=25");
    return;
  }
  String key = server.arg("key");
  key.trim();
  if (key.length() == 0) {
    server.send(400, "text/plain", "Key tidak boleh kosong.");
    return;
  }

  if (!deleteCode(key)) {
    server.send(404, "text/plain", "Key '" + key + "' tidak ditemukan.");
    return;
  }

  server.sendHeader("Location", "/");
  server.send(303); // redirect
}

void handleSaveSchedule() {
  if (!server.hasArg("time") || !server.hasArg("keys")) {
    server.send(400, "text/plain", "Butuh 'time' dan 'keys'.");
    return;
  }
  String t = server.arg("time");
  String k = server.arg("keys");
  t.trim();
  k.trim();
  if (t.length() == 0 || k.length() == 0) {
    server.send(400, "text/plain", "Time dan keys tidak boleh kosong.");
    return;
  }
  if (scheduleCount >= MAX_SCHEDULES) {
    server.send(400, "text/plain", "Slot schedule penuh.");
    return;
  }
  schedules[scheduleCount].timeStr = t;
  schedules[scheduleCount].keysStr = k;
  scheduleCount++;
  saveSchedules();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDeleteSchedule() {
  if (!server.hasArg("i")) {
    server.send(400, "text/plain", "Parameter 'i' diperlukan.");
    return;
  }
  int idx = server.arg("i").toInt();
  if (idx < 0 || idx >= scheduleCount) {
    server.send(404, "text/plain", "Index schedule tidak valid.");
    return;
  }
  for (int i = idx; i < scheduleCount - 1; i++) {
    schedules[i] = schedules[i + 1];
  }
  scheduleCount--;
  if (scheduleCount < 0) scheduleCount = 0;
  saveSchedules();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveSettings() {
  if (server.hasArg("delay")) {
    int v = server.arg("delay").toInt();
    if (v < 0) v = 0;
    if (v > 500) v = 500; // batas aman
    irInterFrameDelayMs = (uint16_t)v;
    saveSettings();
  }
  server.sendHeader("Location", "/");
  server.send(303); // redirect balik ke home
}

void handleUpdatePage() {
  String html;
  html += F(
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>OTA Update</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
    ".card{max-width:500px;margin:0 auto;background:#020617;border-radius:18px;padding:20px;box-shadow:0 20px 40px rgba(0,0,0,.6);border:1px solid #1f2937;}"
    "h2{margin:0 0 8px;font-size:20px;}"
    "p{font-size:13px;color:#9ca3af;}"
    "input[type=file]{width:100%;margin-top:8px;padding:8px 10px;border-radius:12px;border:none;background:#020617;color:#e5e7eb;box-shadow:0 0 0 1px #1f2937 inset;font-size:13px;}"
    "button{margin-top:12px;width:100%;padding:10px;border:none;border-radius:999px;background:#22c55e;color:#022c22;font-size:14px;cursor:pointer;}"
    "button:hover{filter:brightness(1.08);}"
    ".bar-wrap{margin-top:14px;width:100%;height:10px;border-radius:999px;background:#020617;box-shadow:0 0 0 1px #1f2937 inset;overflow:hidden;}"
    ".bar{height:100%;width:0%;background:#22c55e;transition:width .15s linear;}"
    "#log{margin-top:14px;width:100%;height:140px;border-radius:12px;background:#020617;color:#e5e7eb;box-shadow:0 0 0 1px #1f2937 inset;font-size:11px;padding:8px;white-space:pre-wrap;overflow:auto;font-family:ui-monospace,monospace;}"
    "a{color:#38bdf8;text-decoration:none;font-size:13px;}"
    "</style>"
    "</head><body><div class='card'>"
    "<h2>Firmware OTA Update</h2>"
    "<p>Upload file <b>.bin</b> hasil compile (Arduino &rarr; Export compiled binary). Jangan matikan ESP32 saat proses.</p>"
    "<input type='file' id='fw' accept='.bin'>"
    "<button onclick='startOta()'>Upload &amp; Update</button>"
    "<div class='bar-wrap'><div id='bar' class='bar'></div></div>"
    "<div id='log'></div>"
    "<p><a href='/'>&larr; Kembali ke Home</a></p>"
    "<script>"
    "function log(msg){"
      "var l=document.getElementById('log');"
      "l.textContent += msg+'\\n';"
      "l.scrollTop=l.scrollHeight;"
    "}"
    "function setProgress(p){"
      "document.getElementById('bar').style.width=p+'%';"
    "}"
    "function startOta(){"
      "var f=document.getElementById('fw').files;"
      "if(!f||!f.length){log('Pilih file .bin terlebih dahulu.');return;}"
      "var file=f[0];"
      "log('Mulai upload: '+file.name+' ('+file.size+' bytes)');"
      "setProgress(0);"
      "var xhr=new XMLHttpRequest();"
      "xhr.open('POST','/update',true);"
      "xhr.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
          "var p=Math.round((e.loaded/e.total)*100);"
          "setProgress(p);"
        "}"
      "};"
      "xhr.onreadystatechange=function(){"
        "if(xhr.readyState==4){"
          "log('Respons: '+xhr.status+' '+xhr.responseText.trim());"
          "if(xhr.status==200){"
            "log('Jika update BERHASIL, ESP32 akan reboot otomatis...');"
          "}"
        "}"
      "};"
      "xhr.onerror=function(){log('Terjadi error koneksi saat upload.');};"
      "var formData=new FormData();"
      "formData.append('firmware',file);"
      "xhr.send(formData);"
    "}"
    "</script>"
    "</div></body></html>"
  );
  server.send(200, "text/html", html);
}

void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA mulai: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Tulis chunk ke flash
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA sukses: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}


// ================== SETUP ===============================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(F("ESP32 AC IR - Web Configurable + Delete + UI"));

  irsend.begin();

  if (!SPIFFS.begin(true)) {
    Serial.println(F("Gagal mount SPIFFS"));
  } else {
    loadCodes();
  }

  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_ip, gateway, subnet, dns)) {
    Serial.println(F("Gagal set static IP (lanjut DHCP)."));
  }
  WiFi.begin(ssid, password);
  Serial.print(F("Menghubungkan ke WiFi"));
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("."));
  }
  Serial.println();
  Serial.print(F("Terhubung! IP: "));
  Serial.println(WiFi.localIP());

  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println(F("Menunggu sinkronisasi waktu NTP..."));
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {  // waktu belum valid
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
  Serial.println(F("Waktu NTP sudah tersinkron."));
  loadSettings();


  server.on("/", handleRoot);
  server.on("/ac", handleAc);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", handleDelete);
  server.on("/savesched", HTTP_POST, handleSaveSchedule);
  server.on("/delsched", handleDeleteSchedule);
  server.on("/savesettings", HTTP_POST, handleSaveSettings);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, []() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain",
              (Update.hasError()) ? "Update GAGAL." : "Update BERHASIL. ESP32 akan reboot.");
  delay(1000);
  if (!Update.hasError()) {
    ESP.restart();
  }
}, handleFirmwareUpload);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println(F("Web server berjalan di port 80"));
}

// ================== LOOP ================================

void loop() {
  server.handleClient();
  runSchedulesIfDue();
}
