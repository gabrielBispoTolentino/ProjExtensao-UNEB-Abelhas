// Testes do firmware rodando no PC: o ColmeiaMonitor.ino de verdade
// (convertido por gen_sketch.py) sobre a ESP32 simulada de shim/, com um
// celular simulado que faz o que o app faz: conecta, negocia o MTU, manda
// "time" e "dump", junta as linhas das notificações e confirma cada lote.
//
//   fw_sim <cenário> <pasta de trabalho>
//
// Cada cenário roda num processo novo (o sketch tem estado global); o de
// reinício usa dois processos sobre a mesma pasta (cartão SD + NVS).

#include "sketch.cpp"

#include <filesystem>
#include <set>

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                                \
    do {                                                                               \
        if (cond) {                                                                    \
            std::printf("  ok    %s\n", #cond);                                        \
        } else {                                                                       \
            ++failures;                                                                \
            std::printf("  FAIL  %s  (%s:%d) ", #cond, __FILE__, __LINE__);           \
            std::printf(__VA_ARGS__);                                                  \
            std::printf("\n");                                                         \
        }                                                                              \
    } while (0)

int64_t phoneEpochAtZero = 1792590000;         // relógio do celular (certo)
int64_t phoneNow() { return phoneEpochAtZero + int64_t(sim::nowMs / 1000); }

// ── Celular simulado (o lado do app) ────────────────────────────────────────
struct Phone
{
    bool connected = false;
    uint16_t mtu = 23;
    std::string buffer;
    std::map<std::pair<uint32_t, uint32_t>, int64_t> records;   // (boot, seq) -> ts (0 = sem hora)
    int duplicates = 0;
    int brokenLines = 0;
    int sdRecords = 0;                 // registros com "sd": true (do despejo)
    int liveRecords = 0;
    std::vector<int> batches;          // "count" de cada fim de lote recebido
    int done = 0;
    int wifiAnswers = 0;
    // Comportamento
    int skipAcks = 0;                  // os próximos N lotes não são confirmados
    int disconnectAfterBatch = -1;     // desconecta ao receber este lote (1 = o primeiro), sem confirmar
    uint64_t ackDelayMs = 0;           // confirma depois deste tempo
    std::vector<std::pair<uint64_t, uint32_t>> pendingAcks;   // (quando, pos)
    uint64_t firstBatchAt = 0, doneAt = 0;

    void connect(uint16_t negotiatedMtu)
    {
        NimBLEConnInfo info;
        sim::bleServerCallbacks->onConnect(NimBLEDevice::createServer(), info);
        if (negotiatedMtu > 23) {
            info.mtu = negotiatedMtu;
            sim::bleServerCallbacks->onMTUChange(negotiatedMtu, info);
        }
        mtu = negotiatedMtu;
        connected = true;
        buffer.clear();
        // O que BleSensorClient faz ao conectar.
        send("{\"cmd\":\"time\",\"ts\":" + std::to_string(phoneNow()) + "}");
        send("{\"cmd\":\"dump\"}");
    }

    void disconnect()
    {
        NimBLEConnInfo info;
        connected = false;
        pendingAcks.clear();
        sim::bleServerCallbacks->onDisconnect(NimBLEDevice::createServer(), info, 0x13);
    }

    // Comando para o RX da placa, em pedaços do MTU (como sendCommand do app).
    void send(const std::string &json)
    {
        const std::string line = json + "\n";
        const size_t chunk = std::clamp<size_t>(mtu - 3, 20, 180);
        NimBLECharacteristic *rx = sim::characteristics[BLE_RX_UUID];
        NimBLEConnInfo info;
        for (size_t i = 0; i < line.size(); i += chunk) {
            rx->value = line.substr(i, chunk);
            rx->callbacks->onWrite(rx, info);
        }
    }

    void onNotify(const uint8_t *data, size_t length)
    {
        if (!connected) return;
        buffer.append(reinterpret_cast<const char *>(data), length);
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            handleLine(line);
        }
    }

    void handleLine(const std::string &line)
    {
        JsonDocument doc;
        if (deserializeJson(doc, line)) {
            ++brokenLines;
            return;
        }
        if (doc["rec"] | false) {
            (doc["sd"] | false) ? ++sdRecords : ++liveRecords;
            const auto key = std::make_pair(doc["boot"].as<uint32_t>(), doc["seq"].as<uint32_t>());
            if (records.count(key)) ++duplicates;
            records[key] = doc["ts"] | int64_t(0);
            return;
        }
        const std::string type = doc["type"] | "";
        if (type != "dump") return;
        if (doc["done"] | false) {
            ++done;
            doneAt = sim::nowMs;
        } else if (doc["state"] == "wifi") {
            ++wifiAnswers;
        } else if (doc["pos"].is<uint32_t>()) {
            batches.push_back(doc["count"] | 0);
            if (firstBatchAt == 0) firstBatchAt = sim::nowMs;
            if (int(batches.size()) == disconnectAfterBatch) {
                disconnect();
                return;
            }
            if (skipAcks > 0) {
                --skipAcks;
                return;
            }
            pendingAcks.push_back({sim::nowMs + ackDelayMs, doc["pos"].as<uint32_t>()});
        }
    }

    void poll()
    {
        for (auto it = pendingAcks.begin(); it != pendingAcks.end();) {
            if (sim::nowMs >= it->first) {
                const uint32_t pos = it->second;
                it = pendingAcks.erase(it);
                send("{\"cmd\":\"dumpAck\",\"pos\":" + std::to_string(pos) + "}");
            } else {
                ++it;
            }
        }
    }
};

Phone phone;

void runFor(uint64_t ms, uint64_t stepMs = 20)
{
    const uint64_t end = sim::nowMs + ms;
    while (sim::nowMs < end) {
        loop();
        phone.poll();
        sim::nowMs += stepMs;
    }
}

// Roda até o celular receber o fim do despejo (ou o tempo acabar).
bool runUntilDone(uint64_t maxMs)
{
    const uint64_t end = sim::nowMs + maxMs;
    const int before = phone.done;
    while (sim::nowMs < end && phone.done == before) {
        loop();
        phone.poll();
        sim::nowMs += 5;
    }
    return phone.done > before;
}

std::string readFile(const std::string &path)
{
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
        std::fclose(f);
    }
    return out;
}

