# BUILD-IOT-03D-FINAL — preparación de la prueba física

Estado: **preparado y verificado en compilación. Transmisión bloqueada.**
No se cargó firmware, no se transmitió y no se modificó código, perfil RF ni
banderas.

## 1. Estado confirmado de los sketches

| Elemento | Coordinador | Gateway |
|---|---|---|
| Perfil compartido | `CoordinatorConfig.h` → `AgrotechRadioConfig.h` | `GatewayConfig.h` → `AgrotechRadioConfig.h` |
| Revisión de perfil | 1 | 1 |
| Firma (FNV-1a) | `0x82811B29` | `0x82811B29` |
| Log de perfil | `[LoRa] profile revision=... signature=0x...` | Idéntico |
| Log de bloqueo | `[LoRa] activation: <motivo>` y `[LoRa] disabled: <motivo>` | `[LoRa] activation: <motivo>` |
| Modo prueba | `kEnabled=false` → `[LT] link test mode disabled` | Igual |
| Estado final | `[READY] sensor acquisition enabled; LoRa is not activated` | `[READY] gateway initialized` |

La firma se calculó también de forma independiente a partir de los valores
operativos (revisión 1, frecuencia 0, SF7, BW125000, CR5, potencia -1,
sync 0xA6, preámbulo 0, CRC -1): `0x82811B29`. Ambos sketches usan la misma
función compartida, por lo que deben imprimir el mismo valor.

## 2. Las tres protecciones siguen desactivadas

| Protección | Valor | Efecto |
|---|---|---|
| `AGROTECH_LORA_TRANSPORT_ENABLED` | `false` | Primer motivo de bloqueo |
| `AGROTECH_LORA_PROFILE_AUTHORIZED` | `false` | Segundo motivo de bloqueo |
| Perfil operativo completo | No: `FREQUENCY_HZ=0`, `TX_POWER_DBM=-1`, `PREAMBLE_LENGTH=0`, `USE_CRC=-1` | Tercer motivo de bloqueo |

Además, `AGROTECH_MQTT_TRANSPORT_ENABLED=false` y
`agrotech_link_test::kEnabled=false`. No se tocó ninguno de estos valores.

Con `TRANSPORT_ENABLED=false`, el motivo mostrado es siempre
`transport flag is false`; las otras dos protecciones no llegan a evaluarse.
Esto es correcto y esperado.

## 3. Bloqueo de radio en código

- Coordinador: `initializeRadio()` llama a `agrotechRadioActivationBlockReason()`
  y retorna sin inicializar SPI/LoRa si hay motivo. `transmitFrame()` retorna de
  inmediato si `radio_ready == false`.
- Gateway: `setup()` solo ejecuta `SPI.begin`/`LoRa.begin` si
  `agrotechRadioCanActivate()`; `loop()` retorna antes de `LoRa.parsePacket()`
  si `radio_ready == false`.
- Sin radio inicializada no hay `SPI.begin`, ni `LoRa.begin`, ni `receive()`,
  ni paquetes.

## 4. Procedimiento exacto en Arduino IDE

### 4.1 Preparación (una vez)

1. Copiar la carpeta `shared/` completa a
   `C:\Users\<usuario>\Documents\Arduino\libraries\AgrotechProtocol`
   (debe contener `AgrotechProtocol.h`, `AgrotechRadioConfig.h`,
   `AgrotechLinkTest.h`, etc.). Reemplazar la versión anterior.
2. Confirmar que `coordinator/` y `gateway/` **no** contienen copias locales de
   `Agrotech*.h/.cpp` (provocan `multiple definition`; ver
   `docs/BUILD-IOT-03C-FIX.md`).
3. Conectar el ESP32 del coordinador. En Arduino IDE: `Herramientas` →
   `Placa` → modelo real de ESP32 (p. ej. `ESP32 Dev Module`) y el puerto COM
   correspondiente.
4. Abrir `coordinator/coordinator.ino` (carpeta y archivo con el mismo nombre).
5. `Verificar` (compila, **no** carga ni transmite). Resultado esperado:
   330.800 B de programa (25 %), 24.092 B de RAM.
6. Cambiar a la placa y el puerto del gateway. Abrir
   `gateway/gateway.ino`, `Verificar`. Resultado esperado: 566.304 B (43 %),
   31.932 B de RAM.
7. **Esta fase termina aquí.** No usar `Subir`.

### 4.2 Carga y observación (solo tras autorización futura)

1. Antenas conectadas en ambos nodos antes de energizar.
2. Gateway: seleccionar su placa/puerto, `Subir`, abrir Monitor Serie a
   **115200 baudios**.
3. Coordinador: seleccionar su placa/puerto (idealmente otra ventana de Arduino
   IDE), `Subir`, abrir Monitor Serie a 115200.
4. Dejar que el coordinador complete los 10 envíos (uno cada 60 s). No cerrar
   los monitores ni resetear las placas durante la prueba.
5. Registrar los resultados en la tabla de la sección 8.
6. Al terminar, restaurar `kEnabled=false` y volver a cargar ambos sketches.

Notas: cada Monitor Serie ocupa un puerto; no abrir el mismo puerto dos veces.
El LED del coordinador no se usa en modo prueba (queda apagado tras el
arranque). El gateway parpadea su LED 120 ms por trama válida.

## 5. Mensajes esperados con la transmisión bloqueada (estado actual)

### Coordinador

