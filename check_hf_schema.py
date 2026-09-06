import urllib.request
import json

url = "https://datasets-server.huggingface.co/first-rows?dataset=FluidInference%2Fmusan&config=default&split=train"
try:
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req) as response:
        data = json.loads(response.read().decode())
        if "rows" in data and len(data["rows"]) > 0:
            row = data["rows"][0]["row"]
            print("Keys:", list(row.keys()))
            if "audio" in row:
                print("Audio keys:", list(row["audio"][0].keys()) if isinstance(row["audio"], list) else list(row["audio"].keys()))
except Exception as e:
    print(e)
