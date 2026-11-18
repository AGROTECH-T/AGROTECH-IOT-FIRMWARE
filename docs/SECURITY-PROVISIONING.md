# BUILD-IOT-02 — Provisioning and device security

## Coordinator

1. An authenticated farm administrator registers a coordinator through
   `POST /api/v1/iot/farms/{farm_id}/coordinators/` with its display name.
2. The backend creates its UID and a random 32-byte HMAC key, stores only the
   AES-256-GCM ciphertext, and returns the Base64 key exactly once with
   `key_version` and `Cache-Control: no-store`.
3. On a trusted local USB serial connection, send
   `SET <uid> <key_version> <base64_key>`. The firmware validates the format,
   stores the UID, version and key in ESP32 NVS, and never echoes the line.
4. For rotation, request a backend rotation, then at device startup send
   `REPROVISION`, wait for its confirmation, and send the new `SET` line. The
   coordinator sequence counters remain in their separate NVS namespace.

Frame v1 bytes are unchanged. Each frame carries the provisioned UID and
version and uses the provisioned key for HMAC-SHA256. The firmware does not
include a key or operational identity in source control.

## Gateway

The backend creates a gateway UID and individual MQTT password. It returns the
UID as the MQTT username and password once. At startup, send one tab-separated
line over trusted USB serial containing `SET`, UID, MQTT username, MQTT
password, Wi-Fi SSID and Wi-Fi password. The firmware stores these values in
ESP32 NVS, then uses the individual MQTT username/password. To rotate, send
`REPROVISION`, wait for the prompt, and load the replacement credentials.

Broker host, port and CA trust certificate belong in the ignored local file
`gateway/GatewayConfig.local.h`. Empty values fail closed. The gateway uses
`WiFiClientSecure`, validates the configured CA, and does not offer an
unencrypted MQTT connection. The broker ACL should restrict `gw-<UID>` to
publishing only `agrotech/v1/gw/<UID>/up` and validate that username/password
against the backend gateway credential service. Do not enable a real broker
until that broker-side authentication and ACL are installed.

## Hardware limits and handling

ESP32 NVS is not a dedicated secure element. The software avoids compiling or
logging secrets and stores them in NVS, but this does not provide absolute
physical protection. Flash readout may expose credentials unless the exact
board's secure boot, flash encryption and eFuse lifecycle are deliberately
configured and verified. Local serial provisioning requires physical access;
use a trusted operator and serial terminal, keep terminal session logging off,
and avoid leaving plaintext credentials in copied command history. No
production keys or broker parameters are included here.

## Production MQTT provisioning (BUILD-IOT-05)

Do not run these steps until the RF profile and the MQTT transport are
authorized. Never commit any of the values below.

1. **Broker**: TLS-only listener (no plaintext port), `allow_anonymous false`,
   TLS 1.2+, server certificate issued by the CA the gateway pins. The
   certificate must carry SAN = the exact `MQTT_HOST`, KeyUsage,
   ExtendedKeyUsage serverAuth and AKI/SKI. Persistence on, so the backend
   consumer's `clean_session=false` session survives broker restarts.
2. **Authentication**: one username/password per gateway (username = UID,
   client id `gw-<UID>`). The broker validates against the backend gateway
   credential service, or a password file regenerated from it. Disable a
   credential on the broker when the backend revokes or rotates it.
3. **ACL**: gateway `<UID>` may only `write agrotech/v1/gw/<UID>/up`; no read
   access at all. Only the backend consumer user may
   `read agrotech/v1/gw/+/up`. Under MQTT 3.1.1 a denied publish is still
   acknowledged, so the gateway cannot detect a wrong ACL; check it with
   `tools/test_gateway_mqtt_broker.py` against a staging copy of the ACL.
4. **Gateway build config**: create the ignored
   `gateway/GatewayConfig.local.h` with `MQTT_HOST`, `MQTT_PORT` (8883) and
   `MQTT_CA_CERT` (PEM of the issuing CA, not the server leaf, so the server
   certificate can be renewed without reflashing). Empty values fail closed.
5. **Device credentials**: load UID, MQTT username/password and Wi-Fi over
   trusted USB serial (`SET` line, see "Gateway" above). They live in NVS only.
6. **Clock**: NTP must be reachable from the gateway network. Without it,
   frames are sent with `lora_rx_at:null`, `clock_synced:false` and the
   backend marks measurements as approximate.
7. **Buffering**: the RAM queue holds `GATEWAY_MQTT_QUEUE_DEPTH` frames (16
   by default) and is lost on reboot. Size it to the longest expected outage
   × coordinator frame rate, and check free heap on the device.
