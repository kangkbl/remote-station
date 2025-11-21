#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <DHT.h>
#include <BH1750.h>
#include <time.h>
#include <math.h>

// Tambahan
#include <RTClib.h>       // RTC DS1307
#include <SD.h>           // microSD
#include <SPI.h>
#include <ArduinoOTA.h>   // OTA via WiFi
#include <sys/time.h>     // settimeofday()

// ====== PIN & TIPE SENSOR ======
#define DHTPIN 4
#define DHTTYPE DHT22

// ====== WiFi ======
const char* ssid     = "Tenda_3E0D00";
const char* password = "arif1234";

// Static IP (ubah gateway/dns sesuai router)
IPAddress local_IP(192, 168, 0, 200);
IPAddress gateway (192, 168, 0, 1);
IPAddress subnet  (255, 255, 255, 0);
IPAddress dns1    (8, 8, 8, 8);
IPAddress dns2    (1, 1, 1, 1);

// ====== Zona waktu / NTP ======
const char* ntpServer       = "pool.ntp.org";
const long  gmtOffset_sec   = 7 * 3600; // WIB
const int   daylightOffset  = 0;

// Jam siang/malam sederhana
const int DAY_START_HOUR   = 6;
const int NIGHT_START_HOUR = 18;

// ====== Opsi penggunaan lux siang hari ======
const bool USE_LUX_DAY = true;

// ====== Sensor ======
Adafruit_BME280 bme;
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;

// ====== RTC & microSD ======
RTC_DS1307 rtc;
const int SD_CS = 5;                 // Ubah sesuai modul microSD
const char* DAILY_CSV = "/weather_daily.csv";

// ====== Web server ======
WebServer server(80);

// ====== Variabel data (terbaru) ======
float temp_bme = NAN, hum_bme = NAN, pres_bme = NAN, alt_bme = NAN;
float temp_dht = NAN, hum_dht = NAN, lux = NAN, hum_avg = NAN;
float dew_point = NAN, dew_spread = NAN;
float pres_slope_hpa_per_h = 0.0f;
String kondisi = "-";
bool is_night = false;

// ====== Riwayat tekanan untuk tren (3 jam @ 5 menit) ======
const uint8_t TREND_BUF = 36;
float presHist[TREND_BUF];
unsigned long timeHist[TREND_BUF];
uint8_t trendCount = 0, trendIndex = 0;
unsigned long lastTrendSample = 0;
const unsigned long TREND_INTERVAL = 5UL * 60UL * 1000UL; // 5 menit

// ====== Akumulator rata-rata per jam (hari ini) ======
struct Accum { double sum[24]; uint32_t cnt[24]; };
void resetAccum(Accum &a){ for(int i=0;i<24;i++){ a.sum[i]=0.0; a.cnt[i]=0; } }
void addSample(Accum &a, int h, float v){ if(!isnan(v)&&h>=0&&h<24){ a.sum[h]+= (double)v; a.cnt[h]++; } }
float avgAtHour(const Accum &a, int h){ if(h<0||h>=24||a.cnt[h]==0) return NAN; return (float)(a.sum[h]/(double)a.cnt[h]); }

String jsonArrayFromAccum(const Accum &a, unsigned int decimals=2){
  String s="["; char buf[32];
  for(int h=0; h<24; h++){
    if(a.cnt[h]==0) s+="null";
    else { double v=a.sum[h]/(double)a.cnt[h]; dtostrf(v,0,decimals,buf); s+=buf; }
    if(h!=23) s+=",";
  }
  s+="]"; return s;
}

int last_yday = -1;

// satu akumulator per parameter
Accum acc_temp_dht, acc_hum_dht, acc_temp_bme, acc_hum_bme, acc_hum_avg,
      acc_pres_bme, acc_alt_bme, acc_dew_point, acc_dew_spread, acc_lux,
      acc_pres_slope;

