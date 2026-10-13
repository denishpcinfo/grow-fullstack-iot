#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>

// ===================== AHT30 =====================
Adafruit_AHTX0 aht;

// ===================== WiFi =====================
const char* SSID     = "";
const char* PASSWORD = "";

// ===================== Telegram =====================
const char* BOT_TOKEN = "";
const char* CHAT_ID   = "";

WiFiClientSecure tgClient;

// ===================== Pins =====================

const int sensorNivelAlto   = 34;
const int sensorNivelBaixo  = 39;  // VN
const int sensorUmidadeSolo = 35;

const int releBomba      = 14;
const int releVentilador = 27;
const int torneiraFechar = 26;
const int torneiraAbrir  = 25;
const int releExaustor   = 33;
const int luz1           = 32;

// Active-low flags
const bool ACT_LOW_bomba      = true;
const bool ACT_LOW_ventilador = true;
const bool ACT_LOW_valvFechar = true;
const bool ACT_LOW_valvAbrir  = true;
const bool ACT_LOW_exaustor   = true;
const bool ACT_LOW_luz        = false;

// ===================== Limites =====================
const float temperaturaMaxima = 27.0;
const float umidadeLimite     = 85.0;

// Luz 18/6 ou 12/12
unsigned long tempoLuzOn  = 18UL * 60UL * 60UL * 1000UL;
unsigned long tempoLuzOff =  6UL * 60UL * 60UL * 1000UL;

bool cicloLuz12x12 = false;
bool cicloLuz12x12Pendente = false;

// ================== Config Inicial ======================
const unsigned long VALVULA_BLOQUEIO_BOOT_MS = 5000UL; // 5s

// ===================== SOLO (calibração + leitura) =====================
int SOLO_SECO    = 2700;
int SOLO_MOLHADO = 900;
bool soloCalibrado = false;
const int SOLO_SAMPLES = 20;

// ===================== Irrigação automática por solo =====================
const bool IRRIG_SOLO_AUTO = true;
int  SOLO_LIMIAR_LIGA_PCT    = 60;
const unsigned long MIN_INTERVALO_IRRIG_SOLO_MS = 2UL * 24UL * 60UL * 60UL * 1000UL;
const unsigned long IRRIG_BLOQUEIO_BOOT_MS = 1UL * 60UL * 1000UL; // 1 min
unsigned long bootAtMs = 0;

// Tempo máximo da bomba, ajustável pelo painel (padrão: 14s)
unsigned long IRRIG_MAX_MS = 14000UL;

// ===================== Nível (debounce) =====================
unsigned long lastNivelCheckMs = 0;
const unsigned long NIVEL_DEBOUNCE_MS = 1500;

// ===================== Estados =====================
bool irrigando = false;
unsigned long inicioIrrigacaoMs = 0;

unsigned long tempoUltimaTrocaLuz = 0;
bool luzLigada = true;

unsigned long ultimaIrrigacaoPorSoloMs = 0;

// ===================== Telegram =====================
bool wifiJaAvisouTelegram = false;
bool wifiTemIP = false;
bool telegramPendenteConexao = false;
unsigned long wifiGotIpAtMs = 0;

// ===================== envio json =====================
const int UART_JSON_TX = 17;
const uint32_t UART_JSON_INTERVALO_MS = 60000UL;
const size_t STATUS_JSON_CAPACIDADE = 768;

// ===================== Web =====================
WebServer uiServer(81);

// ===================== Leituras cacheadas =====================
float  lastTemp = NAN;
float  lastHum  = NAN;
int    lastNivel = 0;
int    lastSoloRaw = 0;
int    lastSoloPct = 0;

unsigned long lastSensorsAt = 0;

// ===================== Helpers relé =====================
inline void setOn(int pin, bool activeLow)  { digitalWrite(pin, activeLow ? LOW : HIGH); }
inline void setOff(int pin, bool activeLow) { digitalWrite(pin, activeLow ? HIGH : LOW); }

inline bool isOn(int pin, bool activeLow) {
  int v = digitalRead(pin);
  return activeLow ? (v == LOW) : (v == HIGH);
}

// ===================== Ventilador e exaustor =====================
void controleClima() {
  static unsigned long lastRun = 0;
  unsigned long now = millis();
  const unsigned long COOLDOWN = 300000UL; // 5 min

  if (lastRun == 0) lastRun = now - COOLDOWN;
  if (now - lastRun < COOLDOWN) return;
  lastRun = now;

  float t = lastTemp;
  float h = lastHum;

  if (isnan(t) || isnan(h)) return;

  bool precisaExaustao = (t >= temperaturaMaxima) || (h > umidadeLimite);

  if (precisaExaustao) {
    setOn(releVentilador, ACT_LOW_ventilador);
    setOn(releExaustor,   ACT_LOW_exaustor);
  } else {
    setOff(releVentilador, ACT_LOW_ventilador);
    setOff(releExaustor,   ACT_LOW_exaustor);
  }
}

// ===================== Solo =====================
int readSoilRawAvg() {
  static int samples[SOLO_SAMPLES];
  static int idx = 0;
  static bool filled = false;
  static long sum = 0;

  int v = analogRead(sensorUmidadeSolo);

  if (v <= 0 || v >= 4095) {
    return -1;
  }

  sum -= samples[idx];
  samples[idx] = v;
  sum += v;

  idx++;
  if (idx >= SOLO_SAMPLES) {
    idx = 0;
    filled = true;
  }

  if (!filled) return -1;

  return sum / SOLO_SAMPLES;
}

int rawToSoilPercent(int raw) {
    if (SOLO_SECO == SOLO_MOLHADO) {
        return 0;
    }

    int minimo = min(SOLO_SECO, SOLO_MOLHADO);
    int maximo = max(SOLO_SECO, SOLO_MOLHADO);

    raw = constrain(raw, minimo, maximo);

    int pct = map(
        raw,
        SOLO_SECO,
        SOLO_MOLHADO,
        0,
        100
    );

    return constrain(pct, 0, 100);
}

