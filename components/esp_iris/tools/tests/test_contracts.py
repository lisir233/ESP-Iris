from iris_gateway.contracts import GatewayHub
from iris_gateway.hub import IrisHub


def test_real_hub_satisfies_gateway_application_boundary() -> None:
    hub = IrisHub("contract-test")
    assert isinstance(hub, GatewayHub)

