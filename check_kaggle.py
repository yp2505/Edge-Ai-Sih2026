import urllib.request
import json
try:
    url = "https://www.kaggle.com/api/v1/datasets/view/mozillaorg/common-voice"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req) as response:
        data = json.loads(response.read().decode())
        size_bytes = data.get('totalBytes', 0)
        print(f"Size: {size_bytes / (1024**3):.2f} GB")
except Exception as e:
    print(e)
