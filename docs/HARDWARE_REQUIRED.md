# Datos físicos requeridos y condición de parada

La validación física está **pendiente**. Para BUILD-IOT-01 se confirmaron como
referencia histórica BME680, DHT11 y sus pines de coordinador, que el firmware
ya usa. No se han comprobado eléctricamente. La configuración física del enlace
SX1278 sigue sin confirmar.

## 1. ESP32

Se necesita:

- Modelo y variante exactos de la placa (por ejemplo, family/variant y revisión).
- Voltaje de operación y niveles lógicos.
- Pines SPI disponibles y si el módulo LoRa comparte el bus con otro periférico.
- Pin de `BOOT`/reset y método de flasheo.
- Consumo y alimentación previstos.

Sin el modelo exacto no se puede confirmar el pinout ni seleccionar la placa
correcta en Arduino IDE.

## 2. Módulo SX1278

Se necesita:

- Modelo exacto del módulo (chip SX1278 y placa portadora).
- Banda y variante del módulo; antena y conector.
- Nivel lógico de sus pines (3,3 V u otro) y si la placa incluye adaptación de
  nivel.
- Alimentación requerida y consumo de transmisión.
- Presencia de cristal/TCXO y cualquier particularidad de la placa.

No se debe asumir que un módulo "SX1278 genérico" tiene el mismo pinout que otro.

## 3. Conexionado ESP32 <-> SX1278

Confirmar para **cada** extremo:

- `SCK`
- `MISO`
- `MOSI`
- `NSS` / `SS`
- `RESET`
- `DIO0`
- `3V3` y `GND`

También confirmar si `DIO0` es necesario para el modo de recepción elegido y si
hay resistencias de pull-up o divisores. En el firmware, `SCK`, `MISO`, `MOSI` y
`SS` son obligatorios; `RESET` y `DIO0` pueden quedar en `-1` únicamente si el
módulo exacto confirma que no se usan. **No se incluye diagrama de conexiones
porque el pinout no está confirmado.**

## 4. Radio y normativa

Confirmar un único conjunto de parámetros para coordinador y gateway:

- Frecuencia exacta dentro de la banda de 433 MHz.
- Factor de dispersión (SF).
- Ancho de banda (BW).
- Tasa de codificación (CR).
- Potencia de transmisión.
- Sync word.
- Longitud de preámbulo.
- Uso de CRC.
- Condiciones de uso y límites normativos aplicables en Colombia.

El contrato aprueba la banda de 433 MHz, pero no fija estos valores. No se
inventaron.

## 5. Sensores

Para BUILD-IOT-01 se confirmaron como referencia de cableado ESP32-WROOM-32
de 38 pines, BME680 I2C en SDA GPIO 21 / SCL GPIO 22, DHT11 DATA en GPIO 4 y
LED histórico en GPIO 25. El firmware usa esos pines. Esta confirmación no
comprueba continuidad eléctrica, dirección real del BME680 ni funcionamiento.
El LED no se acciona en esta fase.

El coordinador intenta BME680 en `0x77` y después `0x76`; si la lectura de
temperatura y humedad del BME no es válida como pareja, usa ambas métricas del
DHT11. Los rangos programados son BME680: -40..85 °C y 0..100 %RH; DHT11:
0..50 °C y 20..90 %RH. Son límites de aceptación del firmware, no mediciones
del montaje.

Para validar físicamente cada sensor aún se necesita confirmar:

- Modelo exacto y hoja de datos.
- Interfaz física: analógica, digital, I2C, OneWire u otra.
- Pin o bus y dirección.
- Alimentación y nivel lógico.
- Tiempo de conversión/lectura.
- Fórmula exacta de conversión a la escala ×100 del contrato.
- Rango físico válido y comportamiento ante fallo.
- Canal asignado y `metric_code` (1 temperatura, 2 humedad relativa).

Mientras falten estos datos, `COORDINATOR_SENSOR_COUNT` debe permanecer en 0 y
el coordinador no transmitirá.

## 6. Claves y vectores de prueba

- El vector oficial del backend está incluido en `test_vectors/`.
- La clave `00..1f` es una clave pública de prueba, no una credencial real.
- Confirmar si se usará esa clave para pruebas de banco o si se entregará una
  clave de prueba distinta.
- Para dispositivos reales se necesita un procedimiento de provisión de clave
  por coordinador y de `key_version`; no se debe grabar una clave de producción
  en el repositorio.

## 7. Gateway: red y broker

Confirmar:

- SSID y contraseña de Wi-Fi de pruebas (no incluir en el repositorio).
- Host, puerto y versión de MQTT del broker.
- Si se usará TLS y cómo se validará el certificado.
- Usuario, contraseña y ACL del gateway.
- `gateway_uid` provisionado.
- Disponibilidad de NTP y zona horaria (el firmware usa UTC).
- Política de búfer si el gateway está sin red. Este MVP descarta la trama y
  publica `buffered=false`; no hay búfer.

## 8. Reinicio físico

El contrato pide incrementar `boot_counter` en cada reinicio. El firmware lo
incrementa en cada arranque del MCU. Si se debe distinguir un corte de energía
real de un reset por watchdog o por software, se necesita un mecanismo
adicional (por ejemplo, hardware o una marca persistente) que aún no está
especificado.

## Condición de parada

Hasta que estos datos existan y estén confirmados:

- No se valida el enlace LoRa físico.
- No se confirma el pinout del SX1278 ni se genera un diagrama de radio.
- No se prueba el firmware con hardware real.
- No se declara el MVP listo para producción.
