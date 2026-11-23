"""Publish the active OpenTelemetry span to the eTrace agent.

Off-CPU intervals recorded by the agent are tagged with the trace and span
that were current on the thread when it blocked.
"""

from ._runtime import EtraceRuntimeContext, install, publish_current

__all__ = ["EtraceRuntimeContext", "install", "publish_current"]
