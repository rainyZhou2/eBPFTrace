import os
import sys
import threading
import time

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

cache_lock = threading.Lock()


def refresher():
    while True:
        with cache_lock:
            time.sleep(0.08)
        time.sleep(0.02)


def handle(n):
    with tracer.start_as_current_span("GET /orders") as root:
        root.set_attribute("request.n", n)
        with tracer.start_as_current_span("load_cache"):
            with cache_lock:
                pass
        with tracer.start_as_current_span("render"):
            sum(range(20000))
        ctx = root.get_span_context()
        print(f"{ctx.trace_id:032x}", flush=True)


threading.Thread(target=refresher, daemon=True).start()
n = 0
while True:
    handle(n)
    n += 1
    time.sleep(0.05)