void resetAllAccums(){
  resetAccum(acc_temp_dht); resetAccum(acc_hum_dht); resetAccum(acc_temp_bme);
  resetAccum(acc_hum_bme); resetAccum(acc_hum_avg); resetAccum(acc_pres_bme);
  resetAccum(acc_alt_bme); resetAccum(acc_dew_point); resetAccum(acc_dew_spread);
  resetAccum(acc_lux); resetAccum(acc_pres_slope);
}

// ====== Status cuaca harian (untuk CSV) ======
enum StatusCat { CERAH=0, MENDUNG=1, HUJAN=2, KABUT=3, BERAWAN=4, CAT_COUNT=5 };
int statusCount[CAT_COUNT] = {0,0,0,0,0};
int lastCountMinute = -1;

StatusCat categorizeStatus(const String& s){
  String x = s; x.toLowerCase();
  if (x.indexOf("kabut") >=0) return KABUT;
  if (x.indexOf("hujan") >=0) return HUJAN;
  if (x.indexOf("mendung")>=0) return MENDUNG;
  if (x.indexOf("berawan")>=0) return BERAWAN;
  return CERAH;
}
const char* statusLabel(StatusCat c){
  switch(c){
    case CERAH: return "Cerah";
    case MENDUNG: return "Mendung";
    case HUJAN: return "Hujan";
    case KABUT: return "Kabut";
    case BERAWAN: return "Berawan";
    default: return "-";
  }
}
String dominantStatus(){
  int bestIdx = 0; int bestVal = statusCount[0];
  for(int i=1;i<CAT_COUNT;i++){ if(statusCount[i]>bestVal){ bestVal=statusCount[i]; bestIdx=i; } }
  return String(statusLabel((StatusCat)bestIdx));
}
void resetDailyStatus(){ for(int i=0;i<CAT_COUNT;i++) statusCount[i]=0; lastCountMinute=-1; }

