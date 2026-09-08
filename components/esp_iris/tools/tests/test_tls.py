from __future__ import annotations

import os

from cryptography import x509
from cryptography.x509.oid import NameOID

from iris_gateway import tls
from iris_gateway.tls import ensure_certificate, ssl_context


def test_first_run_certificate_and_fingerprint(tmp_path) -> None:
    certificate, key, fingerprint = ensure_certificate(tmp_path)
    assert certificate.is_file()
    assert key.is_file()
    assert len(fingerprint) == 64
    if os.name != "nt":
        assert key.stat().st_mode & 0o077 == 0
    context = ssl_context(certificate, key)
    assert context is not None

    again = ensure_certificate(tmp_path)
    assert again == (certificate, key, fingerprint)


def test_long_hostname_uses_valid_common_name(tmp_path, monkeypatch) -> None:
    hostname = "iad01-dz252-e1c3ab0d-73e6-4672-a22c-0802557cfa6f-028EDEE7C4BC.local"
    assert len(hostname.encode("utf-8")) > 64
    monkeypatch.setattr(tls.socket, "gethostname", lambda: hostname)

    certificate, _, _ = ensure_certificate(tmp_path)

    parsed = x509.load_pem_x509_certificate(certificate.read_bytes())
    common_names = parsed.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
    assert [attribute.value for attribute in common_names] == ["ESP-Iris Local Gateway"]
    alternative_names = parsed.extensions.get_extension_for_class(
        x509.SubjectAlternativeName
    ).value
    assert hostname in alternative_names.get_values_for_type(x509.DNSName)
