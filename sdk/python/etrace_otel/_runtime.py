from opentelemetry import context as otel_context
from opentelemetry import trace
from opentelemetry.context.contextvars_context import ContextVarsRuntimeContext

from ._native import native


def publish_current() -> None:
    ctx = trace.get_current_span().get_span_context()
    if ctx.is_valid:
        native.set(ctx.trace_id, ctx.span_id)
    else:
        native.clear()


class EtraceRuntimeContext(ContextVarsRuntimeContext):
    """Runtime context that mirrors every attach and detach to the agent.

    Select it with OTEL_PYTHON_CONTEXT=etrace_contextvars_context or call
    install() before any span is started.
    """

    def attach(self, context):
        token = super().attach(context)
        publish_current()
        return token

    def detach(self, token):
        super().detach(token)
        publish_current()


def install() -> bool:
    if not native.available:
        return False
    current = otel_context._RUNTIME_CONTEXT
    if isinstance(current, EtraceRuntimeContext):
        return True
    replacement = EtraceRuntimeContext()
    replacement.attach(current.get_current())
    otel_context._RUNTIME_CONTEXT = replacement
    return True