// ===== HTML (dipersingkat: sama seperti punyamu) =====
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!-- (HTML sama persis dengan versi kamu; dipotong demi singkat) -->
<!DOCTYPE html>
<html lang="id" data-theme="light">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Weather Station</title>
<style>/* ... (CSSmu tidak diubah) ... */</style>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<body class="light">
  <div class="container">
    <div class="toolbar">
      <div class="title">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M3 12a9 9 0 1 0 18 0A9 9 0 0 0 3 12Zm9-7v14M5 9h14M5 15h14" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>
        <span>ESP32 Weather Station</span>
        <span class="badge" id="mode">-</span>
      </div>
      <button class="btn" id="themeBtn" title="Toggle tema">🌙</button>
      <a class="btn" href="/daily.csv" title="Unduh ringkasan harian CSV">📥 daily.csv</a>
    </div>
    <!-- (sisanya sama seperti HTML kamu) -->
    <!-- ... metric cards, chart, table, JS ... -->
    <div class="card">
      <div class="metrics">
        <div class="metric"><div class="top"><span class="label">DHT22 Suhu</span><span class="val" id="temp_dht">--</span></div><div class="chip">°C</div></div>
        <div class="metric"><div class="top"><span class="label">DHT22 Kelembaban</span><span class="val" id="hum_dht">--</span></div><div class="chip">%</div></div>
        <div class="metric"><div class="top"><span class="label">Suhu (BME280)</span><span class="val" id="temp_bme">--</span></div><div class="chip">°C</div></div>
        <div class="metric"><div class="top"><span class="label">Kelembaban (rata-rata)</span><span class="val" id="hum_avg">--</span></div><div class="chip">%</div></div>
        <div class="metric"><div class="top"><span class="label">Tekanan Udara</span><span class="val" id="pres_bme">--</span></div><div class="chip">hPa</div></div>
        <div class="metric"><div class="top"><span class="label">Tren Tekanan</span><span class="val" id="pres_slope">--</span></div><div class="chip">hPa/jam</div></div>
        <div class="metric"><div class="top"><span class="label">Ketinggian (BME280)</span><span class="val" id="alt_bme">--</span></div><div class="chip">m</div></div>
        <div class="metric"><div class="top"><span class="label">Dew Point</span><span class="val" id="dew_point">--</span></div><div class="chip">°C</div></div>
        <div class="metric"><div class="top"><span class="label">Spread (T - Td)</span><span class="val" id="dew_spread">--</span></div><div class="chip">°C</div></div>
        <div class="metric"><div class="top"><span class="label">Cahaya (Lux)</span><span class="val" id="lux">--</span></div><div class="chip">lx</div></div>
        <div class="metric"><div class="top"><span class="label">Status Cuaca</span><span class="val" id="kondisi">--</span></div><div class="chip">Realtime</div></div>
        <div class="metric"><div class="top"><span class="label">Terakhir update</span><span class="val" id="updated">-</span></div><div class="chip">Local</div></div>
      </div>
    </div>

    <div class="section">
      <div class="card">
        <div class="controls">
          <label for="paramPicker" class="kecil">Parameter</label>
          <select id="paramPicker"></select>
          <button class="btn" id="refreshBtn">Segarkan</button>
          <span class="kecil" id="stamp">-</span>
        </div>
        <canvas id="chart"></canvas>
      </div>
      <div class="card">
        <div class="tablewrap">
          <table>
            <thead>
              <tr><th>Jam</th><th id="thVal">Nilai</th><th>Δ vs jam sblm</th></tr>
            </thead>
            <tbody id="hourlyTable"></tbody>
          </table>
        </div>
      </div>
    </div>
  </div>
  <script>
    /* (JS kamu tidak diubah, hanya ditambah link /daily.csv di toolbar) */
    function fmt(v, unit) {
      if (v === null || Number.isNaN(v)) return '--';
      if (typeof v === 'number') v = v.toFixed(2);
      return v + (unit || '');
    }
    function isDark(){ return !document.body.classList.contains('light'); }
    function setTheme(dark){ document.body.classList.toggle('light', !dark); localStorage.setItem('theme', dark?'dark':'light'); document.getElementById('themeBtn').textContent = dark?'☀️':'🌙'; }
    (function(){ const pref=localStorage.getItem('theme'); setTheme(pref==='dark'); })();
    document.getElementById('themeBtn').addEventListener('click', ()=> setTheme(!isDark()));


    const PARAMS = {
      temp_bme:   {label:'Suhu (BME280)', unit:' °C'},
      temp_dht:   {label:'Suhu (DHT22)',  unit:' °C'},
      hum_bme:    {label:'Kelembaban (BME280)', unit:' %'},
      hum_dht:    {label:'Kelembaban (DHT22)',  unit:' %'},
      hum_avg:    {label:'Kelembaban (rata-rata)', unit:' %'},
      pres_bme:   {label:'Tekanan Udara', unit:' hPa'},
      pres_slope_hpa_per_h: {label:'Tren Tekanan', unit:' hPa/jam'},
      alt_bme:    {label:'Ketinggian', unit:' m'},
      dew_point:  {label:'Dew Point', unit:' °C'},
      dew_spread: {label:'Spread (T - Td)', unit:' °C'},
      lux:        {label:'Cahaya (Lux)', unit:' lx'}
    };

    let CHART, HIST = null;

    async function load() {
      try {
        const r = await fetch('/data', {cache:'no-store'});
        const d = await r.json();
        document.getElementById('mode').textContent      = d.is_night ? 'Malam' : 'Siang';
        document.getElementById('temp_dht').textContent  = fmt(d.temp_dht, ' °C');
        document.getElementById('hum_dht').textContent   = fmt(d.hum_dht,  ' %');
        document.getElementById('temp_bme').textContent  = fmt(d.temp_bme, ' °C');
        document.getElementById('hum_avg').textContent   = fmt(d.hum_avg,  ' %');
        document.getElementById('pres_bme').textContent  = fmt(d.pres_bme, ' hPa');
        document.getElementById('pres_slope').textContent= fmt(d.pres_slope_hpa_per_h, ' hPa/jam');
        document.getElementById('alt_bme').textContent   = fmt(d.alt_bme,  ' m');
        document.getElementById('dew_point').textContent = fmt(d.dew_point, ' °C');
        document.getElementById('dew_spread').textContent= fmt(d.dew_spread, ' °C');
        document.getElementById('lux').textContent       = d.lux === null ? '--' : fmt(d.lux, ' lx');
        document.getElementById('kondisi').textContent   = d.kondisi || '--';
        document.getElementById('updated').textContent   = new Date().toLocaleTimeString();
      } catch (e) { console.error(e); }
    }

    async function loadHistory() {
      try {
        const r = await fetch('/history', {cache:'no-store'});
        HIST = await r.json();
        document.getElementById('stamp').textContent = 'Terakhir muat: ' + new Date().toLocaleTimeString();
        const pick = document.getElementById('paramPicker');
        if (!pick.options.length) {
          for (const k of Object.keys(PARAMS)) {
            const opt = document.createElement('option');
            opt.value = k; opt.textContent = PARAMS[k].label; pick.appendChild(opt);
          }
        }
        renderSelected();
      } catch(e) { console.error(e); }
    }

    function makeGradient(ctx){ const g = ctx.createLinearGradient(0,0,0,320); g.addColorStop(0,'rgba(110,139,255,.35)'); g.addColorStop(1,'rgba(110,139,255,0.02)'); return g; }

    function renderSelected() {
      const key = document.getElementById('paramPicker').value || 'temp_bme';
      const meta = PARAMS[key];
      document.getElementById('thVal').textContent = meta.label + ' (' + meta.unit.trim() + ')';
      const labels = HIST.labels || [];
      const data = (HIST[key] || []).slice();

      // Chart
      const ctx = document.getElementById('chart').getContext('2d');
      if (CHART) CHART.destroy();
      CHART = new Chart(ctx, {
        type: 'line',
        data: {
          labels,
          datasets: [{ label: meta.label, data, spanGaps: true, tension: 0.32, pointRadius: 2, borderWidth: 2, fill: true, backgroundColor: makeGradient(ctx) }]
        },
        options: {
          responsive: true,
          scales: { y: { beginAtZero: false, grid: { color:'rgba(125,125,125,.15)' } }, x:{ grid:{ color:'rgba(125,125,125,.1)' } } },
          plugins: { legend: { display: false } }
        }
      });

      // Tabel
      const tb = document.getElementById('hourlyTable');
      tb.innerHTML = '';
      for (let i = 0; i < labels.length; i++) {
        const v = data[i];
        const prev = i>0 ? data[i-1] : null;
        const tr = document.createElement('tr');
        const tdH = document.createElement('td'); tdH.textContent = labels[i] + ':00';
        const tdV = document.createElement('td'); tdV.textContent = (v == null ? '--' : Number(v).toFixed(2) + meta.unit);
        const tdD = document.createElement('td');
        if (v == null || prev == null) { tdD.textContent = '—'; tdD.className='delta flat'; }
        else {
          const d = Number(v) - Number(prev);
          const dir = d>0 ? 'up' : (d<0 ? 'down' : 'flat');
          const arrow = d>0 ? '▲' : (d<0 ? '▼' : '■');
          tdD.textContent = arrow + ' ' + Math.abs(d).toFixed(2) + meta.unit;
          tdD.className = 'delta ' + dir;
        }
        tr.appendChild(tdH); tr.appendChild(tdV); tr.appendChild(tdD); tb.appendChild(tr);
      }
    }

    setInterval(load, 2000);
    setInterval(loadHistory, 60000);
    window.addEventListener('load', () => { load(); loadHistory(); });
    document.getElementById('refreshBtn').addEventListener('click', () => { load(); loadHistory(); });
    document.getElementById('paramPicker').addEventListener('change', renderSelected);
  </script>
