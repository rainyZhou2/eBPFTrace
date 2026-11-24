import ctypes
import os
import sys
import time
from itertools import cycle

from opentelemetry import trace
from opentelemetry.exporter.otlp.proto.http.trace_exporter import OTLPSpanExporter
from opentelemetry.sdk.resources import Resource
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor

import etrace_otel

provider = TracerProvider(resource=Resource.create({"service.name": "orders"}))
provider.add_span_processor(BatchSpanProcessor(OTLPSpanExporter(endpoint=os.environ["OTLP_ENDPOINT"])))
trace.set_tracer_provider(provider)
tracer = trace.get_tracer("orders")

if not etrace_otel.install():
    sys.exit("libetrace_ctx not found")

load_cache = ctypes.CDLL(os.environ["ETRACE_LOAD_CACHE_LIBRARY"]).etrace_load_cache
load_cache.argtypes = [ctypes.c_int]
load_cache.restype = ctypes.c_int64


def handle(scenario):
    with tracer.start_as_current_span("GET /orders") as root:
        root.set_attribute("load_cache.scenario", scenario)
        with tracer.start_as_current_span("load_cache"):
            gate_to_unlock_ns = load_cache(int(scenario == "overlap"))
        root.set_attribute("load_cache.gate_to_unlock_ns", gate_to_unlock_ns)
        with tracer.start_as_current_span("render"):
            sum(range(20000))
    print(f"{scenario} {root.get_span_context().trace_id:032x}", flush=True)


for scenario in cycle(("handoff", "overlap")):
    handle(scenario)
    time.sleep(0.05)
