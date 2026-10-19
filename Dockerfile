FROM python:3.13-slim
WORKDIR /app
COPY pyproject.toml ./
COPY crawler ./crawler
RUN pip install --no-cache-dir .
USER 65532:65532
ENTRYPOINT ["python", "-m", "crawler"]
CMD ["/app/config.yaml"]
