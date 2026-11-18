/*
 * BUILD-IOT-03B-SPI
 * Diagnostico temporal de comunicacion SPI entre ESP32 y SX1278 (Ai-Thinker RA-01).
 *
 * Alcance:
 *  - Solo lectura del registro VERSION (0x42). No se escribe ningun registro.
 *  - No transmite por LoRa, no configura RF, no usa Wi-Fi ni MQTT.
 *  - Sketch independiente: no depende del firmware de produccion.
 *
 * Resultado esperado: 0x12 (SX1276/77/78/79).
 */

#include <SPI.h>

static const int PIN_NSS   = 5;
static const int PIN_RESET = 14;
static const int PIN_DIO0  = 26;
static const int PIN_SCK   = 18;
static const int PIN_MOSI  = 23;
static const int PIN_MISO  = 19;

static const uint8_t REG_VERSION = 0x42;
static const uint8_t VERSION_EXPECTED = 0x12;

static const uint8_t READ_ATTEMPTS = 10;
static const uint16_t READ_INTERVAL_MS = 150;
static const uint16_t READY_TIMEOUT_MS = 1000;
static const uint32_t SPI_CLOCK_HZ = 8000000;

static uint8_t readRegister(uint8_t address) {
  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_NSS, LOW);
  SPI.transfer(address & 0x7F);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(PIN_NSS, HIGH);
  SPI.endTransaction();
  return value;
}

static void hardwareReset() {
  digitalWrite(PIN_NSS, HIGH);
  digitalWrite(PIN_RESET, LOW);
  delay(10);
  digitalWrite(PIN_RESET, HIGH);
  delay(20);
}

static bool waitForVersion(uint8_t *lastValue, uint32_t *elapsedMs) {
  const uint32_t start = millis();
  uint32_t elapsed = 0;
  uint8_t value = 0x00;

  do {
    value = readRegister(REG_VERSION);
    elapsed = millis() - start;
    if (value == VERSION_EXPECTED) {
      break;
    }
    delay(10);
  } while (elapsed < READY_TIMEOUT_MS);

  if (lastValue != nullptr) {
    *lastValue = value;
  }
  if (elapsedMs != nullptr) {
    *elapsedMs = elapsed;
  }
  return value == VERSION_EXPECTED;
}

static const char *hintForValue(uint8_t value) {
  if (value == 0x00) {
    return "MISO en bajo / sin alimentacion o reset no liberado";
  }
  if (value == 0xFF) {
    return "MISO en alto / MISO desconectado o NSS no conmutado";
  }
  if (value == VERSION_EXPECTED) {
    return "valor esperado";
  }
  return "valor inesperado";
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("================================================");
  Serial.println(" BUILD-IOT-03B-SPI - Diagnostico SPI SX1278");
  Serial.println(" Solo lectura de registros. Sin TX LoRa, sin RF.");
  Serial.println("================================================");

  Serial.printf(" NSS=%d RESET=%d DIO0=%d\n", PIN_NSS, PIN_RESET, PIN_DIO0);
  Serial.printf(" SCK=%d MOSI=%d MISO=%d\n", PIN_SCK, PIN_MOSI, PIN_MISO);
  Serial.printf(" VERSION esperada: 0x%02X\n\n", VERSION_EXPECTED);

  pinMode(PIN_NSS, OUTPUT);
  pinMode(PIN_RESET, OUTPUT);
  pinMode(PIN_DIO0, INPUT);
  digitalWrite(PIN_NSS, HIGH);
  digitalWrite(PIN_RESET, HIGH);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  Serial.println(" Reinicio controlado del SX1278...");
  hardwareReset();

  uint8_t probeValue = 0x00;
  uint32_t probeMs = 0;
  const bool ready = waitForVersion(&probeValue, &probeMs);
  Serial.printf(" Primer lectura tras reset: 0x%02X en %lu ms (%s)\n\n",
                probeValue, static_cast<unsigned long>(probeMs),
                ready ? "listo" : "sin respuesta esperada");

  Serial.printf(" Lecturas de VERSION (0x%02X), %u intentos:\n",
                REG_VERSION, READ_ATTEMPTS);

  uint8_t successCount = 0;
  uint8_t lastValue = 0x00;
  uint8_t previousValue = 0x00;
  bool allEqual = true;
  bool allExpected = true;

  for (uint8_t i = 0; i < READ_ATTEMPTS; i++) {
    const uint8_t value = readRegister(REG_VERSION);
    lastValue = value;

    if (i > 0 && value != previousValue) {
      allEqual = false;
    }
    if (value == VERSION_EXPECTED) {
      successCount++;
    } else {
      allExpected = false;
    }

    previousValue = value;

    Serial.printf("   #%02u -> 0x%02X  %s\n", i + 1, value, hintForValue(value));

    if (i + 1 < READ_ATTEMPTS) {
      delay(READ_INTERVAL_MS);
    }
  }

  Serial.println();
  Serial.printf(" Correctas: %u/%u  (%.0f%%)\n", successCount, READ_ATTEMPTS,
                (100.0f * successCount) / READ_ATTEMPTS);
  Serial.printf(" Ultima lectura: 0x%02X\n", lastValue);
  Serial.printf(" Estabilidad: %s\n", allEqual ? "estable" : "inestable");

  if (!allExpected) {
    Serial.println();
    Serial.println(" Notas de diagnostico:");
    Serial.println("  - 0x00: revisar 3.3 V, GND, MISO y que RESET llegue a HIGH.");
    Serial.println("  - 0xFF: revisar MISO/SCK y la soldadura del modulo.");
    Serial.println("  - Otro valor: revisar SCK/MOSI/NSS y el modelo del modulo.");
  }

  Serial.println();
  Serial.println("------------------------------------------------");
  if (allExpected && allEqual) {
    Serial.println(" RESULTADO: DETECTADO (VERSION = 0x12 estable)");
  } else if (successCount > 0) {
    Serial.println(" RESULTADO: NO DETECTADO (respuesta inestable)");
  } else {
    Serial.println(" RESULTADO: NO DETECTADO");
  }
  Serial.println("------------------------------------------------");
  Serial.println(" Fin del diagnostico. No se transmitio por LoRa.");
}

void loop() {
  delay(1000);
}
