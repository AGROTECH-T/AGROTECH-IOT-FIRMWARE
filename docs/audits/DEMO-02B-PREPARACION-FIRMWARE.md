# DEMO-02B — Preparación del firmware para la demostración LoRa

Auditoría de solo lectura y propuesta de implementación. **No se ha implementado nada.**

No se modificó ningún archivo existente (este informe es un archivo nuevo). No se compiló, no se ejecutaron pruebas, no se usó Git, no se accedió a puertos serie ni a los ESP32, no se transmitió, no se tocaron NVS ni credenciales, no se leyeron secretos y no se contactó con brokers. **Ninguna afirmación de este documento implica que el firmware funcione:** no se ha compilado ni probado con los cambios propuestos.

---

## 1. Resumen ejecutivo

- **Causa de la dependencia de MQTT [código]:**
  - el `setup()` del Gateway llama **siempre** a `gateway_credentials::loadOrProvision(...)` (`gateway/gateway.ino:848-852`);
  - si no hay credenciales válidas, esa función se queda **en un bucle infinito** esperando un `SET` por serie (`gateway/GatewayCredentials.cpp:269-272`);
  - por eso la radio (`gateway.ino:863-875`) nunca llega a inicializarse.
- **Las cinco credenciales solo las usa la ruta MQTT** [código]:
  - `gateway_uid`: tópico y Client ID (`:877-878`);
  - `mqtt_username` y `mqtt_password`: `mqtt.connect(...)` (`:382`);
  - `wifi_ssid` y `wifi_password`: `startWifi()` (`:223-228`), que solo se llama dentro de `if (kMqttPathEnabled)` (`:880-925`).
  - La recepción LoRa (`loop()`) no usa ninguna. **La dependencia es innecesaria cuando MQTT está desactivado.**
- **Cambio mínimo propuesto en el Gateway:** ejecutar la carga y el aprovisionamiento de credenciales y la construcción del tópico y el Client ID **solo si `kMqttPathEnabled`** (`gateway.ino:34-35`). Con MQTT o el diagnóstico activos, el comportamiento sería idéntico al actual (obligatorio, con parada ante fallo). Sin credenciales ficticias ni atajos.
- **Perfil RF candidato:** ya está definido en `agrotech_radio_proposed` (`shared/src/AgrotechRadioConfig.h:73-83`), con los mismos valores aprobados para preparar la demostración. Se propone **copiar esos valores en las constantes operativas** (`:89-96`) y subir la revisión del perfil, manteniendo **las dos banderas en `false`** (`:20-21`) y el estado `Proposed` (`:34-35`). Con las banderas en `false`, la activación sigue bloqueada (`agrotechRadioActivationBlockReason()`, `:179-187`).
- **Coordinador:** no necesita cambios de código. Su dependencia de credenciales (UID y clave HMAC) es **legítima**: sin la clave no puede firmar las tramas.
- **Almacén persistente:** no hay dependencia con la recepción LoRa. Solo se inicia dentro de la rama MQTT (`:899`) y solo se escribe si `AGROTECH_MQTT_TRANSPORT_ENABLED` (`:1013-1014`).
- **Dictamen: propuesta viable**, pendiente de autorización de implementación y de compilación (sección 14).

---

## 2. Archivos inspeccionados

- `gateway/gateway.ino`:
  - `kMqttPathEnabled` (`:34-35`);
  - `updateGatewayLed` (`:109-135`);
  - `validateGatewayConfig` (`:149-172`);
  - `startWifi` (`:223-228`);
  - `mqtt.connect` (`:382`);
  - `setup` (`:818-932`);
  - `loop` (`:934` en adelante, encolado en `:1013-1014`).
- `gateway/GatewayCredentials.cpp`: `validCredentials` (`:79-83`) y `loadOrProvision` (desde `:224` hasta el cierre del namespace en `:323`; bucle de aprovisionamiento a partir de `:269-272`). `GatewayCredentials.h`.
- `gateway/GatewayConfig.h` (macros `MQTT_HOST`, `MQTT_PORT`, `MQTT_CA_CERT` y las del almacén) y `gateway/GatewayDiagnostics.h` (`gateway_diag::kEnabled`).
- `coordinator/coordinator.ino`:
  - `initializeRadio` (`:204-229`);
  - `transmitFrame` (`:231-259`);
  - `setup` (`:372-416`);
  - `loop` (`:418-507`).
