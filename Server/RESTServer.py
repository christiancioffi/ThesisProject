import datetime
from flask import Flask, redirect, request, jsonify
import time
import os
from io import BytesIO
from mutagen.wave import WAVE
import hashlib
import json
from functools import wraps
from threading import Lock
import psycopg
from psycopg.rows import dict_row
from datetime import datetime, timezone
import csv
from io import StringIO

app = Flask(__name__)

DB_PARAMS = {
    'host': os.getenv('POSTGRES_HOST'),
    'user': os.getenv('POSTGRES_USER'),
    'password': os.getenv('POSTGRES_PASSWORD'),
    'dbname': os.getenv('POSTGRES_DB'),
    'port': 5432
}

#TODO: rimettere HTTPS

SERVER_PORT=8443
BASE_DIR = "."
LOGS_DIR = "logs"
SAMPLES_DIR = "AudioSamples"
CONF_FILE_ALINODE = "alinode_conf.json"
CONF_DIR = "NodesConfiguration"
API_KEY=os.environ.get('API_KEY')
ADMIN_KEY=os.environ.get('ADMIN_KEY')
config_lock = Lock()
METADATA_KEYS = ["tmst", "noId", "blvl", "rmsv"]
# Formato ISO 8601 Standard
DATE_FORMAT = "%Y-%m-%dT%H:%M:%S%z"
PREDICTIONS_DATE_FORMAT = "%Y-%m-%dT%H:%M:%S.%f%z"
TARGET_DOMAIN = "tesi.aliagrid.com"  # "ec2-3-122-216-71.eu-central-1.compute.amazonaws.com"
REDIRECT_TARGET = f"https://{TARGET_DOMAIN}"

@app.before_request      
def enforce_domain_and_https():
    # 1. Otteniamo l'host richiesto (es. "localhost:8443" o "123.45.67.89")
    requested_host = request.host.split(':')[0]  # Rimuove la porta se presente
    
    # 2. Controlliamo se la connessione è sicura (HTTPS)
    # Nota: Se sei dietro un proxy (Docker/Nginx), usa request.is_secure o 'X-Forwarded-Proto'
    is_https = request.is_secure or request.headers.get('X-Forwarded-Proto') == 'https'
    
    # 3. Logica di Redirect
    # Reindirizza se NON è HTTPS O se il dominio non coincide
    if not is_https or requested_host != TARGET_DOMAIN:
        # Costruiamo l'URL finale mantenendo il percorso (es. /configuration)
        new_url = f"https://{TARGET_DOMAIN}{request.full_path}"
        # Rimuove il punto interrogativo finale se non ci sono parametri
        new_url = new_url.rstrip('?')
        
        return redirect(new_url, code=301) # 301 = Permanent Redirect

'''
@app.before_request      
def enforce_https():
    # 2. Controlliamo se la connessione è sicura (HTTPS)
    # Nota: Se sei dietro un proxy (Docker/Nginx), usa request.is_secure o 'X-Forwarded-Proto'
    is_https = request.is_secure or request.headers.get('X-Forwarded-Proto') == 'https'
    
    # 3. Logica di Redirect
    # Reindirizza se NON è HTTPS O se il dominio non coincide
    if not is_https:
        # Costruiamo l'URL finale mantenendo il percorso (es. /configuration)
        new_url = f"https://{TARGET_DOMAIN}{request.full_path}"
        # Rimuove il punto interrogativo finale se non ci sono parametri
        new_url = new_url.rstrip('?')
        
        return redirect(new_url, code=301) # 301 = Permanent Redirect
'''


def api_key_required(f):
    @wraps(f)
    def decorated_function(*args, **kwargs):
        api_key = request.headers.get('X-API-KEY')
        
        if api_key and api_key == API_KEY:
            return f(*args, **kwargs)
        else:
            return jsonify({"error": "Missing (or invalid) API KEY"}), 401
            
    return decorated_function