std::string sdLog() { return readFile(sim::sdDir + LOG_FILE); }
int sdRecordLines() { int n = -1; for (char c : sdLog()) n += c == '\n'; return n; }   // sem o cabeçalho
std::string undatedFile() { return readFile(sim::sdDir + UNDATED_FILE); }
int undatedLines() { const std::string f = undatedFile(); int n = f.empty() ? 0 : -1; for (char c : f) n += c == '\n'; return n; }
int countInLog(const char *needle) { int n = 0; for (size_t p = 0; (p = sim::log.find(needle, p)) != std::string::npos; ++p) ++n; return n; }

// Estado salvo entre os dois processos de um cenário com reinício.
void saveState(const std::string &dir, const std::map<std::string, int64_t> &values)
{
    FILE *f = std::fopen((dir + "/state.txt").c_str(), "wb");
    for (const auto &kv : values) std::fprintf(f, "%s %lld\n", kv.first.c_str(), (long long)kv.second);
    std::fclose(f);
}
std::map<std::string, int64_t> loadState(const std::string &dir)
{
    std::map<std::string, int64_t> values;
    if (FILE *f = std::fopen((dir + "/state.txt").c_str(), "rb")) {
        char key[64];
        long long value;
        while (std::fscanf(f, "%63s %lld", key, &value) == 2) values[key] = value;
        std::fclose(f);
    }
    return values;
}

// Todos os registros deste boot, 1..readingSeq, chegaram ao celular com hora?
std::set<uint32_t> missingSeqs(uint32_t boot, uint32_t from, uint32_t to)
{
    std::set<uint32_t> missing;
    for (uint32_t seq = from; seq <= to; ++seq) {
        const auto it = phone.records.find({boot, seq});
        if (it == phone.records.end() || it->second <= MIN_VALID_EPOCH) missing.insert(seq);
    }
    return missing;
}

