"""UDP admin channel client -- the only module in this package that opens
a socket. One AdminClient talks to exactly one device (by IP); see
discovery.py for finding IPs in the first place.

Every write command internally does its own CHALLENGE round trip (TODO
Fase 2.4: every write command needs a fresh, single-use, IP-bound nonce
now, not just REBOOT/FACTORY_RESET/CHANGE_SECRET) -- callers never handle
nonces themselves.
"""
from __future__ import annotations

import hashlib
import secrets as _secrets_mod
import socket
from typing import Callable

from .models import CommandResult, DeviceInfo
from .protocol import (
    DEFAULT_PORT,
    DEFAULT_RAMP_MS,
    DEFAULT_TIMEOUT,
    GCM_TAG_LEN,
    OTA_CHUNK_MAX_DATA,
    SECRET_LEN,
    ZERO_NONCE,
    AdminStatus,
    DmxConfig,
    Packet,
    PacketType,
    ProtocolError,
    pack_dmx_config,
    parse_dmx_config,
    parse_info_payload,
    status_byte,
)
from .secrets import ADMIN_DEFAULT_SECRET, SecretStore

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:  # only needed for change_secret()
    AESGCM = None


class DriverPoEError(Exception):
    """Base class for every error this package raises deliberately (as
    opposed to letting an unrelated exception propagate)."""


class DeviceTimeoutError(DriverPoEError):
    """No response within the configured timeout."""


class AuthError(DriverPoEError):
    """A CHALLENGE (or a response's HMAC) didn't verify -- wrong admin
    secret, or a response we can't trust."""


class DeviceNotFoundError(DriverPoEError):
    """No device answered a discovery/resolve attempt."""


class CommandRefusedError(DriverPoEError):
    """Raised only by CommandResult.raise_if_refused() -- an ERR_* status
    the caller chose to treat as an exception instead of inspecting
    directly."""

    def __init__(self, status: AdminStatus, command: str, serial: str):
        self.status = status
        self.command = command
        self.serial = serial
        super().__init__(f"{serial}: {command} refused ({status.name})")


class MissingDependencyError(DriverPoEError):
    """A required optional dependency (currently just 'cryptography', for
    change_secret()) isn't installed."""


class OtaTransferError(DriverPoEError):
    """An OTA_BEGIN or OTA_CHUNK step failed outright, aborting the
    transfer before it ever reached OTA_END -- e.g. the device already has
    another OTA session in progress (ADMIN_STATUS_ERR_BUSY), or a chunk
    was rejected as out of order. Distinct from a refused OTA_END, which
    comes back as an ordinary CommandResult (result.applied == False)
    instead of raising -- by the time OTA_END runs, the transfer itself
    already succeeded; only the final validation (size/hash/image check)
    could still fail there."""


