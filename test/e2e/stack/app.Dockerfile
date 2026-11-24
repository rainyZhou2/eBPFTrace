FROM python:3.12-slim-trixie

RUN pip install --no-cache-dir opentelemetry-exporter-otlp-proto-http==1.29.0