void boot(const std::string &dir, bool fresh, bool savedWifi = false)
{
    namespace fs = std::filesystem;
    if (fresh) fs::remove_all(dir);
    fs::create_directories(dir + "/sd");
    sim::sdDir = dir + "/sd";
    sim::prefsFile = dir + "/nvs.txt";
    sim::loadPrefs();
    if (savedWifi) {
        sim::prefs["wifi/ssid"] = "RedeTeste";
        sim::prefs["wifi/pass"] = "senha123";
        sim::savePrefs();
    }
    sim::onNotify = [](const uint8_t *d, size_t n) { phone.onNotify(d, n); };
    setup();
}

void printRate(const char *what, size_t count)
{
    const double secs = double(phone.doneAt - phone.firstBatchAt) / 1000.0;
    std::printf("  info  %s: %zu registros em %.1f s de tempo simulado (%.0f/s pelo ritmo do firmware)\n",
                what, count, secs, secs > 0 ? count / secs : 0.0);
}

// ── Cenários ────────────────────────────────────────────────────────────────

// Placa sem Wi-Fi e sem hora grava 10 registros; o celular conecta (MTU
// padrão), acerta a hora e recebe todos, datados; o cartão fica em dia.
void offlineDumpDefaultMtu(const std::string &dir)
{
    boot(dir, true);
    runFor(10 * 60000 + 1000);
    CHECK(readingSeq == 10, "readingSeq=%u", readingSeq);
    CHECK(sdRecordLines() == 10, "lines=%d", sdRecordLines());
    CHECK(sdLog().find("sem_hora") != std::string::npos, "records saved without a clock");

    phone.connect(23);
    CHECK(runUntilDone(60000), "dump finished");
    CHECK(missingSeqs(bootCount, 1, 10).empty(), "missing=%zu", missingSeqs(bootCount, 1, 10).size());
    CHECK(phone.brokenLines == 0, "broken=%d", phone.brokenLines);
    CHECK(phone.batches.size() == 1 && phone.batches[0] == 10, "batches=%zu", phone.batches.size());
    CHECK(sdRecordLines() == 0, "card emptied, lines=%d", sdRecordLines());
    CHECK(sdSentPos == 0 && !pendingSDData, "sd_pos=%u", sdSentPos);
    CHECK(sim::biggestNotify <= 20, "default MTU: 20-byte chunks, biggest=%zu", sim::biggestNotify);
    CHECK(phone.sdRecords == 10, "dumped records carry \"sd\": true, got %d", phone.sdRecords);
    // Hora calculada pelo millis(): o registro 1 foi feito ~10 min antes da conexão.
    const int64_t ts1 = phone.records[{bootCount, 1}];
    CHECK(std::llabs(ts1 - (phoneNow() - 9 * 60)) < 90, "ts1 off by %lld s", (long long)(ts1 - (phoneNow() - 9 * 60)));
}

// 3 h de registros; MTU 247: lotes de 25, um registro por notificação.
void largeBacklogBigMtu(const std::string &dir)
{
    boot(dir, true);
    runFor(180 * 60000 + 1000, 50);
    CHECK(readingSeq == 180, "readingSeq=%u", readingSeq);
    phone.connect(247);
    CHECK(runUntilDone(5 * 60000), "dump finished");
    CHECK(missingSeqs(bootCount, 1, 180).empty(), "missing=%zu", missingSeqs(bootCount, 1, 180).size());
    CHECK(phone.batches.size() == 8, "batches=%zu (7 x 25 + 5)", phone.batches.size());
    CHECK(phone.batches.front() == DUMP_BATCH, "first batch=%d", phone.batches.front());
    CHECK(sim::biggestNotify > 100 && sim::biggestNotify <= 244, "one record per notification, biggest=%zu", sim::biggestNotify);
    CHECK(phone.brokenLines == 0, "broken=%d", phone.brokenLines);
    CHECK(sdRecordLines() == 0, "card emptied");
    printRate("MTU 247", phone.records.size());
}