- `coordinator/CoordinatorCredentials.cpp`: `loadOrProvision` (`:113-150`) y `storeProvisioningLine` (`:55-…`).
- `shared/src/AgrotechRadioConfig.h` (completo) y `shared/src/AgrotechLinkTest.h` (`kEnabled`, `kFrameLimit`).
- Informe previo: `docs/audits/DEMO-01-DIAGNOSTICO-LORA.md`.

No se leyeron credenciales ni archivos locales de configuración (`GatewayConfig.local.h` no existe en el repositorio).

---

## 3. Causa exacta de la dependencia MQTT

```text
gateway.ino:844   config_valid = validateGatewayConfig()      ← solo valida MQTT si kMqttPathEnabled (correcto)
gateway.ino:848   if (!gateway_credentials::loadOrProvision(...))   ← SIEMPRE, sin condición
                     → GatewayCredentials.cpp:250-259: carga gateway_uid, username de 64,
                       password vacío, SSID y password Wi-Fi, y los valida (validCredentials, :79-83)
                     → si faltan o no son válidas: :269-272 for(;;) esperando un SET
                       (no devuelve nunca: el arranque se detiene aquí)
gateway.ino:852     haltForever("credential storage unavailable")  ← si falla Preferences
gateway.ino:863   radio LoRa (solo si agrotechRadioCanActivate)   ← nunca se alcanza sin credenciales
```

| Credencial | Uso real | ¿La necesita la ruta LoRa? |
|---|---|---|
| `gateway_uid` | Tópico y Client ID MQTT (`:877-878`) | No |
| `mqtt_username` y `mqtt_password` | `mqtt.connect` (`:382`, dentro de `ensureMqtt`, que solo llama `mqttTask`) | No |
| `wifi_ssid` y `wifi_password` | `WiFi.begin` (`:228`), solo desde la rama `kMqttPathEnabled` | No |

| Dependencia | Clasificación |
|---|---|
| Credenciales cuando `kMqttPathEnabled` es verdadero (producción MQTT o diagnóstico) | **Legítima**: se conserva |
| Credenciales cuando `kMqttPathEnabled` es falso (solo LoRa) | **Innecesaria**: se elimina |
| `validateGatewayConfig` (MQTT solo con `kMqttPathEnabled`; LoRa solo con el transporte LoRa activo) | Correcta, sin cambios |

---

## 4. Flujo de inicialización actual del Coordinador

| Paso | Ubicación | Comportamiento con las banderas en `false` |
|---|---|---|
| Serie y mensaje de arranque | `setup()`, `:373-376` | — |
| Firma y motivo de bloqueo del perfil | `:377-384` | Imprime `activation: transport flag is false` |
| LED | `:386` | — |
| Configuración (intervalo y checkpoint > 0) | `initializeCoordinatorConfig`, `:388` | — |
| Credenciales (UID `CO-`, `key_version`, clave HMAC) | `coordinator_credentials::loadOrProvision`, `:389-391` → `CoordinatorCredentials.cpp:113-150` | **Legítimas**: sin clave no se pueden firmar las tramas |
| Sensores | `initializeSensors()`, `:392` | BME680 y DHT11 sin cambios |
| Radio | `initializeRadio()`, `:393` → `:204-210` | **Vuelve antes de `SPI.begin`**: no hay inicialización SPI ni `LoRa.begin`, y `radio_ready = false` |
| Contadores NVS | `:395-401` | — |
| Bucle | `loop()`: adquisición cada 60 s y `encodeFrame` → `transmitFrame()` (`:231-233`) | **Vuelve sin transmitir** si `!radio_ready` |

**Cambio mínimo para preparar el perfil:** **ninguno en el código del Coordinador**. Los parámetros vienen de `CoordinatorConfig.h:20-27`, que mapea las constantes compartidas. Basta con el cambio de la sección 8.1.