class AdminClient:
    """Talks to one device at a fixed IP/port. Not thread-safe (one socket,
    used synchronously) -- create one per concurrent operation if needed."""

    def __init__(self, ip: str, port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT):
        self.ip = ip
        self.port = port
        self.addr = (ip, port)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(timeout)

    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> "AdminClient":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # -- transport ------------------------------------------------------
    def _send(self, pkt: Packet, key: bytes | None) -> None:
        self._sock.sendto(pkt.pack(key), self.addr)

    def _recv(self) -> Packet:
        try:
            data, _src = self._sock.recvfrom(4096)
        except OSError as e:
            # socket.timeout (a plain OSError subclass since Python 3.10)
            # covers "nobody answered". ConnectionResetError specifically
            # covers a Windows-only quirk: sending UDP to a port nobody is
            # listening on gets an ICMP port-unreachable back, which
            # Windows (unlike Linux) surfaces as WSAECONNRESET on this
            # same socket's *next* recv call instead of just letting the
            # timeout fire -- observed directly via this package's own
            # test suite (tests/test_client.py). Both cases mean the same
            # thing to a caller: this device isn't there to answer.
            raise DeviceTimeoutError(f"No response from {self.ip}:{self.port}") from e
        return Packet.unpack(data)  # raises ProtocolError/ProtocolVersionMismatchError

    # -- INFO (unauthenticated) -----------------------------------------
    def info(self) -> DeviceInfo:
        pkt = Packet(type=PacketType.INFO, serial="", nonce=b"", payload=b"")
        self._send(pkt, None)
        resp = self._recv()
        if resp.type != PacketType.INFO_RESP:
            raise ProtocolError(f"unexpected response type {resp.type!r} to INFO")
        fields = parse_info_payload(resp.payload)
        return DeviceInfo(serial=resp.serial, source_ip=self.ip, **fields)

    # -- CHALLENGE --------------------------------------------------------
    def challenge(self, key: bytes, serial: str) -> bytes:
        """Returns the freshly issued nonce. Every write method below
        calls this itself -- callers normally never need it directly
        (exposed mainly for tests and for change_secret()'s AES-GCM IV
        reuse)."""
        pkt = Packet(type=PacketType.CHALLENGE, serial=serial, nonce=b"", payload=b"")
        self._send(pkt, key)
        resp = self._recv()
        if resp.type != PacketType.CHALLENGE_RESP or not resp.verify_hmac(key):
            raise AuthError(f"CHALLENGE rejected for {serial} (wrong admin secret?)")
        return resp.nonce

    # -- generic write-command helper -------------------------------------
    def _write_command(self, command: str, ptype: PacketType, resp_type: PacketType,
                        serial: str, key: bytes, nonce: bytes, payload: bytes = b"") -> CommandResult:
        pkt = Packet(type=ptype, serial=serial, nonce=nonce, payload=payload)
        self._send(pkt, key)
        resp = self._recv()
        if resp.type != resp_type or not resp.verify_hmac(key):
            raise ProtocolError(f"invalid or unauthenticated response to {command}")
        return CommandResult(status=AdminStatus(status_byte(resp.payload)), serial=serial, command=command)

    # -- write commands ---------------------------------------------------
    def on(self, key: bytes, serial: str, ramp_ms: int = DEFAULT_RAMP_MS) -> CommandResult:
        nonce = self.challenge(key, serial)
        payload = ramp_ms.to_bytes(4, "big")
        return self._write_command("ON", PacketType.ON, PacketType.ON_RESP, serial, key, nonce, payload)

    def off(self, key: bytes, serial: str, ramp_ms: int = DEFAULT_RAMP_MS) -> CommandResult:
        nonce = self.challenge(key, serial)
        payload = ramp_ms.to_bytes(4, "big")
        return self._write_command("OFF", PacketType.OFF, PacketType.OFF_RESP, serial, key, nonce, payload)

    def dim(self, key: bytes, serial: str, percent: int, ramp_ms: int = DEFAULT_RAMP_MS) -> CommandResult:
        """Sets the brightness (percent 0-100, ramp_ms). High-rate
        brightness control belongs on the DMX layer, not a stream of DIM
        commands (this one still costs a CHALLENGE round trip each)."""
        if not 0 <= percent <= 100:
            raise ValueError(f"percent must be 0-100, got {percent}")
        nonce = self.challenge(key, serial)
        payload = bytes([percent]) + ramp_ms.to_bytes(4, "big")
        return self._write_command("DIM", PacketType.DIM, PacketType.DIM_RESP, serial, key, nonce, payload)

    def identify(self, key: bytes, serial: str) -> CommandResult:
        nonce = self.challenge(key, serial)
        return self._write_command("IDENTIFY", PacketType.IDENTIFY, PacketType.IDENTIFY_RESP, serial, key, nonce)

    def reboot(self, key: bytes, serial: str) -> CommandResult:
        nonce = self.challenge(key, serial)
        return self._write_command("REBOOT", PacketType.REBOOT, PacketType.REBOOT_RESP, serial, key, nonce)

    def factory_reset(self, key: bytes, serial: str) -> CommandResult:
        nonce = self.challenge(key, serial)
        return self._write_command("FACTORY_RESET", PacketType.FACTORY_RESET, PacketType.FACTORY_RESET_RESP,
                                    serial, key, nonce)

    def change_secret(self, key: bytes, serial: str, new_secret: bytes | None = None) -> tuple[CommandResult, bytes]:
        """Single round trip, no staging: payload = AES-256-GCM(new_secret)
        under the OLD secret, GCM nonce = the CHALLENGE nonce, AAD = the
        packet header -- must match handle_change_secret() in
        components/admin_channel/admin_channel.c exactly. Returns
        (result, new_secret) -- the caller (client.py's callers, e.g.
        cli.py) is responsible for persisting new_secret to a SecretStore
        on success; this method has no storage side effects of its own."""
        if AESGCM is None:
            raise MissingDependencyError(
                "The 'cryptography' package isn't installed (pip install cryptography) -- required for change_secret()."
            )
        if new_secret is None:
            new_secret = _secrets_mod.token_bytes(SECRET_LEN)
        elif len(new_secret) != SECRET_LEN:
            raise ValueError(f"new secret must be {SECRET_LEN} bytes; got {len(new_secret)}")

        nonce = self.challenge(key, serial)
        header_stub = Packet(type=PacketType.CHANGE_SECRET, serial=serial, nonce=nonce, payload=b"")
        aad = header_stub.header_bytes(payload_len=SECRET_LEN + GCM_TAG_LEN)
        ciphertext = AESGCM(key).encrypt(nonce, new_secret, aad)

        pkt = Packet(type=PacketType.CHANGE_SECRET, serial=serial, nonce=nonce, payload=ciphertext)
        self._send(pkt, key)
        resp = self._recv()
        # The device authenticates its response with the NEW secret (it's
        # already active by the time the response goes out) -- proves the
        # device really adopted the value this call just sent, not just
        # that it accepted the request.
        if resp.type != PacketType.CHANGE_SECRET_RESP or not resp.verify_hmac(new_secret):
            raise ProtocolError("CHANGE_SECRET response invalid, or the device didn't confirm with the new secret")
        result = CommandResult(status=AdminStatus(status_byte(resp.payload)), serial=serial, command="CHANGE_SECRET")
        return result, new_secret

    def ota_update(self, key: bytes, serial: str, image: bytes,
                    progress_callback: Callable[[int, int], None] | None = None,
                    max_retries_per_chunk: int = 5) -> CommandResult:
        """Pushes `image` (raw firmware .bin bytes) to the device over
        OTA_BEGIN/OTA_CHUNK*/OTA_END -- see components/admin_channel/
        admin_protocol.h's ADMIN_TYPE_OTA_* comments for the wire contract.
        Does NOT reboot the device -- the new image only ever runs after a
        separate reboot() call, and only once this returns a CommandResult
        with .applied True (OTA_END staged it as the next boot partition).

        progress_callback, if given, is called as
        progress_callback(bytes_sent, total_bytes) after every chunk the
        device confirms -- the only I/O this method itself ever does is on
        the network; no print()/logging here, same as the rest of this
        module (see cli.py/webui/app.py for where that actually happens).

        Raises OtaTransferError if OTA_BEGIN or a chunk is refused outright
        (e.g. ADMIN_STATUS_ERR_BUSY -- another OTA already in progress);
        DeviceTimeoutError if a chunk still doesn't get a response after
        max_retries_per_chunk attempts. A refusal at the OTA_END step
        (size/hash/image validation) is NOT raised -- by then the transfer
        itself already succeeded, so that comes back as an ordinary
        CommandResult with .applied False instead, same as any other
        write command's refusal."""
        total_size = len(image)
        if total_size == 0:
            raise ValueError("image must not be empty")

        image_hash = hashlib.sha256(image).digest()
        nonce = self.challenge(key, serial)
        begin_payload = total_size.to_bytes(4, "big") + image_hash
        begin_result = self._write_command("OTA_BEGIN", PacketType.OTA_BEGIN, PacketType.OTA_BEGIN_RESP,
                                            serial, key, nonce, begin_payload)
        if not begin_result.applied:
            raise OtaTransferError(f"OTA_BEGIN refused for {serial} ({begin_result.status.name})")

        bytes_sent = 0
        while bytes_sent < total_size:
            chunk = image[bytes_sent:bytes_sent + OTA_CHUNK_MAX_DATA]
            # ZERO_NONCE, not a CHALLENGE nonce -- OTA_CHUNK is deliberately
            # HMAC-only (see admin_channel.c's needs_pool_nonce comment),
            # the device never checks this field for this packet type.
            pkt = Packet(type=PacketType.OTA_CHUNK, serial=serial, nonce=ZERO_NONCE,
                         payload=bytes_sent.to_bytes(4, "big") + chunk)

            timeout_error: DeviceTimeoutError | None = None
            for _attempt in range(max_retries_per_chunk):
                self._send(pkt, key)
                try:
                    resp = self._recv()
                except DeviceTimeoutError as e:
                    timeout_error = e
                    continue
                if resp.type != PacketType.OTA_CHUNK_RESP or not resp.verify_hmac(key):
                    raise ProtocolError(f"invalid or unauthenticated response to OTA_CHUNK at offset {bytes_sent}")
                if len(resp.payload) != 5:
                    raise ProtocolError(f"unexpected OTA_CHUNK_RESP payload size: {len(resp.payload)}")
                status = AdminStatus(resp.payload[0])
                device_bytes_written = int.from_bytes(resp.payload[1:5], "big")
                if status != AdminStatus.OK:
                    raise OtaTransferError(
                        f"OTA_CHUNK at offset {bytes_sent} refused for {serial} ({status.name})")
                bytes_sent = device_bytes_written
                timeout_error = None
                break
            if timeout_error is not None:
                raise DeviceTimeoutError(
                    f"OTA_CHUNK at offset {bytes_sent} for {serial} timed out after "
                    f"{max_retries_per_chunk} attempts") from timeout_error

            if progress_callback is not None:
                progress_callback(bytes_sent, total_size)

        nonce = self.challenge(key, serial)
        return self._write_command("OTA_END", PacketType.OTA_END, PacketType.OTA_END_RESP, serial, key, nonce)

    def ota_abort(self, key: bytes, serial: str) -> CommandResult:
        """Cancels an in-progress OTA session on the device, if any
        (idempotent -- also succeeds if there wasn't one)."""
        nonce = self.challenge(key, serial)
        return self._write_command("OTA_ABORT", PacketType.OTA_ABORT, PacketType.OTA_ABORT_RESP, serial, key, nonce)

    # -- DMX layer config ----------------------------------------------------
    def get_dmx_config(self, key: bytes, serial: str) -> DmxConfig:
        """Reads the Art-Net/sACN DMX layer configuration (authenticated)."""
        nonce = self.challenge(key, serial)
        pkt = Packet(type=PacketType.DMX_GET_CONFIG, serial=serial, nonce=nonce, payload=b"")
        self._send(pkt, key)
        resp = self._recv()
        if resp.type != PacketType.DMX_GET_CONFIG_RESP or not resp.verify_hmac(key):
            raise ProtocolError("invalid or unauthenticated response to DMX_GET_CONFIG")
        return parse_dmx_config(resp.payload)

    def set_dmx_config(self, key: bytes, serial: str, cfg: DmxConfig) -> CommandResult:
        """Writes (and persists) the DMX layer configuration. The device
        clamps out-of-range fields; a malformed payload comes back
        ERR_BAD_ARG."""
        nonce = self.challenge(key, serial)
        return self._write_command("DMX_SET_CONFIG", PacketType.DMX_SET_CONFIG,
                                    PacketType.DMX_SET_CONFIG_RESP, serial, key, nonce,
                                    pack_dmx_config(cfg))


