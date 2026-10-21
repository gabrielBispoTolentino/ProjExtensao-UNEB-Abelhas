// ColmeiaMonitor - firmware da ESP32 que monitora uma colmeia.
//
// A cada 2 s lê temperatura e umidade (DHT22) e envia o valor ao vivo por
// Bluetooth (BLE) para o app. A cada intervalo de registro (1 min por padrão,
// 1 a 60, definido pelo app) a média das leituras vira um *registro*, enviado
// ao Supabase por Wi-Fi; sem internet, o registro fica no cartão SD e sobe
// depois, pelo Wi-Fi da placa ou, sem ele, pelo celular conectado por
// Bluetooth (que guarda os registros e os envia quando tiver internet). A
// placa também recebe comandos do app pelo BLE (configurar Wi-Fi, nome,
// intervalo, reiniciar) e abre um hotspot de configuração quando não tem
// rede.
//
// Para compilar (Arduino IDE, núcleo ESP32 3.3.x):
//   * Partition Scheme: "Huge APP" (o programa tem ~1,4 MB);
//   * bibliotecas: DHT sensor library, ArduinoJson e NimBLE-Arduino.

#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
// Bluetooth com NimBLE (biblioteca "NimBLE-Arduino"): usa bem menos RAM que a
// pilha BLE padrão (Bluedroid). Com a Bluedroid sobrava pouca memória para o
// TLS (HTTPS) e a conexão falhava com "SSL - Memory allocation failed".
#include <NimBLEDevice.h>
#include <esp_sntp.h>
#include <vector>
#include <algorithm>

// Um registro (média das leituras do intervalo). boot + seq identificam o
// registro: o Supabase e o app usam isso para não gravar duas vezes o mesmo
// registro que chegou pela placa e pelo app (ou pelo celular e pelo PC).
// Fica no topo porque a IDE do Arduino gera os protótipos antes da 1ª função.
struct SDReading {
  time_t        ts   = 0;  // hora real (0 = desconhecida)
  uint32_t      boot = 0;  // 0 = desconhecido (formato antigo)
  unsigned long ms   = 0;
  uint32_t      seq  = 0;  // 0 = desconhecido (formato antigo)
  float         t    = 0;
  float         h    = 0;
};

// Um lote lido do SD por readSDBatch(). Também no topo, pelo mesmo motivo.
struct SDBatchInfo {
  int      count    = 0;      // registros lidos
  int      dropped  = 0;      // linhas que não dá mais para enviar, puladas
  uint32_t endPos   = 0;      // posição no arquivo depois do lote
  uint32_t fileSize = 0;
  bool     toEnd    = false;  // o lote chegou ao fim do arquivo
};

// ── Pinos ──────────────────────────────────────────────────────────────────
#define DHT_PIN        32
#define DHT_TYPE       DHT22

// Módulo microSD (barramento SPI)
#define SD_CS          5
#define SD_SCK         18
#define SD_MISO        19
#define SD_MOSI        23
#define LOG_FILE       "/datalog.txt"

// ── Hotspot de configuração ────────────────────────────────────────────────
#define AP_SSID "Colmeia-Setup"
#define AP_PASS ""               // aberto, sem senha, para facilitar o acesso

// ── Intervalos ─────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS      2000    // leitura do DHT (valores ao vivo pelo BLE)
// Intervalo de registro: a cada N minutos a média das leituras vira um registro
// (o que vai para o Supabase). Ajustável pelo app (comando "interval"), salvo no NVS.
#define RECORD_MINUTES_DEFAULT  1
#define RECORD_MINUTES_MAX      60
#define WIFI_RETRY_INTERVAL_MS  30000   // entre tentativas de reconectar ao Wi-Fi
// Se a rede salva cair durante o uso, espera este tempo antes de reabrir o
// hotspot (não abre por uma queda de poucos segundos). No boot, se a rede
// salva não conectar, o hotspot abre na hora.
#define AP_FALLBACK_DELAY_MS    60000

// ── Bluetooth (BLE): serviço e comandos ────────────────────────────────────
// Usa o Nordic UART Service (NUS), compatível também com apps como
// nRF Connect e Serial Bluetooth Terminal. Cada leitura (a cada 2 s, ao
// vivo) e cada registro (a cada N min, com boot/seq e "rec": true) vão
// como uma linha JSON terminada em '\n'.
#define BLE_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // app -> ESP32
#define BLE_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> app (notify)
#define BLE_CHUNK_SIZE    20   // pedaço seguro com o MTU padrão (23)

// Comandos do app (RX): uma linha JSON terminada em '\n', que pode chegar em
// vários pedaços. As respostas vão pelo TX com um campo "type":
//   {"cmd":"status"}                        -> {"type":"status", ...}
//   {"cmd":"scan"}                          -> {"type":"networks","networks":[...]}
//   {"cmd":"wifi","ssid":"..","pass":".."}  -> {"type":"wifi","state":"connecting|connected|failed"}
//   {"cmd":"time","ts":<epoch UTC>}         -> (sem resposta) acerta o relógio pela hora do celular
//   {"cmd":"name","name":".."}              -> {"type":"name","name":".."}   renomeia a placa
//   {"cmd":"restart"}                       -> {"type":"restart"}            e reinicia em seguida
//   {"cmd":"interval","minutes":N}          -> {"type":"interval","minutes":N} intervalo de registro
//   {"cmd":"dump"}                          -> registros do SD em lotes (ver dumpSDStep())
//   {"cmd":"dumpAck","pos":N}               -> (sem resposta) o app guardou o lote até N
#define BLE_CMD_MAX_LEN   256  // maior linha de comando aceita
#define BLE_CMD_QUEUE_LEN 4
#define WIFI_SCAN_MAX     15   // redes enviadas na resposta do scan
#define WIFI_PROVISION_TIMEOUT_MS 20000  // prazo para conectar à rede enviada pelo app
#define BOARD_NAME_MAX    24   // nome da placa (cabe na resposta à busca BLE)
#define RESTART_DELAY_MS  500  // dá tempo da resposta do "restart" sair antes de reiniciar

// ── Relógio ────────────────────────────────────────────────────────────────
// A hora real vem da internet (NTP) quando há Wi-Fi, ou do celular pelo
// Bluetooth (comando "time"). As leituras levam "ts" = segundos desde
// 01/01/1970 em UTC; o app converte para o fuso do celular.
#define TIMEZONE        "<-03>3"     // Horário de Brasília (UTC-3, sem horário de verão)
#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define MIN_VALID_EPOCH 1700000000   // antes disso (nov/2023) o relógio ainda não foi acertado
#define SD_LOG_HEADER   "epoch,data_hora,boot,millis_ms,seq,temp_c,umidade_pct"