</body>
</html>
)rawliteral";

// ---------- Util waktu ----------
bool getTimeNowLocal(tm &t){
  return getLocalTime(&t, 1000); // timeout 1s
}

void setSystemTimeFromEpoch(uint32_t epoch){
  struct timeval tv; tv.tv_sec = epoch; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// Sinkron ke RTC dari system time (UTC epoch)
void syncRTCFromSystem(){
  time_t now = time(nullptr);
  if (now > 1700000000) { // sanity check (>= 2023)
    rtc.adjust(DateTime((uint32_t)now));
    Serial.println("RTC diset dari NTP/system time.");
  }
}

// Fallback: system time dari RTC (UTC)
bool syncSystemFromRTC(){
  if (!rtc.isrunning()) return false;
  DateTime dt = rtc.now();
  setSystemTimeFromEpoch(dt.unixtime());
  tm t;
  bool ok = getTimeNowLocal(t);
  if (ok) Serial.println("System time diset dari RTC.");
  return ok;
}

bool isNightNow(){
  tm t;
  if (!getTimeNowLocal(t)) return false; // fallback: anggap siang
  return (t.tm_hour < DAY_START_HOUR) || (t.tm_hour >= NIGHT_START_HOUR);
}

// Magnus dew point
float calcDewPoint(float T, float RH){
  if (isnan(T) || isnan(RH) || RH <= 0.0f) return NAN;
  const float a=17.62f, b=243.12f;
  float gamma = (a*T)/(b+T) + log(RH/100.0f);
  return (b*gamma)/(a-gamma);
}

void updatePressureTrend(float pres_hpa){
  unsigned long now = millis();
  if (lastTrendSample == 0 || now - lastTrendSample >= TREND_INTERVAL){
    lastTrendSample = now;
    presHist[trendIndex] = pres_hpa;
    timeHist[trendIndex] = now;
    trendIndex = (trendIndex + 1) % TREND_BUF;
    if (trendCount < TREND_BUF) trendCount++;
  }
}

float computePressureSlopeHpaPerHour(){
  if (trendCount < 2) return 0.0f;
  int newest = (trendIndex + TREND_BUF - 1) % TREND_BUF;
  int oldest = (trendIndex + TREND_BUF - trendCount) % TREND_BUF;
  float dp = presHist[newest] - presHist[oldest];
  unsigned long dt_ms = timeHist[newest] - timeHist[oldest];
  if (dt_ms == 0) return 0.0f;
  return dp / ((float)dt_ms / 3600000.0f);
}

// Threshold sederhana
const float SLP_RAINY_NIGHT   = -0.8f;
const float HUM_CLOUDY_NIGHT  = 80.0f;
const float HUM_FOG           = 90.0f;
const float SPREAD_FOG_C      = 1.0f;

// ====== Daily roll & CSV ======
String lastDateStr = ""; // "YYYY-MM-DD"

String makeDateStr(const tm &t){
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year+1900, t.tm_mon+1, t.tm_mday);
  return String(buf);
}