// O mesmo despejo com o MTU padrão, para comparar o ritmo.
void largeBacklogDefaultMtu(const std::string &dir)
{
    boot(dir, true);
    runFor(180 * 60000 + 1000, 50);
    phone.connect(23);
    CHECK(runUntilDone(10 * 60000), "dump finished");
    CHECK(missingSeqs(bootCount, 1, 180).empty(), "missing=%zu", missingSeqs(bootCount, 1, 180).size());
    CHECK(phone.brokenLines == 0, "broken=%d", phone.brokenLines);
    printRate("MTU 23", phone.records.size());
}

// Lote sem confirmação: a placa manda o mesmo de novo depois de 15 s.
void unacknowledgedBatchIsResent(const std::string &dir)
{
    boot(dir, true);
    runFor(30 * 60000 + 1000, 50);
    phone.skipAcks = 1;
    phone.connect(247);
    CHECK(runUntilDone(2 * 60000), "dump finished");
    CHECK(missingSeqs(bootCount, 1, 30).empty(), "missing=%zu", missingSeqs(bootCount, 1, 30).size());
    CHECK(phone.duplicates == DUMP_BATCH, "the unacked batch came twice, duplicates=%d", phone.duplicates);
    CHECK(phone.batches.size() == 3, "batches=%zu (25 lost ack, 25 again, 5)", phone.batches.size());
    CHECK(sim::log.find("não confirmou o lote") != std::string::npos, "timeout logged");
    CHECK(sdRecordLines() == 0, "card emptied");
}

// Conexão cai no meio: a próxima continua do último lote confirmado.
void disconnectMidwayResumes(const std::string &dir)
{
    boot(dir, true);
    runFor(100 * 60000 + 1000, 50);
    phone.disconnectAfterBatch = 3;   // 2 confirmados; cai ao receber o 3º
    phone.connect(247);
    runFor(20000, 5);
    CHECK(!phone.connected, "phone disconnected");
    CHECK(!dumpWanted && !dumpAwaitingAck, "dump state reset on disconnect");
    CHECK(sdSentPos > 0, "position saved after 2 batches, sd_pos=%u", sdSentPos);

    runFor(5 * 60000, 50);            // mais registros enquanto ninguém está conectado
    const uint32_t last = readingSeq;
    phone.disconnectAfterBatch = -1;
    phone.connect(247);
    CHECK(runUntilDone(2 * 60000), "dump finished after reconnecting");
    CHECK(missingSeqs(bootCount, 1, last).empty(), "missing=%zu of %u", missingSeqs(bootCount, 1, last).size(), last);
    CHECK(phone.duplicates == DUMP_BATCH, "only the unacked batch came twice, duplicates=%d", phone.duplicates);
    CHECK(sdRecordLines() == 0, "card emptied");
}

// Registro novo gravado enquanto o último lote espera confirmação: o
// cartão não pode ser zerado junto (o registro novo se perderia).
void recordWrittenWhileWaitingForAck(const std::string &dir)
{
    boot(dir, true);
    runFor(10 * 60000 + 1000, 50);
    phone.ackDelayMs = 70000;          // o próximo registro (1/min) sai antes da confirmação
    phone.connect(247);
    runFor(75000, 10);
    phone.ackDelayMs = 0;
    phone.disconnect();                // o registro ao vivo chegou; tira o celular para ver o cartão
    const uint32_t last = readingSeq;
    CHECK(last == 11, "a record was made during the wait, readingSeq=%u", last);
    CHECK(sdRecordLines() >= 1, "card not wiped: %d line(s) left", sdRecordLines());
    CHECK(pendingSDData, "still pending");

    phone.records.clear();
    phone.connect(247);
    CHECK(runUntilDone(60000), "dump finished");
    CHECK(phone.records.count({bootCount, last}) == 1, "record %u delivered", last);
    CHECK(sdRecordLines() == 0, "card emptied");
}

