from unittest.mock import Mock

from aiohttp.test_utils import make_mocked_request

from iris_gateway.http_support import error_response, request_is_loopback


def test_http_support_uses_actual_peer_and_stable_error_shape() -> None:
    transport = Mock()
    transport.get_extra_info.return_value = ("127.0.0.1", 1)
    assert request_is_loopback(make_mocked_request("GET", "/", transport=transport))
    transport.get_extra_info.return_value = ("192.0.2.1", 1)
    assert not request_is_loopback(make_mocked_request("GET", "/", transport=transport))
    response = error_response(409, "conflict", "state changed", state="ready")
    assert response.status == 409
    assert b'"code": "conflict"' in response.body
