FROM python:3.12-slim
WORKDIR /app
COPY receiver.py .
RUN useradd -r -u 1000 bms && mkdir /data && chown bms /data
USER bms
ENV BMS_DB=/data/bms.db BMS_PORT=8080 PYTHONUNBUFFERED=1
EXPOSE 8080
CMD ["python", "receiver.py"]