---

## 5. Flujo de inicialización actual del Gateway

| Paso | Ubicación | Observación |
|---|---|---|
| Serie, LED, firma del perfil, modo de prueba de enlace | `setup()`, `:819-842` | — |
| `validateGatewayConfig()` | `:844-847` | Correcto |
| **Credenciales** | `:848-852` | **Incondicional: causa del bloqueo** |
| Diagnóstico (si `gateway_diag::kEnabled`) | `:854-861` | — |
| Radio RA-01 | `:863-875` | Solo si `!gateway_diag::kEnabled && agrotechRadioCanActivate()`. Independiente de MQTT |
| Tópico y Client ID | `:877-878` | Solo los usa MQTT; se calculan siempre |
| MQTT: TLS, NTP, `mqtt.begin`, almacén, Wi-Fi y `mqttTask` | `:880-925` | Solo si `kMqttPathEnabled` y la configuración es válida |
| Mensajes finales | `:926-931` | — |
| `loop()` | `:934` en adelante | LoRa: `parsePacket` → validación → registro. Encolado a MQTT solo si `AGROTECH_MQTT_TRANSPORT_ENABLED` (`:1013-1014`) |

**Tareas:**
- `loop()` en el núcleo 1 se ocupa de LoRa.
- `mqttTask` en el núcleo 0 se ocupa de Wi-Fi, TLS, MQTT y del mantenimiento del almacén, y **solo se crea** en la rama MQTT (`:906`). En una compilación solo LoRa no existe.

**Modo de prueba de enlace:** `agrotech_link_test::kEnabled = false`. Con `true`, `loop()` registra `[LT] rx …` y las estadísticas, sin depender de MQTT.

---

## 6. Separación propuesta entre LoRa y MQTT

| Bloque | Contenido | Condición |
|---|---|---|
| **A. General** | Serie, LED, firma del perfil, `validateGatewayConfig()` y diagnóstico | Siempre (sin cambios) |
| **B. LoRa** | `SPI.begin`, `LoRa.*` y `loop()` de recepción | `!gateway_diag::kEnabled && agrotechRadioCanActivate()` (sin cambios) |
| **C. MQTT** | **Credenciales y aprovisionamiento (se trasladan aquí)**, tópico, Client ID, TLS, NTP, `mqtt.begin`, almacén, Wi-Fi y `mqttTask` | `kMqttPathEnabled` (producción MQTT **o** diagnóstico) |

**Garantías:**
- Con `kMqttPathEnabled` verdadero, el bloque C empieza exactamente como hoy: credenciales obligatorias, bucle de `SET` si faltan, parada si falla Preferences.
- Con `kMqttPathEnabled` falso, no se abre Preferences y no se piden credenciales. Las credenciales que ya estén guardadas en NVS **se conservan intactas** para una compilación posterior con MQTT.

---

## 7. Archivos que sería necesario modificar

| Archivo | Cambio | ¿Obligatorio? |
|---|---|---|
| `gateway/gateway.ino` | Condicionar las credenciales y el tópico/Client ID a `kMqttPathEnabled` (8.2) | Sí |
| `shared/src/AgrotechRadioConfig.h` | Constantes operativas = perfil candidato; revisión 2; banderas sin cambios (8.1) | Sí (para "perfil definido") |
| `coordinator/*` | Ninguno | — |
| `gateway/GatewayCredentials.*` | Ninguno (el aprovisionamiento no se altera) | — |
| `gateway/gateway.ino` (`updateGatewayLed`, `:122`) | Opcional (8.3) | No |
| `gateway/gateway.ino` (registro de UID, `seq` y valores en `loop()`) | Opcional (DEMO-01 P6) | No |
| Documentación (`BUILD-IOT-03C` y `03D` citan la firma `0x82811B29`) | Actualizar la firma y la revisión cuando se implemente | Recomendado |

---

## 8. Descripción exacta de cada modificación propuesta

### 8.1 `shared/src/AgrotechRadioConfig.h` — perfil candidato definido y no autorizado