// ===================== Telegram =====================
bool sendTelegramMessage(const String& text) {
  tgClient.setInsecure();
  tgClient.setTimeout(5000);

  if (!tgClient.connect("api.telegram.org", 443)) return false;

  String msg = text;
  msg.replace(" ", "%20");

  String url = "/bot" + String(BOT_TOKEN)
             + "/sendMessage?chat_id=" + String(CHAT_ID)
             + "&text=" + msg;

  tgClient.print(
    "GET " + url + " HTTP/1.1\r\n"
    "Host: api.telegram.org\r\n"
    "Connection: close\r\n\r\n"
  );

  tgClient.stop();
  return true;
}

// ===================== WiFi events =====================
void WiFiEventHandler(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println(F("[WiFi] STA_START"));
      wifiTemIP = false;
      break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println(F("[WiFi] Conectado ao AP (sem IP ainda)"));
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print(F("[WiFi] IP obtido: "));
      Serial.println(WiFi.localIP());

      wifiTemIP = true;
      wifiGotIpAtMs = millis();

      if (!wifiJaAvisouTelegram) {
        telegramPendenteConexao = true;
      }
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println(F("[WiFi] Desconectado do AP, tentando reconectar..."));
      wifiTemIP = false;
      wifiJaAvisouTelegram = false;
      telegramPendenteConexao = false;
      break;

    default:
      break;
  }
}

void tentarReconectarWiFi() {
  static unsigned long ultimaTentativaWiFi = 0;
  const unsigned long INTERVALO_RECONEXAO_WIFI_MS = 10000UL;

  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - ultimaTentativaWiFi < INTERVALO_RECONEXAO_WIFI_MS) return;

  ultimaTentativaWiFi = now;

  Serial.println(F("[WiFi] Tentando reconectar..."));
  WiFi.disconnect(false, false);
  WiFi.begin(SSID, PASSWORD);
}

// ===================== Persistência (SÓ LUZ - CONGELADO) =====================
Preferences prefs;

struct LightSnap {
  uint32_t version = 2;
  bool luzLigada = true;
  uint32_t restanteMs = 0;
};

LightSnap lightSnap;

void applyLightState(bool ligar) {
  if (ligar) setOn(luz1, ACT_LOW_luz);
  else       setOff(luz1, ACT_LOW_luz);
  luzLigada = ligar;
}

void saveLightSnap() {
  LightSnap atual;

  if (prefs.getBytesLength("lsnap") == sizeof(LightSnap)) {
    prefs.getBytes("lsnap", &atual, sizeof(LightSnap));

    if (atual.version == lightSnap.version &&
        atual.luzLigada == lightSnap.luzLigada &&
        atual.restanteMs == lightSnap.restanteMs) {
      return;
    }
  }

  prefs.putBytes("lsnap", &lightSnap, sizeof(lightSnap));
}

void saveRemainingLight() {
  unsigned long now = millis();
  unsigned long elapsed = now - tempoUltimaTrocaLuz;
  unsigned long period  = luzLigada ? tempoLuzOn : tempoLuzOff;
  unsigned long nextMs  = (elapsed < period) ? (period - elapsed) : 0;

  lightSnap.luzLigada  = luzLigada;
  lightSnap.restanteMs = (uint32_t)nextMs;
  saveLightSnap();
}

void restoreLightFrozenFromSnap() {
  applyLightState(lightSnap.luzLigada);

  unsigned long period = luzLigada ? tempoLuzOn : tempoLuzOff;
  unsigned long restante = lightSnap.restanteMs;

  if (restante == 0 || restante > period) restante = period;

  tempoUltimaTrocaLuz = millis() - (period - restante);
}

void trocaLuzPersist(bool ligar) {
  applyLightState(ligar);
  tempoUltimaTrocaLuz = millis();

  lightSnap.luzLigada  = luzLigada;
  lightSnap.restanteMs = (uint32_t)(luzLigada ? tempoLuzOn : tempoLuzOff);
  saveLightSnap();
}

// ===================== Leitura de sensores =====================
void updateSensors() {
  unsigned long now = millis();
  if (now - lastSensorsAt < 1000) return;
  lastSensorsAt = now;


// aqui comentei para o mock temp
  // sensors_event_t humidity, temp;
  // aht.getEvent(&humidity, &temp);

  // float t = temp.temperature;
  // float h = humidity.relative_humidity;

  // lastTemp = isnan(t) ? lastTemp : t;
  // lastHum  = isnan(h) ? lastHum  : h;


  // MOCK: valores simulados
float t = 25.0 + (random(-20, 20) / 10.0);  // 23.0 ~ 27.0 °C
float h = 70.0 + (random(-100, 100) / 10.0); // 60 ~ 80 %

lastTemp = t;
lastHum  = h;


// mater daqui pra abaixo igual


  lastNivel = digitalRead(sensorNivelAlto);

  int raw = readSoilRawAvg();
  if (raw >= 0) {
    lastSoloRaw = raw;
    if (soloCalibrado && SOLO_SECO > 0 && SOLO_MOLHADO > 0) {
      lastSoloPct = rawToSoilPercent(lastSoloRaw);
    } else {
      lastSoloPct = 0;
    }
  }
}

