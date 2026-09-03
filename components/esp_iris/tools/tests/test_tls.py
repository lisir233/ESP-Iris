from __future__ import annotations

import os

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
