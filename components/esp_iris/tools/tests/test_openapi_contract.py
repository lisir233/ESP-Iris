from iris_gateway.openapi_contract import build_openapi


def test_openapi_contract_exposes_observability_and_auth_policy() -> None:
    local = build_openapi(False)
    remote = build_openapi(True)
    assert "/v1/metrics" in local["paths"]
    assert "/v1/devices/{device_id}/file" in local["paths"]
    assert "/v1/devices/{device_id}/files" in local["paths"]
    assert set(local["paths"]["/v1/devices/{device_id}/file"]) == {
        "get",
        "put",
        "delete",
    }
    assert "/v1/devices/{device_id}/directories" in local["paths"]
    assert "/v1/devices/{device_id}/file-rename" in local["paths"]
    assert "delete" in local["paths"]["/v1/devices/{device_id}"]
    assert "security" not in local
    assert remote["security"] == [{"cookieAuth": []}, {"agentToken": []}]