// ===================== UI =====================
const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="pt-br">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Grow Monitor</title>

  <style>
    :root{
      --bg0:#050914;
      --bg1:#071024;
      --card:rgba(14, 24, 54, .55);
      --card2:rgba(10, 18, 40, .55);
      --border:rgba(255,255,255,.08);
      --text:#EAF0FF;
      --muted:rgba(234,240,255,.72);
      --shadow: 0 20px 60px rgba(0,0,0,.45);
      --radius: 18px;

      --ok:#33d17a;
      --warn:#ffb86c;
      --bad:#ff4d6d;
      --info:#7aa2ff;
    }

    *{ box-sizing:border-box; }
    body{
      margin:0;
      font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial;
      color:var(--text);
      background:
        radial-gradient(1200px 600px at 20% 10%, rgba(122,162,255,.22), transparent 60%),
        radial-gradient(900px 500px at 85% 20%, rgba(51,209,122,.18), transparent 55%),
        radial-gradient(900px 500px at 60% 85%, rgba(255,77,109,.10), transparent 55%),
        linear-gradient(180deg, var(--bg0), var(--bg1));
      min-height:100vh;
    }

    .wrap{
      max-width: 1180px;
      margin: 0 auto;
      padding: 22px 18px 40px;
    }

    header{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:12px;
      margin-bottom: 18px;
    }

    .brand{
      display:flex;
      align-items:center;
      gap:10px;
      font-weight: 700;
      font-size: 20px;
    }

    .brand .logo{
      width:36px; height:36px;
      border-radius:12px;
      display:grid; place-items:center;
      background: linear-gradient(135deg, rgba(122,162,255,.35), rgba(51,209,122,.25));
      border:1px solid var(--border);
    }

    .topchips{
      display:flex; gap:10px; flex-wrap:wrap;
      align-items:center; justify-content:flex-end;
    }

    .chip{
      display:flex; align-items:center; gap:8px;
      padding:8px 12px;
      border-radius:999px;
      background: rgba(255,255,255,.08);
      border:1px solid rgba(255,255,255,.10);
      font-size:13px;
      color:var(--muted);
    }

    .dot{
      width:10px; height:10px; border-radius:999px;
      background: var(--info);
    }

    .grid{
      display:grid;
      gap:14px;
      grid-template-columns:1.2fr 1fr 1fr;
      align-items:stretch;
    }

    @media (max-width:980px){
      .grid{ grid-template-columns:1fr; }
    }

    .card{
      background: linear-gradient(180deg, var(--card), var(--card2));
      border:1px solid var(--border);
      border-radius:var(--radius);
      box-shadow:var(--shadow);
      overflow:hidden;
      position:relative;
    }

    .card h2{
      margin:0;
      padding:16px 16px 10px;
      font-size:16px;
    }

    .sub{
      padding:0 16px 12px;
      color:var(--muted);
      font-size:12px;
    }

    .rows{ padding:6px 14px 14px; }

    .row{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:12px;
      padding:10px 10px;
      border-radius:12px;
    }

    .label{
      display:flex; align-items:center; gap:10px;
      font-size:14px; font-weight:600;
    }

    .ico{
      width:28px; height:28px;
      border-radius:10px;
      display:grid; place-items:center;
      background:rgba(255,255,255,.06);
      border:1px solid rgba(255,255,255,.08);
    }

    .value{
      font-size:14px;
      font-weight:700;
      color:rgba(234,240,255,.92);
    }

    .pill{
      display:inline-flex;
      align-items:center;
      gap:8px;
      padding:6px 12px;
      border-radius:999px;
      border:1px solid rgba(255,255,255,.10);
      background:rgba(0,0,0,.25);
      font-weight:800;
      font-size:12px;
      text-transform:uppercase;
    }

    .pill .dot2{
      width:10px;height:10px;border-radius:999px;
      background:var(--warn);
    }

    .pill.ok .dot2{ background:var(--ok); }
    .pill.bad .dot2{ background:var(--bad); }

    .progress{
      width:180px;
      height:10px;
      border-radius:999px;
      background:rgba(255,255,255,.08);
      border:1px solid rgba(255,255,255,.08);
      overflow:hidden;
    }

    .bar{
      height:100%;
      width:0%;
      background: linear-gradient(90deg, rgba(255,77,109,.9), rgba(255,184,108,.9), rgba(51,209,122,.9));
      border-radius:999px;
      transition: width .35s ease;
    }

    .muted{
      color:var(--muted);
      font-weight:600;
      font-size:12px;
    }

    .right{
      display:flex;
      align-items:center;
      gap:10px;
    }

    .mono{
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size:12px;
      color:rgba(234,240,255,.75);
    }

    .btn{
      appearance:none;
      border:1px solid rgba(255,255,255,.12);
      background:rgba(0,0,0,.22);
      color:var(--text);
      padding:10px 12px;
      border-radius:12px;
      font-weight:800;
      cursor:pointer;
    }
  </style>
</head>

