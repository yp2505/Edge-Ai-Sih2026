$ErrorActionPreference = 'Stop'
$env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path','User')
$venv = 'k:\Edge_AI\Edge-Ai-Sih2026\training\.venv\Scripts\python.exe'
$pip = 'k:\Edge_AI\Edge-Ai-Sih2026\training\.venv\Scripts\pip.exe'
cd k:\Edge_AI\Edge-Ai-Sih2026\training
& $pip install pydub
& $venv download_dataset.py
& $venv convert_phone_recordings.py --input "..\voice sample\Divy's Voice.zip" --speaker Divy
& $venv convert_phone_recordings.py --input "..\voice sample\Khush Voice.zip" --speaker Khush
& $venv convert_phone_recordings.py --input "..\voice sample\Nil_SIH_Audio.zip" --speaker Nil
& $venv convert_phone_recordings.py --input "..\voice sample\Yug sample.zip" --speaker Yug
& $venv convert_phone_recordings.py --input "..\voice sample\vaani_sample.zip" --speaker Vaani
& $venv augment_data.py
& $venv train_model.py
& $venv convert_tflite.py
cd ..\esp32_firmware
pio run --target upload
