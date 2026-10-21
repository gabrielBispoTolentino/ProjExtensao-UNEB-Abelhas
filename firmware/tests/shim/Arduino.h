#pragma once

// Simulação da ESP32 no PC, para rodar o ColmeiaMonitor.ino de verdade nos
// testes: no lugar do hardware, um mundo simulado (namespace sim) com relógio,
// cartão SD (uma pasta), NVS (um arquivo), Wi-Fi, Supabase e um link
// Bluetooth com um "celular" (o teste). Os outros cabeçalhos da pasta (WiFi.h,
// SD.h, NimBLEDevice.h...) só incluem este.
//
// millis() e delay() usam o relógio simulado: delay(5000) não espera nada,
// só avança o tempo. time() segue o relógio da placa (sem hora até alguém
// chamar settimeofday, como faz o comando "time" do app).

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <sys/time.h>
#include <vector>

using std::isnan;
using std::max;
using std::min;

#define PROGMEM

// ── String do Arduino (o necessário para o sketch e o ArduinoJson) ──────────
class String
{
public:
    std::string s;

    String() = default;
    String(const char *text) { if (text) s = text; }
    String(const std::string &text) : s(text) {}
    explicit String(char c) : s(1, c) {}
    explicit String(int v) : s(std::to_string(v)) {}
    explicit String(unsigned v) : s(std::to_string(v)) {}
    explicit String(long v) : s(std::to_string(v)) {}
    explicit String(unsigned long v) : s(std::to_string(v)) {}

    String &operator=(const char *text) { s = text ? text : ""; return *this; }
    size_t length() const { return s.size(); }
    const char *c_str() const { return s.c_str(); }
    bool isEmpty() const { return s.empty(); }
    char operator[](size_t i) const { return i < s.size() ? s[i] : 0; }
    char &operator[](size_t i) { return s[i]; }

    int indexOf(char c, unsigned from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : int(p); }
    int indexOf(const char *t, unsigned from = 0) const { auto p = s.find(t, from); return p == std::string::npos ? -1 : int(p); }
    int indexOf(const String &t, unsigned from = 0) const { return indexOf(t.c_str(), from); }
    String substring(unsigned from) const { return from >= s.size() ? String() : String(s.substr(from)); }
    String substring(unsigned from, unsigned to) const
    {
        if (from > to) std::swap(from, to);
        if (from >= s.size()) return String();
        return String(s.substr(from, std::min<size_t>(to, s.size()) - from));
    }
    float toFloat() const { return std::strtof(s.c_str(), nullptr); }
    long toInt() const { return std::strtol(s.c_str(), nullptr, 10); }
    void trim()
    {
        const char *ws = " \t\r\n";
        const auto b = s.find_first_not_of(ws);
        if (b == std::string::npos) { s.clear(); return; }
        s = s.substr(b, s.find_last_not_of(ws) - b + 1);
    }
    void replace(const String &from, const String &to)
    {
        if (from.s.empty()) return;
        for (size_t p = 0; (p = s.find(from.s, p)) != std::string::npos; p += to.s.size()) s.replace(p, from.s.size(), to.s);
    }
    bool startsWith(const String &p) const { return s.rfind(p.s, 0) == 0; }
    bool concat(const char *t) { if (t) s += t; return true; }
    bool concat(const String &t) { s += t.s; return true; }
    void reserve(size_t n) { s.reserve(n); }

    String &operator+=(const String &t) { s += t.s; return *this; }
    String &operator+=(const char *t) { if (t) s += t; return *this; }
    String &operator+=(char c) { s += c; return *this; }
    bool operator==(const String &o) const { return s == o.s; }
    bool operator==(const char *o) const { return s == (o ? o : ""); }
    bool operator!=(const String &o) const { return s != o.s; }
    bool operator<(const String &o) const { return s < o.s; }
};
inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
inline String operator+(const String &a, const char *b) { return String(a.s + (b ? b : "")); }
inline String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s); }
inline String operator+(const String &a, char c) { return String(a.s + c); }