<body>
  <div class="wrap">
    <header>
      <div class="brand">
        <div class="logo">🌱</div>
        <div>Grow Monitor</div>
      </div>
      <div class="topchips">
        <div class="chip"><span class="dot" id="netDot"></span><span id="netText">Conectando…</span></div>
      </div>
    </header>

    <div class="grid">
      <section class="card">
        <h2>Sensores</h2>
        <div class="sub"></div>
        <div class="rows">
          <div class="row">
            <div class="label"><span class="ico">🌡️</span>Temperatura</div>
            <div class="value" id="t">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">💧</span>Umidade do ar</div>
            <div class="value" id="h">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🫗</span>Nível d’água</div>
            <div class="value" id="n">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🌿</span>Umidade do solo</div>
            <div class="right">
              <div class="progress" title="Solo (%)">
                <div class="bar" id="soilBar"></div>
              </div>
              <div class="value" id="s">--</div>
            </div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">📟</span>Sensor de humidade do solo</div>
            <div class="value mono" id="sr">--</div>
          </div>
        </div>
      </section>

      <section class="card">
        <h2>Relés</h2>
        <div class="sub">Status em tempo real</div>
        <div class="rows">
          <div class="row">
            <div class="label"><span class="ico">💡</span>Ciclo da luz</div>
            <div class="muted" id="cicloLuz">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">💡</span>Luz</div>
            <div class="pill" id="rluz"><span class="dot2"></span><span>--</span></div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🚿</span>Bomba d’água</div>
            <div class="pill" id="rbom"><span class="dot2"></span><span>--</span></div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🌀</span>Ventilador</div>
            <div class="pill" id="rvent"><span class="dot2"></span><span>--</span></div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🚰</span>Torneira</div>
            <div class="pill" id="rvf"><span class="dot2"></span><span>--</span></div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🌬️</span>Exaustor</div>
            <div class="pill" id="rexa"><span class="dot2"></span><span>--</span></div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">⏱️</span>Irrigando agora?</div>
            <div class="pill" id="ir"><span class="dot2"></span><span>--</span></div>
          </div>
        </div>
      </section>

      <section class="card">
        <h2>Ciclos </h2>
        <div class="sub"></div>
        <div class="rows">
          <div class="row">
            <div class="label"><span class="ico">🕘</span>Próx. troca da luz</div>
            <div class="value" id="nextL">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🔁</span>Status</div>
            <div class="muted" id="lightState">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🚿</span>Próx. irrigação</div>
            <div class="value" id="nextIrrig">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">🏜️</span>Calibração SECO</div>
            <div class="value mono" id="calibSeco">--</div>
          </div>

          <div class="row">
            <div class="label"><span class="ico">💧</span>Calibração MOLHADO</div>
            <div class="value mono" id="calibMolhado">--</div>
          </div>

        </div>
      </section>
    </div>

    <br>

    <div class="grid">

      <div class="row">
        <button class="chip" onclick="toggleCicloLuz()" style="cursor:pointer; color: var(--text);">
          💡 Alternar ciclo luz 18/6 ↔ 12/12
        </button>
      </div>

      <div class="row">
        <button class="chip" onclick="abrirModalCalib()" style="cursor:pointer; color: var(--text);">
          🛠️ Editar SECO / MOLHADO
        </button>
      </div>

      <div class="row">
        <button class="chip" onclick="abrirModalSoloLiga()" style="cursor:pointer; color: var(--text);">
          ⚙️ Alterar próx. irrigação do solo
        </button>
      </div>

      <div class="row">
        <button class="chip" onclick="alterarTempoIrrigacao()" style="cursor:pointer; color: var(--text);">
          ⏱️ Alterar tempo de irrigação (<span id="tempoIrrigacao">--</span> s)
        </button>
      </div>
    </div>
  </div>

  <script>
    function pad(ms){
      const s = Math.max(0, Math.floor(ms/1000));
      const h = Math.floor(s/3600);
      const m = Math.floor((s%3600)/60);
      return `${h}h ${m}m`;
    }

    function setPill(id, on){
      const el = document.getElementById(id);
      el.className = 'pill ' + (on ? 'ok' : 'bad');
      el.querySelector('span:last-child').textContent = on ? 'ON' : 'OFF';
    }

    function setNet(ok){
      const dot = document.getElementById('netDot');
      const text = document.getElementById('netText');
      dot.style.background = ok ? 'var(--ok)' : 'var(--warn)';
      dot.style.boxShadow = ok ? '0 0 0 4px rgba(51,209,122,.14)' : '0 0 0 4px rgba(255,184,108,.15)';
      text.textContent = ok ? 'Online' : 'Reconectando…';
    }

    async function load(){
      try{
        const r = await fetch('/status.json', {cache:'no-store'});
        const j = await r.json();
        setNet(true);

        document.getElementById('cicloLuz').textContent = j.cicloLuz === '12/12' ? '12h ligada / 12h desligada' : '18h ligada / 6h desligada';
        document.getElementById('t').textContent = (j.temp?.toFixed?.(1) ?? '--') + ' °C';
        document.getElementById('h').textContent = (j.umid?.toFixed?.(0) ?? '--') + ' %';
        document.getElementById('n').textContent = j.nivel ? 'BAIXO' : 'CHEIO';

        const soilPct = (j.soloPct ?? null);
        document.getElementById('s').textContent = soilPct === null ? '--' : (soilPct + ' %');
        document.getElementById('sr').textContent = (j.soloRaw ?? '--');

        const bar = document.getElementById('soilBar');
        if(soilPct === null){ bar.style.width = '0%'; }
        else{ bar.style.width = Math.max(0, Math.min(100, soilPct)) + '%'; }

        setPill('rluz',  !!j.relays?.luz);
        setPill('rbom',  !!j.relays?.bomba);
        setPill('rvent', !!j.relays?.vent);
        setPill('rvf',   !!j.relays?.valvula);
        setPill('rexa',  !!j.relays?.exaustor);
        setPill('ir',    !!j.irrigando);

        document.getElementById('nextL').textContent = pad(j.next?.lightMs ?? 0);
        document.getElementById('lightState').textContent = j.relays?.luz ? 'Luz ligada (fase ON)' : 'Luz desligada (fase OFF)';
        
        document.getElementById('nextIrrig').textContent = `solo ≤ ${j.soloLiga ?? 80}%`;
        document.getElementById('tempoIrrigacao').textContent = j.irrigacaoSegundos ?? '--';
        document.getElementById('calibSeco').textContent = j.calib?.seco ?? '--';
        document.getElementById('calibMolhado').textContent = j.calib?.molhado ?? '--';

      }catch(e){
        setNet(false);
      }
    }

    async function alterarTempoIrrigacao() {
      try {
        const atual = await fetch('/status.json', { cache: 'no-store' });
        if (!atual.ok) throw new Error('Erro ao carregar tempo atual');
        const j = await atual.json();

        const valor = prompt('Tempo de irrigação em segundos (1 a 300):', j.irrigacaoSegundos);
        if (valor === null) return;

        const segundos = Number(valor);
        if (!Number.isInteger(segundos) || segundos < 1 || segundos > 300) {
          alert('Informe um número inteiro entre 1 e 300 segundos');
          return;
        }

        const r = await fetch('/setTempoIrrigacao', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `segundos=${encodeURIComponent(segundos)}`
        });
        const txt = await r.text();
        if (!r.ok) {
          alert(txt);
          return;
        }

        await load();
        alert(txt);
      } catch (e) {
        alert('Erro ao alterar tempo de irrigação');
      }
    }

    async function abrirModalSoloLiga() {
      try {
        const r = await fetch('/getSoloLiga', { cache: 'no-store' });
        const j = await r.json();
        document.getElementById('novoSoloLiga').value = j.soloLiga ?? 80;
        document.getElementById('soloLigaModal').style.display = 'flex';
      } catch (e) {
        alert('Erro ao carregar valor atual');
      }
    }

    function fecharModalSoloLiga() {
      document.getElementById('soloLigaModal').style.display = 'none';
    }

    async function salvarSoloLiga() {
      const valor = document.getElementById('novoSoloLiga').value;

      if (!valor || Number(valor) < 1 || Number(valor) > 100) {
        alert('Informe um valor entre 1 e 100');
        return;
      }

      try {
        const r = await fetch(`/setSoloLiga?valor=${encodeURIComponent(valor)}`);
        const txt = await r.text();

        if (!r.ok) {
          alert(txt);
          return;
        }

        fecharModalSoloLiga();
        await load();
        alert(txt);
      } catch (e) {
        alert('Erro ao salvar valor');
      }
    }

    async function abrirModalCalib() {
      try {
        const r = await fetch('/getCalibValores', { cache: 'no-store' });
        const j = await r.json();

        document.getElementById('novoSeco').value = j.seco ?? '';
        document.getElementById('novoMolhado').value = j.molhado ?? '';
        document.getElementById('calibModal').style.display = 'flex';
      } catch (e) {
        alert('Erro ao carregar valores de calibracao');
      }
    }

    function fecharModalCalib() {
      document.getElementById('calibModal').style.display = 'none';
    }

    async function salvarCalibManual() {
      const seco = document.getElementById('novoSeco').value;
      const molhado = document.getElementById('novoMolhado').value;

      if (!seco || !molhado) {
        alert('Informe os dois valores');
        return;
      }

      try {
        const r = await fetch(`/setCalibValores?seco=${encodeURIComponent(seco)}&molhado=${encodeURIComponent(molhado)}`);
        const txt = await r.text();

        if (!r.ok) {
          alert(txt);
          return;
        }

        fecharModalCalib();
        await load();
        alert(txt);
      } catch (e) {
        alert('Erro ao salvar calibracao');
      }
    }

    async function toggleCicloLuz() {

      const r = await fetch('/status.json', { cache:'no-store' });
      const j = await r.json();

      const proximoCiclo =
          j.cicloLuz === '18/6'
          ? '12/12'
          : '18/6';

      const confirmar = confirm(
          'Tem certeza que deseja alterar o ciclo da luz?\n\n' +
          'Novo ciclo: ' + proximoCiclo + '\n\n' +
          'A mudança será aplicada na próxima troca da luz.'
      );

      if (!confirmar) {
          alert('Alteração cancelada');
          return;
      }

      try {
          const resp = await fetch('/toggleCicloLuz');
          const txt = await resp.text();

          alert(txt);
          await load();

      } catch(e) {
          alert('Erro ao alterar ciclo');
      }
    }

    load();
    setInterval(load, 10000);
  </script>

  <div id="soloLigaModal" style="display:none; position:fixed; inset:0; background:rgba(0,0,0,.55); z-index:9999; align-items:center; justify-content:center;">
    <div style="width:min(92vw,420px); background:#0b1326; border:1px solid rgba(255,255,255,.12); border-radius:16px; padding:18px; color:#EAF0FF; box-shadow:0 20px 60px rgba(0,0,0,.45);">
      <h3 style="margin-top:0;">Deseja alterar o valor da próx. irrigação do solo?</h3>
      <p style="color:rgba(234,240,255,.72); font-size:13px;">
        Informe o novo percentual para irrigação automática.
      </p>

      <input id="novoSoloLiga"
            type="number"
            min="1"
            max="100"
            style="width:100%; padding:10px 12px; border-radius:10px; border:1px solid rgba(255,255,255,.12); background:rgba(255,255,255,.05); color:#EAF0FF; margin:10px 0 14px 0;" />

      <div style="display:flex; gap:10px; justify-content:flex-end;">
        <button class="btn" onclick="fecharModalSoloLiga()">Cancelar</button>
        <button class="btn primary" onclick="salvarSoloLiga()">Salvar</button>
      </div>
    </div>
  </div>

  <div id="calibModal" style="display:none; position:fixed; inset:0; background:rgba(0,0,0,.55); z-index:9999; align-items:center; justify-content:center;">
    <div style="width:min(92vw,420px); background:#0b1326; border:1px solid rgba(255,255,255,.12); border-radius:16px; padding:18px; color:#EAF0FF; box-shadow:0 20px 60px rgba(0,0,0,.45);">
      <h3 style="margin-top:0;">Editar calibração do solo</h3>
      <p style="color:rgba(234,240,255,.72); font-size:13px;">
        Informe os valores manuais de SECO e MOLHADO.
      </p>

      <div class="label"> SECO</div>
      <input id="novoSeco"
            type="number"
            min="1"
            max="4094"
            placeholder="Valor SECO"
            style="width:100%; padding:10px 12px; border-radius:10px; border:1px solid rgba(255,255,255,.12); background:rgba(255,255,255,.05); color:#EAF0FF; margin:10px 0 10px 0;" />

      <div class="label"> MOLHADO</div>
      <input id="novoMolhado"
            type="number"
            min="1"
            max="4094"
            placeholder="Valor MOLHADO"
            style="width:100%; padding:10px 12px; border-radius:10px; border:1px solid rgba(255,255,255,.12); background:rgba(255,255,255,.05); color:#EAF0FF; margin:0 0 14px 0;" />

      <div style="display:flex; gap:10px; justify-content:flex-end;">
        <button class="btn" onclick="fecharModalCalib()">Cancelar</button>
        <button class="btn primary" onclick="salvarCalibManual()">Salvar</button>
      </div>
    </div>
  </div>
