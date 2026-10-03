#!/usr/bin/env python3
"""SensorForge ONVIF regression smoke test.

Tests the stable, read-only SensorForge ONVIF integration without third-party
Python packages: WS-Discovery, Device/Media/Imaging SOAP, snapshot retrieval,
and an RTSP DESCRIBE against the URI returned by ONVIF.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import re
import socket
import sys
from datetime import datetime, timezone
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError
import uuid

SOAP_ENV = "http://www.w3.org/2003/05/soap-envelope"
DEVICE_NS = "http://www.onvif.org/ver10/device/wsdl"
MEDIA_NS = "http://www.onvif.org/ver10/media/wsdl"
IMAGING_NS = "http://www.onvif.org/ver20/imaging/wsdl"
WSSE_NS = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
WSU_NS = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
PASSWORD_DIGEST = WSSE_NS + "#PasswordDigest"
BASE64_BINARY = WSSE_NS + "#Base64Binary"
DISCOVERY_GROUP = ("239.255.255.250", 3702)


class TestFailure(RuntimeError):
    pass


def wsse_header(username: str, password: str) -> str:
    nonce = os.urandom(20)
    created = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    digest = hashlib.sha1(nonce + created.encode() + password.encode()).digest()
    return (
        f'<s:Header><wsse:Security s:mustUnderstand="1" xmlns:wsse="{WSSE_NS}" xmlns:wsu="{WSU_NS}">'
        '<wsse:UsernameToken>'
        f'<wsse:Username>{xml_escape(username)}</wsse:Username>'
        f'<wsse:Password Type="{PASSWORD_DIGEST}">{base64.b64encode(digest).decode()}</wsse:Password>'
        f'<wsse:Nonce EncodingType="{BASE64_BINARY}">{base64.b64encode(nonce).decode()}</wsse:Nonce>'
        f'<wsu:Created>{created}</wsu:Created>'
        '</wsse:UsernameToken></wsse:Security></s:Header>'
    )


def xml_escape(value: str) -> str:
    return (value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                 .replace('"', "&quot;").replace("'", "&apos;"))


def soap(operation_xml: str, username: str, password: str, auth: bool = True) -> bytes:
    header = wsse_header(username, password) if auth and username else "<s:Header/>"
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        f'<s:Envelope xmlns:s="{SOAP_ENV}" xmlns:tds="{DEVICE_NS}" '
        f'xmlns:trt="{MEDIA_NS}" xmlns:timg="{IMAGING_NS}" '
        'xmlns:tt="http://www.onvif.org/ver10/schema">'
        f'{header}<s:Body>{operation_xml}</s:Body></s:Envelope>'
    ).encode()


def post_soap(url: str, operation_xml: str, username: str, password: str, *, auth: bool = True, timeout: float = 5.0) -> str:
    data = soap(operation_xml, username, password, auth=auth)
    req = Request(url, data=data, method="POST", headers={
        "Content-Type": "application/soap+xml; charset=utf-8",
        "User-Agent": "SensorForge-ONVIF-Test/1.0",
        "Connection": "close",
    })
    try:
        with urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            if resp.status != 200:
                raise TestFailure(f"HTTP {resp.status} from {url}")
            return body
    except HTTPError as exc:
        text = exc.read().decode("utf-8", errors="replace")
        raise TestFailure(f"HTTP {exc.code} from {url}: {text[:300]}") from exc
    except URLError as exc:
        raise TestFailure(f"SOAP connection failed for {url}: {exc}") from exc


def local_name_contains(xml: str, name: str) -> bool:
    return re.search(rf"<(?:[A-Za-z_][\w.-]*:)?{re.escape(name)}(?:\s|>)", xml) is not None


def text_of(xml: str, local_name: str) -> str:
    m = re.search(
        rf"<(?:[A-Za-z_][\w.-]*:)?{re.escape(local_name)}(?:\s[^>]*)?>(.*?)</(?:[A-Za-z_][\w.-]*:)?{re.escape(local_name)}>",
        xml,
        re.S,
    )
    if not m:
        return ""
    return re.sub(r"<[^>]+>", "", m.group(1)).strip()


def attribute_token(xml: str, local_name: str) -> str:
    m = re.search(rf"<(?:[A-Za-z_][\w.-]*:)?{re.escape(local_name)}\b[^>]*\btoken=\"([^\"]+)\"", xml)
    return m.group(1) if m else ""


def discover(timeout: float = 3.0) -> tuple[str, str]:
    msg_id = f"urn:uuid:{uuid.uuid4()}"
    probe = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" '
        'xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
        'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" '
        'xmlns:tds="http://www.onvif.org/ver10/device/wsdl">'
        f'<e:Header><w:MessageID>{msg_id}</w:MessageID>'
        '<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>'
        '<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>'
        '<e:Body><d:Probe><d:Types>tds:Device</d:Types></d:Probe></e:Body></e:Envelope>'
    ).encode()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.settimeout(timeout)
        sock.sendto(probe, DISCOVERY_GROUP)
        while True:
            payload, addr = sock.recvfrom(8192)
            xml = payload.decode("utf-8", errors="replace")
            if "ProbeMatch" not in xml:
                continue
            xaddr = text_of(xml, "XAddrs")
            if xaddr:
                return addr[0], xaddr.split()[0]
    except socket.timeout as exc:
        raise TestFailure("WS-Discovery: no ProbeMatch received") from exc
    finally:
        sock.close()


def snapshot_test(url: str, username: str, password: str, timeout: float) -> None:
    token = base64.b64encode(f"{username}:{password}".encode()).decode()
    req = Request(url, headers={"Authorization": f"Basic {token}", "Connection": "close"})
    try:
        with urlopen(req, timeout=timeout) as resp:
            head = resp.read(2)
            if resp.status != 200 or head != b"\xff\xd8":
                raise TestFailure(f"Snapshot is not a JPEG (HTTP {resp.status}, first bytes={head!r})")
    except (HTTPError, URLError) as exc:
        raise TestFailure(f"Snapshot failed: {exc}") from exc


def rtsp_describe(url: str, username: str, password: str, timeout: float) -> None:
    parsed = urlsplit(url)
    host = parsed.hostname or ""
    port = parsed.port or 554
    path = parsed.path or "/stream"
    auth = base64.b64encode(f"{username}:{password}".encode()).decode()
    request = (
        f"DESCRIBE rtsp://{host}:{port}{path} RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Accept: application/sdp\r\n"
        f"Authorization: Basic {auth}\r\n"
        "User-Agent: SensorForge-ONVIF-Test/1.0\r\n\r\n"
    ).encode()
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.sendall(request)
            response = sock.recv(8192).decode("utf-8", errors="replace")
    except OSError as exc:
        raise TestFailure(f"RTSP connection failed: {exc}") from exc
    first = response.splitlines()[0] if response else ""
    if " 200 " not in first:
        raise TestFailure(f"RTSP DESCRIBE failed: {first or 'empty response'}")
    if "m=video" not in response:
        raise TestFailure("RTSP DESCRIBE succeeded but SDP contains no video track")


def run_step(name: str, fn):
    try:
        value = fn()
    except Exception as exc:
        print(f"[FAIL] {name}: {exc}")
        raise
    print(f"[ OK ] {name}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description="SensorForge ONVIF read-only regression test (no third-party packages).",
        epilog=(
            "Example: python3 sensorforge_onvif_test.py 192.168.0.3 -u admin -p secret\n"
            "If HOST is omitted, WS-Discovery is used to find the first ONVIF device."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("host", nargs="?", help="SensorForge IPv4 address or hostname")
    parser.add_argument("-u", "--username", default="", help="SensorForge admin/streaming username")
    parser.add_argument("-p", "--password", default="", help="SensorForge password")
    parser.add_argument("--timeout", type=float, default=5.0, help="network timeout in seconds (default: 5)")
    parser.add_argument("--skip-discovery", action="store_true", help="do not test WS-Discovery when HOST is given")
    args = parser.parse_args()

    host = args.host
    discovered_url = ""
    if host is None or not args.skip_discovery:
        ip, discovered_url = run_step("WS-Discovery Probe/ProbeMatch", lambda: discover(min(args.timeout, 5.0)))
        print(f"      discovered {ip} -> {discovered_url}")
        if host is None:
            host = urlsplit(discovered_url).hostname or ip

    if not host:
        raise TestFailure("No target host available")

    base = f"http://{host}"
    device = discovered_url if discovered_url and (urlsplit(discovered_url).hostname == host) else base + "/onvif/device_service"
    media = base + "/onvif/media_service"
    imaging = base + "/onvif/imaging_service"

    time_xml = run_step("Device GetSystemDateAndTime (PRE_AUTH)", lambda: post_soap(
        device, "<tds:GetSystemDateAndTime/>", args.username, args.password, auth=False, timeout=args.timeout))
    if not local_name_contains(time_xml, "GetSystemDateAndTimeResponse"):
        raise TestFailure("GetSystemDateAndTime response missing")

    calls = [
        ("Device GetDeviceInformation", device, "<tds:GetDeviceInformation/>", "GetDeviceInformationResponse"),
        ("Device GetServices", device, "<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>", "GetServicesResponse"),
        ("Device GetNetworkInterfaces", device, "<tds:GetNetworkInterfaces/>", "GetNetworkInterfacesResponse"),
        ("Device GetNetworkProtocols", device, "<tds:GetNetworkProtocols/>", "GetNetworkProtocolsResponse"),
        ("Device GetDNS", device, "<tds:GetDNS/>", "GetDNSResponse"),
        ("Device GetNTP", device, "<tds:GetNTP/>", "GetNTPResponse"),
    ]
    for name, url, request_xml, expected in calls:
        response = run_step(name, lambda u=url, x=request_xml: post_soap(u, x, args.username, args.password, timeout=args.timeout))
        if not local_name_contains(response, expected):
            raise TestFailure(f"{name}: expected {expected}")

    profiles = run_step("Media GetProfiles", lambda: post_soap(media, "<trt:GetProfiles/>", args.username, args.password, timeout=args.timeout))
    profile_token = attribute_token(profiles, "Profiles") or "sensorforge_main"
    source_cfg = attribute_token(profiles, "VideoSourceConfiguration") or "sensorforge_video_source_cfg"
    encoder_cfg = attribute_token(profiles, "VideoEncoderConfiguration") or "sensorforge_video_encoder_cfg"
    source_token = text_of(profiles, "SourceToken") or "sensorforge_video_source"

    run_step("Media GetVideoSourceConfiguration", lambda: post_soap(
        media, f"<trt:GetVideoSourceConfiguration><trt:ConfigurationToken>{xml_escape(source_cfg)}</trt:ConfigurationToken></trt:GetVideoSourceConfiguration>",
        args.username, args.password, timeout=args.timeout))
    run_step("Media GetVideoEncoderConfiguration", lambda: post_soap(
        media, f"<trt:GetVideoEncoderConfiguration><trt:ConfigurationToken>{xml_escape(encoder_cfg)}</trt:ConfigurationToken></trt:GetVideoEncoderConfiguration>",
        args.username, args.password, timeout=args.timeout))

    stream_xml = run_step("Media GetStreamUri", lambda: post_soap(
        media, f"<trt:GetStreamUri><trt:ProfileToken>{xml_escape(profile_token)}</trt:ProfileToken></trt:GetStreamUri>",
        args.username, args.password, timeout=args.timeout))
    stream_uri = text_of(stream_xml, "Uri")
    if not stream_uri.startswith("rtsp://"):
        raise TestFailure(f"Invalid RTSP URI: {stream_uri!r}")

    snapshot_xml = run_step("Media GetSnapshotUri", lambda: post_soap(
        media, f"<trt:GetSnapshotUri><trt:ProfileToken>{xml_escape(profile_token)}</trt:ProfileToken></trt:GetSnapshotUri>",
        args.username, args.password, timeout=args.timeout))
    snapshot_uri = text_of(snapshot_xml, "Uri")
    if not snapshot_uri.startswith("http://"):
        raise TestFailure(f"Invalid snapshot URI: {snapshot_uri!r}")

    run_step("Imaging GetImagingSettings", lambda: post_soap(
        imaging, f"<timg:GetImagingSettings><timg:VideoSourceToken>{xml_escape(source_token)}</timg:VideoSourceToken></timg:GetImagingSettings>",
        args.username, args.password, timeout=args.timeout))
    run_step("Imaging GetOptions", lambda: post_soap(
        imaging, f"<timg:GetOptions><timg:VideoSourceToken>{xml_escape(source_token)}</timg:VideoSourceToken></timg:GetOptions>",
        args.username, args.password, timeout=args.timeout))

    run_step("Snapshot JPEG", lambda: snapshot_test(snapshot_uri, args.username, args.password, args.timeout))
    run_step("RTSP DESCRIBE", lambda: rtsp_describe(stream_uri, args.username, args.password, args.timeout))

    print("\nPASS: SensorForge ONVIF read-only regression test completed successfully.")
    print(f"Stream:   {stream_uri}")
    print(f"Snapshot: {snapshot_uri}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TestFailure as exc:
        print(f"\nFAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
