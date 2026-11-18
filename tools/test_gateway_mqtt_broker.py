#!/usr/bin/env python3
"""BUILD-IOT-05 reproducible broker test for the gateway MQTT contract.

Starts a disposable Mosquitto container (TLS only, per-device users, ACL) and
drives it with clients configured exactly like the parties in production:

* "gateway": MQTT 3.1.1, clean session, keepalive 30 s, client id gw-<UID>,
  username <UID>, QoS 1, CA-verified TLS -- the settings in gateway.ino.
* "consumer": MQTT 3.1.1, clean_session=False, QoS 1 on agrotech/v1/gw/+/up --
  the settings in the backend apps/iot/mqtt/consumer.py.

It proves broker-side behavior and the store-and-forward *protocol* the
firmware follows (hold frames during an outage, resend byte-identical JSON in
order after reconnecting). It does NOT execute the ESP32 firmware; that still
requires the physical gateway.

Requirements: Docker with eclipse-mosquitto:2, Python with paho-mqtt>=2 and
cryptography (the backend .venv has both):

  & "C:\\Users\\israe\\Videos\\AGROTECH-T BACKEND\\.venv\\Scripts\\python.exe" `
      tools\\test_gateway_mqtt_broker.py

Only disposable test credentials and throw-away certificates are used.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
except ImportError as exc:  # pragma: no cover - dependency guard
    print(f"Missing test dependency: {exc}", file=sys.stderr)
    print("Use the backend .venv Python (paho-mqtt + cryptography).", file=sys.stderr)
    sys.exit(2)

IMAGE = "eclipse-mosquitto:2"
UID = "GW-7K3M9QXT2A"
OTHER_UID = "GW-ZZZZZZZZZZ"
TOPIC = f"agrotech/v1/gw/{UID}/up"
OTHER_TOPIC = f"agrotech/v1/gw/{OTHER_UID}/up"
CONSUMER_FILTER = "agrotech/v1/gw/+/up"
CONSUMER_USER = "consumer-test"
CONSUMER_ID = "consumer-contract-test"
TEST_PASSWORD = "gateway-test-only-2026"
OTHER_PASSWORD = "other-gateway-test-only-2026"
CONSUMER_PASSWORD = "consumer-test-only-2026"
GATEWAY_KEEPALIVE = 30  # gateway.ino: mqtt.setKeepAlive(30)

# Official Frame v1 vector payload, validated against the Django parser in
# BUILD-IOT-04 (exact output of shared/src/AgrotechMqttPayload.cpp).
BASE_PAYLOAD = (
    '{"v":1,"frame":"AUNPLTdLM005UVhUMkEAAQAAAAcAAAB4AgABAQnm'
    'AAICGLXdurSB47T3HcyTRSsdCUOtJBY8WdajwubyetPra6sa9Q==",'
    '"lora_rx_at":"2026-11-15T00:20:31Z","clock_synced":true,'
    '"buffered":false,"rssi":-87,"snr":7.50}'
)


# Exact output of the BUILD-IOT-06 diagnostic (tools/test_gateway_diag_vector.py)
# and the SHA-256 the backend stores as IotMqttDelivery.payload_sha256.
_DIAG = (
    '{"v":1,"frame":"AUNPLTdLM005UVhUMkEAAQAAAAcAAAB4AgABAQnm'
    'AAICGLXdurSB47T3HcyTRSsdCUOtJBY8WdajwubyetPra6sa9Q==",'
    '"lora_rx_at":null,"clock_synced":false,"buffered":%s,"rssi":-87,"snr":7.50}'
)
DIAG_PAYLOADS = [
    (_DIAG % "false", "c4ce765421841ac930f2aa091a60e16839f6c0f94ece847e5792374484162ecd"),
    (_DIAG % "true", "344a30ffbb48a612349c0902a50b7179f3f6ff37fcb4b45005a8220bc11fa0b2"),
]


def buffered(payload: str, snr: str | None = None) -> str:
    out = payload.replace('"buffered":false', '"buffered":true')
    if snr is not None:
        out = out.replace('"snr":7.50', f'"snr":{snr}')
    return out


class TestFailure(RuntimeError):
    pass


class Report:
    def __init__(self) -> None:
        self.passed: list[str] = []

    def ok(self, name: str) -> None:
        self.passed.append(name)
        print(f"PASS {len(self.passed):02d} {name}", flush=True)


def docker(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        ["docker", *args], capture_output=True, text=True, encoding="utf-8"
    )
    if check and proc.returncode != 0:
        raise TestFailure(
            f"docker {' '.join(args)} failed ({proc.returncode})\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc


# --------------------------------------------------------------------------
# Broker fixture
# --------------------------------------------------------------------------

def _self_signed_ca(common_name: str, now: datetime):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=7))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=False, content_commitment=False, key_encipherment=False,
                data_encipherment=False, key_agreement=False, key_cert_sign=True,
                crl_sign=True, encipher_only=False, decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        .sign(key, hashes.SHA256())
    )
    return key, name, cert


def write_certificates(config: Path, work: Path) -> tuple[Path, Path]:
    now = datetime.now(timezone.utc)
    ca_key, ca_name, ca_cert = _self_signed_ca("AGROTECH test CA", now)
    _, _, rogue_cert = _self_signed_ca("Unrelated CA", now)

    server_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    server_cert = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")]))
        .issuer_name(ca_name)
        .public_key(server_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=7))
        .add_extension(
            x509.SubjectAlternativeName(
                [x509.DNSName("localhost"), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
            ),
            critical=False,
        )
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True, content_commitment=False, key_encipherment=True,
                data_encipherment=False, key_agreement=False, key_cert_sign=False,
                crl_sign=False, encipher_only=False, decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
            critical=False,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(server_key.public_key()), critical=False
        )
        .sign(ca_key, hashes.SHA256())
    )

    pem = serialization.Encoding.PEM
    ca_path = config / "ca.crt"
    ca_path.write_bytes(ca_cert.public_bytes(pem))
    (config / "server.crt").write_bytes(server_cert.public_bytes(pem))
    (config / "server.key").write_bytes(
        server_key.private_bytes(
            pem,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    rogue_path = work / "rogue-ca.crt"  # never given to the broker
    rogue_path.write_bytes(rogue_cert.public_bytes(pem))
    return ca_path, rogue_path


def write_broker_config(config: Path) -> None:
    (config / "acl").write_text(
        "\n".join(
            [
                f"user {UID}",
                f"topic write {TOPIC}",
                "",
                f"user {OTHER_UID}",
                f"topic write {OTHER_TOPIC}",
                "",
                f"user {CONSUMER_USER}",
                f"topic read {CONSUMER_FILTER}",
                "",
            ]
        ),
        encoding="ascii",
    )
    # TLS-only: production gateways never get a plaintext listener.
    (config / "mosquitto.conf").write_text(
        "\n".join(
            [
                "allow_anonymous false",
                "password_file /mosquitto/config/passwd",
                "acl_file /mosquitto/config/acl",
                "persistence true",
                "persistence_location /mosquitto/data/",
                "log_dest stdout",
                "listener 8883 0.0.0.0",
                "cafile /mosquitto/config/ca.crt",
                "certfile /mosquitto/config/server.crt",
                "keyfile /mosquitto/config/server.key",
                "tls_version tlsv1.2",
                "",
            ]
        ),
        encoding="ascii",
    )


def create_password_file(config: Path) -> None:
    users = [(UID, TEST_PASSWORD), (OTHER_UID, OTHER_PASSWORD), (CONSUMER_USER, CONSUMER_PASSWORD)]
    for index, (user, password) in enumerate(users):
        args = ["run", "--rm", "-v", f"{config}:/config", IMAGE, "mosquitto_passwd", "-b"]
        if index == 0:
            args.append("-c")
        args.extend(["/config/passwd", user, password])
        docker(*args)
    # mosquitto_passwd runs as root; the broker drops to user "mosquitto".
    docker("run", "--rm", "-v", f"{config}:/config", IMAGE, "sh", "-c",
           "chown mosquitto:mosquitto /config/passwd /config/acl && "
           "chmod 0600 /config/passwd /config/acl")


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_tls_ready(port: int, ca_path: Path, timeout: float = 30.0) -> None:
    """Wait until a real TLS handshake succeeds (port open is not enough)."""
    context = ssl.create_default_context(cafile=str(ca_path))
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as raw:
                with context.wrap_socket(raw, server_hostname="localhost"):
                    return
        except OSError:
            time.sleep(0.3)
    raise TestFailure(f"broker TLS listener on {port} did not become ready")


# --------------------------------------------------------------------------
# Clients
# --------------------------------------------------------------------------

class Client:
    """paho wrapper with synchronous connect/publish helpers."""

    def __init__(self, client_id: str, username: str, password: str, ca: Path,
                 clean_session: bool, keepalive: int = GATEWAY_KEEPALIVE) -> None:
        self.keepalive = keepalive
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
            clean_session=clean_session,
            protocol=mqtt.MQTTv311,
        )
        self.client.username_pw_set(username, password)
        self.client.tls_set(ca_certs=str(ca), tls_version=ssl.PROTOCOL_TLS_CLIENT)
        self.client.tls_insecure_set(False)
        # Fail fast on outages instead of paho's internal auto-retry.
        self.client.reconnect_delay_set(min_delay=60, max_delay=60)
        self.messages: list[mqtt.MQTTMessage] = []
        self.message_event = threading.Event()
        self.connack = threading.Event()
        self.reason = None
        self.disconnected = threading.Event()
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.started = False

    def _on_connect(self, _c, _u, _flags, reason_code, _p):
        self.reason = reason_code
        self.connack.set()

    def _on_message(self, _c, _u, message):
        self.messages.append(message)
        self.message_event.set()

    def _on_disconnect(self, _c, _u, _flags, _reason_code, _p):
        self.disconnected.set()

    def connect(self, port: int, timeout: float = 10.0):
        self.connack.clear()
        self.disconnected.clear()
        self.client.connect("127.0.0.1", port, keepalive=self.keepalive)
        if not self.started:
            self.client.loop_start()
            self.started = True
        if not self.connack.wait(timeout):
            raise TestFailure("CONNACK timeout")
        return self.reason

    def subscribe(self, topic: str) -> None:
        done = threading.Event()
        self.client.on_subscribe = lambda *_a: done.set()
        self.client.subscribe(topic, qos=1)
        if not done.wait(10):
            raise TestFailure("SUBACK timeout")

    def publish_qos1(self, topic: str, payload: str, timeout: float = 10.0) -> None:
        info = self.client.publish(topic, payload, qos=1)
        info.wait_for_publish(timeout)
        if not info.is_published():
            raise TestFailure(f"PUBACK timeout for {topic}")

    def wait_messages(self, count: int, timeout: float = 15.0) -> list[str]:
        deadline = time.time() + timeout
        while len(self.messages) < count and time.time() < deadline:
            self.message_event.wait(0.2)
            self.message_event.clear()
        if len(self.messages) < count:
            raise TestFailure(f"expected {count} messages, got {len(self.messages)}")
        got = [m.payload.decode("utf-8") for m in self.messages[:count]]
        for m in self.messages[:count]:
            if m.qos != 1:
                raise TestFailure(f"message delivered with QoS {m.qos}, expected 1")
            if m.topic != TOPIC:
                raise TestFailure(f"message arrived on unexpected topic {m.topic}")
        del self.messages[:count]
        return got

    def assert_silent(self, seconds: float = 1.5) -> None:
        time.sleep(seconds)
        if self.messages:
            raise TestFailure(f"unexpected message on {self.messages[0].topic}")

    def close(self) -> None:
        try:
            self.client.disconnect()
        except Exception:
            pass
        if self.started:
            self.client.loop_stop()
            self.started = False


def gateway_client(ca: Path, uid: str = UID, password: str = TEST_PASSWORD) -> Client:
    return Client(f"gw-{uid}", uid, password, ca, clean_session=True)


def consumer_client(ca: Path) -> Client:
    return Client(CONSUMER_ID, CONSUMER_USER, CONSUMER_PASSWORD, ca,
                  clean_session=False, keepalive=60)


# --------------------------------------------------------------------------
# Scenario
# --------------------------------------------------------------------------

def run_scenario(container: str, port: int, ca: Path, rogue_ca: Path, report: Report,
                 clients: list[Client]) -> None:
    consumer = consumer_client(ca)
    clients.append(consumer)
    if consumer.connect(port) != 0:
        raise TestFailure(f"consumer CONNECT rejected: {consumer.reason}")
    consumer.subscribe(CONSUMER_FILTER)
    report.ok("consumer (backend profile) TLS connect + QoS1 subscribe")

    # --- TLS / identity -----------------------------------------------------
    context = ssl.create_default_context(cafile=str(rogue_ca))
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
            with context.wrap_socket(raw, server_hostname="localhost"):
                raise TestFailure("TLS handshake succeeded with an unrelated CA")
    except ssl.SSLCertVerificationError:
        pass
    report.ok("server certificate rejected when the client trusts a different CA")

    with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
        # MQTT 3.1.1 CONNECT in plaintext against the TLS listener.
        raw.sendall(bytes.fromhex("101000044d5154540402001e0004746573740000"))
        raw.settimeout(5)
        try:
            data = raw.recv(4)
        except (ConnectionResetError, socket.timeout):
            data = b""
    if data.startswith(b"\x20"):
        raise TestFailure("broker answered a plaintext MQTT CONNECT")
    report.ok("plaintext MQTT is not accepted (TLS-only listener)")

    bad = gateway_client(ca, password="wrong-password")
    clients.append(bad)
    reason = bad.connect(port)
    bad.close()
    if reason == 0:
        raise TestFailure("broker accepted a wrong password")
    report.ok(f"wrong password rejected by CONNACK ({reason})")

    gateway = gateway_client(ca)
    clients.append(gateway)
    if gateway.connect(port) != 0:
        raise TestFailure(f"gateway CONNECT rejected: {gateway.reason}")
    report.ok("gateway TLS connect (3.1.1, clean session, keepalive 30, gw-<UID>)")

    # --- contract -----------------------------------------------------------
    gateway.publish_qos1(TOPIC, BASE_PAYLOAD)
    got = consumer.wait_messages(1)
    if got != [BASE_PAYLOAD]:
        raise TestFailure("consumer received a different payload")
    report.ok("QoS1 publish delivered byte-exact to agrotech/v1/gw/<UID>/up")

    # MQTT 3.1.1 has no negative PUBACK: the broker acks and drops silently.
    gateway.publish_qos1(OTHER_TOPIC, BASE_PAYLOAD)
    consumer.assert_silent()
    report.ok("ACL: gateway cannot publish to another gateway topic (dropped)")

    other = gateway_client(ca, uid=OTHER_UID, password=OTHER_PASSWORD)
    clients.append(other)
    if other.connect(port) != 0:
        raise TestFailure("second gateway CONNECT rejected")
    other.subscribe(CONSUMER_FILTER)  # SUBACK may be granted; reads are filtered
    gateway.publish_qos1(TOPIC, BASE_PAYLOAD)
    consumer.wait_messages(1)
    other.assert_silent()
    other.close()
    report.ok("ACL: a gateway cannot read other gateways' uplinks")

    # --- ambiguous PUBACK: firmware resends the frozen bytes -------------
    gateway.publish_qos1(TOPIC, BASE_PAYLOAD)
    gateway.publish_qos1(TOPIC, BASE_PAYLOAD)
    first, second = consumer.wait_messages(2)
    digest = lambda p: hashlib.sha256((TOPIC + p).encode()).hexdigest()  # noqa: E731
    if digest(first) != digest(second):
        raise TestFailure("retransmission was not byte-identical")
    report.ok("retransmission is byte-identical (same SHA-256 receipt key)")

    # --- consumer offline: broker session holds QoS1 messages --------------
    consumer.close()
    held = buffered(BASE_PAYLOAD, snr="6.25")
    gateway.publish_qos1(TOPIC, held)
    consumer = consumer_client(ca)
    clients.append(consumer)
    if consumer.connect(port) != 0:
        raise TestFailure("consumer reconnect rejected")
    if consumer.wait_messages(1) != [held]:
        raise TestFailure("persistent consumer session lost the message")
    report.ok("backend offline: persistent session delivers on reconnect")

    # --- broker outage: gateway store-and-forward --------------------------
    docker("stop", container)
    if not gateway.disconnected.wait(15):
        raise TestFailure("gateway did not observe the broker outage")
    report.ok("gateway detects broker outage")

    # Frames received over LoRa during the outage stay in the gateway queue,
    # marked buffered=true, oldest first (firmware enqueueFrame/mqttTask).
    outage_queue = [buffered(BASE_PAYLOAD, snr="5.75"), buffered(BASE_PAYLOAD, snr="4.50")]

    docker("start", container)
    wait_tls_ready(port, ca)
    gateway.close()
    consumer.close()

    consumer = consumer_client(ca)
    clients.append(consumer)
    if consumer.connect(port) != 0:
        raise TestFailure("consumer reconnect after broker restart rejected")
    gateway = gateway_client(ca)
    clients.append(gateway)
    if gateway.connect(port) != 0:
        raise TestFailure("gateway reconnect after broker restart rejected")
    report.ok("gateway and consumer reconnect after broker restart")

    for payload in outage_queue:
        gateway.publish_qos1(TOPIC, payload)
    if consumer.wait_messages(len(outage_queue)) != outage_queue:
        raise TestFailure("queued frames were not delivered in order")
    report.ok("queued frames flushed after reconnect, in order, buffered=true")

    gateway.publish_qos1(TOPIC, BASE_PAYLOAD)
    if consumer.wait_messages(1) != [BASE_PAYLOAD]:
        raise TestFailure("live publish after flush failed")
    report.ok("live forwarding resumes with buffered=false")

    # --- BUILD-IOT-06 diagnostic payloads (no radio, unsynced clock) -------
    for payload, sha in DIAG_PAYLOADS:
        gateway.publish_qos1(TOPIC, payload)
        received = consumer.wait_messages(1)[0]
        if received != payload or hashlib.sha256(received.encode()).hexdigest() != sha:
            raise TestFailure("diagnostic payload not delivered byte-exact")
    report.ok("diagnostic payloads delivered byte-exact with expected payload_sha256")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--keep", action="store_true", help="keep the temporary directory")
    args = parser.parse_args()

    if not shutil.which("docker"):
        print("docker is not available", file=sys.stderr)
        return 2

    work = Path(tempfile.mkdtemp(prefix="agrotech-gw-mqtt-"))
    config, data = work / "config", work / "data"
    config.mkdir()
    data.mkdir()
    container = f"agrotech-gw-mqtt-{uuid.uuid4().hex[:8]}"
    port = free_port()
    clients: list[Client] = []
    report = Report()

    try:
        ca, rogue_ca = write_certificates(config, work)
        write_broker_config(config)
        create_password_file(config)
        docker(
            "run", "-d", "--name", container,
            "-p", f"127.0.0.1:{port}:8883",
            "-v", f"{config}:/mosquitto/config",
            "-v", f"{data}:/mosquitto/data",
            IMAGE,
        )
        wait_tls_ready(port, ca)
        run_scenario(container, port, ca, rogue_ca, report, clients)
        print(f"\n{len(report.passed)} checks passed, 0 failed")
        return 0
    except TestFailure as exc:
        print(f"\nFAIL after {len(report.passed)} passed: {exc}", file=sys.stderr)
        logs = docker("logs", container, check=False)
        if logs.stdout or logs.stderr:
            print("broker log (tail):\n" + (logs.stdout + logs.stderr)[-3000:], file=sys.stderr)
        return 1
    finally:
        for client in clients:
            client.close()
        docker("rm", "-f", container, check=False)
        if args.keep:
            print(f"kept temporary directory: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