</body>
</html>
)HTML";

// ===================== Handlers =====================
void handleToggleCicloLuz() {
  cicloLuz12x12Pendente = !cicloLuz12x12Pendente;

  if (prefs.getBool("luz12x12Pendente", cicloLuz12x12) != cicloLuz12x12Pendente) {
    prefs.putBool("luz12x12Pendente", cicloLuz12x12Pendente);
  }

  saveRemainingLight();

  String atual = cicloLuz12x12 ? "12/12" : "18/6";
  String pendente = cicloLuz12x12Pendente ? "12/12" : "18/6";

  uiServer.send(
  200,
  "text/plain",
  "Ciclo atual: " + atual +
  "\nPróximo ciclo: " + pendente +
  "\nMudará na próxima troca."
  );
}

void aplicarCicloLuzPendente() {
  if (cicloLuz12x12 == cicloLuz12x12Pendente) return;

  cicloLuz12x12 = cicloLuz12x12Pendente;

  if (cicloLuz12x12) {
    tempoLuzOn  = 12UL * 60UL * 60UL * 1000UL;
    tempoLuzOff = 12UL * 60UL * 60UL * 1000UL;
  } else {
    tempoLuzOn  = 18UL * 60UL * 60UL * 1000UL;
    tempoLuzOff = 6UL * 60UL * 60UL * 1000UL;
  }

  if (prefs.getBool("luz12x12", !cicloLuz12x12) != cicloLuz12x12) {
    prefs.putBool("luz12x12", cicloLuz12x12);
  }

  if (prefs.getBool("luz12x12Pendente", !cicloLuz12x12Pendente) != cicloLuz12x12Pendente) {
    prefs.putBool("luz12x12Pendente", cicloLuz12x12Pendente);
  }

  saveRemainingLight();
}

