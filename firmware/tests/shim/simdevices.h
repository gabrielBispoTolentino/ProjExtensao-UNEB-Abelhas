#pragma once

// Periféricos simulados da ESP32 (ver Arduino.h): Wi-Fi, cartão SD, NVS,
// DHT22, HTTPS para o Supabase, portal web e NimBLE. Os cabeçalhos com os
// nomes das bibliotecas de verdade (WiFi.h, SD.h...) incluem este.

#include "Arduino.h"

// ── Wi-Fi ───────────────────────────────────────────────────────────────────
struct IPAddress
{
    String toString() const { return String("192.168.4.1"); }
};

enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3 };
enum { WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };
enum { WIFI_AUTH_OPEN = 0 };
enum {
    WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15, WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
};
enum WiFiEvent_t { ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 21 };
struct WiFiEventInfo_t { struct { uint8_t reason = 0; } wifi_sta_disconnected; };

struct WiFiClass
{
    int modeValue = WIFI_STA;
    int status() const { return sim::wifiBegun && sim::networkInRange ? WL_CONNECTED : WL_DISCONNECTED; }
    void mode(int m) { modeValue = m; }
    int getMode() const { return modeValue; }
    void begin(const char *, const char *) { sim::wifiBegun = true; }
    void reconnect() { ++sim::wifiReconnects; sim::wifiBegun = true; }
    void disconnect() { sim::wifiBegun = false; }
    bool softAP(const char *, const char *) { return true; }
    void softAPdisconnect(bool) {}
    int softAPgetStationNum() const { return 0; }
    IPAddress softAPIP() const { return {}; }
    IPAddress localIP() const { return {}; }
    String macAddress() const { return String("30:76:F5:90:C1:D0"); }
    String SSID(int = 0) const { return String("RedeTeste"); }
    int RSSI(int = 0) const { return -55; }
    int encryptionType(int) const { return 3; }
    int scanNetworks() { return 0; }
    void scanDelete() {}
    void onEvent(std::function<void(WiFiEvent_t, WiFiEventInfo_t)>, WiFiEvent_t) {}
};
inline WiFiClass WiFi;

// ── HTTPS para o Supabase ───────────────────────────────────────────────────
struct WiFiClientSecure
{
    void setInsecure() {}
    int lastError(char *buffer, size_t size) { std::snprintf(buffer, size, "sim"); return 0; }
};

struct HTTPClient
{
    String reply;
    bool begin(WiFiClientSecure &, const String &) { return true; }
    void setTimeout(int) {}
    void addHeader(const char *, const char *) {}
    void addHeader(const char *, const String &) {}
    int POST(const String &body)
    {
        if (sim::cloudStatus < 0) return sim::cloudStatus;   // sem conexão
        sim::cloudBodies.push_back(body.s);
        reply = sim::cloudStatus >= 300 ? String("{\"message\":\"sim error\"}") : String();
        return sim::cloudStatus;
    }
    String getString() { return reply; }
    String errorToString(int code) { return String("connection refused (") + String(code) + ")"; }
    void end() {}
};

// ── Portal de configuração (não usado nos testes) ───────────────────────────
enum HTTPMethod { HTTP_GET, HTTP_POST };
struct WebServer
{
    explicit WebServer(int) {}
    void on(const char *, HTTPMethod, std::function<void()>) {}
    void onNotFound(std::function<void()>) {}
    void begin() {}
    void stop() {}
    void handleClient() {}
    String arg(const char *) { return String(); }
    void send(int, const char *, const char *) {}
    void send(int, const char *, const String &) {}
    void send_P(int, const char *, const char *) {}
    void sendHeader(const char *, const String &, bool = false) {}
};
struct DNSServer
{
    bool start(int, const char *, const IPAddress &) { return true; }
    void stop() {}
    void processNextRequest() {}
};

// ── DHT22 ───────────────────────────────────────────────────────────────────
#define DHT22 22
struct DHT
{
    DHT(int, int) {}
    void begin() {}
    float readTemperature() { return float(sim::temperature); }
    float readHumidity() { return float(sim::humidity); }
};