```text
AGROTECH Coordinator BUILD-IOT-01 - acquire and encode only
[LoRa] profile revision=1 signature=0x82811B29
[LoRa] activation: transport flag is false
[LoRa] disabled: transport flag is false
[LT] link test mode disabled
[READY] sensor acquisition enabled; LoRa is not activated
```

Cada 60 s, si hay sensores conectados y responden:

```text
[FRAME] valid Frame v1 generated; boot=<n> seq=<n> readings=<1-2> bytes=<...>
```

Sin sensores no aparecen líneas de ciclo (el LED pasa a error). En ningún caso
debe aparecer `[LoRa] Frame v1 queued` ni `[LT] tx`.

### Gateway

```text
AGROTECH Gateway MVP - LoRa frame v1
[LoRa] profile revision=1 signature=0x82811B29
[LoRa] activation: transport flag is false
[LT] link test mode disabled
[CONFIG] incomplete; transports remain disabled
[LoRa] receiver disabled; authorized shared profile required
[MQTT] transport disabled; no network connection attempted
[READY] gateway initialized
```

`[CONFIG] incomplete` se refiere a los datos MQTT/NTP de `GatewayConfig.local.h`,
no al perfil LoRa. No debe aparecer ninguna línea `[LoRa] rx` ni `[LT] rx`.

## 6. Cómo identificar un bloqueo correcto

Señales de bloqueo correcto (todas deben cumplirse):

1. Ambos monitores muestran la **misma** firma `0x82811B29` y revisión 1.
2. Ambos muestran `[LoRa] activation: transport flag is false`.
3. El coordinador muestra `[LoRa] disabled: transport flag is false`.
4. Aparece `[LT] link test mode disabled` en ambos.
5. El gateway muestra `[LoRa] receiver disabled; authorized shared profile required`
   y `[MQTT] transport disabled...`.
6. No aparecen `[LoRa] configured; transmission enabled...`,
   `[LoRa] Frame v1 queued`, `[LT] tx`, `[LT] rx` ni `[LoRa] rx`.

Señal de alarma: que aparezca `transmission enabled` o un `[LT] tx` con
`started=1`. En ese caso detener la prueba, desconectar alimentación y revisar
las banderas antes de continuar.

Diferencias de firma entre nodos indican versiones distintas de
`AgrotechRadioConfig.h` (librería `AgrotechProtocol` desactualizada en una
placa): no continuar.

## 7. Mensajes esperados tras autorización (referencia)

Solo con perfil autorizado, banderas en `true` y `kEnabled=true`:

```text
[LT] link test mode ENABLED
[LoRa] configured; transmission enabled by build configuration   (coordinador)
[LT] tx id=CO-XXXXXXXXXXXX/b<n>/s<n>/tXXXXXXXX bytes=67 air_ms=123.14 hex=01 43 4F ...
[LT] tx_stats encoded=1 encode_err=0 started=1 done=0 busy=0 write_err=0 start_err=0 timeouts=0
[LT] rx id=CO-XXXXXXXXXXXX/b<n>/s<n>/tXXXXXXXX bytes=67 expected=67 rssi=-45 snr=9.75 dt_ms=60000 hex=01 43 4F ...
[LT] rx_stats frames=1 malformed=0 oversized=0 incomplete=0 lost=0 duplicates=0 out_of_order=0
```

Recordatorio: el gateway no verifica HMAC, no hay ACK y la primera trama solo
fija la línea base de pérdidas. `air_ms` es cálculo, no medición.

## 8. Tabla de registro

Una fila por envío. `t` es el prefijo del tag; debe coincidir entre TX y RX.

| # | Fecha/hora | Nodo | ID `uid/boot/seq` | `t` (tag) | bytes | hex idéntico | air_ms | RSSI | SNR | dt_ms | Resultado | Observaciones |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 TX | | CO | | | | — | | — | — | — | | |
| 1 RX | | GW | | | | sí / no | | | | | OK / perdido / corrupto | |
| 2 TX | | CO | | | | — | | — | — | — | | |
| 2 RX | | GW | | | | sí / no | | | | | OK / perdido / corrupto | |
| 3 TX | | CO | | | | — | | — | — | — | | |
| 3 RX | | GW | | | | sí / no | | | | | OK / perdido / corrupto | |
| … | | | | | | | | | | | | |
| 10 TX | | CO | | | | — | | — | — | — | | |
| 10 RX | | GW | | | | sí / no | | | | | OK / perdido / corrupto | |

Totales al final: `tx_stats` y `rx_stats` copiados literalmente, más una frase
de conclusión (p. ej. "10/10 tramas recibidas, 0 malformadas, 0 pérdidas por
secuencia").

## 9. Validación de esta fase

- `coordinator`: compilado correctamente — 330.800 B programa, 24.092 B RAM.
- `gateway`: compilado correctamente — 566.304 B programa, 31.932 B RAM.
- Compilación con `arduino-cli` de Arduino IDE, core ESP32 3.3.12, FQBN
  `esp32:esp32:esp32`, `shared` montado como `AgrotechProtocol`, desde las
  carpetas definitivas `coordinator/` y `gateway/`.
- Sin carga de firmware, sin transmisión/recepción, sin Wi-Fi/MQTT.
- Sin cambios de código en esta fase: solo este documento.

Pendiente para la prueba física: autorización del perfil (frecuencia, potencia,
antena, pérdidas y ciclo de trabajo), habilitar banderas y `kEnabled` en una
fase separada, y completar la tabla de resultados. Hasta entonces **no existe
evidencia de comunicación LoRa**.