**Se mantienen sin cambios:**
- `AGROTECH_LORA_TRANSPORT_ENABLED = false` y `AGROTECH_LORA_PROFILE_AUTHORIZED = false` (`:20-21`);
- `AGROTECH_MQTT_TRANSPORT_ENABLED = false` (`:24`);
- `AGROTECH_LORA_PROFILE_STATUS = Proposed` (`:34-35`);
- el `static_assert` (`:103-105`).

**Cambios propuestos:**

```text
:39   AGROTECH_LORA_PROFILE_REVISION = 1   →   2
:89   AGROTECH_LORA_FREQUENCY_HZ       = 0     → agrotech_radio_proposed::kFrequencyHz        (433500000)
:90   AGROTECH_LORA_SPREADING_FACTOR   = 7     → agrotech_radio_proposed::kSpreadingFactor    (7)
:91   AGROTECH_LORA_BANDWIDTH_HZ       = 125000→ agrotech_radio_proposed::kBandwidthHz        (125000)
:92   AGROTECH_LORA_CODING_RATE_DENOM. = 5     → agrotech_radio_proposed::kCodingRateDenominator (5)
:93   AGROTECH_LORA_TX_POWER_DBM       = -1    → agrotech_radio_proposed::kTxPowerDbm         (7)
:94   AGROTECH_LORA_SYNC_WORD          = 0xA6  → agrotech_radio_proposed::kSyncWord           (0xA6)
:95   AGROTECH_LORA_PREAMBLE_LENGTH    = 0     → agrotech_radio_proposed::kPreambleLength     (8)
:96   AGROTECH_LORA_USE_CRC            = -1    → agrotech_radio_proposed::kUseCrc             (1)
```

Además, actualizar el comentario de las líneas 85-88: el perfil operativo pasa a "candidato, completo, **no autorizado**; el bloqueo depende de las dos banderas".

**Efectos [lectura del código]:**
- `agrotechRadioProfileValid()` pasa a `true` (`:162-176`);
- `agrotechRadioActivationBlockReason()` sigue devolviendo `"transport flag is false"` (`:180`), así que `agrotechRadioCanActivate()` es `false` en los dos dispositivos;
- la firma (`:202-215`) cambia, y será la misma en los dos.

**Contrapartida:** la barrera "perfil incompleto" desaparece y solo quedan las dos banderas y el `static_assert`. Para emitir sigue haciendo falta poner **las dos** a `true`. Una alternativa más conservadora es dejar las constantes operativas sin definir hasta la autorización: el perfil ya figura en `agrotech_radio_proposed`, pero entonces no se cumpliría el criterio de "perfil definido" en la configuración operativa. **Decisión del propietario.**

### 8.2 `gateway/gateway.ino` — credenciales solo con MQTT

Sustituir las líneas 848-852 y trasladar las 877-878:

```cpp
// Pseudocódigo de setup(), entre :844 y :880
const bool config_valid = validateGatewayConfig();          // sin cambios
if (!config_valid) Serial.println("[CONFIG] incomplete; transports remain disabled");

// Las credenciales del Gateway (UID, token MQTT y Wi-Fi) solo las usa la ruta
// MQTT o la de diagnóstico. Sin ella no se abre NVS ni se pide SET; las
// credenciales ya guardadas no se tocan.
if (kMqttPathEnabled) {
  if (!gateway_credentials::loadOrProvision(
          gateway_uid, sizeof(gateway_uid), mqtt_username, sizeof(mqtt_username),
          mqtt_password, sizeof(mqtt_password), wifi_ssid, sizeof(wifi_ssid),
          wifi_password, sizeof(wifi_password)))
    haltForever("credential storage unavailable");            // idéntico a :852
  snprintf(topic, sizeof(topic), "agrotech/v1/gw/%s/up", gateway_uid);   // antes :877
  snprintf(client_id, sizeof(client_id), "gw-%s", gateway_uid);         // antes :878
} else {
  Serial.println("[PROVISION] skipped: MQTT path disabled; gateway network credentials not required");
}

// :854-875 (diagnóstico y radio) sin cambios
// :880 if (kMqttPathEnabled) { ... }  sin cambios
```

