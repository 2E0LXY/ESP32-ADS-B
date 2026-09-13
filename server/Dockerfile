FROM python:3.12-slim

WORKDIR /srv

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app
# Offline operator, type, country and livery lists. Read once at startup;
# see app/reference.py, and reference/README.md for provenance.
COPY reference ./reference

# The SQLite file lives outside the image in a mounted volume (see
# docker-compose.yml) so rebuilding/redeploying the container never touches
# customer/device/key data.
RUN mkdir -p /data

EXPOSE 8090
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8090"]