// ── Supabase ───────────────────────────────────────────────────────────────
// Banco na nuvem que o app também lê. Os registros vão para a tabela
// "readings" (supabase/schema.sql neste repositório) pela API REST. A
// chave é a pública (anon / publishable), nunca a service_role/secreta.
// Mesmos valores de app/supabaseconfig.h.
#define SUPABASE_URL  "https://ysepuwpswahtobpatarh.supabase.co"
#define SUPABASE_KEY  "sb_publishable_LOlfANl1hGM5-YZ_mTqaWQ_6FuXoHk1"
#define UPLOAD_BATCH  25      // registros por requisição ao enviar o que ficou no SD
// O que ficou no SD sobe um lote por vez (o loop() segue atendendo o
// Bluetooth entre um lote e outro), a cada SD_FLUSH_INTERVAL_MS enquanto houver
// Wi-Fi; depois de uma falha, espera SD_FLUSH_RETRY_MS para tentar de novo.
#define SD_FLUSH_INTERVAL_MS  5000
#define SD_FLUSH_RETRY_MS     60000
// Sem Wi-Fi, o celular conectado leva o que ficou no SD, um lote por vez
// (cada registro leva ~60 ms pelo BLE com o MTU padrão).
#define DUMP_BATCH            10
#define DUMP_ACK_TIMEOUT_MS   15000   // sem confirmação do app: o lote é enviado de novo

// ── Identificação ──────────────────────────────────────────────────────────
String deviceId;              // MAC sem ":" (ex.: A4CF12345678), montado no setup()
String deviceName;            // nome amigável (NVS); padrão "Colmeia-XXXX". Vai no BLE e nas leituras
unsigned long restartAt = 0;  // != 0: reiniciar quando millis() passar disso (pedido pelo app)

// ── Objetos ────────────────────────────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);
DNSServer dnsServer;          // responde qualquer domínio com o IP da ESP32 (portal cativo)
Preferences prefs;
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleTx = nullptr;

// O núcleo da ESP32 libera a memória do Bluetooth no boot se nenhuma
// biblioteca BLE "dele" estiver em uso, e o NimBLE não avisa isso. Sem esta
// função o NimBLEDevice::init() falharia.
bool bleInUse() { return true; }

// ── Estado ─────────────────────────────────────────────────────────────────
bool sdReady = false;
bool hasCredentials = false;  // já existe SSID salvo no NVS
bool apMode = false;          // o portal de configuração está ativo em paralelo
bool routesReady = false;     // as rotas do WebServer só são registradas uma vez
bool pendingSDData = false;   // há registros no SD ainda não enviados ao Supabase
uint32_t sdSentPos = 0;       // posição no LOG_FILE até onde já foi enviado (NVS "sys"/"sd_pos"; 0 = início)
unsigned long lastFlushAt = 0;
bool lastFlushFailed = false;
unsigned long lastWifiAttempt = 0;
unsigned long lastSensorRead = 0;
volatile bool bleConnected = false;

// Despejo do SD pelo Bluetooth (ver dumpSDStep())
bool dumpWanted = false;        // o celular conectado pediu os registros guardados
bool dumpAwaitingAck = false;   // lote enviado, esperando o app confirmar
uint32_t dumpFromPos = 0;       // sdSentPos quando o lote foi lido
SDBatchInfo dumpBatch;          // o lote à espera de confirmação
unsigned long dumpSentAt = 0;
unsigned long wifiDownSince = 0; // quando o Wi-Fi caiu (0 = está conectado)

// Comandos BLE: o callback de escrita roda na tarefa do Bluetooth, então só
// monta a linha e a põe na fila; quem executa o comando é o loop().
QueueHandle_t bleCmdQueue = nullptr;
char bleRxLine[BLE_CMD_MAX_LEN];
size_t bleRxLen = 0;
bool bleRxOverflow = false;

// Configuração de Wi-Fi pelo app (BLE)
bool provisioning = false;          // tentando a rede enviada pelo app
unsigned long provisionStart = 0;
String provisionSsid;
String provisionPass;
volatile uint8_t lastWifiDisconnectReason = 0; // motivo da última queda (evento do Wi-Fi)

// Relógio
uint32_t bootCount = 0;              // nº do boot atual (NVS); millis() só vale dentro do mesmo boot
uint32_t readingSeq = 0;             // nº do registro neste boot; boot + seq identificam cada registro
bool clockWasValid = false;          // só para avisar no Serial quando a hora for acertada
volatile bool ntpSynced = false;     // hora veio da internet: a hora do celular não a substitui

// Registro em andamento: soma das leituras válidas desde o último registro
float recordSumT = 0;
float recordSumH = 0;
int recordCount = 0;
unsigned long lastRecordAt = 0;
uint16_t recordMinutes = RECORD_MINUTES_DEFAULT; // intervalo de registro (NVS "sys"/"rec_min")
bool cloudOk = false;                // último envio ao Supabase deu certo

// ── ID do dispositivo (MAC sem ":") ────────────────────────────────────────
String getDeviceId() {
  String id = WiFi.macAddress();
  id.replace(":", "");
  return id;
}

// ── Relógio (hora real) ────────────────────────────────────────────────────
bool timeIsValid() {
  return time(nullptr) > MIN_VALID_EPOCH;
}

// Ex.: "14/10/2026 13:20:05" no fuso definido em TIMEZONE
String formatTime(time_t ts) {
  if (ts <= MIN_VALID_EPOCH) return "sem_hora";
  struct tm local;
  localtime_r(&ts, &local);
  char text[24];
  strftime(text, sizeof(text), "%d/%m/%Y %H:%M:%S", &local);
  return String(text);
}