float dailyAvgFromAccum(const Accum &a){
  double s=0; uint16_t c=0;
  for(int h=0; h<24; h++){ if(a.cnt[h]>0){ s += a.sum[h]/(double)a.cnt[h]; c++; } }
  return c ? (float)(s/(double)c) : NAN;
}

void ensureDailyCSVHeader(){
  if (!SD.exists(DAILY_CSV)){
    File f = SD.open(DAILY_CSV, FILE_WRITE);
    if (f){
      f.println("date,avg_temp_c,avg_hum_pct,avg_press_hpa,avg_lux,dominant_status");
      f.close();
      Serial.println("Buat header daily CSV.");
    }
  }
}

String fmt2(float v){ if(isnan(v)||isinf(v)) return ""; char b[24]; dtostrf(v,0,2,b); return String(b); }

void logDailyCSV(const String& dateStr){
  ensureDailyCSVHeader();
  float avgT   = dailyAvgFromAccum(acc_temp_bme);  // suhu dari BME280
  float avgH   = dailyAvgFromAccum(acc_hum_avg);   // kelembaban rata-rata
  float avgP   = dailyAvgFromAccum(acc_pres_bme);  // tekanan
  float avgLux = dailyAvgFromAccum(acc_lux);       // lux (bisa kosong jika USE_LUX_DAY=false)
  String dom   = dominantStatus();

  File f = SD.open(DAILY_CSV, FILE_APPEND);
  if (f){
    String line = dateStr + "," + fmt2(avgT) + "," + fmt2(avgH) + "," + fmt2(avgP) + "," + fmt2(avgLux) + "," + dom;
    f.println(line);
    f.close();
    Serial.println("Tulis daily CSV: " + line);
  } else {
    Serial.println("Gagal buka daily CSV untuk tulis.");
  }
}

