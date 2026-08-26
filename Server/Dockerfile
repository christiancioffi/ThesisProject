FROM python:latest

WORKDIR /app

# Dipendenze
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Codice
COPY RESTServer.py .


# Porta esposta
EXPOSE 8443

# Avvio
CMD ["python", "RESTServer.py"]
