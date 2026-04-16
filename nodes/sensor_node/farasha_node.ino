#include <SPI.h>
#include <AESLib.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_mac.h"
#include <Preferences.h>

// ── Pin map ───────────────────────────────────────────────────
#define LORA_CS          5
#define LORA_RST         14
#define LORA_DIO0        2
#define LORA_SCK         18
#define LORA_MISO        19
#define LORA_MOSI        23

#define I2C_SDA          21
#define I2C_SCL          22

#define DHTPIN           27
#define DHTTYPE          DHT11
#define LDR_PIN          26
#define SOIL_PIN         32

#define REG_LED_PIN      33

DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;

// ── Sensor calibration ────────────────────────────────────────
#define SOIL_DRY         3200
#define SOIL_WET         1200

// ── LoRa SX1278 registers ─────────────────────────────────────
#define REG_FIFO                0x00
#define REG_OP_MODE             0x01
#define REG_FRF_MSB             0x06
#define REG_FRF_MID             0x07
#define REG_FRF_LSB             0x08
#define REG_PA_CONFIG           0x09
#define REG_PA_DAC              0x4D
#define REG_LNA                 0x0C
#define REG_FIFO_ADDR_PTR       0x0D
#define REG_FIFO_TX_BASE_ADDR   0x0E
#define REG_FIFO_RX_BASE_ADDR   0x0F
#define REG_FIFO_RX_CURRENT     0x10
#define REG_IRQ_FLAGS           0x12
#define REG_RX_NB_BYTES         0x13
#define REG_PKT_SNR_VALUE       0x19
#define REG_PKT_RSSI_VALUE      0x1A
#define REG_MODEM_CONFIG1       0x1D
#define REG_MODEM_CONFIG2       0x1E
#define REG_PAYLOAD_LENGTH      0x22
#define REG_MODEM_CONFIG3       0x26
#define REG_DETECTION_OPTIMIZE  0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD           0x39
#define REG_VERSION             0x42
#define MODE_SLEEP    0x00
#define MODE_STDBY    0x01
#define MODE_TX       0x03
#define MODE_RX_CONT  0x05
#define MODE_LORA     0x80
#define IRQ_TX_DONE   0x08
#define IRQ_RX_DONE   0x40

// ── Protocol ──────────────────────────────────────────────────
#define MSG_JOIN_REQ  0xFF
#define MSG_JOIN_ACK  0xFE
#define MSG_DATA      0xAA

static const uint8_t AES_KEY[16] = {
  0x2B,0x7E,0x15,0x16,0x28,0xAE,0xD2,0xA6,
  0xAB,0xF7,0x15,0x88,0x09,0xCF,0x4F,0x3C
};
static const uint8_t AES_IV_BASE[12] = {
  0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB
};

// ── Sleep durations ───────────────────────────────────────────
#define SLEEP_NORMAL_US   (600ULL * 1000000ULL)
#define SLEEP_ALERT_US    ( 60ULL * 1000000ULL)
#define ALERT_SOIL_THRESH  35
#define ALERT_CLEAR_COUNT   3

// ── LoRa RF ───────────────────────────────────────────────────
#define LORA_FREQ_HZ   433000000UL
#define LORA_SF        12
#define LORA_BW_CODE   7       // 125 kHz
#define LORA_CR        4       // CR4/8
#define LORA_SYNC      0x12

// ── Payload struct ────────────────────────────────────────────
#pragma pack(push, 1)
struct SensorPayload {
  uint8_t  flags;
  uint8_t  soil;
  uint8_t  hum;
  uint8_t  temp_hi;
  uint8_t  temp_lo;
  uint8_t  pres_hi;
  uint8_t  pres_lo;
  uint8_t  light;
  uint8_t  tank;
  int8_t   rssi_node;
  uint8_t  vbat_hi;
  uint8_t  vbat_lo;
  uint8_t  reserved[2];
};
#pragma pack(pop)
static_assert(sizeof(SensorPayload) == 14, "Payload must be 14 bytes");

#define SET_U16BE(hi,lo,val) do{(hi)=((val)>>8)&0xFF;(lo)=(val)&0xFF;}while(0)
#define SET_I16BE(hi,lo,val) SET_U16BE(hi,lo,(uint16_t)(int16_t)(val))

// ── RTC memory (survives deep sleep, lost on power cycle) ─────
RTC_DATA_ATTR uint8_t  rtc_node_id    = 0;
RTC_DATA_ATTR uint32_t rtc_counter    = 0;
RTC_DATA_ATTR uint32_t rtc_slot_ms    = 0;
RTC_DATA_ATTR bool     rtc_joined     = false;
RTC_DATA_ATTR bool     rtc_alert_mode = false;
RTC_DATA_ATTR uint8_t  rtc_alert_clr  = 0;
// Store MAC in RTC so we can detect power-cycle vs deep-sleep wakeup
RTC_DATA_ATTR uint8_t  rtc_mac[6]     = {0,0,0,0,0,0};