// ── Mundo simulado ──────────────────────────────────────────────────────────
namespace sim {

struct Restart {};   // ESP.restart(): o teste decide o que fazer

inline uint64_t nowMs = 0;
inline bool clockSet = false;
inline int64_t epochAtZero = 0;        // epoch = epochAtZero + nowMs / 1000, com a hora acertada
inline std::string log;                // tudo o que a placa escreveu no Serial
inline bool echo = false;

// Wi-Fi e Supabase
inline bool networkInRange = false;    // a rede salva está ao alcance
inline bool wifiBegun = false;
inline int wifiReconnects = 0;
inline int cloudStatus = 201;          // resposta do Supabase; <0 = sem conexão
inline std::vector<std::string> cloudBodies;

// Cartão SD e NVS
inline std::string sdDir;               // pasta que faz de cartão
inline bool sdPresent = true;
inline std::string prefsFile;          // NVS persistido entre "boots" (processos)
inline std::map<std::string, std::string> prefs;   // "ns/key" -> valor

// Bluetooth
inline std::function<void(const uint8_t *, size_t)> onNotify;   // o celular recebe
inline int notifyFailures = 0;          // próximas N chamadas de notify() falham (sem buffer)
inline int notifyFailEvery = 0;         // ou: toda N-ésima chamada falha
inline int notifyFailed = 0;
inline int notifyCalls = 0;
inline size_t biggestNotify = 0;

// Relógio DS3231 no I2C (opcional): conta a hora mesmo com a placa desligada.
inline bool rtcPresent = false;
inline bool rtcOscillatorStopped = true;   // OSF: sem hora até alguém gravar
inline int64_t rtcEpochAtZero = 0;          // hora do RTC = rtcEpochAtZero + nowMs / 1000
inline int rtcWrites = 0;

inline double temperature = 32.5;
inline double humidity = 50.0;

inline int64_t epoch() { return clockSet ? epochAtZero + int64_t(nowMs / 1000) : int64_t(nowMs / 1000); }

inline void loadPrefs()
{
    prefs.clear();
    if (FILE *f = std::fopen(prefsFile.c_str(), "rb")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const auto tab = l.find('\t');
            if (tab != std::string::npos) prefs[l.substr(0, tab)] = l.substr(tab + 1);
        }
        std::fclose(f);
    }
}
inline void savePrefs()
{
    if (prefsFile.empty()) return;
    if (FILE *f = std::fopen(prefsFile.c_str(), "wb")) {
        for (const auto &kv : prefs) std::fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
        std::fclose(f);
    }
}

} // namespace sim

// ── Núcleo do Arduino ───────────────────────────────────────────────────────
inline unsigned long millis() { return (unsigned long)sim::nowMs; }
inline void delay(unsigned long ms) { sim::nowMs += ms; }
inline uint32_t esp_random() { return 123456789u; }

struct SerialClass
{
    void begin(unsigned long) {}
    void out(const std::string &text) { sim::log += text; if (sim::echo) std::fputs(text.c_str(), stdout); }
    int printf(const char *format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(buffer, sizeof buffer, format, args);
        va_end(args);
        out(buffer);
        return n;
    }
    void print(const char *t) { out(t); }
    void print(const String &t) { out(t.s); }
    void println(const char *t = "") { out(std::string(t) + "\n"); }
    void println(const String &t) { out(t.s + "\n"); }
};
inline SerialClass Serial;

struct EspClass
{
    [[noreturn]] void restart() { throw sim::Restart{}; }
    uint32_t getFreeHeap() { return 100000; }
    uint32_t getMaxAllocHeap() { return 90000; }
};
inline EspClass ESP;

// ── Relógio (o fuso da placa é fixo: UTC-3) ─────────────────────────────────
struct sim_timeval { int64_t tv_sec; long tv_usec; };
inline time_t sim_time(time_t *out) { const time_t t = time_t(sim::epoch()); if (out) *out = t; return t; }
inline struct tm *sim_gmtime_r(const time_t *t, struct tm *out) { gmtime_s(out, t); return out; }
inline struct tm *sim_localtime_r(const time_t *t, struct tm *out) { const time_t local = *t - 3 * 3600; gmtime_s(out, &local); return out; }
inline int settimeofday(const sim_timeval *tv, const void *)
{
    sim::epochAtZero = tv->tv_sec - int64_t(sim::nowMs / 1000);
    sim::clockSet = true;
    return 0;
}
inline void configTzTime(const char *, const char *, const char *) {}

// ── FreeRTOS: só a fila dos comandos BLE ────────────────────────────────────
struct SimQueue { size_t itemSize, capacity; std::deque<std::vector<char>> items; };
using QueueHandle_t = SimQueue *;
#define pdTRUE 1
#define pdFALSE 0
inline QueueHandle_t xQueueCreate(size_t length, size_t itemSize) { return new SimQueue{itemSize, length, {}}; }
inline int xQueueSend(QueueHandle_t q, const void *item, int)
{
    if (q->items.size() >= q->capacity) return pdFALSE;
    q->items.emplace_back((const char *)item, (const char *)item + q->itemSize);
    return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q, void *item, int)
{
    if (q->items.empty()) return pdFALSE;
    std::memcpy(item, q->items.front().data(), q->itemSize);
    q->items.pop_front();
    return pdTRUE;
}

// time/localtime_r/gmtime_r/timeval da placa no lugar dos do PC (depois dos
// cabeçalhos do sistema, que declaram os originais).
#define time(x) sim_time(x)
#define localtime_r sim_localtime_r
#define gmtime_r sim_gmtime_r
#define timeval sim_timeval
