from datasets import load_dataset

try:
    ds = load_dataset("openslr/musan", "music", split="train", streaming=True)
    for sample in ds:
        print("Music sample:", sample.keys())
        break
        
    ds2 = load_dataset("openslr/musan", "noise", split="train", streaming=True)
    for sample in ds2:
        print("Noise sample:", sample.keys())
        break
except Exception as e:
    print("Error:", e)