def admin_key_required(f):
    @wraps(f)
    def decorated_function(*args, **kwargs):
        admin_key = request.headers.get('X-ADMIN-KEY')
        
        if admin_key and admin_key == ADMIN_KEY:
            return f(*args, **kwargs)
        else:
            return jsonify({"error": "Missing (or invalid) ADMIN KEY"}), 401
            
    return decorated_function

def get_db_connection():
    try:
        conn = psycopg.connect(**DB_PARAMS, row_factory=dict_row)
        return conn
    except psycopg.Error as err:
        print(f"Connection error: {err}")
        return None

def is_valid_wav_bytes(data):
    return (
        len(data) >= 44 and
        data[0:4] == b"RIFF" and
        data[8:12] == b"WAVE"
    )

def getMetadata(wav_bytes):
    metadata = {}
    i = 12  # salta RIFF + size + WAVE

    size = len(wav_bytes)

    while i + 8 <= size:
        chunk_id = wav_bytes[i:i+4]
        chunk_size = int.from_bytes(wav_bytes[i+4:i+8], "little")
        chunk_data_start = i + 8
        chunk_data_end = chunk_data_start + chunk_size

        if chunk_id == b"LIST" and wav_bytes[chunk_data_start:chunk_data_start+4] == b"INFO":
            j = chunk_data_start + 4  # salta "INFO"

            while j + 8 <= chunk_data_end:
                key = wav_bytes[j:j+4].decode("ascii", errors="ignore").strip()
                length = int.from_bytes(wav_bytes[j+4:j+8], "little")
                value_bytes = wav_bytes[j+8:j+8+length]

                # rimuove \x00 finali
                value = value_bytes.rstrip(b"\x00").decode("ascii", errors="ignore")
                if key in METADATA_KEYS:
                    metadata[key] = value
                else:
                    raise Exception(f"Unexpected metadata key: {key}")
                j += 8 + length

        # chunk allineati a 2 byte
        i = chunk_data_end + (chunk_size % 2)

    

    return metadata

@app.route('/', methods=['GET'])
def home():
    return "Audio REST server is running."

