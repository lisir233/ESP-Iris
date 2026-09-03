"""Explicit, side-effect-free state machines shared by Gateway modules."""

from __future__ import annotations

from .compat import StrEnum


class StateTransitionError(ValueError):
    """Raised when persisted or runtime state attempts an illegal transition."""


class OperationState(StrEnum):
    QUEUED = "queued"
    RUNNING = "running"
    PRESERVING_EVIDENCE = "preserving_evidence"
    ENTERING_RECOVERY = "entering_recovery"
    WAITING_RECOVERY = "waiting_recovery"
    RECOVERY_CONNECTED = "recovery_connected"
    PREPARING_OTA = "preparing_ota"
    ERASING = "erasing"
    VALIDATING_PLAN = "validating_plan"
    TRANSFERRING = "transferring"
    VERIFYING = "verifying"
    COMMITTING = "committing"
    WAITING_DEVICE = "waiting_device"
    RECONNECTING = "reconnecting"
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    CANCELLED = "cancelled"
    INTERRUPTED = "interrupted"
    OUTCOME_UNKNOWN = "outcome_unknown"


TERMINAL_OPERATION_STATES = frozenset(
    {
        OperationState.SUCCEEDED,
        OperationState.FAILED,
        OperationState.CANCELLED,
        OperationState.INTERRUPTED,
        OperationState.OUTCOME_UNKNOWN,
    }
)
ACTIVE_OPERATION_STATES = frozenset(set(OperationState) - TERMINAL_OPERATION_STATES)

_OPERATION_TRANSITIONS: dict[OperationState, frozenset[OperationState]] = {
    OperationState.QUEUED: frozenset(
        {
            OperationState.RUNNING,
            OperationState.CANCELLED,
            OperationState.INTERRUPTED,
            OperationState.OUTCOME_UNKNOWN,
        }
    ),
    **{
        state: frozenset(ACTIVE_OPERATION_STATES | TERMINAL_OPERATION_STATES)
        for state in ACTIVE_OPERATION_STATES
        if state is not OperationState.QUEUED
    },
    **{state: frozenset() for state in TERMINAL_OPERATION_STATES},
}


def operation_transition(current: str, requested: str) -> OperationState:
    """Validate and return the requested operation state.

    Repeating an active state is allowed because OTA progress updates retain the
    same stage.  Terminal states are immutable, which makes retry/idempotency
    behavior deterministic after a Gateway restart.
    """

    try:
        source = OperationState(current)
        target = OperationState(requested)
    except ValueError as exc:
        raise StateTransitionError(str(exc)) from exc
    if source == target and source not in TERMINAL_OPERATION_STATES:
        return target
    if target not in _OPERATION_TRANSITIONS[source]:
        raise StateTransitionError(
            f"illegal operation transition: {source.value} -> {target.value}"
        )
    return target


class SessionState(StrEnum):
    NEGOTIATING = "negotiating"
    READY = "ready"
    CLOSED = "closed"


class SessionEvent(StrEnum):
    AUTHENTICATED = "authenticated"
    CLOSE = "close"


_SESSION_TRANSITIONS = {
    (SessionState.NEGOTIATING, SessionEvent.AUTHENTICATED): SessionState.READY,
    (SessionState.NEGOTIATING, SessionEvent.CLOSE): SessionState.CLOSED,
    (SessionState.READY, SessionEvent.CLOSE): SessionState.CLOSED,
}


def session_transition(current: SessionState, event: SessionEvent) -> SessionState:
    try:
        return _SESSION_TRANSITIONS[(current, event)]
    except KeyError as exc:
        raise StateTransitionError(
            f"illegal session transition: {current.value} + {event.value}"
        ) from exc


__all__ = [
    "ACTIVE_OPERATION_STATES",
    "TERMINAL_OPERATION_STATES",
    "OperationState",
    "SessionEvent",
    "SessionState",
    "StateTransitionError",
    "operation_transition",
    "session_transition",
]