void handleSetTempoIrrigacao() {
  if (!uiServer.hasArg("segundos")) {
    uiServer.send(400, "text/plain", "Tempo nao informado");
    return;
  }

  String valor = uiServer.arg("segundos");
  valor.trim();
  int segundos = valor.toInt();
  if (segundos < 1 || segundos > 300 || valor != String(segundos)) {
    uiServer.send(400, "text/plain", "Use um numero inteiro entre 1 e 300 segundos.");
    return;
  }

  if (irrigando) {
    uiServer.send(409, "text/plain", "Aguarde a irrigacao terminar para alterar o tempo.");
    return;
  }

  unsigned long novoTempoMs = (unsigned long)segundos * 1000UL;
  if (novoTempoMs != IRRIG_MAX_MS &&
      prefs.putInt("irrigTempoSeg", segundos) != sizeof(int32_t)) {
    uiServer.send(500, "text/plain", "Erro ao salvar tempo de irrigacao.");
    return;
  }

  IRRIG_MAX_MS = novoTempoMs;
  uiServer.send(200, "text/plain", "Tempo de irrigacao salvo com sucesso.");
}


void handleGetSoloLiga() {
  String json;
  json.reserve(40);
  json = "{";
  json += "\"soloLiga\":" + String(SOLO_LIMIAR_LIGA_PCT);
  json += "}";
  uiServer.send(200, "application/json", json);
}

void handleSetSoloLiga() {
  if (!uiServer.hasArg("valor")) {
    uiServer.send(400, "text/plain", "Valor nao informado");
    return;
  }

  int novoValor = uiServer.arg("valor").toInt();

  if (novoValor < 1 || novoValor > 100) {
    uiServer.send(400, "text/plain", "Valor invalido. Use 1 a 100.");
    return;
  }

  SOLO_LIMIAR_LIGA_PCT = novoValor;
  prefs.putInt("soloLigaPct", SOLO_LIMIAR_LIGA_PCT);

  uiServer.send(200, "text/plain", "Valor salvo com sucesso");
}

void handleGetCalibValores() {
  String json;
  json.reserve(60);
  json = "{";
  json += "\"seco\":" + String(SOLO_SECO) + ",";
  json += "\"molhado\":" + String(SOLO_MOLHADO);
  json += "}";
  uiServer.send(200, "application/json", json);
}

void handleSetCalibValores() {
  if (!uiServer.hasArg("seco") || !uiServer.hasArg("molhado")) {
    uiServer.send(
      400,
      "text/plain",
      "Parametros seco e molhado sao obrigatorios"
    );
    return;
  }

  int novoSeco = uiServer.arg("seco").toInt();
  int novoMolhado = uiServer.arg("molhado").toInt();

  if (
    novoSeco <= 0 || novoSeco >= 4095 ||
    novoMolhado <= 0 || novoMolhado >= 4095
  ) {
    uiServer.send(
      400,
      "text/plain",
      "Valores invalidos. Use faixa entre 1 e 4094."
    );
    return;
  }

  if (novoSeco == novoMolhado) {
    uiServer.send(
      400,
      "text/plain",
      "SECO e MOLHADO nao podem ser iguais."
    );
    return;
  }

  // Salva exatamente como o usuario informou
  SOLO_SECO = novoSeco;
  SOLO_MOLHADO = novoMolhado;
  soloCalibrado = true;

  prefs.putInt("soloSeco", SOLO_SECO);
  prefs.putInt("soloMolhado", SOLO_MOLHADO);

  // Segurança
  irrigando = false;
  setOff(releBomba, ACT_LOW_bomba);

  uiServer.send(
    200,
    "text/plain",
    "Valores de calibracao salvos com sucesso"
  );
}

void handleRoot() {
  uiServer.send_P(200, "text/html", PAGE);
}

size_t montarStatusJson(char* destino, size_t capacidade) {
  if (capacidade == 0) return 0;

  const uint32_t agora = millis();
  const uint32_t decorrido = agora - (uint32_t)tempoUltimaTrocaLuz;
  const uint32_t periodo = luzLigada ? tempoLuzOn : tempoLuzOff;
  const uint32_t proximaTroca = decorrido < periodo ? periodo - decorrido : 0;

  char temperatura[48] = "null";
  char umidade[48] = "null";

  if (isfinite(lastTemp)) {
    snprintf(temperatura, sizeof(temperatura), "%.1f", (double)lastTemp);
  }
  if (isfinite(lastHum)) {
    snprintf(umidade, sizeof(umidade), "%.0f", (double)lastHum);
  }

  const int n = snprintf(
    destino, capacidade,
    "{\"temp\":%s,\"umid\":%s,\"nivel\":%d,"
    "\"soloRaw\":%d,\"soloPct\":%d,\"irrigando\":%s,"
    "\"irrigacaoSegundos\":%lu,"
    "\"relays\":{\"luz\":%s,\"bomba\":%s,\"vent\":%s,"
    "\"valvula\":%s,\"exaustor\":%s},"
    "\"next\":{\"lightMs\":%lu},"
    "\"calib\":{\"seco\":%d,\"molhado\":%d},"
    "\"soloLiga\":%d,\"cicloLuz\":\"%s\"}",
    temperatura,
    umidade,
    lastNivel,
    lastSoloRaw,
    lastSoloPct,
    irrigando ? "true" : "false",
    (unsigned long)(IRRIG_MAX_MS / 1000UL),
    isOn(luz1, ACT_LOW_luz) ? "true" : "false",
    isOn(releBomba, ACT_LOW_bomba) ? "true" : "false",
    isOn(releVentilador, ACT_LOW_ventilador) ? "true" : "false",
    isOn(torneiraFechar, ACT_LOW_valvFechar) ? "false" : "true",
    isOn(releExaustor, ACT_LOW_exaustor) ? "true" : "false",
    (unsigned long)proximaTroca,
    SOLO_SECO,
    SOLO_MOLHADO,
    SOLO_LIMIAR_LIGA_PCT,
    cicloLuz12x12 ? "12/12" : "18/6"
  );

  if (n < 0 || (size_t)n >= capacidade) {
    destino[0] = '\0';
    return 0;
  }

  return (size_t)n;
}

