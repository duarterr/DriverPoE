"""device_api -- Python API for the DriverPoE admin UDP channel.

Pure library, no interactive I/O: tools/lumtool.py (CLI) and
tools/webui/ (FastAPI) are both thin consumers of this package, not the
other way around. See README.md "Canal de administração" for the wire
protocol this implements, and TODO for the Fase 3 requirements this
package was written to satisfy.

Typical use::

    from device_api import AdminClient, MemorySecretStore, connect, discovery

    store = MemorySecretStore()  # or KeyfileSecretStore.load_text(open("keys.txt").read())
    devices = discovery.broadcast_info(discovery.guess_broadcast_address())
    client, info, secret = connect(devices[0].source_ip, store)
    client.on(secret, info.serial)
"""
from .client import (
    AdminClient,
    AuthError,
    CommandRefusedError,
    DeviceNotFoundError,
    DeviceTimeoutError,
    DriverPoEError,
    MissingDependencyError,
    OtaTransferError,
    connect,
    find_working_secret,
)
from .models import CommandResult, DeviceInfo
from .protocol import (
    AdminStatus,
    DmxConfig,
    PacketType,
    ProtocolError,
    ProtocolVersionMismatchError,
    mac_from_serial,
    mac_from_str,
    mac_to_str,
    pack_dmx_config,
    parse_dmx_config,
    serial_from_mac,
)
from .secrets import (
    ADMIN_DEFAULT_SECRET,
    KeyfileSecretStore,
    KeysFileError,
    MemorySecretStore,
    SecretStore,
    parse_keys_file,
)

__all__ = [
    "AdminClient",
    "AuthError",
    "CommandRefusedError",
    "CommandResult",
    "DeviceInfo",
    "DeviceNotFoundError",
    "DeviceTimeoutError",
    "DriverPoEError",
    "MissingDependencyError",
    "OtaTransferError",
    "AdminStatus",
    "DmxConfig",
    "PacketType",
    "ProtocolError",
    "ProtocolVersionMismatchError",
    "pack_dmx_config",
    "parse_dmx_config",
    "ADMIN_DEFAULT_SECRET",
    "KeyfileSecretStore",
    "KeysFileError",
    "MemorySecretStore",
    "SecretStore",
    "parse_keys_file",
    "connect",
    "find_working_secret",
    "mac_from_serial",
    "mac_from_str",
    "mac_to_str",
    "serial_from_mac",
]
