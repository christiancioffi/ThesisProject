import requests
import json
import os

URL = "https://tesi.aliagrid.com/configuration"
ADMIN_KEY_FILE=".admin-key"
JSON_FILE_PATH = "new_alinode_conf.json"

def send_configuration():


    if not os.path.exists(JSON_FILE_PATH):
        print(f"Error: The file {JSON_FILE_PATH} does not exist.")
        return

    try:
        with open(JSON_FILE_PATH, "r", encoding="utf-8") as f:
            payload = json.load(f)
    except json.JSONDecodeError:
        print("Error: JSON file not valid.")
        return

    if not os.path.exists(ADMIN_KEY_FILE):
        print(f"Error: The file {ADMIN_KEY_FILE} does not exist.")
        return

    try:
        with open(ADMIN_KEY_FILE, "r", encoding="utf-8") as f:
            ADMIN_KEY = f.read().strip()
    except Exception as e:
        print(f"Error reading admin key file: {e}")
        return

    headers = {
        "X-ADMIN-KEY": ADMIN_KEY,
        "Content-Type": "application/json"
    }

    try:
        print(f"Sending new configuration to {URL}...")
        response = requests.post(URL, json=payload, headers=headers)

        if response.status_code == 200:
            print("Success!")
            print("Server response:", response.json())
        else:
            print(f"Failed with status code: {response.status_code}")
            print("Error details:", response.text)

    except Exception as e:
        print(f"An error occurred during the connection: {e}")

if __name__ == "__main__":
    send_configuration()