void enviarStatusUART() {
  static char buffer[STATUS_JSON_CAPACIDADE];
  static size_t tamanho = 0;
  static size_t enviados = 0;
  static uint32_t ultimoEnvioMs = 0;

  if (enviados >= tamanho) {
    const uint32_t agora = millis();
    if ((uint32_t)(agora - ultimoEnvioMs) < UART_JSON_INTERVALO_MS) return;
    ultimoEnvioMs = agora;

    enviados = 0;
    tamanho = montarStatusJson(buffer, sizeof(buffer) - 1);
    if (tamanho == 0) return;

    buffer[tamanho++] = '\n';  // Delimita uma mensagem completa.
  }

  const int livre = Serial2.availableForWrite();
  if (livre <= 0) return;

  size_t lote = tamanho - enviados;
  if (lote > 32) lote = 32;
  if (lote > (size_t)livre) lote = (size_t)livre;

  enviados += Serial2.write((const uint8_t*)buffer + enviados, lote);

  Serial.println("enviados");
  Serial.println(enviados);
}

void handleStatus() {
  unsigned long now = millis();
  unsigned long elapsedLight = now - tempoUltimaTrocaLuz;
  unsigned long period = luzLigada ? tempoLuzOn : tempoLuzOff;
  unsigned long nextLight = (elapsedLight < period) ? (period - elapsedLight) : 0;

  String json;
  json.reserve(700);
  json = "{";
  json += "\"temp\":" + (isnan(lastTemp) ? String("null") : String(lastTemp,1)) + ",";
  json += "\"umid\":" + (isnan(lastHum)  ? String("null") : String(lastHum,0)) + ",";
  json += "\"nivel\":" + String(lastNivel) + ",";
  json += "\"soloRaw\":" + String(lastSoloRaw) + ",";
  json += "\"soloPct\":" + String(lastSoloPct) + ",";
  json += "\"irrigando\":" + String(irrigando ? "true":"false") + ",";
  json += "\"irrigacaoSegundos\":" + String(IRRIG_MAX_MS / 1000UL) + ",";

  json += "\"relays\":{";
  json += "\"luz\":"      + String(isOn(luz1,           ACT_LOW_luz)        ? "true":"false") + ",";
  json += "\"bomba\":"    + String(isOn(releBomba,      ACT_LOW_bomba)      ? "true":"false") + ",";
  json += "\"vent\":"     + String(isOn(releVentilador, ACT_LOW_ventilador) ? "true":"false") + ",";
  json += "\"valvula\":"  + String(isOn(torneiraFechar, ACT_LOW_valvFechar) ? "false":"true") + ",";
  json += "\"exaustor\":" + String(isOn(releExaustor,   ACT_LOW_exaustor)   ? "true":"false");
  json += "},";

  json += "\"next\":{";
  json += "\"lightMs\":" + String(nextLight);
  json += "},";

  json += "\"calib\":{";
  json += "\"seco\":" + String(SOLO_SECO) + ",";
  json += "\"molhado\":" + String(SOLO_MOLHADO);
  json += "},";

  json += "\"soloLiga\":" + String(SOLO_LIMIAR_LIGA_PCT);
  json += ",";
  json += "\"cicloLuz\":\"" + String(cicloLuz12x12 ? "12/12" : "18/6") + "\"";

  json += "}";

  // Envia a String json ORIGINAL, intacta
  uiServer.send(200, "application/json", json);

  char json2[STATUS_JSON_CAPACIDADE];
  json.toCharArray(json2, sizeof(json2));

  // Processa/valida usando o json2 (o json original NÃO é alterado)
  if (montarStatusJson(json2, sizeof(json2)) == 0) {
    Serial.println("JSON excedeu o tamanho permitido.");
  }
}

// ===================== Controles =====================
void controleLuz() {
  unsigned long now = millis();

  if (luzLigada && now - tempoUltimaTrocaLuz >= tempoLuzOn) {
    aplicarCicloLuzPendente();
    trocaLuzPersist(false);
  } 
  else if (!luzLigada && now - tempoUltimaTrocaLuz >= tempoLuzOff) {
    aplicarCicloLuzPendente();
    trocaLuzPersist(true);
  }
}

void controleNivelAgua() {
  unsigned long now = millis();

  if (now - bootAtMs < VALVULA_BLOQUEIO_BOOT_MS) {
    setOn(torneiraFechar, ACT_LOW_valvFechar);
    setOff(torneiraAbrir, ACT_LOW_valvAbrir);
    return;
  }

  if (now - lastNivelCheckMs < NIVEL_DEBOUNCE_MS) return;
  lastNivelCheckMs = now;

    bool nivelAlto  = (digitalRead(sensorNivelAlto) == HIGH);
    bool nivelBaixo = (digitalRead(sensorNivelBaixo) == HIGH);

    if(nivelAlto && nivelBaixo){
      setOn(torneiraAbrir, ACT_LOW_valvAbrir);
      setOff(torneiraFechar, ACT_LOW_valvFechar);

      Serial.println("Enchendo");
    }else if(!nivelAlto && !nivelBaixo){
      setOn(torneiraFechar, ACT_LOW_valvFechar);
      setOff(torneiraAbrir, ACT_LOW_valvAbrir);

      Serial.println("Cheio");
    }

}