def find_working_secret(client: AdminClient, serial: str, store: SecretStore,
                         manual_secret_provider=None) -> tuple[bytes, bytes]:
    """Returns (secret, nonce). Tries, in order: a secret saved in `store`
    for this serial, the documented factory default, then -- only if
    `manual_secret_provider` is given -- calls it (no args, returns
    SECRET_LEN bytes) to get one from the caller and validates it. This
    function itself never does I/O beyond the network (no input()/print())
    -- prompting the operator, if wanted, is entirely the caller's
    responsibility via manual_secret_provider (see cli.py for a getpass-
    based one)."""
    candidates = []
    stored = store.get(serial)
    if stored is not None:
        candidates.append(stored)
    if ADMIN_DEFAULT_SECRET not in candidates:
        candidates.append(ADMIN_DEFAULT_SECRET)

    last_error: Exception | None = None
    for secret in candidates:
        try:
            nonce = client.challenge(secret, serial)
            return secret, nonce
        except (AuthError, DeviceTimeoutError) as e:
            # A wrong secret and an unreachable device look IDENTICAL on
            # the wire here, by design: the firmware silently drops any
            # packet whose HMAC doesn't verify (see
            # components/admin_channel/admin_channel.c's file-top comment,
            # "anti-oracle") instead of sending back an authenticated
            # rejection -- so a wrong CHALLENGE never gets a CHALLENGE_RESP
            # at all, it just times out, indistinguishable here from the
            # device being offline. Treating DeviceTimeoutError the same
            # as AuthError while trying candidates is what makes falling
            # back from a stale stored secret to the factory default (or
            # to a manually typed one) actually work -- without this, one
            # wrong candidate would abort the whole lookup instead of
            # trying the next one.
            last_error = e
            continue

    if manual_secret_provider is None:
        raise AuthError(f"No working admin secret found for {serial} (or the device is unreachable)") from last_error
    secret = manual_secret_provider()
    if len(secret) != SECRET_LEN:
        raise ValueError(f"secret must be {SECRET_LEN} bytes; got {len(secret)}")
    try:
        nonce = client.challenge(secret, serial)
    except DeviceTimeoutError as e:
        # Same silent-drop ambiguity as the candidates loop above, but for
        # the manually-supplied secret specifically -- by this point two
        # earlier CHALLENGE attempts (stored + factory default) already
        # got SOME kind of response from this device (an AuthError from
        # the very first call would have required at least one to be
        # reachable... though both could also have silently timed out),
        # so a manually typed secret timing out too is most usefully
        # reported to the caller as "wrong secret", not "device
        # unreachable" -- re-raised as AuthError so every caller of this
        # function only ever needs to catch one exception type for "the
        # secret lookup failed", regardless of which step it failed at.
        raise AuthError(f"Manually provided secret was rejected for {serial} (or the device is unreachable)") from e
    return secret, nonce


def connect(ip: str, store: SecretStore, manual_secret_provider=None,
            port: int = DEFAULT_PORT, timeout: float = DEFAULT_TIMEOUT) -> tuple[AdminClient, DeviceInfo, bytes]:
    """Common preamble for an authenticated session against one device:
    INFO (to learn the serial) + find_working_secret(). Returns
    (client, info, secret) -- the caller keeps the client open for
    subsequent commands (or uses it as a context manager)."""
    client = AdminClient(ip, port, timeout)
    info = client.info()
    secret, _nonce = find_working_secret(client, info.serial, store, manual_secret_provider)
    return client, info, secret
