/*
  Monitor cardíaco con alarma - ESP32-S3

  Funcionamiento:
    1) ESPERANDO:    espera a detectar latidos (pon el dedo en el sensor).
    2) CALIBRANDO:   durante 30 s mide y calcula el promedio de BPM (queda fijo).
    3) MONITOREANDO: si el BPM sube o baja más del 20% del promedio,
                     el buzzer suena de forma repetida.
    Desde el teléfono se ve: BPM actual, promedio, límite superior e inferior.

  Conexiones:
    Buzzer KY-006:   S -> GPIO 3 | (-) -> GND
    Sensor cardíaco: S -> GPIO 4 | + -> 3V3 | - -> GND

  Arduino IDE: Placa "ESP32S3 Dev Module", USB CDC On Boot: Enabled, Serial 115200.
  No necesita librerías externas.

  CÓMO VER LA PÁGINA:
    - MODO_RED_PROPIA = true  -> conéctate a la red "ESP32-Pulso" (clave 12345678)
                                 y abre http://192.168.4.1
    - MODO_RED_PROPIA = false -> la placa entra a tu WiFi; la IP sale en el Monitor Serie.
*/

#include <WiFi.h>
#include <WebServer.h>

// ================== CONFIGURACIÓN (edita esto) ==================
const bool MODO_RED_PROPIA = true;      // true = la placa crea su propia red WiFi

// Red propia de la placa (si MODO_RED_PROPIA = true)
const char* AP_SSID = "ESP32-Pulso";
const char* AP_PASS = "12345678";       // mínimo 8 caracteres

// Tu WiFi (si MODO_RED_PROPIA = false)
const char* WIFI_SSID = "A56";
const char* WIFI_PASS = "12345678";

const unsigned long CALIB_MS = 30000;   // duración de la calibración
const float TOLERANCIA = 0.20f;         // 20% sobre/bajo el promedio
const bool ALARMA_SIN_SENAL = false;    // true = también suena si se pierde la señal en modo monitoreo
// ================================================================

const int PIN_BUZZER = 3;
const int PIN_SENSOR = 4;
const int PIN_LED = LED_BUILTIN;        // solo decorativo; en algunas S3 no hay LED simple
const bool LED_ACTIVO_BAJO = false;

// Detección de latidos
const unsigned long MUESTREO_MS = 10;       // 100 muestras por segundo
const unsigned long REFRACTARIO_MS = 250;   // tiempo mínimo entre latidos
const unsigned long IBI_MIN = 300;          // 200 BPM
const unsigned long IBI_MAX = 1500;         // 40 BPM
const unsigned long SIN_LATIDO_MS = 3000;   // sin latidos por 3 s = sin señal
const float AMP_MIN = 80.0f;                // amplitud mínima de señal válida
const int MIN_LATIDOS_CALIB = 10;

enum Estado { ESPERANDO, CALIBRANDO, MONITOREANDO };
Estado estado = ESPERANDO;

WebServer server(80);

float senal = 2048, maxV = 2048, minV = 2048;
bool arriba = false;
unsigned long ultLatido = 0, ultMuestra = 0, ledHasta = 0;

unsigned long ibis[5];
int idxIbi = 0, nIbi = 0;
float bpmActual = 0;

unsigned long tCalib = 0;
float sumaCalib = 0;
int nCalib = 0;
float promedio = 0, limSup = 0, limInf = 0;

bool alarmaActiva = false;
bool buzzerSonando = false;
unsigned long ultToggleBuzzer = 0;
unsigned long ultSerial = 0;
String direccion = "";