@app.route('/audio', methods=['POST'])
@api_key_required
def audio():

    # Legge i byte grezzi del body HTTP
    wav_bytes = request.get_data()
    audio_tmst=""
    
    
    try:

        if not wav_bytes:
            raise Exception("Empty request body")

        # Controlla magic bytes WAV
        if not is_valid_wav_bytes(wav_bytes):
            raise Exception("Chunk is not a valid WAV file")

         # Estrae metadati dal WAV (dai byte)
        metadata = getMetadata(wav_bytes)
        print("Received WAV metadata:", metadata)

        try:
            audio_tmst=datetime.strptime(metadata['tmst'], DATE_FORMAT)
        except Exception as e:
            raise Exception(f"Invalid timestamp format in metadata: {metadata.get('tmst', 'N/A')}. Expected format: {DATE_FORMAT}")
    except Exception as e:
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 400
        
        
    try:

        # Salva il file WAV nella cartella AudioSamples
        base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "."))
        samples_dir = os.path.join(base_dir, "AudioSamples")
        os.makedirs(samples_dir, exist_ok=True)

        # Salvataggio file
        hash_obj = hashlib.sha256(wav_bytes)
        hash_hex = hash_obj.hexdigest()
        filename = f"audio_{hash_hex}.wav"
        filepath = os.path.join(samples_dir, filename)

        try:

            with open(filepath, "wb") as f:
                f.write(wav_bytes)

            insert_chunk_into_db(filename, metadata['tmst'], metadata['blvl'], metadata['noId'], metadata['rmsv'])

        except Exception as e:
            if os.path.exists(filepath):
                os.remove(filepath)  # Rimuove il file se c'è un errore nel DB
                print(f"Removed file {filename} due to error")
            raise e


        return jsonify({
            "status": "Chunk received successfully",
        })

    except Exception as e:
        print(f"caught exception {type(e).__name__} {e}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500

@app.route('/test_audio', methods=['POST'])
@api_key_required
def test_audio():

    # Legge i byte grezzi del body HTTP
    wav_bytes = request.get_data()
    audio_tmst=""
    
    
    try:

        if not wav_bytes:
            raise Exception("Empty request body")

        # Controlla magic bytes WAV
        if not is_valid_wav_bytes(wav_bytes):
            raise Exception("Chunk is not a valid WAV file")

         # Estrae metadati dal WAV (dai byte)
        metadata = getMetadata(wav_bytes)
        print("Received WAV metadata:", metadata)

        try:
            audio_tmst=datetime.strptime(metadata['tmst'], DATE_FORMAT)
        except Exception as e:
            raise Exception(f"Invalid timestamp format in metadata: {metadata.get('tmst', 'N/A')}. Expected format: {DATE_FORMAT}")
    except Exception as e:
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 400
        
        
    try:

        # Salva il file WAV nella cartella AudioSamples
        base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "./Test"))
        samples_dir = os.path.join(base_dir, "AudioSamplesTest")
        os.makedirs(samples_dir, exist_ok=True)

        # Salvataggio file
        hash_obj = hashlib.sha256(wav_bytes)
        hash_hex = hash_obj.hexdigest()
        filename = f"audio_{hash_hex}.wav"
        filepath = os.path.join(samples_dir, filename)
        
        try:

            with open(filepath, "wb") as f:
                f.write(wav_bytes)

            #insert_chunk_into_db(filename, metadata['tmst'], metadata['blvl'], metadata['noId'], metadata['rmsv'])

        except Exception as e:
            if os.path.exists(filepath):
                os.remove(filepath)  # Rimuove il file se c'è un errore nel DB
                print(f"Removed file {filename} due to error")
            raise e
        

        return jsonify({
            "status": "Chunk received successfully",
        })

    except Exception as e:
        print(f"caught exception {type(e).__name__} {e}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500


@app.route('/predictions', methods=['POST'])
@api_key_required
def predictions():
    try:
        raw_data = request.get_data().decode("utf-8")

        if not raw_data.strip():
            raise Exception("Empty request body")

        rows = []
        reader = csv.reader(StringIO(raw_data))

        header = next(reader, None)  # salta la riga di intestazione
        if header is None:
            raise Exception("Empty request body")

        for i, row in enumerate(reader):
            if len(row) != 3:
                raise Exception(f"Invalid CSV row at line {i}: expected 3 columns, got {len(row)}")

            timestamp_str, prediction_int8_str, prediction_f32_str = row[0].strip(), row[1].strip(), row[2].strip()

            try:
                datetime.strptime(timestamp_str, PREDICTIONS_DATE_FORMAT)
            except Exception:
                raise Exception(f"Invalid timestamp format at line {i}: {timestamp_str}")

            try:
                prediction_int8_val = float(prediction_int8_str)
            except Exception:
                raise Exception(f"Invalid prediction_int8 value at line {i}: {prediction_int8_str}")

            try:
                prediction_f32_val = float(prediction_f32_str)
            except Exception:
                raise Exception(f"Invalid prediction_f32 value at line {i}: {prediction_f32_str}")

            rows.append((timestamp_str, prediction_int8_val, prediction_f32_val))

        if not rows:
            raise Exception("No valid rows found in CSV")

        #print(f"Rows: {rows}")

        insert_predictions_into_db(rows)
        insert_event_into_db("predictions_received")

        return jsonify({
            "status": "Predictions saved successfully",
            "rows_inserted": len(rows)
        })

    except Exception as e:
        print(f"caught exception {type(e).__name__} {e}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 400


def insert_predictions_into_db(rows):
    db = None
    cursor = None

    db = get_db_connection()
    if not db:
        raise Exception("Database connection failed")

    cursor = db.cursor()

    sql = """
        INSERT INTO Predictions (timestamp, prediction_int8, prediction_f32)
        VALUES (%s, %s, %s)
        """

    try:
        cursor.executemany(sql, rows)
        db.commit()
        print(f"Insert executed successfully ({len(rows)} rows)")
    except psycopg.Error as err:
        print(f"Error during insert: {err}")
        db.rollback()
        raise Exception(f"Database insert error: {err}")
    finally:
        if cursor:
            cursor.close()
        if db:
            db.close()

@app.route('/logs', methods=['POST'])
@api_key_required
def save_logs():
    try:
        data = request.get_data().decode("utf-8")

        base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), BASE_DIR))
        logs_dir = os.path.join(base_dir, LOGS_DIR)
        timestamp = str(time.time()).replace(".", "_")
        filename=f"log_{timestamp}.txt"

        with open(os.path.join(logs_dir, filename), "w", encoding="utf-8") as f:
                f.write(data)

        insert_event_into_db("log_received")

        return jsonify({
                "status": "Logs saved successfully",
        })
        
    except Exception as e:
        print(f"caught exception {type(e).__name__} {e}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500

@app.route('/configuration', methods=['GET'])
@api_key_required
def get_configuration():
    try:
        base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), BASE_DIR))
        conf_dir = os.path.join(base_dir, CONF_DIR)
        conf_path = os.path.join(conf_dir, CONF_FILE_ALINODE)
        config_data = ""

        with config_lock:

            with open(conf_path, "r", encoding="utf-8") as f:
                config_data =  json.load(f)
        
        insert_event_into_db("configuration_requested")

        return jsonify(config_data)
        
    except Exception as e:
        print(f"caught exception {type(e).__name__} {e}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500

@app.route('/configuration', methods=['POST'])
@admin_key_required
def update_configuration():

    if not request.is_json:
        return jsonify({"status": "error", "message": "Content-Type must be application/json"}), 415
    
    try:
        new_config = request.get_json()

        base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), BASE_DIR))
        conf_dir = os.path.join(base_dir, CONF_DIR)
        conf_path = os.path.join(conf_dir, CONF_FILE_ALINODE)

        with config_lock:

            with open(conf_path, "w", encoding="utf-8") as f:
                json.dump(new_config, f, indent=4, ensure_ascii=False)

        return jsonify({
                "status": "Configuration updated successfully",
        })
        
    except Exception as e:
        print(f"caught exception {type(e).__name__} {e}")
        return jsonify({
            "status": "error",
            "message": str(e)
        }), 500