// Com Wi-Fi funcionando, a placa envia o cartão sozinha e recusa o despejo.
void wifiFirst(const std::string &dir)
{
    boot(dir, true, true);             // rede salva, mas fora de alcance no boot
    runFor(20 * 60000 + 1000, 50);
    CHECK(readingSeq == 20 && sdRecordLines() == 20, "readingSeq=%u lines=%d", readingSeq, sdRecordLines());
    settimeofday(new sim_timeval{phoneNow(), 0}, nullptr);   // hora pelo NTP quando a rede voltar
    ntpSynced = true;
    sim::networkInRange = true;
    sim::cloudStatus = 201;
    runFor(2 * 60000, 20);             // reconecta e envia o cartão em lotes de 25
    CHECK(sdRecordLines() == 0, "uploaded by the board itself, lines=%d", sdRecordLines());
    phone.connect(247);
    runFor(10000, 5);
    CHECK(phone.wifiAnswers == 1, "phone told the board uses Wi-Fi, answers=%d", phone.wifiAnswers);
    CHECK(phone.batches.empty(), "no batches over BLE, got %zu", phone.batches.size());
    std::set<uint32_t> uploaded;
    for (const std::string &body : sim::cloudBodies) {
        JsonDocument rows;
        deserializeJson(rows, body);
        for (JsonObject row : rows.as<JsonArray>()) {
            if (row["boot"].as<uint32_t>() == bootCount) uploaded.insert(row["seq"].as<uint32_t>());
        }
    }
    CHECK(uploaded.size() >= readingSeq, "cloud got %zu of this boot's records", uploaded.size());
}

// Wi-Fi conectado, mas o Supabase fora do ar: o celular leva o cartão.
void wifiUpCloudDown(const std::string &dir)
{
    boot(dir, true, true);
    settimeofday(new sim_timeval{phoneNow(), 0}, nullptr);
    ntpSynced = true;
    sim::networkInRange = true;
    sim::cloudStatus = 503;
    runFor(10 * 60000 + 1000, 50);
    CHECK(sdRecordLines() == 10, "records kept on the card, lines=%d", sdRecordLines());
    CHECK(lastFlushFailed, "flush failed");
    phone.connect(247);
    CHECK(runUntilDone(60000), "dump finished over BLE");
    CHECK(missingSeqs(bootCount, 1, 10).empty(), "missing=%zu", missingSeqs(bootCount, 1, 10).size());
}

// Durante o despejo a placa não para (até 5 s) para tentar o Wi-Fi.
void noWifiRetriesDuringDump(const std::string &dir)
{
    boot(dir, true, true);             // rede salva, fora de alcance
    runFor(150 * 60000 + 1000, 50);
    const int before = sim::wifiReconnects;
    CHECK(before > 0, "board retries Wi-Fi while idle, retries=%d", before);
    phone.connect(23);                 // MTU padrão: despejo longo (~1 min)
    const uint64_t start = sim::nowMs;
    CHECK(runUntilDone(5 * 60000), "dump finished");
    const uint64_t took = sim::nowMs - start;
    CHECK(sim::wifiReconnects == before, "no retries during %.0f s of dump, retries=%d", took / 1000.0, sim::wifiReconnects - before);
    phone.disconnect();
    runFor(2 * 60000, 50);
    CHECK(sim::wifiReconnects > before, "retries resume after the dump");
}

// notify() sem buffer livre de vez em quando: nenhuma linha estragada.
void notifyFailuresAreRetried(const std::string &dir)
{
    boot(dir, true);
    runFor(40 * 60000 + 1000, 50);
    sim::notifyFailEvery = 9;          // 1 em cada 9 notificações falha na 1ª tentativa
    phone.connect(23);
    runUntilDone(2 * 60000);
    const int injected = sim::notifyFailed;
    sim::notifyFailEvery = 0;
    CHECK(injected > 20, "injected %d failures", injected);
    CHECK(phone.done == 1, "dump finished");
    CHECK(phone.brokenLines == 0, "no broken line, broken=%d", phone.brokenLines);
    CHECK(missingSeqs(bootCount, 1, 40).empty(), "missing=%zu", missingSeqs(bootCount, 1, 40).size());
}