// ── NVS (Preferences) ───────────────────────────────────────────────────────
struct Preferences
{
    std::string ns;
    bool begin(const char *name, bool = false) { ns = name; return true; }
    void end() {}
    std::string key(const char *k) const { return ns + "/" + k; }
    bool isKey(const char *k) const { return sim::prefs.count(key(k)) > 0; }
    String getString(const char *k, const char *fallback) const
    {
        const auto it = sim::prefs.find(key(k));
        return it == sim::prefs.end() ? String(fallback) : String(it->second);
    }
    uint32_t getUInt(const char *k, uint32_t fallback) const
    {
        const auto it = sim::prefs.find(key(k));
        return it == sim::prefs.end() ? fallback : uint32_t(std::strtoul(it->second.c_str(), nullptr, 10));
    }
    uint16_t getUShort(const char *k, uint16_t fallback) const { return uint16_t(getUInt(k, fallback)); }
    size_t putString(const char *k, const String &v) { sim::prefs[key(k)] = v.s; sim::savePrefs(); return v.length(); }
    size_t putUInt(const char *k, uint32_t v) { sim::prefs[key(k)] = std::to_string(v); sim::savePrefs(); return 4; }
    size_t putUShort(const char *k, uint16_t v) { return putUInt(k, v); }
};

// ── Cartão SD (uma pasta do PC) ─────────────────────────────────────────────
#define FILE_READ "rb"
#define FILE_WRITE "wb"
#define FILE_APPEND "ab"
enum { CARD_NONE = 0, CARD_SD = 2 };

class File
{
public:
    File() = default;
    explicit File(std::FILE *f) : m_f(f) {}
    explicit operator bool() const { return m_f != nullptr; }
    void close() { if (m_f) std::fclose(m_f); m_f = nullptr; }
    size_t size() const
    {
        const long here = std::ftell(m_f);
        std::fseek(m_f, 0, SEEK_END);
        const long end = std::ftell(m_f);
        std::fseek(m_f, here, SEEK_SET);
        return size_t(end);
    }
    size_t position() const { return size_t(std::ftell(m_f)); }
    bool seek(uint32_t pos) { return std::fseek(m_f, long(pos), SEEK_SET) == 0; }
    int available() const { return int(size() - position()); }
    String readStringUntil(char terminator)
    {
        std::string out;
        int c;
        while ((c = std::fgetc(m_f)) != EOF && c != terminator) out += char(c);
        return String(out);
    }
    // Print::println do Arduino termina a linha com "\r\n".
    size_t println(const String &line) { return std::fwrite((line.s + "\r\n").data(), 1, line.length() + 2, m_f); }
    size_t println(const char *line) { return println(String(line)); }

private:
    std::FILE *m_f = nullptr;
};

struct SDClass
{
    std::string path(const char *p) const { return sim::sdDir + p; }
    bool begin(int) { return sim::sdPresent; }
    int cardType() const { return sim::sdPresent ? CARD_SD : CARD_NONE; }
    bool exists(const char *p) const
    {
        std::FILE *f = std::fopen(path(p).c_str(), "rb");
        if (f) std::fclose(f);
        return f != nullptr;
    }
    File open(const char *p, const char *mode) { return File(std::fopen(path(p).c_str(), mode)); }
    bool remove(const char *p) { return std::remove(path(p).c_str()) == 0; }
};
inline SDClass SD;
struct SPIClass { void begin(int, int, int, int) {} };
inline SPIClass SPI;

// ── I2C com um DS3231 simulado (endereço 0x68) ──────────────────────────────
// Registradores 0x00-0x06: hora em BCD (UTC); 0x0F: estado (bit 7 = OSF).
struct TwoWire
{
    uint8_t address = 0;
    uint8_t pointer = 0;
    std::vector<uint8_t> tx;
    std::deque<uint8_t> rx;

    static uint8_t bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
    static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
    bool present() const { return address == 0x68 && sim::rtcPresent; }

    uint8_t reg(uint8_t r) const
    {
        const time_t now = time_t(sim::rtcEpochAtZero + int64_t(sim::nowMs / 1000));
        struct tm t;
        gmtime_s(&t, &now);
        switch (r) {
        case 0: return bcd(t.tm_sec);
        case 1: return bcd(t.tm_min);
        case 2: return bcd(t.tm_hour);
        case 3: return bcd(t.tm_wday + 1);
        case 4: return bcd(t.tm_mday);
        case 5: return bcd(t.tm_mon + 1);
        case 6: return bcd(t.tm_year - 100);
        case 0x0F: return sim::rtcOscillatorStopped ? 0x80 : 0x00;
        default: return 0;
        }
    }