- **Orden:** las credenciales siguen cargándose **antes** que la radio cuando hay MQTT, igual que ahora. Sin MQTT, la radio se inicializaría sin esperar ningún `SET`, si su propia condición lo permite.
- **Variables:** `gateway_uid`, `mqtt_username`, `mqtt_password`, `wifi_ssid`, `wifi_password`, `topic` y `client_id` quedan vacíos (inicializados a `{}` en `:52-57`) cuando no hay MQTT. Su único uso está en la ruta MQTT [comprobado con grep: `:228`, `:382`, `:848-851`, `:877-878`].
- **Diagnóstico:** `printDiagStatus()` imprime `topic` y `client_id`, pero solo existe con `gateway_diag::kEnabled`, que implica `kMqttPathEnabled` y por tanto credenciales cargadas.

### 8.3 Opcional (no necesario para la demostración)

**LED (`updateGatewayLed`, `:122`):** `!kMqttPathEnabled` hace que, en una compilación solo LoRa, el LED muestre **siempre el patrón de error** (tres destellos), salvo el destello de recepción de 120 ms. Para la demostración podría interpretarse mal. Hay dos opciones:
- dejarlo como está y documentarlo;
- excluir `!kMqttPathEnabled` del patrón de error cuando la radio esté lista.

Decisión aparte; no se recomienda mezclarlo con 8.2.

---

## 9. Riesgos de regresión

| # | Riesgo | Probabilidad e impacto | Mitigación |
|---|---|---|---|
| R1 | Cambio de comportamiento en las compilaciones MQTT o de diagnóstico | Nulo si `kMqttPathEnabled` es verdadero: el código queda en la misma rama y en el mismo orden | Prueba T3 |
| R2 | En las compilaciones solo LoRa ya no aparece el aviso de `REPROVISION` ni de `SET` del Gateway | Esperado y documentado | Aprovisionar con una compilación MQTT cuando haga falta |
| R3 | Algún uso futuro de `gateway_uid` fuera de MQTT encontraría una cadena vacía | Hoy no existe | Revisión de código en el futuro |
| R4 | 8.1 elimina la barrera de perfil incompleto | Solo quedan las dos banderas y el `static_assert` | Las dos banderas en `false`, revisión obligatoria antes de cambiarlas y criterio de aceptación T5 |
| R5 | La firma del perfil cambia y la documentación queda desactualizada (`0x82811B29`) | Bajo | Actualizar los documentos 03C y 03D al implementar |
| R6 | Con las banderas en `false`, el compilador puede eliminar el código de radio, así que el código real de la radio sigue sin compilarse (DEMO-01 P8) | Medio | Compilar copias temporales con las banderas en `true` **fuera del repositorio**, sin flashearlas (T4) |
| R7 | Potencia de 7 dBm: es potencia conducida, no radiada. La antena helicoidal está sin medir | Fuera del alcance de software | Verificación física antes de autorizar |

---

## 10. Compatibilidad con el código existente

| Elemento | Compatibilidad |
|---|---|
| Protocolo Frame v1 (`AgrotechProtocol.*`) | Sin cambios |
| `GatewayCredentials.*`, formato del `SET`, `cred_state`, `REPROVISION` | Sin cambios |
| Coordinador | Sin cambios de código; recibe el perfil de la cabecera compartida |
| MQTT-RESILIENCE-04 y el almacén MQTT-QUEUE-PERSISTENCE-05 | Sin cambios; siguen en la rama MQTT (`:880-925`) |
| `validateGatewayConfig` | Sin cambios |
| Firmas del perfil | Iguales en los dos dispositivos (cabecera compartida) |
| Pruebas de host existentes (`test_vectors/*`) | No dependen de `gateway.ino` ni del perfil operativo [inferencia por lectura: `frame_log_tests.cpp` solo enlaza `GatewayFrameLog.cpp`] |

---

## 11. Qué debe permanecer desactivado

