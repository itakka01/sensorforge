#!/usr/bin/env python3
"""Generate the SensorForge production license key hierarchy locally.

Run this ONLY on a trusted/offline computer. The script creates three P-256
keypairs and a public-only license_keyring.h:

  TRIAL    key: EVALUATION only; this is the only private key intended for the
               online license service (prefer a non-exportable HSM/KMS later).
  FULL     key: FULL + SERVICE; keep the private key offline.
  RECOVERY key: all editions; keep the private key offline and separately
                backed up for emergency rotation/recovery.

Private PEM files are never embedded in the generated header.
"""

from __future__ import annotations

import argparse
import os
import pathlib

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

DEFAULT_IDS = {
    "trial": 10,
    "full": 20,
    "recovery": 30,
}

CAPS = {
    "trial": "SENSORFORGE_LICENSE_KEY_CAP_EVALUATION",
    "full": "SENSORFORGE_LICENSE_KEY_CAP_FULL | SENSORFORGE_LICENSE_KEY_CAP_SERVICE",
    "recovery": "SENSORFORGE_LICENSE_KEY_CAP_ALL",
}

LABELS = {
    "trial": "ONLINE-TRIAL",
    "full": "OFFLINE-FULL-AUTHORITY",
    "recovery": "OFFLINE-RECOVERY",
}


def write_private(path: pathlib.Path, key: ec.EllipticCurvePrivateKey) -> None:
    data = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    path.write_bytes(data)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def write_public(path: pathlib.Path, key: ec.EllipticCurvePrivateKey) -> bytes:
    data = key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    path.write_bytes(data)
    return data


def c_string(name: str, pem: bytes) -> str:
    lines = pem.decode("ascii").splitlines()
    body = [f"static const char {name}[] ="]
    for line in lines:
        body.append(f'    "{line}\\n"')
    body[-1] += ";"
    return "\n".join(body)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate SensorForge license production keys")
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=pathlib.Path.home() / ".sensorforge-license-keys",
        help="private/public PEM destination; defaults outside the source tree",
    )
    parser.add_argument(
        "--header",
        type=pathlib.Path,
        default=pathlib.Path("license_keyring.production.h"),
        help="public-only generated firmware header; copy to license_keyring.h after review",
    )
    parser.add_argument("--trial-key-id", type=int, default=DEFAULT_IDS["trial"])
    parser.add_argument("--full-key-id", type=int, default=DEFAULT_IDS["full"])
    parser.add_argument("--recovery-key-id", type=int, default=DEFAULT_IDS["recovery"])
    args = parser.parse_args()

    ids = {
        "trial": args.trial_key_id,
        "full": args.full_key_id,
        "recovery": args.recovery_key_id,
    }

    if any(not 1 <= value <= 255 for value in ids.values()):
        parser.error("all key IDs must be in range 1..255")
    if len(set(ids.values())) != 3:
        parser.error("trial/full/recovery key IDs must be different")

    out = args.output_dir
    header = args.header

    if out.exists() and any(out.iterdir()):
        parser.error(f"output directory is not empty: {out}")
    if header.exists():
        parser.error(f"header already exists: {header}")

    out.mkdir(parents=True, exist_ok=True)

    public_pems: dict[str, bytes] = {}
    for role in ("trial", "full", "recovery"):
        key = ec.generate_private_key(ec.SECP256R1())
        private_path = out / f"sensorforge_license_{role}_private.pem"
        public_path = out / f"sensorforge_license_{role}_public.pem"
        write_private(private_path, key)
        public_pems[role] = write_public(public_path, key)

    parts = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "// AUTO-GENERATED PUBLIC-KEY KEYRING. PRIVATE KEYS MUST NEVER BE COMMITTED.",
        "static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_EVALUATION = 1U << 0;",
        "static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_FULL       = 1U << 1;",
        "static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_SERVICE    = 1U << 2;",
        "static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_ALL =",
        "    SENSORFORGE_LICENSE_KEY_CAP_EVALUATION |",
        "    SENSORFORGE_LICENSE_KEY_CAP_FULL |",
        "    SENSORFORGE_LICENSE_KEY_CAP_SERVICE;",
        "",
        "struct SensorForgeLicenseKeyEntry {",
        "    uint8_t keyId;",
        "    uint8_t capabilities;",
        "    bool enabled;",
        "    const char *label;",
        "    const char *publicKeyPem;",
        "};",
        "",
        "#define SENSORFORGE_LICENSE_ACCEPT_LEGACY_SF1 0",
        "#define SENSORFORGE_LICENSE_KEYRING_PRODUCTION_READY 1",
        "",
    ]

    symbol = {}
    for role in ("trial", "full", "recovery"):
        sym = f"SENSORFORGE_LICENSE_{role.upper()}_PUBLIC_KEY_PEM"
        symbol[role] = sym
        parts.append(c_string(sym, public_pems[role]))
        parts.append("")

    parts += [
        "static const SensorForgeLicenseKeyEntry SENSORFORGE_LICENSE_KEYRING[] = {",
    ]
    for role in ("trial", "full", "recovery"):
        parts += [
            "    {",
            f"        {ids[role]}U,",
            f"        {CAPS[role]},",
            "        true,",
            f'        "{LABELS[role]}",',
            f"        {symbol[role]}",
            "    },",
        ]
    parts += [
        "};",
        "",
        "static constexpr size_t SENSORFORGE_LICENSE_KEYRING_COUNT =",
        "    sizeof(SENSORFORGE_LICENSE_KEYRING) /",
        "    sizeof(SENSORFORGE_LICENSE_KEYRING[0]);",
        "",
    ]

    header.write_text("\n".join(parts), encoding="utf-8")

    print("SensorForge license key hierarchy generated.")
    print()
    print(f"Public-only firmware header: {header}")
    print(f"Private/public key directory: {out}")
    print()
    print(f"TRIAL key id={ids['trial']}  -> online service, EVALUATION only")
    print(f"FULL key id={ids['full']}   -> KEEP OFFLINE, FULL/SERVICE")
    print(f"RECOVERY key id={ids['recovery']} -> KEEP OFFLINE, emergency only")
    print()
    print("Do NOT commit, upload, email, or place private PEM files on devices.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