// Acerta o relógio com a hora enviada pelo celular, só enquanto a placa não
// tiver a hora da internet (NTP), mais confiável que um celular com o
// relógio errado.
void setClockFromApp(time_t ts) {
  if (ts <= MIN_VALID_EPOCH || ntpSynced) return;

  time_t now = time(nullptr);
  if (timeIsValid() && llabs((long long)(now - ts)) < 60) return; // já está certo

  struct timeval tv = { ts, 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[Hora] Relógio acertado pelo app: %s\n", formatTime(ts).c_str());
}

// Linha do log no SD. Sem hora certa, grava epoch 0 + boot/millis, para a
// hora ser calculada depois se o relógio for acertado neste mesmo boot.
String sdLogLine(const SDReading& r) {
  char line[112];
  // 2 casas: os mesmos valores que vão pelo BLE e direto ao Supabase
  snprintf(line, sizeof(line), "%lld,%s,%lu,%lu,%lu,%.2f,%.2f",
           (long long)(r.ts > MIN_VALID_EPOCH ? r.ts : 0), formatTime(r.ts).c_str(),
           (unsigned long)r.boot, r.ms, (unsigned long)r.seq, r.t, r.h);
  return String(line);
}

// ── Páginas HTML do portal de configuração ─────────────────────────────────
const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Colmeia — Configurar WiFi</title>
  <style>
    body { font-family: sans-serif; max-width: 360px; margin: 60px auto; padding: 0 16px; }
    h2   { color: #f0a500; }
    input { width: 100%; padding: 8px; margin: 6px 0 16px; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px; }
    button { background: #f0a500; color: #fff; border: none; padding: 10px 24px; border-radius: 4px; cursor: pointer; font-size: 1rem; }
  </style>
</head>
<body>
  <h2> BeeDuino</h2>
  <p>Conecte o ESP32 à sua rede WiFi.</p>
  <p style="color:#666;font-size:0.9rem;">O sensor já está gravando leituras no cartão SD mesmo sem WiFi configurado.</p>
  <form action="/save" method="POST">
    <label>Nome da rede (SSID)</label>
    <input name="ssid" type="text" placeholder="MinhaRede" required>
    <label>Senha</label>
    <input name="pass" type="password" placeholder="••••••••">
    <button type="submit">Salvar e conectar</button>
  </form>
</body>
</html>
)rawliteral";

const char SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Salvo!</title>
  <style>body{font-family:sans-serif;text-align:center;margin-top:80px;}</style>
</head>
<body>
  <h2>Credenciais salvas!</h2>
  <p>ID do dispositivo: <b>%DEVICE_ID%</b></p>
  <p>O ESP32 vai reiniciar e se conectar à rede.</p>
  <p>Você pode fechar esta página.</p>
</body>
</html>
)rawliteral";

// ── Rotas do WebServer ─────────────────────────────────────────────────────
void setupRoutes() {
  // Formulário principal
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", PORTAL_HTML);
  });

  // Salva as credenciais no NVS e reinicia
  server.on("/save", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");

    if (newSsid.length() == 0) {
      server.send(400, "text/plain", "SSID nao pode ser vazio.");
      return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid", newSsid);
    prefs.putString("pass", newPass);
    prefs.end();

    // Monta a página de confirmação com o ID do dispositivo
    String page = String(SAVED_HTML);
    page.replace("%DEVICE_ID%", getDeviceId());
    server.send(200, "text/html", page);

    Serial.println("[WiFi] Credenciais salvas. Reiniciando...");
    delay(1500);
    ESP.restart();
  });

  // Portal cativo: os testes de conexão de cada sistema são redirecionados
  // para o formulário, que assim abre sozinho ao entrar no hotspot.
  auto redirect = []() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  };

  // Android / ChromeOS
  server.on("/generate_204",          HTTP_GET, redirect);
  server.on("/gen_204",               HTTP_GET, redirect);
  // Windows / Edge
  server.on("/ncsi.txt",              HTTP_GET, redirect);
  server.on("/connecttest.txt",       HTTP_GET, redirect);
  server.on("/redirect",              HTTP_GET, redirect);
  // Apple / iOS / macOS
  server.on("/hotspot-detect.html",   HTTP_GET, redirect);
  server.on("/library/test/success.html", HTTP_GET, redirect);
  server.on("/success.txt",           HTTP_GET, redirect);
  // Qualquer outra URL não mapeada
  server.onNotFound(redirect);
}

// ── Portal de configuração (hotspot) ───────────────────────────────────────
// Roda em segundo plano (não bloqueia): o sensor continua lendo e
// gravando no SD enquanto o Wi-Fi não é configurado. Também abre quando
// há credenciais mas a rede salva está fora do ar; nesse caso o modo é
// AP+STA, para a placa seguir tentando a rede salva com o hotspot aberto.
void startConfigPortalBackground() {
  if (apMode) return; // já está aberto

  if (hasCredentials) {
    Serial.println("[WiFi] Rede salva indisponível — reabrindo hotspot de configuração.");
  } else {
    Serial.println("[WiFi] Sem credenciais salvas — portal de configuração ativo em paralelo.");
  }
  Serial.println("[WiFi] O sensor continua lendo e gravando no SD normalmente.");

  WiFi.mode(hasCredentials ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[WiFi] Hotspot: \"%s\"  IP: %s\n", AP_SSID, apIP.toString().c_str());

  dnsServer.start(53, "*", apIP);
  if (!routesReady) {
    setupRoutes();
    routesReady = true;
  }
  server.begin();

  apMode = true;
}

// Fecha o hotspot quando a rede salva voltou
void stopConfigPortal() {
  if (!apMode) return;

  Serial.println("[WiFi] Rede salva reconectada — fechando hotspot de configuração.");
  dnsServer.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apMode = false;
}

// Chamado a cada volta do loop():
//   * Wi-Fi caiu há mais de AP_FALLBACK_DELAY_MS      -> reabre o hotspot
//   * Wi-Fi voltou e ninguém está conectado ao hotspot -> fecha o hotspot
void manageWifiFallback() {
  if (!hasCredentials) return; // sem credenciais o portal fica aberto sempre

  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
    if (apMode && WiFi.softAPgetStationNum() == 0) {
      stopConfigPortal();
    }
    return;
  }

  if (wifiDownSince == 0) {
    wifiDownSince = millis();
  }

  if (!apMode && millis() - wifiDownSince > AP_FALLBACK_DELAY_MS) {
    startConfigPortalBackground();
  }
}

// ── Conexão Wi-Fi com as credenciais salvas no NVS ─────────────────────────
// Não bloqueia: sem credenciais, abre o portal em segundo plano e o
// setup() segue para a leitura/log no SD. Com credenciais mas a rede fora
// do ar, abre o hotspot e segue offline; o loop() tenta reconectar de
// tempos em tempos.
void connectWiFi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    hasCredentials = false;
    startConfigPortalBackground();
    return;
  }

  hasCredentials = true;
  Serial.printf("[WiFi] Conectando a \"%s\"", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Rede indisponível no momento — seguindo offline (log no SD).");
    startConfigPortalBackground(); // reabre o hotspot para reconfigurar
  }
}

// ── Cartão SD ──────────────────────────────────────────────────────────────
bool setupSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    Serial.println("[SD] Falha ao montar o cartão SD.");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("[SD] Nenhum cartão SD encontrado.");
    return false;
  }

  prefs.begin("sys", true);
  sdSentPos = prefs.getUInt("sd_pos", 0);
  prefs.end();

  if (!SD.exists(LOG_FILE)) {
    File f = SD.open(LOG_FILE, FILE_WRITE);
    if (f) {
      f.println(SD_LOG_HEADER);
      f.close();
    }
    sdSentPos = 0;
    pendingSDData = false;
  } else {
    // Há registros pendentes se o arquivo vai além do que já foi enviado
    File f = SD.open(LOG_FILE, FILE_READ);
    if (f) {
      f.readStringUntil('\n'); // cabeçalho
      uint32_t headerEnd = f.position();
      if (sdSentPos > f.size()) sdSentPos = 0; // arquivo trocado: recomeça do início
      pendingSDData = f.size() > max(sdSentPos, headerEnd);
      f.close();
      if (pendingSDData) {
        Serial.println("[SD] Há registros pendentes de um boot anterior — serão enviados assim que houver conexão.");
      }
    }
  }

  Serial.println("[SD] Cartão montado com sucesso.");
  return true;
}