Preferences nvs;

// ── NVS helpers ───────────────────────────────────────────────
// NVS stores: mac (6 bytes), node_id, slot_ms, counter
// Key design: all under "farasha" namespace

void nvsClear() {
  nvs.begin("farasha", false);
  nvs.clear();
  nvs.end();
  Serial.println("[NVS] Cleared.");
}

// Returns true if NVS has a valid entry that matches this chip's MAC
bool nvsLoadIfMacMatches(const uint8_t* my_mac) {
  nvs.begin("farasha", true);
  size_t mac_len = nvs.getBytesLength("mac");
  if (mac_len != 6) {
    nvs.end();
    Serial.println("[NVS] No valid entry found.");
    return false;
  }
  uint8_t stored_mac[6];
  nvs.getBytes("mac", stored_mac, 6);
  nvs.end();

  if (memcmp(stored_mac, my_mac, 6) != 0) {
    Serial.println("[NVS] MAC MISMATCH — this chip has no valid assignment.");
    Serial.printf("[NVS] Stored: %02X:%02X:%02X:%02X:%02X:%02X\n",
      stored_mac[0],stored_mac[1],stored_mac[2],
      stored_mac[3],stored_mac[4],stored_mac[5]);
    Serial.printf("[NVS] Mine:   %02X:%02X:%02X:%02X:%02X:%02X\n",
      my_mac[0],my_mac[1],my_mac[2],
      my_mac[3],my_mac[4],my_mac[5]);
    return false;
  }

  // MAC matches — load the rest
  nvs.begin("farasha", true);
  rtc_node_id = nvs.getUChar("node_id", 0);
  rtc_slot_ms = nvs.getULong("slot_ms", 0);
  rtc_counter = nvs.getULong("counter", 0);
  nvs.end();

  if (rtc_node_id == 0) {
    Serial.println("[NVS] MAC matched but node_id=0, will rejoin.");
    return false;
  }

  rtc_joined = true;
  memcpy(rtc_mac, my_mac, 6);
  Serial.printf("[NVS] Loaded: nodeID=%u slot=%ums ctr=%u\n",
    rtc_node_id, rtc_slot_ms, rtc_counter);
  return true;
}

void nvsSave(const uint8_t* mac) {
  nvs.begin("farasha", false);
  nvs.putBytes("mac",     mac,          6);
  nvs.putUChar("node_id", rtc_node_id);
  nvs.putULong("slot_ms", rtc_slot_ms);
  nvs.putULong("counter", rtc_counter);
  nvs.end();
  Serial.printf("[NVS] Saved nodeID=%u slot=%ums ctr=%u\n",
    rtc_node_id, rtc_slot_ms, rtc_counter);
}

// ── CRC-16 ────────────────────────────────────────────────────
uint16_t crc16(const uint8_t* d, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
  }
  return crc;
}

// ── SPI helpers ───────────────────────────────────────────────
uint8_t loraRead(uint8_t reg) {
  digitalWrite(LORA_CS, LOW);
  SPI.transfer(reg & 0x7F); uint8_t v = SPI.transfer(0);
  digitalWrite(LORA_CS, HIGH); return v;
}
void loraWrite(uint8_t reg, uint8_t val) {
  digitalWrite(LORA_CS, LOW);
  SPI.transfer(reg | 0x80); SPI.transfer(val);
  digitalWrite(LORA_CS, HIGH);
}
void loraWriteBuf(uint8_t reg, const uint8_t* buf, size_t len) {
  digitalWrite(LORA_CS, LOW); SPI.transfer(reg | 0x80);
  for (size_t i = 0; i < len; i++) SPI.transfer(buf[i]);
  digitalWrite(LORA_CS, HIGH);
}
void loraReadBuf(uint8_t reg, uint8_t* buf, size_t len) {
  digitalWrite(LORA_CS, LOW); SPI.transfer(reg & 0x7F);
  for (size_t i = 0; i < len; i++) buf[i] = SPI.transfer(0);
  digitalWrite(LORA_CS, HIGH);
}

