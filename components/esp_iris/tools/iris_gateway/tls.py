from __future__ import annotations

import datetime
import hashlib
import ipaddress
import os
import pathlib
import socket
import ssl


def ensure_certificate(
    state_dir: pathlib.Path,
    *,
    cert_path: pathlib.Path | None = None,
    key_path: pathlib.Path | None = None,
) -> tuple[pathlib.Path, pathlib.Path, str]:
    if (cert_path is None) != (key_path is None):
        raise ValueError("--tls-cert and --tls-key must be supplied together")
    if cert_path is None:
        cert_path = state_dir / "tls" / "gateway.crt"
        key_path = state_dir / "tls" / "gateway.key"
    assert key_path is not None
    if not cert_path.exists() or not key_path.exists():
        if cert_path.parent != key_path.parent:
            cert_path.parent.mkdir(parents=True, exist_ok=True)
            key_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            cert_path.parent.mkdir(parents=True, exist_ok=True)
        _generate(cert_path, key_path)
    der = ssl.PEM_cert_to_DER_cert(cert_path.read_text(encoding="ascii"))
    fingerprint = hashlib.sha256(der).hexdigest()
    return cert_path, key_path, fingerprint


def ssl_context(cert_path: pathlib.Path, key_path: pathlib.Path) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert_path, key_path)
    return context


def _generate(cert_path: pathlib.Path, key_path: pathlib.Path) -> None:
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID
    except ImportError as exc:
        raise RuntimeError(
            "cryptography is required to create the first-run HTTPS certificate"
        ) from exc

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    hostname = socket.gethostname()
    names: list[x509.GeneralName] = [
        x509.DNSName("localhost"),
        x509.DNSName(hostname),
        x509.IPAddress(ipaddress.ip_address("127.0.0.1")),
        x509.IPAddress(ipaddress.ip_address("::1")),
    ]
    with_context = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        with_context.connect(("8.8.8.8", 80))
        address = with_context.getsockname()[0]
        names.append(x509.IPAddress(ipaddress.ip_address(address)))
    except OSError:
        pass
    finally:
        with_context.close()
    subject = issuer = x509.Name(
        [
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "ESP-Iris Local Gateway"),
            x509.NameAttribute(NameOID.COMMON_NAME, hostname),
        ]
    )
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.SubjectAlternativeName(names), critical=False)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )
    key_data = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    cert_data = certificate.public_bytes(serialization.Encoding.PEM)
    descriptor = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(key_data)
    cert_path.write_bytes(cert_data)


__all__ = ["ensure_certificate", "ssl_context"]