// Guarda um registro no SD até ele poder subir para o Supabase
void logToSD(const SDReading& r) {
  if (!sdReady) {
    Serial.println("[SD] Registro não guardado — cartão indisponível.");
    return;
  }

  // A 1ª linha é sempre o cabeçalho (a leitura do log pula a 1ª linha)
  bool newFile = !SD.exists(LOG_FILE);
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Falha ao abrir arquivo de log.");
    return;
  }

  if (newFile) f.println(SD_LOG_HEADER);
  f.println(sdLogLine(r));
  f.close();
  pendingSDData = true;

  Serial.printf("[SD] Registro guardado localmente: %s, %.2f°C, %.2f%%\n",
                formatTime(r.ts).c_str(), r.t, r.h);
}

// Lê uma linha do log. Aceita os três formatos já usados:
//   7 campos: epoch,data_hora,boot,millis_ms,seq,temp_c,umidade_pct (atual)
//   6 campos: epoch,data_hora,boot,millis_ms,temp_c,umidade_pct    (sem seq)
//   3 campos: millis_ms,temp_c,umidade_pct                         (sem hora)
// Se a leitura não tinha hora mas é deste boot e o relógio já foi
// acertado, calcula a hora a partir do millis().
bool parseSDLine(const String& line, SDReading* r) {
  const int maxFields = 8; // um a mais, para detectar linha com campos sobrando
  String fields[maxFields];
  int count = 0;
  int start = 0;
  while (count < maxFields) {
    int comma = line.indexOf(',', start);
    fields[count++] = comma < 0 ? line.substring(start) : line.substring(start, comma);
    if (comma < 0) break;
    start = comma + 1;
  }

  *r = SDReading();
  if (count == 7 || count == 6) {
    int i = 0;
    r->ts   = (time_t)atoll(fields[i++].c_str());
    i++; // data_hora: só para leitura humana
    r->boot = strtoul(fields[i++].c_str(), nullptr, 10);
    r->ms   = strtoul(fields[i++].c_str(), nullptr, 10);
    if (count == 7) r->seq = strtoul(fields[i++].c_str(), nullptr, 10);
    r->t    = fields[i++].toFloat();
    r->h    = fields[i++].toFloat();
  } else if (count == 3) {
    r->ms = strtoul(fields[0].c_str(), nullptr, 10);
    r->t  = fields[1].toFloat();
    r->h  = fields[2].toFloat();
  } else {
    return false; // linha corrompida
  }

  if (r->ts == 0 && r->boot != 0 && r->boot == bootCount && timeIsValid() && r->ms <= millis()) {
    r->ts = time(nullptr) - (time_t)((millis() - r->ms) / 1000);
  }
  return true;
}

// ── Supabase: envio de registros pela API REST (HTTPS) ─────────────────────
// "2026-10-14T16:20:05Z" (UTC), o formato que o Postgres espera
String isoTime(time_t ts) {
  struct tm utc;
  gmtime_r(&ts, &utc);
  char text[24];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(text);
}

// Envia `count` registros (com hora, boot e seq) numa única requisição.
// Registros que o banco já tem (mesmo device_id/boot/seq, ex.: enviados antes
// pelo app) são ignorados pelo próprio Supabase e contam como sucesso.
// Abre e fecha a conexão TLS a cada envio: com o Bluetooth ligado não sobra
// RAM para manter a conexão aberta o tempo todo.
bool uploadRecords(const SDReading* records, int count) {
  if (count == 0) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  JsonDocument doc;
  JsonArray rows = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    const SDReading& r = records[i];
    JsonObject row = rows.add<JsonObject>();
    row["device_id"]   = deviceId;
    row["device_name"] = deviceName;
    row["recorded_at"] = isoTime(r.ts);
    row["temperature"] = r.t;
    row["humidity"]    = r.h;
    row["boot"]        = r.boot;
    row["seq"]         = r.seq;
    row["source"]      = "esp32";
  }
  String body;
  serializeJson(doc, body);

  WiFiClientSecure tls;
  tls.setInsecure(); // não valida o certificado do servidor
  HTTPClient http;
  if (!http.begin(tls, String(SUPABASE_URL) + "/rest/v1/readings?on_conflict=device_id,boot,seq")) {
    Serial.println("[Nuvem] URL do Supabase inválida.");
    cloudOk = false;
    return false;
  }
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  // Chaves antigas (anon, JWT) também vão no Authorization; as novas
  // (sb_publishable_...) só no apikey.
  if (strncmp(SUPABASE_KEY, "sb_", 3) != 0) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  }
  http.addHeader("Prefer", "resolution=ignore-duplicates,return=minimal");

  int code = http.POST(body);
  bool ok = code >= 200 && code < 300;
  if (!ok) {
    String detail = code > 0 ? http.getString() : http.errorToString(code);
    Serial.printf("[Nuvem] Falha ao enviar %d registro(s): HTTP %d %s\n", count, code, detail.c_str());
    if (code < 0) {
      char tlsError[100] = "";
      tls.lastError(tlsError, sizeof(tlsError));
      Serial.printf("[Nuvem] TLS: %s | memória livre: %u, maior bloco: %u\n",
                    tlsError, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
  }
  http.end();

  cloudOk = ok;
  return ok;
}

// ── Envio do que ficou no SD ───────────────────────────────────────────────
// Sobe os registros guardados enquanto a placa estava offline, UM lote por
// chamada, continuando de onde parou (sd_pos), para o loop() seguir
// atendendo o Bluetooth entre um lote e outro. O arquivo só é zerado
// quando o servidor (ou o app, no despejo pelo Bluetooth) confirmou tudo.
void saveSentPos(uint32_t pos) {
  sdSentPos = pos;
  prefs.begin("sys", false);
  prefs.putUInt("sd_pos", pos);
  prefs.end();
}

// Tudo enviado: recomeça o log só com o cabeçalho
void resetSDLog() {
  SD.remove(LOG_FILE);
  File f = SD.open(LOG_FILE, FILE_WRITE);
  if (f) {
    f.println(SD_LOG_HEADER);
    f.close();
  }
  saveSentPos(0);
  pendingSDData = false;
}

// Lê o próximo lote ainda não enviado (a partir de sd_pos), até maxCount
// registros. Linhas que não dá mais para enviar são puladas e contadas em
// info->dropped. Para num registro deste boot ainda sem hora (ele espera o
// relógio ser acertado). Retorna false se o arquivo não abriu.
bool readSDBatch(SDReading* batch, int maxCount, SDBatchInfo* info) {
  *info = SDBatchInfo();
  File f = SD.open(LOG_FILE, FILE_READ);
  if (!f) {
    Serial.println("[SD] Falha ao abrir log para sincronizar.");
    return false;
  }
  info->fileSize = f.size();
  if (sdSentPos == 0 || sdSentPos > f.size()) {
    f.readStringUntil('\n'); // pula o cabeçalho
  } else {
    f.seek(sdSentPos);
  }

  info->endPos = f.position(); // até onde este lote resolve o arquivo
  bool waitForClock = false;

  while (info->count < maxCount && f.available()) {
    String line = f.readStringUntil('\n');
    uint32_t afterLine = f.position();
    line.trim();

    SDReading r;
    if (line.length() == 0) {
      info->endPos = afterLine;
      continue;
    }
    if (!parseSDLine(line, &r) || r.seq == 0 || (r.ts <= MIN_VALID_EPOCH && r.boot != bootCount)) {
      // Corrompida, formato antigo (sem seq) ou sem hora de um boot anterior:
      // o banco exige seq e hora, que não podem mais ser recuperados.
      info->dropped++;
      info->endPos = afterLine;
      continue;
    }
    if (r.ts <= MIN_VALID_EPOCH) {
      waitForClock = true; // deste boot, sem hora: espera o relógio ser acertado
      break;
    }
    batch[info->count++] = r;
    info->endPos = afterLine;
  }
  info->toEnd = !waitForClock && !f.available();
  f.close();

  if (info->dropped > 0) {
    Serial.printf("[SD] %d linha(s) antiga(s) sem identificação/hora descartada(s).\n", info->dropped);
  }
  return true;
}

// O lote foi entregue: avança sd_pos ou, no fim do arquivo, zera o log.
void commitSDBatch(const SDBatchInfo& info) {
  if (info.toEnd) {
    resetSDLog();
  } else if (info.endPos != sdSentPos) {
    saveSentPos(info.endPos);
  }
}

// Retorna false se o envio falhou (o loop() espera SD_FLUSH_RETRY_MS para tentar de novo)
bool flushSDStep() {
  if (!sdReady || !pendingSDData) return true;

  SDReading batch[UPLOAD_BATCH];
  SDBatchInfo info;
  if (!readSDBatch(batch, UPLOAD_BATCH, &info)) return false;

  if (info.count > 0 && !uploadRecords(batch, info.count)) {
    Serial.println("[SD] Envio dos registros guardados falhou — tenta de novo mais tarde.");
    return false;
  }
  commitSDBatch(info);

  if (info.count > 0) {
    Serial.printf("[SD] %d registro(s) guardado(s) enviado(s) ao Supabase%s.\n",
                  info.count, info.toEnd ? " — cartão em dia" : "");
  }
  return true;
}

// ── Bluetooth (BLE) ────────────────────────────────────────────────────────
// Envia as leituras ao celular conectado (com ou sem Wi-Fi) e recebe os
// comandos do app.
class BleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    Serial.println("[BLE] Cliente conectado.");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    bleRxLen = 0;        // descarta comando incompleto
    bleRxOverflow = false;
    Serial.println("[BLE] Cliente desconectado — voltando a anunciar.");
    NimBLEDevice::startAdvertising();
  }
};