// Cek ganti hari: saat berganti, simpan ringkasan hari sebelumnya lalu reset akumulator & status
void handleDayRollover(const tm &t){
  String today = makeDateStr(t);
  if (lastDateStr.length()==0){
    lastDateStr = today;
    last_yday   = t.tm_yday;
    return;
  }
  if (t.tm_yday != last_yday){
    // simpan ringkasan untuk lastDateStr
    logDailyCSV(lastDateStr);
    // reset untuk hari baru
    resetAllAccums();
    resetDailyStatus();
    last_yday = t.tm_yday;
    lastDateStr = today;
  }
}

void updateHourlyAccums(const tm &t){
  int h = t.tm_hour;
  addSample(acc_temp_dht, h, temp_dht);
  addSample(acc_hum_dht,  h, hum_dht);
  addSample(acc_temp_bme, h, temp_bme);
  addSample(acc_hum_bme,  h, hum_bme);
  addSample(acc_hum_avg,  h, hum_avg);
  addSample(acc_pres_bme, h, pres_bme);
  addSample(acc_alt_bme,  h, alt_bme);
  addSample(acc_dew_point,h, dew_point);
  addSample(acc_dew_spread,h, dew_spread);
  if (USE_LUX_DAY) addSample(acc_lux, h, lux);
  addSample(acc_pres_slope, h, pres_slope_hpa_per_h);
}

// ---------- Sensor Read + Penentuan Kondisi ----------
void bacaSensor(){
  tm t; getTimeNowLocal(t);
  handleDayRollover(t);

  temp_bme = bme.readTemperature();
  hum_bme  = bme.readHumidity();
  pres_bme = bme.readPressure() / 100.0F; // hPa
  alt_bme  = bme.readAltitude(1013.25);   // m
  temp_dht = dht.readTemperature();
  hum_dht  = dht.readHumidity();
  lux      = USE_LUX_DAY ? lightMeter.readLightLevel() : NAN;

  hum_avg  = (hum_bme + hum_dht) / 2.0f;

  float t_avg = (temp_bme + temp_dht) / 2.0f;
  dew_point   = calcDewPoint(t_avg, hum_avg);
  dew_spread  = (isnan(dew_point)||isnan(t_avg)) ? NAN : (t_avg - dew_point);

  updatePressureTrend(pres_bme);
  pres_slope_hpa_per_h = computePressureSlopeHpaPerHour();

  is_night = isNightNow();

  if (is_night){
    if (!isnan(dew_spread) && dew_spread <= SPREAD_FOG_C && hum_avg >= HUM_FOG)      kondisi = "Kabut / Sangat Lembab (Malam)";
    else if (pres_slope_hpa_per_h <= SLP_RAINY_NIGHT && hum_avg >= 80.0f)            kondisi = "Kemungkinan Hujan (Malam)";
    else if (hum_avg >= HUM_CLOUDY_NIGHT)                                            kondisi = "Berawan (Malam)";
    else                                                                              kondisi = "Cerah (Malam)";
  } else {
    if (pres_bme < 1000.0f && hum_avg > 80.0f)                                       kondisi = "Kemungkinan Hujan";
    else if (USE_LUX_DAY && !isnan(lux) && lux < 5000.0f && hum_avg > 70.0f)         kondisi = "Mendung";
    else if (!USE_LUX_DAY && hum_avg > 85.0f && pres_slope_hpa_per_h <= -0.5f)       kondisi = "Mendung";
    else                                                                              kondisi = "Cerah";
  }

  // Hitung status dominan: increment maksimal 1x per menit
  if (t.tm_min != lastCountMinute){
    statusCount[categorizeStatus(kondisi)]++;
    lastCountMinute = t.tm_min;
  }

  updateHourlyAccums(t);
}