def insert_chunk_into_db(filename: str, timestamp: str, battery_level: int, node_id: str, rms: float):
    db=None
    cursor=None
    
    db = get_db_connection()
    if not db:
        raise Exception("Database connection failed")

    cursor = db.cursor()

    sql = """
        INSERT INTO AudioChunks (filename, timestamp, battery_level, node_id, rms)
        VALUES (%s, %s, %s, %s, %s)
        """

    values = (
        filename,           # Filename
        timestamp,   # Timestamp
        battery_level,   # Battery level
        node_id,   # Node ID
        rms    # RMS value
    )

    try:
        cursor.execute(sql, values)
        db.commit()
        print(f"Insert executed successfully")
    except psycopg.Error as err:
        print(f"Error during insert: {err}")
        db.rollback()
        raise Exception(f"Database insert error: {err}")
    finally:
        if cursor:
            cursor.close()
        if db:
            db.close()

def insert_event_into_db(event_type):
    db=None
    cursor=None
    
    db = get_db_connection()
    if not db:
        print("Database connection failed")
        return

    cursor = db.cursor()
    if not cursor:
        db.close()
        print("Database cursor creation failed")
        return

    sql = """
        INSERT INTO Events (event_type, timestamp)
        VALUES (%s, %s)
        """

    current_time = datetime.now(timezone.utc).strftime(DATE_FORMAT)

    try:
        cursor.execute(sql, (event_type, current_time))
        db.commit()
        print(f"Insert executed successfully")
    except psycopg.Error as err:
        print(f"Error during insert: {err}")
        db.rollback()
        #raise Exception(f"Database insert error: {err}")
    finally:
        if cursor:
            cursor.close()
        if db:
            db.close()

if __name__ == '__main__':
    os.makedirs(LOGS_DIR, exist_ok=True)
    app.run(host='0.0.0.0', port=SERVER_PORT, debug=True)