// Recebe os pedaços escritos pelo app no RX e monta linhas completas
class BleRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = c->getValue();
    for (size_t i = 0; i < value.length(); i++) {
      char ch = (char)value.data()[i];
      if (ch == '\r') continue;

      if (ch == '\n') {
        if (!bleRxOverflow && bleRxLen > 0) {
          bleRxLine[bleRxLen] = '\0';
          if (xQueueSend(bleCmdQueue, bleRxLine, 0) != pdTRUE) {
            Serial.println("[BLE] Fila de comandos cheia — comando descartado.");
          }
        }
        bleRxLen = 0;
        bleRxOverflow = false;
      } else if (bleRxLen < BLE_CMD_MAX_LEN - 1) {
        bleRxLine[bleRxLen++] = ch;
      } else {
        bleRxOverflow = true; // linha grande demais: ignora até o próximo '\n'
      }
    }
  }
};

void setupBLE() {
  if (!NimBLEDevice::init(deviceName.c_str())) {
    Serial.println("[BLE] Falha ao iniciar o Bluetooth.");
    return;
  }

  bleCmdQueue = xQueueCreate(BLE_CMD_QUEUE_LEN, BLE_CMD_MAX_LEN);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());

  NimBLEService* service = bleServer->createService(BLE_SERVICE_UUID);

  // TX: o NimBLE já cria o descritor de notificação (0x2902) sozinho
  bleTx = service->createCharacteristic(
    BLE_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
  );

  // RX: comandos do app (Wi-Fi, estado, hora, nome, intervalo, reinício)
  NimBLECharacteristic* bleRx = service->createCharacteristic(
    BLE_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  bleRx->setCallbacks(new BleRxCallbacks());

  bleServer->start();

  // UUID do serviço no anúncio; o nome vai na resposta ao scan (não cabem os dois)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->setName(deviceName.c_str());
  NimBLEDevice::startAdvertising();

  Serial.printf("[BLE] Anunciando como \"%s\"\n", deviceName.c_str());
  Serial.printf("[Mem] Livre após iniciar o Bluetooth: %u bytes, maior bloco: %u bytes\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Envia uma linha JSON pelo TX, em pedaços para funcionar mesmo com o MTU
// padrão; o app (e os apps de UART) juntam os pedaços até o '\n'.
void notifyBLELine(const String& json) {
  if (!bleTx || !bleConnected) return;

  String line = json + "\n";
  for (size_t i = 0; i < line.length(); i += BLE_CHUNK_SIZE) {
    size_t len = min((size_t)BLE_CHUNK_SIZE, line.length() - i);
    bleTx->setValue((uint8_t*)line.c_str() + i, len);
    bleTx->notify();
    delay(10);
  }
}

void sendBLE(const char* json) {
  if (!bleTx) return;

  notifyBLELine(String(json));
  bleTx->setValue(String(json)); // última leitura completa, disponível via READ
}

void sendBLEJson(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  notifyBLELine(out);
}

void sendBLEError(const char* message) {
  JsonDocument doc;
  doc["type"]    = "error";
  doc["message"] = message;
  sendBLEJson(doc);
}

// Um registro para o app, que o guarda no banco local e o envia ao Supabase
// se a placa não conseguir. live: o registro que acabou de ser feito (fica
// também como o valor lido pelo READ); senão, um registro antigo do SD.
void sendRecordBLE(const SDReading& r, bool live) {
  JsonDocument payload;
  payload["deviceId"] = deviceId;
  payload["name"]     = deviceName;
  payload["tempDHT"]  = r.t;
  payload["umidade"]  = r.h;
  payload["boot"]     = r.boot;
  payload["seq"]      = r.seq;
  payload["rec"]      = true;   // marca de registro: o app só guarda/envia o que tem isso
  if (r.ts) payload["ts"] = (long long)r.ts;

  char buffer[250];
  serializeJson(payload, buffer, sizeof(buffer));
  if (live) {
    sendBLE(buffer);
  } else {
    notifyBLELine(String(buffer));
  }
}

// ── Despejo do SD pelo Bluetooth ───────────────────────────────────────────
// Sem Wi-Fi (ou com o Supabase fora do alcance), o celular conectado leva os
// registros guardados no SD. Ao conectar, o app pede {"cmd":"dump"}; a placa
// manda um lote de registros (iguais aos ao vivo, com "rec": true) seguido
// de {"type":"dump","pos":N,"count":K,"left":<bytes que faltam>}. O app
// guarda o lote no banco dele e responde {"cmd":"dumpAck","pos":N}; só então
// sd_pos avança e sai o próximo lote. Se a conexão cair antes, o mesmo lote
// vai de novo na próxima (o app e o Supabase ignoram registros repetidos).
// No fim: {"type":"dump","done":true}. Com o Wi-Fi funcionando, a própria
// placa envia o SD e responde {"type":"dump","state":"wifi"}.
bool cloudWorking() {
  return WiFi.status() == WL_CONNECTED && !lastFlushFailed;
}

void sendDumpDone() {
  JsonDocument doc;
  doc["type"] = "dump";
  doc["done"] = true;
  sendBLEJson(doc);
}

void requestDump() {
  dumpWanted = true;
  if (cloudWorking()) {
    JsonDocument doc;
    doc["type"]  = "dump";
    doc["state"] = "wifi";
    sendBLEJson(doc);
  }
}

// Chamado a cada volta do loop() enquanto o app quer os registros e a placa
// não tem como enviá-los sozinha.
void dumpSDStep() {
  if (dumpAwaitingAck) return;
  if (!sdReady || !pendingSDData) {
    sendDumpDone();
    dumpWanted = false;
    return;
  }

  SDReading batch[DUMP_BATCH];
  SDBatchInfo info;
  if (!readSDBatch(batch, DUMP_BATCH, &info)) {
    sendBLEError("Não foi possível ler o cartão SD.");
    dumpWanted = false;
    return;
  }
  if (info.count == 0) {
    if (info.endPos != sdSentPos || info.toEnd) {
      commitSDBatch(info);   // só linhas descartadas: nada a confirmar
    }
    if (info.toEnd || info.endPos == sdSentPos) {
      // Fim do arquivo, ou só restam registros esperando o relógio
      sendDumpDone();
      dumpWanted = false;
    }
    return;
  }

  for (int i = 0; i < info.count; i++) {
    sendRecordBLE(batch[i], false);
  }
  JsonDocument doc;
  doc["type"]  = "dump";
  doc["pos"]   = info.endPos;
  doc["count"] = info.count;
  doc["left"]  = info.fileSize - info.endPos;
  sendBLEJson(doc);

  dumpFromPos = sdSentPos;
  dumpBatch = info;
  dumpAwaitingAck = true;
  dumpSentAt = millis();
}

void handleDumpAck(uint32_t pos) {
  if (!dumpAwaitingAck || pos != dumpBatch.endPos) return; // confirmação velha ou repetida
  dumpAwaitingAck = false;
  if (sdSentPos != dumpFromPos) return; // o Wi-Fi enviou o SD nesse meio-tempo

  // O lote ia até o fim, mas um registro novo pode ter sido gravado depois:
  // aí só avança, para não apagar o registro novo junto.
  SDBatchInfo info = dumpBatch;
  if (info.toEnd) {
    File f = SD.open(LOG_FILE, FILE_READ);
    if (!f || f.size() != info.fileSize) info.toEnd = false;
    if (f) f.close();
  }
  commitSDBatch(info);
  Serial.printf("[BLE] %d registro(s) guardado(s) entregue(s) ao app%s.\n",
                info.count, info.toEnd ? " — cartão em dia" : "");
}

// ── Configuração de Wi-Fi pelo app (comandos BLE) ──────────────────────────
void sendWifiStatus() {
  prefs.begin("wifi", true);
  String savedSsid = prefs.getString("ssid", "");
  prefs.end();

  bool connected = (WiFi.status() == WL_CONNECTED);

  JsonDocument doc;
  doc["type"]         = "status";
  doc["deviceId"]     = deviceId;
  doc["name"]         = deviceName;
  doc["interval"]     = recordMinutes;
  doc["wifi"]         = connected;
  doc["savedSsid"]    = savedSsid;
  doc["cloud"]        = connected && cloudOk; // último envio ao Supabase deu certo
  doc["provisioning"] = provisioning;
  if (connected) {
    doc["ssid"] = WiFi.SSID();
    doc["ip"]   = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
  }
  sendBLEJson(doc);
}

void sendWifiScan() {
  if (provisioning) {
    sendBLEError("Ainda conectando à rede anterior, aguarde.");
    return;
  }

  // A busca precisa da interface STA ligada (sem credenciais o modo é só AP)
  if (!(WiFi.getMode() & WIFI_MODE_STA)) {
    WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  }
  // Sem conexão a placa fica tentando reconectar e a busca falha; interrompe
  // a tentativa (o loop() volta a tentar depois).
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
  }

  Serial.println("[BLE] Procurando redes WiFi a pedido do app...");
  int n = WiFi.scanNetworks();
  lastWifiAttempt = millis(); // adia a próxima reconexão automática

  JsonDocument doc;
  doc["type"] = "networks";

  if (n < 0) {
    doc["error"] = "Falha ao procurar redes.";
  } else {
    // Mais fortes primeiro; cada SSID aparece uma vez só (vários APs da mesma rede)
    std::vector<int> order;
    for (int i = 0; i < n; i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

    JsonArray list = doc["networks"].to<JsonArray>();
    std::vector<String> seen;
    for (int i : order) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue; // rede oculta
      if (std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
      seen.push_back(ssid);

      JsonObject net = list.add<JsonObject>();
      net["ssid"]   = ssid;
      net["rssi"]   = WiFi.RSSI(i);
      net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      if ((int)seen.size() >= WIFI_SCAN_MAX) break;
    }
  }
  WiFi.scanDelete();

  Serial.printf("[BLE] %d rede(s) encontrada(s).\n", n < 0 ? 0 : n);
  sendBLEJson(doc);
}

// Recebe SSID/senha do app e tenta conectar. As credenciais só são gravadas
// no NVS se a conexão der certo: um erro de digitação não apaga a rede boa.
void startWifiProvisioning(const String& ssid, const String& pass) {
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63) {
    sendBLEError("SSID ou senha inválidos.");
    return;
  }

  Serial.printf("[BLE] App pediu para conectar em \"%s\".\n", ssid.c_str());

  provisionSsid = ssid;
  provisionPass = pass;
  provisioning = true;
  provisionStart = millis();

  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  delay(100);
  lastWifiDisconnectReason = 0;
  WiFi.begin(ssid.c_str(), pass.c_str());

  JsonDocument doc;
  doc["type"]  = "wifi";
  doc["state"] = "connecting";
  doc["ssid"]  = ssid;
  sendBLEJson(doc);
}

// Volta para a rede salva (ou fica sem rede) depois de uma tentativa que falhou
void restoreSavedWifi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  WiFi.disconnect();
  delay(100);
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    lastWifiAttempt = millis();
  }
}