// ---------- JSON helper ----------
String jsonNumber(float v, unsigned int decimals=2){
  if (isnan(v) || isinf(v)) return "null";
  char buf[24]; dtostrf(v,0,decimals,buf); return String(buf);
}
String jsonHourLabels(){ String s="["; for(int i=0;i<24;i++){ if(i<10)s+="\"0"+String(i)+"\""; else s+="\""+String(i)+"\""; if(i!=23)s+=","; } s+="]"; return s; }

// ---------- HTTP Handlers ----------
void handleRoot(){ server.send_P(200, "text/html", INDEX_HTML); }

void handleData(){
  bacaSensor();
  String json = "{";
  json += "\"is_night\":" + String(is_night?"true":"false") + ",";
  json += "\"temp_dht\":" + jsonNumber(temp_dht) + ",";
  json += "\"hum_dht\":"  + jsonNumber(hum_dht)  + ",";
  json += "\"temp_bme\":" + jsonNumber(temp_bme) + ",";
  json += "\"hum_bme\":"  + jsonNumber(hum_bme)  + ",";
  json += "\"pres_bme\":" + jsonNumber(pres_bme) + ",";
  json += "\"alt_bme\":"  + jsonNumber(alt_bme) + ",";
  json += "\"hum_avg\":"  + jsonNumber(hum_avg) + ",";
  json += "\"dew_point\":" + jsonNumber(dew_point) + ",";
  json += "\"dew_spread\":" + jsonNumber(dew_spread) + ",";
  json += "\"pres_slope_hpa_per_h\":" + jsonNumber(pres_slope_hpa_per_h) + ",";
  json += "\"lux\":"      + (isnan(lux)?String("null"):jsonNumber(lux)) + ",";
  json += "\"kondisi\":\"" + kondisi + "\"";
  json += "}";
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma","no-cache"); server.sendHeader("Expires","0");
  server.send(200, "application/json", json);
}

void handleHistory(){
  tm t; getTimeNowLocal(t);
  String json = "{";
  json += "\"labels\":" + jsonHourLabels() + ",";
  json += "\"temp_dht\":" + jsonArrayFromAccum(acc_temp_dht) + ",";
  json += "\"hum_dht\":"  + jsonArrayFromAccum(acc_hum_dht) + ",";
  json += "\"temp_bme\":" + jsonArrayFromAccum(acc_temp_bme) + ",";
  json += "\"hum_bme\":"  + jsonArrayFromAccum(acc_hum_bme) + ",";
  json += "\"hum_avg\":"  + jsonArrayFromAccum(acc_hum_avg) + ",";
  json += "\"pres_bme\":" + jsonArrayFromAccum(acc_pres_bme) + ",";
  json += "\"alt_bme\":"  + jsonArrayFromAccum(acc_alt_bme) + ",";
  json += "\"dew_point\":" + jsonArrayFromAccum(acc_dew_point) + ",";
  json += "\"dew_spread\":" + jsonArrayFromAccum(acc_dew_spread) + ",";
  json += "\"lux\":"       + jsonArrayFromAccum(acc_lux) + ",";
  json += "\"pres_slope_hpa_per_h\":" + jsonArrayFromAccum(acc_pres_slope);
  json += "}";
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma","no-cache"); server.sendHeader("Expires","0");
  server.send(200, "application/json", json);
}

void handleDailyCSV(){
  if (!SD.begin(SD_CS)){ server.send(500,"text/plain","SD init gagal"); return; }
  if (!SD.exists(DAILY_CSV)){ server.send(404,"text/plain","CSV belum ada"); return; }
  File f = SD.open(DAILY_CSV, FILE_READ);
  if (!f){ server.send(500,"text/plain","Gagal buka CSV"); return; }
  server.streamFile(f, "text/csv");
  f.close();
}