// Linhas antigas no cartão: com hora vão; sem hora de outro boot e
// corrompidas são descartadas (não podem ir para o banco).
void oldLinesOnTheCard(const std::string &dir)
{
    namespace fs = std::filesystem;
    fs::remove_all(dir);
    fs::create_directories(dir + "/sd");
    {
        FILE *f = std::fopen((dir + "/sd/datalog.txt").c_str(), "wb");
        std::fputs(SD_LOG_HEADER "\r\n", f);
        std::fputs("1792500000,13/10/2026 12:00:00,7,60000,1,31.00,55.00\r\n", f);   // boot 7, com hora: vai
        std::fputs("0,sem_hora,8,60000,1,31.00,55.00\r\n", f);                       // boot 8 sem hora: descarta
        std::fputs("1792500060,13/10/2026 12:01:00,7,120000,2,31.10,55.10\r\n", f);  // vai
        std::fputs("lixo,,quebrado\r\n", f);                                         // descarta
        std::fputs("120000,31.20,55.20\r\n", f);                                     // formato antigo, sem seq: descarta
        std::fclose(f);
    }
    boot(dir, false);
    CHECK(pendingSDData, "old lines detected at boot");
    phone.connect(247);
    CHECK(runUntilDone(60000), "dump finished");
    CHECK(phone.records.count({7, 1}) && phone.records.count({7, 2}), "dated lines from boot 7 delivered");
    CHECK(phone.records.count({8, 1}) == 0, "undatable line not sent");
    CHECK(undatedLines() == 1 && undatedFile().find(",8,60000,1,") != std::string::npos,
          "undatable line kept in " UNDATED_FILE ": %d line(s)", undatedLines());
    CHECK(phone.brokenLines == 0, "broken=%d", phone.brokenLines);
    CHECK(sim::log.find("corrompida") != std::string::npos, "corrupt lines logged");
    CHECK(sdRecordLines() == 0, "card emptied");
}

// Reinício, parte 1: hora certa desde o início (o celular passou antes), 60
// registros; o celular leva 2 lotes e cai no 3º. Parte 2 (outro processo,
// mesma pasta): a placa reinicia e o resto chega, sem repetir os confirmados.
void rebootPart1(const std::string &dir)
{
    boot(dir, true);
    phone.connect(247);                // só acerta a hora
    runFor(2000, 5);
    phone.disconnect();
    runFor(60 * 60000 + 1000, 50);
    phone.disconnectAfterBatch = 3;
    phone.connect(247);
    runFor(20000, 5);
    CHECK(sdSentPos > 0, "position saved, sd_pos=%u", sdSentPos);
    CHECK(sim::prefs["sys/sd_pos"] == std::to_string(sdSentPos), "sd_pos in NVS");
    std::printf("  info  part 1: boot %u, %u records, %zu delivered before the reboot\n",
                bootCount, readingSeq, phone.records.size());
    FILE *f = std::fopen((dir + "/part1.txt").c_str(), "wb");
    std::fprintf(f, "%u %u", bootCount, readingSeq);
    std::fclose(f);
}

void rebootPart2(const std::string &dir)
{
    unsigned oldBoot = 0, oldLast = 0;
    if (FILE *f = std::fopen((dir + "/part1.txt").c_str(), "rb")) {
        std::fscanf(f, "%u %u", &oldBoot, &oldLast);
        std::fclose(f);
    }
    boot(dir, false);
    CHECK(bootCount == oldBoot + 1, "boot count advanced: %u -> %u", oldBoot, bootCount);
    CHECK(pendingSDData, "pending records found after reboot");
    phone.connect(247);
    CHECK(runUntilDone(60000), "dump finished");
    // Os 2 lotes confirmados antes não voltam; o 3º (não confirmado) e o resto sim.
    const std::set<uint32_t> missing = missingSeqs(oldBoot, 2 * DUMP_BATCH + 1, oldLast);
    CHECK(missing.empty(), "missing=%zu", missing.size());
    CHECK(phone.records.count({oldBoot, 1}) == 0, "acknowledged batches not resent");
    CHECK(sdRecordLines() == 0, "card emptied");
}