// Chamado a cada volta do loop(): acompanha a tentativa e avisa o app
void updateWifiProvisioning() {
  if (!provisioning) return;

  if (WiFi.status() == WL_CONNECTED) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", provisionSsid);
    prefs.putString("pass", provisionPass);
    prefs.end();

    provisioning = false;
    hasCredentials = true;
    wifiDownSince = 0;
    Serial.printf("[WiFi] Conectado via app a \"%s\": %s\n",
                  provisionSsid.c_str(), WiFi.localIP().toString().c_str());

    JsonDocument doc;
    doc["type"]  = "wifi";
    doc["state"] = "connected";
    doc["ssid"]  = provisionSsid;
    doc["ip"]    = WiFi.localIP().toString();
    sendBLEJson(doc);
    provisionPass = "";
    return;
  }

  uint8_t reason = lastWifiDisconnectReason;
  bool wrongPassword = reason == WIFI_REASON_AUTH_FAIL
                    || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                    || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
  bool timedOut = millis() - provisionStart > WIFI_PROVISION_TIMEOUT_MS;
  if (!wrongPassword && !timedOut) return;

  provisioning = false;
  provisionPass = "";

  JsonDocument doc;
  doc["type"]  = "wifi";
  doc["state"] = "failed";
  doc["ssid"]  = provisionSsid;
  if (wrongPassword) {
    doc["reason"] = "wrong_password";
  } else if (reason == WIFI_REASON_NO_AP_FOUND || (reason >= 210 && reason <= 212)) {
    doc["reason"] = "not_found"; // 210-212: variantes de "AP não encontrado" no IDF 5
  } else {
    doc["reason"] = "timeout";
  }
  Serial.printf("[WiFi] Falha ao conectar em \"%s\" (motivo %u).\n", provisionSsid.c_str(), reason);
  sendBLEJson(doc);

  restoreSavedWifi();
}

