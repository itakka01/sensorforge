#!/usr/bin/env python3
"""Generate board-bound SensorForge SF1 activation codes.

Keep the private key offline. This tool is intentionally separate from the
firmware project and never needs to be deployed to a SensorForge device.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import pathlib
import struct
import uuid

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

MAGIC = b"SFL1"
FORMAT_VERSION = 1
PRODUCT_SENSORFORGE = 1
PAYLOAD_STRUCT = struct.Struct("<4sBBBBIII16s16s")

EDITION = {
    "full": 1,
    "evaluation": 2,
    "service": 3,
}

FEATURE = {
    "recording": 1 << 0,
    "radar": 1 << 1,
    "sync_api": 1 << 2,
    "advanced_analytics": 1 << 3,
}


def decode_hardware_id(value: str) -> bytes:
    normalized = value.strip().upper()
    if not normalized.startswith("SF-"):
        raise ValueError("hardware ID must start with SF-")

    encoded = normalized[3:].replace("-", "")
    if len(encoded) != 26:
        raise ValueError("hardware ID must contain 26 base32 characters")

    padding = "=" * ((8 - len(encoded) % 8) % 8)
    try:
        raw = base64.b32decode(encoded + padding, casefold=False)
    except Exception as exc:
        raise ValueError("hardware ID is not valid base32") from exc

    if len(raw) != 16:
        raise ValueError("hardware ID does not decode to 16 bytes")
    return raw


def epoch_day(value: dt.date) -> int:
    return (value - dt.date(1970, 1, 1)).days


def parse_date(value: str) -> dt.date:
    try:
        return dt.date.fromisoformat(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("date must be YYYY-MM-DD") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a SensorForge board activation code")
    parser.add_argument("hardware_id", help="SensorForge hardware ID shown by the device")
    parser.add_argument("--private-key", required=True, type=pathlib.Path)
    parser.add_argument("--edition", choices=sorted(EDITION), default="full")
    parser.add_argument(
        "--features",
        nargs="+",
        choices=sorted(FEATURE),
        default=list(FEATURE),
        help="licensed feature flags",
    )
    parser.add_argument("--issued", type=parse_date, default=dt.datetime.now(dt.timezone.utc).date())
    parser.add_argument(
        "--expires",
        type=parse_date,
        help="optional last valid UTC calendar day; omit for perpetual",
    )
    parser.add_argument(
        "--license-id",
        type=uuid.UUID,
        help="optional fixed UUID; generated automatically when omitted",
    )
    args = parser.parse_args()

    hardware_id = decode_hardware_id(args.hardware_id)
    license_uuid = args.license_id or uuid.uuid4()

    features = 0
    for name in args.features:
        features |= FEATURE[name]

    issued_days = epoch_day(args.issued)
    expires_days = epoch_day(args.expires) if args.expires else 0
    if expires_days and expires_days < issued_days:
        parser.error("--expires must not be earlier than --issued")

    payload = PAYLOAD_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        PRODUCT_SENSORFORGE,
        EDITION[args.edition],
        0,
        features,
        issued_days,
        expires_days,
        hardware_id,
        license_uuid.bytes,
    )
    assert len(payload) == 52

    private_key = serialization.load_pem_private_key(
        args.private_key.read_bytes(),
        password=None,
    )
    if not isinstance(private_key, ec.EllipticCurvePrivateKey):
        raise SystemExit("private key is not an EC key")
    if private_key.curve.name not in {"secp256r1", "prime256v1"}:
        raise SystemExit("private key must use P-256 / secp256r1")

    signature = private_key.sign(payload, ec.ECDSA(hashes.SHA256()))

    payload_code = base64.urlsafe_b64encode(payload).rstrip(b"=").decode("ascii")
    signature_code = base64.urlsafe_b64encode(signature).rstrip(b"=").decode("ascii")
    activation_code = f"SF1:{payload_code}.{signature_code}"

    print("SensorForge Activation Code")
    print()
    print(f"hardware_id={args.hardware_id.upper()}")
    print(f"license_id={license_uuid}")
    print(f"edition={args.edition.upper()}")
    print(f"features=0x{features:08X}")
    print(f"issued={args.issued.isoformat()}")
    print(f"expires={args.expires.isoformat() if args.expires else 'never'}")
    print()
    print(activation_code)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