    bool begin(int, int) { return true; }
    void beginTransmission(int a) { address = uint8_t(a); tx.clear(); }
    size_t write(uint8_t b) { tx.push_back(b); return 1; }
    uint8_t endTransmission(bool = true)
    {
        if (!present()) return 2;   // NACK: ninguém neste endereço
        if (tx.empty()) return 0;
        pointer = tx[0];
        if (pointer == 0x00 && tx.size() >= 8) {
            struct tm t{};
            t.tm_sec = unbcd(tx[1]);
            t.tm_min = unbcd(tx[2]);
            t.tm_hour = unbcd(tx[3] & 0x3F);
            t.tm_mday = unbcd(tx[5]);
            t.tm_mon = unbcd(tx[6] & 0x1F) - 1;
            t.tm_year = unbcd(tx[7]) + 100;
            sim::rtcEpochAtZero = int64_t(_mkgmtime(&t)) - int64_t(sim::nowMs / 1000);
            ++sim::rtcWrites;
        } else if (pointer == 0x0F && tx.size() >= 2) {
            sim::rtcOscillatorStopped = (tx[1] & 0x80) != 0;
        }
        return 0;
    }
    uint8_t requestFrom(int a, int count)
    {
        rx.clear();
        if (a != 0x68 || !sim::rtcPresent) return 0;
        for (int i = 0; i < count; ++i) rx.push_back(reg(uint8_t(pointer + i)));
        return uint8_t(count);
    }
    int available() const { return int(rx.size()); }
    int read()
    {
        if (rx.empty()) return -1;
        const int b = rx.front();
        rx.pop_front();
        return b;
    }
};
inline TwoWire Wire;

// ── NTP ─────────────────────────────────────────────────────────────────────
inline void (*sntpCallback)(struct timeval *) = nullptr;
inline void sntp_set_time_sync_notification_cb(void (*cb)(struct timeval *)) { sntpCallback = cb; }

// ── NimBLE: um único celular, ligado ao teste ───────────────────────────────
class NimBLEServer;
class NimBLECharacteristic;

struct NimBLEConnInfo
{
    uint16_t mtu = 23;
    uint16_t getMTU() const { return mtu; }
};

struct NimBLEAttValue
{
    std::string bytes;
    size_t length() const { return bytes.size(); }
    const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(bytes.data()); }
};

namespace NIMBLE_PROPERTY {
enum : uint32_t { READ = 0x02, WRITE = 0x08, WRITE_NR = 0x04, NOTIFY = 0x10 };
}

class NimBLECharacteristicCallbacks
{
public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onWrite(NimBLECharacteristic *, NimBLEConnInfo &) {}
};

class NimBLECharacteristic
{
public:
    std::string uuid;
    std::string value;
    NimBLECharacteristicCallbacks *callbacks = nullptr;

    void setCallbacks(NimBLECharacteristicCallbacks *cb) { callbacks = cb; }
    void setValue(const uint8_t *data, size_t length) { value.assign(reinterpret_cast<const char *>(data), length); }
    void setValue(const String &text) { value = text.s; }
    NimBLEAttValue getValue() const { return {value}; }
    bool notify(const uint8_t *data, size_t length, uint16_t = 0xFFFF)
    {
        ++sim::notifyCalls;
        if (sim::notifyFailures > 0
            || (sim::notifyFailEvery > 0 && sim::notifyCalls % sim::notifyFailEvery == 0)) {
            if (sim::notifyFailures > 0) --sim::notifyFailures;
            ++sim::notifyFailed;
            return false;   // sem buffer livre: o pedaço não saiu
        }
        sim::biggestNotify = std::max(sim::biggestNotify, length);
        if (sim::onNotify) sim::onNotify(data, length);
        return true;
    }
    bool notify(uint16_t handle = 0xFFFF) { return notify(reinterpret_cast<const uint8_t *>(value.data()), value.size(), handle); }
};

class NimBLEServerCallbacks
{
public:
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer *, NimBLEConnInfo &) {}
    virtual void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) {}
    virtual void onMTUChange(uint16_t, NimBLEConnInfo &) {}
};

namespace sim {
inline NimBLEServerCallbacks *bleServerCallbacks = nullptr;
inline std::map<std::string, NimBLECharacteristic *> characteristics;   // por UUID
}

class NimBLEService
{
public:
    NimBLECharacteristic *createCharacteristic(const char *uuid, uint32_t)
    {
        auto *c = new NimBLECharacteristic;
        c->uuid = uuid;
        sim::characteristics[uuid] = c;
        return c;
    }
};

class NimBLEServer
{
public:
    void setCallbacks(NimBLEServerCallbacks *cb) { sim::bleServerCallbacks = cb; }
    NimBLEService *createService(const char *) { return new NimBLEService; }
    void start() {}
};

class NimBLEAdvertising
{
public:
    void addServiceUUID(const char *) {}
    void enableScanResponse(bool) {}
    bool setName(const char *) { return true; }
};

class NimBLEDevice
{
public:
    static bool init(const char *) { return true; }
    static NimBLEServer *createServer() { static NimBLEServer server; return &server; }
    static NimBLEAdvertising *getAdvertising() { static NimBLEAdvertising adv; return &adv; }
    static bool startAdvertising() { return true; }
    static bool setDeviceName(const char *) { return true; }
};