// ── LoRa init ─────────────────────────────────────────────────
bool loraInit() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  pinMode(LORA_CS,  OUTPUT); digitalWrite(LORA_CS,  HIGH);
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW); delay(10);
  digitalWrite(LORA_RST, HIGH); delay(20);
  if (loraRead(REG_VERSION) != 0x12) return false;
  loraWrite(REG_OP_MODE, MODE_SLEEP);             delay(10);
  loraWrite(REG_OP_MODE, MODE_SLEEP | MODE_LORA); delay(10);
  uint32_t frf = (uint32_t)((double)LORA_FREQ_HZ / (32e6 / (1 << 19)));
  loraWrite(REG_FRF_MSB, (frf >> 16) & 0xFF);
  loraWrite(REG_FRF_MID, (frf >>  8) & 0xFF);
  loraWrite(REG_FRF_LSB,  frf        & 0xFF);
  loraWrite(REG_MODEM_CONFIG1, (LORA_BW_CODE << 4) | (LORA_CR << 1));
  loraWrite(REG_MODEM_CONFIG2, (LORA_SF << 4) | 0x04);
  loraWrite(REG_MODEM_CONFIG3, 0x04);
  loraWrite(REG_DETECTION_OPTIMIZE,  0xC3);
  loraWrite(REG_DETECTION_THRESHOLD, 0x0A);
  loraWrite(REG_SYNC_WORD, LORA_SYNC);
  loraWrite(REG_PA_CONFIG, 0x8F); // PA_BOOST +20dBm
  loraWrite(REG_PA_DAC, 0x87);
  loraWrite(REG_FIFO_TX_BASE_ADDR, 0x00);
  loraWrite(REG_FIFO_RX_BASE_ADDR, 0x00);
  loraWrite(REG_LNA, loraRead(REG_LNA) | 0x03);
  loraWrite(REG_OP_MODE, MODE_LORA | MODE_STDBY); delay(5);
  return true;
}

bool loraTx(const uint8_t* buf, size_t len) {
  loraWrite(REG_OP_MODE, MODE_LORA | MODE_STDBY);
  loraWrite(REG_FIFO_ADDR_PTR, 0x00);
  loraWrite(REG_FIFO_TX_BASE_ADDR, 0x00);
  loraWriteBuf(REG_FIFO, buf, len);
  loraWrite(REG_PAYLOAD_LENGTH, len);
  loraWrite(REG_IRQ_FLAGS, 0xFF);
  loraWrite(REG_OP_MODE, MODE_LORA | MODE_TX);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    if (loraRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) {
      loraWrite(REG_IRQ_FLAGS, 0xFF);
      loraWrite(REG_OP_MODE, MODE_SLEEP | MODE_LORA);
      return true;
    }
    delay(1);
  }
  return false;
}

int loraRx(uint8_t* buf, size_t maxlen, uint32_t timeout_ms) {
  loraWrite(REG_IRQ_FLAGS, 0xFF);
  loraWrite(REG_OP_MODE, MODE_LORA | MODE_RX_CONT);
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    uint8_t irq = loraRead(REG_IRQ_FLAGS);
    if (irq & IRQ_RX_DONE) {
      loraWrite(REG_IRQ_FLAGS, 0xFF);
      if (irq & 0x20) return -1; // CRC error
      uint8_t nb  = loraRead(REG_RX_NB_BYTES);
      uint8_t ptr = loraRead(REG_FIFO_RX_CURRENT);
      loraWrite(REG_FIFO_ADDR_PTR, ptr);
      size_t n = (nb < maxlen) ? nb : maxlen;
      loraReadBuf(REG_FIFO, buf, n);
      loraWrite(REG_OP_MODE, MODE_SLEEP | MODE_LORA);
      return (int)n;
    }
    delay(1);
  }
  loraWrite(REG_OP_MODE, MODE_SLEEP | MODE_LORA);
  return 0;
}

// ── AES-CBC encrypt (14-byte payload → 16 bytes) ──────────────
void aesEncrypt(const uint8_t* plain, uint8_t* cipher, uint32_t counter) {
  uint8_t iv[16];
  memcpy(iv, AES_IV_BASE, 12);
  iv[12]=(counter>>24)&0xFF; iv[13]=(counter>>16)&0xFF;
  iv[14]=(counter>> 8)&0xFF; iv[15]= counter     &0xFF;
  uint8_t buf[16] = {0};
  memcpy(buf, plain, 14);
  AESLib aes;
  aes.encrypt(buf, 16, cipher, (byte*)AES_KEY, 128, iv);
}

// ── Power helpers ─────────────────────────────────────────────
void ledOn()  { digitalWrite(REG_LED_PIN, HIGH); }
void ledOff() { digitalWrite(REG_LED_PIN, LOW);  }

// ── Sensor reads ──────────────────────────────────────────────
uint8_t readSoil() {
  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) { sum += analogRead(SOIL_PIN); delay(5); }
  int raw = sum / 4;
  int pct = map(raw, SOIL_DRY, SOIL_WET, 0, 100);
  return (uint8_t)constrain(pct, 0, 100);
}

uint8_t readLight() {
  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) { sum += analogRead(LDR_PIN); delay(2); }
  int raw = sum / 4;