// Registros feitos antes de a placa ter hora, num boot que terminou sem
// celular: depois do reinício não há como dar hora a eles e são descartados.
void undatableAfterRebootPart1(const std::string &dir)
{
    boot(dir, true);
    runFor(15 * 60000 + 1000, 50);     // ninguém acertou a hora
    CHECK(sdRecordLines() == 15, "15 undated records on the card");
}

void undatableAfterRebootPart2(const std::string &dir)
{
    boot(dir, false);
    phone.connect(247);
    runUntilDone(60000);
    CHECK(phone.records.empty(), "undatable records are not sent, got %zu", phone.records.size());
    CHECK(undatedLines() == 15, "the 15 records are kept in " UNDATED_FILE ", got %d", undatedLines());
    CHECK(sdRecordLines() == 0, "main log emptied");
}

// Rede salva fora de alcance por 1 h: as tentativas de reconectar não travam
// o loop() nem atrasam os registros (antes: ~19 a cada 20 min).
void recordIntervalHasNoDrift(const std::string &dir)
{
    boot(dir, true, true);
    runFor(60 * 60000 + 500, 20);
    CHECK(sim::wifiReconnects >= 100, "Wi-Fi retried every 30 s, retries=%d", sim::wifiReconnects);
    CHECK(readingSeq == 60, "one record per minute, readingSeq=%u", readingSeq);
    const int reads = countInLog("Temp DHT");
    CHECK(reads >= 1790, "no 2 s readings skipped, reads=%d (expected ~1800)", reads);
}

// Com o DS3231: o celular acerta a hora uma vez, a placa grava; depois de uma
// queda de energia de 2 h (parte 2, outro processo) ela liga já com a hora.
void rtcPart1(const std::string &dir)
{
    sim::rtcPresent = true;
    sim::rtcOscillatorStopped = true;   // módulo novo, sem hora
    boot(dir, true);
    CHECK(!timeIsValid(), "no clock yet (RTC empty)");
    phone.connect(247);
    runFor(2000, 5);
    phone.disconnect();
    CHECK(sim::rtcWrites == 1 && !sim::rtcOscillatorStopped, "phone time written to the RTC, writes=%d", sim::rtcWrites);
    runFor(30 * 60000 + 500, 50);
    CHECK(readingSeq == 30 && sdLog().find("sem_hora") == std::string::npos, "30 dated records on the card");
    saveState(dir, {{"boot", bootCount}, {"last", readingSeq},
                    {"rtc", sim::rtcEpochAtZero + int64_t(sim::nowMs / 1000)},
                    {"phone", phoneNow()}});
}

void rtcPart2(const std::string &dir)
{
    auto st = loadState(dir);
    const int64_t gap = 2 * 3600;           // 2 h sem energia
    sim::rtcPresent = true;
    sim::rtcOscillatorStopped = false;
    sim::rtcEpochAtZero = st["rtc"] + gap;  // o DS3231 continuou contando
    phoneEpochAtZero = st["phone"] + gap;
    boot(dir, false);
    CHECK(timeIsValid(), "clock restored from the RTC at boot");
    CHECK(std::llabs(int64_t(time(nullptr)) - phoneNow()) <= 2, "RTC time matches the phone, off by %lld s",
          (long long)(int64_t(time(nullptr)) - phoneNow()));
    CHECK(sim::log.find("Hora restaurada do DS3231") != std::string::npos, "logged");
    runFor(15 * 60000 + 500, 50);           // registros antes de qualquer celular
    CHECK(sdLog().find("sem_hora") == std::string::npos, "records of this boot are dated without a phone");
    phone.connect(247);
    CHECK(runUntilDone(60000), "dump finished");
    CHECK(missingSeqs(uint32_t(st["boot"]), 1, uint32_t(st["last"])).empty(), "all records of the first boot delivered");
    CHECK(missingSeqs(bootCount, 1, readingSeq).empty(), "all records of this boot delivered, dated");
    CHECK(undatedLines() == 0, "nothing undatable");
}