// ── Nome, intervalo e reinício da placa (comandos BLE) ─────────────────────
// Lê o nome salvo no NVS (ou monta o padrão "Colmeia-XXXX" a partir do MAC)
void loadDeviceName() {
  prefs.begin("sys", true);
  deviceName = prefs.getString("name", "");
  prefs.end();
  if (deviceName.length() == 0) {
    deviceName = "Colmeia-" + deviceId.substring(deviceId.length() - 4);
  }
}

void renameDevice(String newName) {
  newName.trim();
  if (newName.length() == 0 || newName.length() > BOARD_NAME_MAX) {
    sendBLEError("O nome deve ter de 1 a 24 caracteres.");
    return;
  }
  for (size_t i = 0; i < newName.length(); i++) {
    if ((uint8_t)newName[i] < 0x20 || newName[i] == '"' || newName[i] == '\\') {
      sendBLEError("O nome tem caracteres inválidos.");
      return;
    }
  }

  prefs.begin("sys", false);
  prefs.putString("name", newName);
  prefs.end();
  deviceName = newName;

  // Vale na hora para as leituras; o anúncio BLE usa o nome novo na próxima
  // vez que anunciar (quando o celular desconectar ou a placa reiniciar)
  NimBLEDevice::setDeviceName(deviceName.c_str());
  NimBLEDevice::getAdvertising()->setName(deviceName.c_str());
  Serial.printf("[BLE] Placa renomeada para \"%s\".\n", deviceName.c_str());

  JsonDocument doc;
  doc["type"] = "name";
  doc["name"] = deviceName;
  sendBLEJson(doc);
}

void loadRecordInterval() {
  prefs.begin("sys", true);
  recordMinutes = prefs.getUShort("rec_min", RECORD_MINUTES_DEFAULT);
  prefs.end();
  if (recordMinutes < 1 || recordMinutes > RECORD_MINUTES_MAX) {
    recordMinutes = RECORD_MINUTES_DEFAULT;
  }
}

// Muda de quanto em quanto tempo a placa grava um registro. Vale a partir
// do registro em andamento (que fecha quando completar o novo intervalo).
void setRecordInterval(int minutes) {
  if (minutes < 1 || minutes > RECORD_MINUTES_MAX) {
    sendBLEError("O intervalo deve ser de 1 a 60 minutos.");
    return;
  }

  recordMinutes = minutes;
  prefs.begin("sys", false);
  prefs.putUShort("rec_min", recordMinutes);
  prefs.end();
  Serial.printf("[Registro] Intervalo alterado para %u min.\n", recordMinutes);

  JsonDocument doc;
  doc["type"]    = "interval";
  doc["minutes"] = recordMinutes;
  sendBLEJson(doc);
}

void requestRestart() {
  Serial.println("[Sistema] Reinício pedido pelo app.");
  JsonDocument doc;
  doc["type"] = "restart";
  sendBLEJson(doc);
  restartAt = millis() + RESTART_DELAY_MS; // o loop() reinicia, depois da resposta sair
}

// Executa os comandos que chegaram pelo BLE (fora da tarefa do Bluetooth)
void processBleCommands() {
  if (!bleCmdQueue) return;

  char line[BLE_CMD_MAX_LEN];
  while (xQueueReceive(bleCmdQueue, line, 0) == pdTRUE) {
    JsonDocument cmd;
    if (deserializeJson(cmd, line)) {
      sendBLEError("Comando inválido (JSON).");
      continue;
    }

    String name = cmd["cmd"] | "";
    if (name == "status") {
      sendWifiStatus();
    } else if (name == "scan") {
      sendWifiScan();
    } else if (name == "wifi") {
      startWifiProvisioning(cmd["ssid"] | "", cmd["pass"] | "");
    } else if (name == "time") {
      setClockFromApp((time_t)(cmd["ts"] | (int64_t)0));
    } else if (name == "name") {
      renameDevice(cmd["name"] | "");
    } else if (name == "restart") {
      requestRestart();
    } else if (name == "interval") {
      setRecordInterval(cmd["minutes"] | 0);
    } else if (name == "dump") {
      requestDump();
    } else if (name == "dumpAck") {
      handleDumpAck(cmd["pos"] | (uint32_t)0);
    } else {
      sendBLEError("Comando desconhecido.");
    }
  }
}