void controleIrrigacaoPorSolo() {
  if (!IRRIG_SOLO_AUTO) return;
  if (!soloCalibrado || SOLO_SECO <= 0 || SOLO_MOLHADO <= 0) return;

  unsigned long now = millis();

  if (now - bootAtMs < IRRIG_BLOQUEIO_BOOT_MS) {
    setOff(releBomba, ACT_LOW_bomba);
    irrigando = false;
    return;
  }

  if (irrigando) {
    if (now - inicioIrrigacaoMs >= IRRIG_MAX_MS) {
      setOff(releBomba, ACT_LOW_bomba);
      irrigando = false;
      return;
    }
    return;
  }

  if (lastSoloRaw <= 0) return;

  if (ultimaIrrigacaoPorSoloMs != 0 &&
      (now - ultimaIrrigacaoPorSoloMs) < MIN_INTERVALO_IRRIG_SOLO_MS) return;

  if (lastSoloPct <= SOLO_LIMIAR_LIGA_PCT) {
    setOn(releBomba, ACT_LOW_bomba);
    irrigando = true;
    inicioIrrigacaoMs = now;
    ultimaIrrigacaoPorSoloMs = now;
  }
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);

  Serial2.setTxBufferSize(256);
  Serial2.begin(115200, SERIAL_8N1, -1, UART_JSON_TX);
  bootAtMs = millis();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  WiFi.onEvent(WiFiEventHandler);

  pinMode(sensorNivelAlto, INPUT);
  pinMode(sensorNivelBaixo, INPUT);
  pinMode(releBomba, OUTPUT);
  pinMode(releVentilador, OUTPUT);
  pinMode(torneiraFechar, OUTPUT);
  pinMode(torneiraAbrir, OUTPUT);
  pinMode(releExaustor, OUTPUT);
  pinMode(luz1, OUTPUT);

  setOff(releBomba, ACT_LOW_bomba);
  setOff(releVentilador, ACT_LOW_ventilador);
  setOff(releExaustor, ACT_LOW_exaustor);
  setOn(torneiraFechar, ACT_LOW_valvFechar);
  setOff(torneiraAbrir, ACT_LOW_valvAbrir);

  Wire.begin(21, 22); // SDA = GPIO 21, SCL = GPIO 22

  // if (!aht.begin()) {
  //   Serial.println("Erro ao inicializar AHT30!");
  //   while (1) delay(10);
  // }


  // MOCK: não inicializa o AHT30 de verdade
  Serial.println("[MOCK] AHT30 simulado (sem hardware).");




  prefs.begin("grow", false);
  
  int tempoIrrigacaoSeg = prefs.getInt("irrigTempoSeg", 14);
  if (tempoIrrigacaoSeg >= 1 && tempoIrrigacaoSeg <= 300) {
    IRRIG_MAX_MS = (unsigned long)tempoIrrigacaoSeg * 1000UL;
  }
  
  cicloLuz12x12 = prefs.getBool("luz12x12", false);
  cicloLuz12x12Pendente = prefs.getBool("luz12x12Pendente", cicloLuz12x12);

  if (cicloLuz12x12) {
    tempoLuzOn  = 12UL * 60UL * 60UL * 1000UL;
    tempoLuzOff = 12UL * 60UL * 60UL * 1000UL;
  } else {
    tempoLuzOn  = 18UL * 60UL * 60UL * 1000UL;
    tempoLuzOff =  6UL * 60UL * 60UL * 1000UL;
  }
  SOLO_LIMIAR_LIGA_PCT = prefs.getInt("soloLigaPct", 80);

int s = prefs.getInt("soloSeco", SOLO_SECO);
int m = prefs.getInt("soloMolhado", SOLO_MOLHADO);

if (
  s > 0 && s < 4095 &&
  m > 0 && m < 4095 &&
  s != m
) {

  // Mantém exatamente o que foi salvo
  SOLO_SECO = s;
  SOLO_MOLHADO = m;
  soloCalibrado = true;

} else {

  SOLO_SECO = 2700;
  SOLO_MOLHADO = 900;
  soloCalibrado = true;
}

  lightSnap.version    = 2;
  lightSnap.luzLigada  = true;
  lightSnap.restanteMs = (uint32_t)tempoLuzOn;

  if (prefs.getBytesLength("lsnap") == sizeof(LightSnap)) {
    prefs.getBytes("lsnap", &lightSnap, sizeof(LightSnap));

    if (lightSnap.version != 2 ||
        lightSnap.restanteMs > (tempoLuzOn + tempoLuzOff)) {
      lightSnap.version    = 2;
      lightSnap.luzLigada  = true;
      lightSnap.restanteMs = (uint32_t)tempoLuzOn;
    }
  }

  restoreLightFrozenFromSnap();

  lastNivel = digitalRead(sensorNivelAlto);

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  WiFi.setSleep(false);

  Serial.println("\nConectando WiFi");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Sem Wi-Fi!");
  }

  updateSensors();
  controleClima();

  uiServer.on("/toggleCicloLuz", handleToggleCicloLuz);
  uiServer.on("/getSoloLiga", handleGetSoloLiga);
  uiServer.on("/setSoloLiga", handleSetSoloLiga);
  uiServer.on("/setTempoIrrigacao", HTTP_POST, handleSetTempoIrrigacao);

  uiServer.on("/getCalibValores", handleGetCalibValores);
  uiServer.on("/setCalibValores", handleSetCalibValores);
  uiServer.on("/", handleRoot);
  uiServer.on("/status.json", handleStatus);
  uiServer.begin();
}

void loop() {
  uiServer.handleClient();

  updateSensors();

  static unsigned long last = 0;
  if (millis() - last >= 200) {
    last = millis();
    controleLuz();
    controleNivelAgua();
    controleIrrigacaoPorSolo();
    controleClima();
  }

  enviarStatusUART();

  tentarReconectarWiFi();

  if (telegramPendenteConexao && wifiTemIP) {
    if (millis() - wifiGotIpAtMs > 5000UL) {
      if (sendTelegramMessage("ESP32 grow conectado. IP: " + WiFi.localIP().toString())) {
        wifiJaAvisouTelegram = true;
        telegramPendenteConexao = false;
        Serial.println(F("[Telegram] Aviso de conexao enviado."));
      } else {
        Serial.println(F("[Telegram] Falha ao enviar aviso de conexao. Vai tentar novamente."));
      }
    }
  }

  static unsigned long lastSave = 0;
  if (millis() - lastSave >= 10UL * 60UL * 1000UL) {
    lastSave = millis();
    saveRemainingLight();
  }
}