// ---------------- Página web ----------------
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Monitor cardíaco</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#f2f4f7;color:#1a1a1a;text-align:center;transition:background .3s}
body.alarma{background:#ffb3b3}
h1{font-size:1.2rem;margin:4px 0 12px}
.bpm{font-size:5rem;font-weight:700;line-height:1}
.unidad{font-size:1rem;color:#555}
.estado{margin:10px 0;font-size:1.1rem;font-weight:600}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-top:14px}
.card{background:#fff;border-radius:10px;padding:10px 4px}
.card b{display:block;font-size:1.5rem}
.card span{font-size:.75rem;color:#555}
.barra{height:10px;background:#ddd;border-radius:5px;overflow:hidden;margin:10px 0}
.barra div{height:100%;width:0;background:#3b82f6;transition:width .5s}
button{margin-top:18px;padding:12px 20px;font-size:1rem;border:0;border-radius:8px;background:#1f2937;color:#fff}
</style></head><body>
<h1>Pulsaciones</h1>
<div class="bpm" id="bpm">--</div><div class="unidad">BPM</div>
<div class="estado" id="estado">Conectando...</div>
<div class="barra" id="bc" style="display:none"><div id="prog"></div></div>
<div class="grid">
<div class="card"><b id="inf">--</b><span>Límite inferior</span></div>
<div class="card"><b id="prom">--</b><span>Promedio fijo</span></div>
<div class="card"><b id="sup">--</b><span>Límite superior</span></div>
</div>
<button onclick="recal()">Recalibrar</button>
<script>
function recal(){if(confirm('¿Volver a calibrar el promedio?'))fetch('/recalibrar')}
async function act(){
 try{
  const d=await (await fetch('/data')).json();
  document.getElementById('bpm').textContent=d.bpm>0?Math.round(d.bpm):'--';
  const f=v=>v>0?v.toFixed(1):'--';
  document.getElementById('prom').textContent=f(d.promedio);
  document.getElementById('sup').textContent=f(d.sup);
  document.getElementById('inf').textContent=f(d.inf);
  let t='';
  if(d.estado=='esperando')t='Pon el dedo en el sensor';
  else if(d.estado=='calibrando')t='Calibrando... '+d.progreso+'%';
  else t=d.alarma?'⚠️ FUERA DE RANGO':(d.bpm>0?'Normal':'Sin señal');
  document.getElementById('estado').textContent=t;
  document.getElementById('bc').style.display=d.estado=='calibrando'?'block':'none';
  document.getElementById('prog').style.width=d.progreso+'%';
  document.body.classList.toggle('alarma',d.alarma);
 }catch(e){document.getElementById('estado').textContent='Sin conexión con la placa'}
}
setInterval(act,1000);act();
</script></body></html>
)rawliteral";

// ---------------- Servidor ----------------
void manejarRaiz() {
  server.send(200, "text/html", PAGINA);
}

void manejarDatos() {
  const char* est = estado == ESPERANDO ? "esperando" : (estado == CALIBRANDO ? "calibrando" : "monitoreando");
  int progreso = 0;
  if (estado == CALIBRANDO) {
    progreso = constrain((int)((millis() - tCalib) * 100UL / CALIB_MS), 0, 100);
  } else if (estado == MONITOREANDO) {
    progreso = 100;
  }
  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"estado\":\"%s\",\"bpm\":%.1f,\"promedio\":%.1f,\"sup\":%.1f,\"inf\":%.1f,\"alarma\":%s,\"progreso\":%d}",
           est, bpmActual, promedio, limSup, limInf, alarmaActiva ? "true" : "false", progreso);
  server.send(200, "application/json", buf);
}

void reiniciarCalibracion() {
  estado = ESPERANDO;
  promedio = limSup = limInf = 0;
  sumaCalib = 0;
  nCalib = 0;
  alarmaActiva = false;
}

void manejarRecalibrar() {
  reiniciarCalibracion();
  server.send(200, "text/plain", "ok");
}

void iniciarRedPropia() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  direccion = "Red '" + String(AP_SSID) + "' (clave " + String(AP_PASS) + ") -> http://" + WiFi.softAPIP().toString();
}

void conectarWiFi() {
  if (MODO_RED_PROPIA) {
    iniciarRedPropia();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando a WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    direccion = "Tu WiFi '" + String(WIFI_SSID) + "' -> http://" + WiFi.localIP().toString();
  } else {
    Serial.println("No se pudo conectar. Creando red propia...");
    iniciarRedPropia();
  }
}

// ---------------- Detección de latidos ----------------
void registrarLatido(unsigned long now) {
  if (ultLatido > 0) {
    unsigned long ibi = now - ultLatido;
    if (ibi >= IBI_MIN && ibi <= IBI_MAX) {
      ibis[idxIbi] = ibi;
      idxIbi = (idxIbi + 1) % 5;
      if (nIbi < 5) nIbi++;
      if (nIbi >= 3) {
        unsigned long suma = 0;
        for (int i = 0; i < nIbi; i++) suma += ibis[i];
        bpmActual = 60000.0f / ((float)suma / nIbi);
        if (estado == CALIBRANDO) {
          sumaCalib += bpmActual;
          nCalib++;
        }
      }
    } else {
      nIbi = 0;
      idxIbi = 0;
    }
  }
  ultLatido = now;
  ledHasta = now + 60;
}

void procesarMuestra(unsigned long now) {
  int raw = analogRead(PIN_SENSOR);
  senal = senal * 0.7f + raw * 0.3f;

  // Máximo y mínimo adaptativos (decaen lentamente hacia la señal)
  if (senal > maxV) maxV = senal; else maxV -= (maxV - senal) * 0.005f;
  if (senal < minV) minV = senal; else minV += (senal - minV) * 0.005f;

  float amp = maxV - minV;
  if (amp < AMP_MIN) {
    arriba = false;
    return;
  }
  float umbralAlto = minV + 0.6f * amp;
  float umbralBajo = minV + 0.4f * amp;

  if (!arriba && senal > umbralAlto) {
    arriba = true;
    if (now - ultLatido >= REFRACTARIO_MS) registrarLatido(now);
  } else if (arriba && senal < umbralBajo) {
    arriba = false;
  }
}

// ---------------- Lógica de estados ----------------
void pitidoConfirmacion() {
  for (int i = 0; i < 2; i++) {
    tone(PIN_BUZZER, 1500);
    delay(150);
    noTone(PIN_BUZZER);
    delay(100);
  }
}

void actualizarEstado(unsigned long now) {
  // Pérdida de señal
  if (ultLatido > 0 && now - ultLatido > SIN_LATIDO_MS) {
    bpmActual = 0;
    nIbi = 0;
    idxIbi = 0;
  }

  switch (estado) {
    case ESPERANDO:
      if (bpmActual > 0) {
        estado = CALIBRANDO;
        tCalib = now;
        sumaCalib = 0;
        nCalib = 0;
        Serial.println(">> Calibrando 30 s...");
      }
      break;

    case CALIBRANDO:
      if (bpmActual == 0) {
        estado = ESPERANDO;
        Serial.println(">> Señal perdida, reiniciando calibración");
      } else if (now - tCalib >= CALIB_MS) {
        if (nCalib >= MIN_LATIDOS_CALIB) {
          promedio = sumaCalib / nCalib;
          limSup = promedio * (1.0f + TOLERANCIA);
          limInf = promedio * (1.0f - TOLERANCIA);
          estado = MONITOREANDO;
          Serial.printf(">> Promedio fijo: %.1f BPM | Límites: %.1f - %.1f\n", promedio, limInf, limSup);
          pitidoConfirmacion();
        } else {
          estado = ESPERANDO;
          Serial.println(">> Pocos latidos detectados, reintentando calibración");
        }
      }
      break;

    case MONITOREANDO:
      if (bpmActual > 0) {
        alarmaActiva = (bpmActual > limSup || bpmActual < limInf);
      } else {
        alarmaActiva = ALARMA_SIN_SENAL;
      }
      break;
  }
}

void actualizarBuzzer(unsigned long now) {
  if (alarmaActiva && estado == MONITOREANDO) {
    if (now - ultToggleBuzzer >= 250) {
      ultToggleBuzzer = now;
      buzzerSonando = !buzzerSonando;
      if (buzzerSonando) tone(PIN_BUZZER, 2000); else noTone(PIN_BUZZER);
    }
  } else if (buzzerSonando) {
    noTone(PIN_BUZZER);
    buzzerSonando = false;
  }
}

void actualizarLed(unsigned long now) {
  bool encendido = now < ledHasta;
  digitalWrite(PIN_LED, (encendido != LED_ACTIVO_BAJO) ? HIGH : LOW);
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Monitor cardiaco ESP32-S3 ===");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_SENSOR, INPUT);

  senal = maxV = minV = analogRead(PIN_SENSOR);

  conectarWiFi();
  server.on("/", manejarRaiz);
  server.on("/data", manejarDatos);
  server.on("/recalibrar", manejarRecalibrar);
  server.begin();

  Serial.println(direccion);
  Serial.println("Pon el dedo en el sensor para comenzar.");
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  if (now - ultMuestra >= MUESTREO_MS) {
    ultMuestra = now;
    procesarMuestra(now);
  }

  actualizarEstado(now);
  actualizarBuzzer(now);
  actualizarLed(now);

  if (now - ultSerial >= 1000) {
    ultSerial = now;
    const char* est = estado == ESPERANDO ? "ESPERANDO" : (estado == CALIBRANDO ? "CALIBRANDO" : "MONITOREANDO");
    Serial.printf("[%s] BPM: %.1f | Promedio: %.1f | Rango: %.1f - %.1f | Alarma: %s | %s\n",
                  est, bpmActual, promedio, limInf, limSup, alarmaActiva ? "SI" : "no", direccion.c_str());
  }
}