| Elemento | Valor | Ubicación |
|---|---|---|
| Transporte LoRa | `AGROTECH_LORA_TRANSPORT_ENABLED = false` | `AgrotechRadioConfig.h:20` |
| Autorización del perfil | `AGROTECH_LORA_PROFILE_AUTHORIZED = false` | `:21` |
| MQTT | `AGROTECH_MQTT_TRANSPORT_ENABLED = false` | `:24` |
| Estado del perfil | `Proposed` | `:34-35` |
| Modo de prueba de enlace | `agrotech_link_test::kEnabled = false` | `AgrotechLinkTest.h` |
| Diagnóstico | `GATEWAY_MQTT_DIAG_ENABLED` sin definir (0) | `GatewayDiagnostics.h` |
| Almacén persistente | No se inicia (solo en la rama MQTT) | `gateway.ino:899` |

---

## 12. Secuencia recomendada para la futura implementación

Cada paso necesita una autorización propia.

1. Aprobar 8.1 (o la alternativa conservadora) y 8.2. Decidir sobre 8.3.
2. Implementar 8.2 en `gateway.ino`.
3. Implementar 8.1 en `AgrotechRadioConfig.h`.
4. Revisión estática independiente del diff.
5. Compilar los dos sketches con las banderas en `false`, y copias temporales fuera del repositorio con las banderas en `true` y sin flashear (T1 a T4).
6. Actualizar la documentación de firma y revisión (03C y 03D).
7. **Después, en otra fase** (DEMO-03): verificación física de la antena y la potencia, autorización formal y activación de las banderas.

---

## 13. Pruebas que deberán realizarse después

Todas son propuestas; ninguna se ha ejecutado.

| # | Prueba | Tipo | Criterio |
|---|---|---|---|
| T1 | Compilación del Coordinador y del Gateway (banderas en `false`) | Build | 0 errores; 0 avisos nuevos en `coordinator/`, `gateway/` y `shared/` |
| T2 | Compilación del Gateway con MQTT activo (copia temporal y broker ficticio, como en la validación de MQTT-QUEUE-PERSISTENCE-05) | Build | El aprovisionamiento sigue presente en la rama MQTT |
| T3 | Compilación de diagnóstico (`GATEWAY_MQTT_DIAG_ENABLED 1` en una copia) | Build | Credenciales obligatorias, igual que hoy |
| T4 | Copias temporales con las banderas LoRa en `true` (sin flashear) | Build | El código de radio compila y enlaza |
| T5 | Revisión de los registros esperados | Estática | Los dos dispositivos con la misma firma (revisión 2); `activation: transport flag is false` |
| T6 | Arranque del Gateway solo LoRa **sin** credenciales (requiere autorización de hardware; sin radio activa) | Física | Llega a `[READY]` sin el aviso `[PROVISION] Send…`; NVS sin escrituras |
| T7 | Arranque del Gateway MQTT sin credenciales | Física | Aparece el aviso de `SET` como hoy |
| T8 | Arranque del Coordinador con las banderas en `false` | Física | Sin `SPI.begin` ni `LoRa.begin`; `[LoRa] disabled: transport flag is false`; tramas codificadas y no transmitidas |
| T9 | Pruebas de host existentes (`frame_log_tests`, `host_tests` y las de failover) | Host | Sin regresiones |

---

## 14. Dictamen técnico

**PROPUESTA VIABLE, PENDIENTE DE AUTORIZACIÓN DE IMPLEMENTACIÓN.**

Cumple los criterios de aceptación **en diseño**; no está compilada ni probada:
- el Coordinador se inicializa sin transmitir (sin cambios de código);
- el Gateway podría arrancar sin credenciales MQTT cuando MQTT está desactivado (8.2);
- las validaciones MQTT siguen siendo obligatorias cuando MQTT está activado, en la misma rama y el mismo orden;
- el perfil candidato queda definido en la configuración operativa sin autorización (8.1), con las dos banderas en `false`;
- nada se activa automáticamente.

Quedan pendientes las decisiones sobre la barrera de perfil incompleto (8.1, R4) y sobre el LED (8.3), y las compilaciones T1 a T4.
