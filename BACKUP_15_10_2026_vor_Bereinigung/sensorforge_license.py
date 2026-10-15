#!/usr/bin/env python3
"""Generate board-bound SensorForge activation codes.

SF2 is the production-oriented format. It carries an issuer key ID so firmware
can enforce key roles (for example, a web-server key may issue EVALUATION only,
while FULL/SERVICE require a separate offline authority key).

Keep private keys outside the firmware/source repository. For commercial use,
FULL/SERVICE private keys should remain offline or inside a non-exportable
HSM/KMS. The web application should not have direct access to them.
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

MAGIC_SF1 = b"SFL1"
MAGIC_SF2 = b"SFL2"
FORMAT_VERSION_SF1 = 1
FORMAT_VERSION_SF2 = 2
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


def load_p256_private_key(path: pathlib.Path) -> ec.EllipticCurvePrivateKey:
    private_key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(private_key, ec.EllipticCurvePrivateKey):
        raise SystemExit("private key is not an EC key")
    if private_key.curve.name not in {"secp256r1", "prime256v1"}:
        raise SystemExit("private key must use P-256 / secp256r1")
    return private_key


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a SensorForge board activation code")
    parser.add_argument("hardware_id", help="SensorForge hardware ID shown by the device")
    parser.add_argument("--private-key", required=True, type=pathlib.Path)
    parser.add_argument("--edition", choices=sorted(EDITION), default="full")
    parser.add_argument(
        "--key-id",
        type=int,
        help="SF2 issuer key ID (1..255); must match license_keyring.h",
    )
    parser.add_argument(
        "--legacy-sf1",
        action="store_true",
        help="generate legacy SF1 instead of SF2 (development/migration only)",
    )
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

    if args.legacy_sf1:
        if args.key_id is not None:
            parser.error("--key-id must not be used with --legacy-sf1")
        magic = MAGIC_SF1
        format_version = FORMAT_VERSION_SF1
        issuer_key_id = 0
        code_prefix = "SF1"
    else:
        if args.key_id is None or not 1 <= args.key_id <= 255:
            parser.error("SF2 requires --key-id in range 1..255")
        magic = MAGIC_SF2
        format_version = FORMAT_VERSION_SF2
        issuer_key_id = args.key_id
        code_prefix = "SF2"

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
        magic,
        format_version,
        PRODUCT_SENSORFORGE,
        EDITION[args.edition],
        issuer_key_id,
        features,
        issued_days,
        expires_days,
        hardware_id,
        license_uuid.bytes,
    )
    assert len(payload) == 52

    private_key = load_p256_private_key(args.private_key)
    signature = private_key.sign(payload, ec.ECDSA(hashes.SHA256()))

    payload_code = base64.urlsafe_b64encode(payload).rstrip(b"=").decode("ascii")
    signature_code = base64.urlsafe_b64encode(signature).rstrip(b"=").decode("ascii")
    activation_code = f"{code_prefix}:{payload_code}.{signature_code}"

    print("SensorForge Activation Code")
    print()
    print(f"format={code_prefix}")
    print(f"issuer_key_id={issuer_key_id if issuer_key_id else 'legacy'}")
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