// ---------- Setup & Loop ----------
void setup(){
  Serial.begin(115200);

  // I2C (ubah jika SDA/SCL berbeda)
  Wire.begin(21,22);

  // Sensor
  dht.begin();
  if (!bme.begin(0x76)){ Serial.println("BME280 tidak terdeteksi!"); while(1) delay(10); }
  if (USE_LUX_DAY){
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)){
      Serial.println("BH1750 tidak terdeteksi! (set USE_LUX_DAY=false jika tidak pakai)");
    }
  }

  // RTC
  if (!rtc.begin()) Serial.println("RTC DS1307 tidak terdeteksi!");
  if (!rtc.isrunning()) Serial.println("RTC belum jalan / belum di-set. Akan diset saat NTP sukses.");

  // microSD
  if (!SD.begin(SD_CS)) Serial.println("microSD gagal init! (cek wiring/CS)");
  else ensureDailyCSVHeader();

  resetAllAccums();
  resetDailyStatus();

  // WiFi + Static IP
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-weather");
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);
  WiFi.begin(ssid, password);
  Serial.printf("Menghubungkan ke WiFi \"%s\" ...\n", ssid);
  while (WiFi.status() != WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());

  // NTP + TZ
  configTime(gmtOffset_sec, daylightOffset, ntpServer);

  // Tunggu waktu; fallback RTC jika NTP tidak dapat
  tm t;
  if (getTimeNowLocal(t)){
    Serial.printf("Waktu lokal (NTP): %04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    last_yday = t.tm_yday; lastDateStr = makeDateStr(t);
    if (rtc.begin()) syncRTCFromSystem(); // set RTC dari NTP
  } else {
    Serial.println("NTP gagal, mencoba set waktu dari RTC...");
    if (syncSystemFromRTC()){
      getTimeNowLocal(t);
      last_yday = t.tm_yday; lastDateStr = makeDateStr(t);
    } else {
      Serial.println("RTC juga tidak tersedia. Waktu lokal belum valid.");
    }
  }

  // OTA
  ArduinoOTA.setHostname("esp32-weather");
  // ArduinoOTA.setPassword("ganti_password_ota"); // opsional
  ArduinoOTA.onStart([](){ Serial.println("OTA start"); });
  ArduinoOTA.onEnd([](){ Serial.println("\nOTA selesai"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total){
    Serial.printf("OTA %u%%\r", (progress*100)/total);
  });
  ArduinoOTA.onError([](ota_error_t error){
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("OTA siap. Gunakan 'Network Ports' di Arduino IDE.");

  // Web server
  server.on("/", handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/daily.csv", HTTP_GET, handleDailyCSV);
  server.onNotFound([](){ server.send(404,"text/plain","Not found"); });
  server.begin();
  Serial.println("Web server dimulai. Akses http://192.168.0.200/");
}

void loop(){
  server.handleClient();
  ArduinoOTA.handle(); // penting untuk OTA

  // Logging ke Serial setiap 5 detik
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 5000){
    lastPrint = millis();
    bacaSensor();
    Serial.println("=== Data Sensor ===");
    Serial.printf("Mode: %s\n", is_night ? "Malam" : "Siang");
    Serial.print("DHT22 Suhu: "); Serial.print(temp_dht); Serial.println(" °C");
    Serial.print("DHT22 Kelembaban: "); Serial.print(hum_dht); Serial.println(" %");
    Serial.print("Suhu (BME): "); Serial.print(temp_bme); Serial.println(" °C");
    Serial.print("Kelembaban (avg): "); Serial.print(hum_avg); Serial.println(" %");
    Serial.print("Tekanan (BME): "); Serial.print(pres_bme); Serial.println(" hPa");
    Serial.print("Tren Tekanan: "); Serial.print(pres_slope_hpa_per_h); Serial.println(" hPa/jam");
    Serial.print("BME280 Ketinggian: "); Serial.print(alt_bme); Serial.println(" m");
    Serial.print("Dew Point: "); Serial.print(dew_point); Serial.println(" °C");
    Serial.print("Spread (T-Td): "); Serial.print(dew_spread); Serial.println(" °C");
    if (USE_LUX_DAY) { Serial.print("Cahaya (Lux): "); Serial.println(lux); }
    Serial.print("Status Cuaca: "); Serial.println(kondisi);
    Serial.print("Status Dominan Sementara: "); Serial.println(dominantStatus());
    Serial.println("====================\n");
  }
}