// ── setup() ────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  dht.begin();

  // Conta os boots: boot + seq identificam cada registro, e registros do SD
  // sem hora só podem ter a hora calculada pelo millis() se forem do boot
  // atual. Com o NVS vazio (placa nova ou flash apagado) começa de um número
  // aleatório grande, para nunca repetir um (boot, seq) que o banco já tem
  // (um registro repetido seria descartado como duplicado).
  prefs.begin("sys", false);
  if (prefs.isKey("boot")) {
    bootCount = prefs.getUInt("boot", 0) + 1;
  } else {
    bootCount = 1000000UL + (esp_random() % 1000000000UL); // cabe no integer do Postgres
  }
  prefs.putUInt("boot", bootCount);
  prefs.end();

  sdReady = setupSD();

  // Guarda o motivo de cada queda do Wi-Fi (para dizer ao app se a senha
  // está errada ou a rede não foi encontrada)
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  connectWiFi(); // não bloqueia: abre o hotspot se não houver rede utilizável

  // Hora pela internet: o SNTP sincroniza sozinho sempre que houver Wi-Fi.
  // (Precisa vir depois do connectWiFi(), que inicializa a pilha de rede.)
  sntp_set_time_sync_notification_cb([](struct timeval* tv) {
    ntpSynced = true; // a partir daqui a hora do celular é ignorada
  });
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);

  // O ID vem depois do connectWiFi(): o MAC só é lido corretamente com o
  // Wi-Fi já inicializado.
  deviceId = getDeviceId();
  loadDeviceName();
  loadRecordInterval();
  Serial.printf("[Registro] Um registro a cada %u min.\n", recordMinutes);
  Serial.printf("[ID] Dispositivo: %s (\"%s\")\n", deviceId.c_str(), deviceName.c_str());

  setupBLE();

  lastRecordAt = millis(); // primeiro registro daqui a recordMinutes
}

// ── Registro do intervalo ──────────────────────────────────────────────────
// Média das leituras, enviada ao app (BLE) e ao Supabase; sem internet
// fica no SD até poder subir (mesmo que o app a tenha recebido: só o
// despejo confirmado tira um registro do SD).
void saveRecord(time_t ts, bool wifiOk) {
  if (recordCount == 0) return; // nenhuma leitura válida no último intervalo

  SDReading r;
  r.ts   = ts;
  r.boot = bootCount;
  r.ms   = millis();
  r.seq  = ++readingSeq;
  r.t    = roundf(recordSumT / recordCount * 100) / 100;
  r.h    = roundf(recordSumH / recordCount * 100) / 100;
  recordSumT = recordSumH = 0;
  recordCount = 0;

  sendRecordBLE(r, true);

  // Sem hora certa o banco não aceita: fica no SD até o relógio ser acertado.
  // (O que já estava no SD é enviado aos poucos pelo loop(), em flushSDStep().)
  bool uploaded = wifiOk && r.ts > MIN_VALID_EPOCH && uploadRecords(&r, 1);
  if (uploaded) {
    Serial.printf("[Nuvem] Registro %lu enviado: %.2f°C, %.2f%%\n", (unsigned long)r.seq, r.t, r.h);
  } else {
    logToSD(r);
  }
}

// ── loop() ─────────────────────────────────────────────────────────────────
void loop() {
  // Mantém o portal de configuração respondendo, se estiver ativo
  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  // Abre/fecha o hotspot conforme o estado da rede salva
  manageWifiFallback();

  // Comandos do app via Bluetooth
  processBleCommands();
  updateWifiProvisioning();

  // Despejo do SD pelo Bluetooth: termina com a conexão; um lote sem
  // confirmação é enviado de novo depois do tempo limite.
  if (!bleConnected) {
    dumpWanted = false;
    dumpAwaitingAck = false;
  } else if (dumpAwaitingAck && millis() - dumpSentAt >= DUMP_ACK_TIMEOUT_MS) {
    Serial.println("[BLE] O app não confirmou o lote — enviando de novo.");
    dumpAwaitingAck = false;
  }
  if (dumpWanted && !cloudWorking()) {
    dumpSDStep();
  }

  // Reinício pedido pelo app (espera a resposta BLE sair antes)
  if (restartAt != 0 && (long)(millis() - restartAt) >= 0) {
    ESP.restart();
  }

  // Espera o intervalo de leitura sem travar o portal acima
  if (millis() - lastSensorRead < SENSOR_INTERVAL_MS) {
    return;
  }
  lastSensorRead = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  // Hora real da leitura (0 = relógio ainda não acertado)
  time_t ts = timeIsValid() ? time(nullptr) : 0;
  if (ts && !clockWasValid) {
    Serial.printf("[Hora] Relógio acertado: %s\n", formatTime(ts).c_str());
  }
  clockWasValid = (ts != 0);

  if (isnan(t) || isnan(h)) {
    Serial.println("Erro ao ler DHT!");
  } else {
    Serial.printf("[%s] Temp DHT: %.1f°C | Umidade: %.1f%%\n", formatTime(ts).c_str(), t, h);
    recordSumT += t;
    recordSumH += h;
    recordCount++;

    // Bluetooth: valor ao vivo para a tela do app (sem seq: não é guardado)
    JsonDocument live;
    live["deviceId"] = deviceId;
    live["name"]     = deviceName;
    live["tempDHT"]  = t;
    live["umidade"]  = h;
    if (ts) live["ts"] = (long long)ts;

    char liveBuffer[200];
    serializeJson(live, liveBuffer, sizeof(liveBuffer));
    sendBLE(liveBuffer);
  }

  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  // Só tenta reconectar se já existirem credenciais salvas (e se o app não
  // estiver testando uma rede nova agora)
  if (!provisioning && hasCredentials && !wifiOk && millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
    Serial.println("[WiFi] Tentando reconectar...");
    WiFi.reconnect();
    lastWifiAttempt = millis();

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
      delay(200);
    }
    wifiOk = (WiFi.status() == WL_CONNECTED);
  }
  if (!wifiOk) {
    cloudOk = false;
  }

  // Envia aos poucos o que ficou no SD (um lote por vez), assim que houver
  // Wi-Fi, sem esperar o próximo registro (nem mexer no arquivo enquanto um
  // lote do despejo pelo Bluetooth espera confirmação)
  unsigned long flushWait = lastFlushFailed ? SD_FLUSH_RETRY_MS : SD_FLUSH_INTERVAL_MS;
  if (wifiOk && pendingSDData && !dumpAwaitingAck && timeIsValid() && millis() - lastFlushAt >= flushWait) {
    lastFlushAt = millis();
    lastFlushFailed = !flushSDStep();
  }

  if (millis() - lastRecordAt >= (unsigned long)recordMinutes * 60000UL) {
    lastRecordAt = millis();
    saveRecord(ts, wifiOk);
  }
}