// DS3231 com a bateria gasta (OSF ligado): ignorado até alguém dar a hora;
// o NTP também grava a hora nele.
void rtcDeadBatteryAndNtp(const std::string &dir)
{
    sim::rtcPresent = true;
    sim::rtcOscillatorStopped = true;
    sim::rtcEpochAtZero = 946684800;        // 2000-01-01: hora sem sentido
    boot(dir, true, true);
    CHECK(!timeIsValid(), "RTC time ignored while OSF is set");
    CHECK(sim::log.find("sem hora válida") != std::string::npos, "logged");
    // O NTP acerta a hora quando a rede volta.
    sim::networkInRange = true;
    sim_timeval tv{phoneNow(), 0};
    settimeofday(&tv, nullptr);
    sntpCallback(&tv);
    runFor(3000, 20);
    CHECK(sim::rtcWrites == 1 && !sim::rtcOscillatorStopped, "NTP time written to the RTC, writes=%d", sim::rtcWrites);
    const int64_t rtcNow = sim::rtcEpochAtZero + int64_t(sim::nowMs / 1000);
    CHECK(std::llabs(rtcNow - phoneNow()) <= 2, "RTC now right, off by %lld s", (long long)(rtcNow - phoneNow()));
}

// Sem módulo: nada muda (e não trava procurando um).
void noRtc(const std::string &dir)
{
    sim::rtcPresent = false;
    boot(dir, true);
    CHECK(!rtcPresent && !timeIsValid(), "no RTC, no clock");
    CHECK(sim::log.find("Nenhum DS3231") != std::string::npos, "logged");
    runFor(2 * 60000 + 500, 50);
    CHECK(readingSeq == 2, "records as usual");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::printf("usage: fw_sim <scenario> <work dir>\n");
        return 2;
    }
    const std::string name = argv[1], dir = argv[2];
    sim::echo = std::getenv("FW_SIM_ECHO") != nullptr;
    const std::map<std::string, void (*)(const std::string &)> scenarios{
        {"offline_dump_default_mtu", offlineDumpDefaultMtu},
        {"large_backlog_big_mtu", largeBacklogBigMtu},
        {"large_backlog_default_mtu", largeBacklogDefaultMtu},
        {"unacknowledged_batch_is_resent", unacknowledgedBatchIsResent},
        {"disconnect_midway_resumes", disconnectMidwayResumes},
        {"record_written_while_waiting_for_ack", recordWrittenWhileWaitingForAck},
        {"wifi_first", wifiFirst},
        {"wifi_up_cloud_down", wifiUpCloudDown},
        {"no_wifi_retries_during_dump", noWifiRetriesDuringDump},
        {"notify_failures_are_retried", notifyFailuresAreRetried},
        {"old_lines_on_the_card", oldLinesOnTheCard},
        {"reboot_part1", rebootPart1},
        {"reboot_part2", rebootPart2},
        {"undatable_after_reboot_part1", undatableAfterRebootPart1},
        {"undatable_after_reboot_part2", undatableAfterRebootPart2},
        {"record_interval_has_no_drift", recordIntervalHasNoDrift},
        {"rtc_part1", rtcPart1},
        {"rtc_part2", rtcPart2},
        {"rtc_dead_battery_and_ntp", rtcDeadBatteryAndNtp},
        {"no_rtc", noRtc},
    };
    const auto it = scenarios.find(name);
    if (it == scenarios.end()) {
        std::printf("unknown scenario %s\n", name.c_str());
        return 2;
    }
    std::printf("%s\n", name.c_str());
    try {
        it->second(dir);
    } catch (const sim::Restart &) {
        std::printf("  FAIL  unexpected ESP.restart()\n");
        ++failures;
    }
    std::printf("%s: %d failure(s)\n", name.c_str(), failures);
    return failures == 0 ? 0 : 1;
}
