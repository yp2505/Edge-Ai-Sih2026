# deploy_to_aws.ps1 — Hey Vaani: Deploy server.py to AWS EC2 + update ESP32 IP
# Usage: Run this script from PowerShell after confirming EC2 is running.

$EC2_IP       = "13.233.100.83"
$EC2_USER     = "ubuntu"
$KEY_FILE     = "C:\Heyvaani_EC2_Key\hey-vaani-key.pem"
$SERVER_PY    = "c:\Other files from K drive\SIH\Edge-Ai-Sih2026\cloud_server\server.py"
$REQUIREMENTS = "c:\Other files from K drive\SIH\Edge-Ai-Sih2026\cloud_server\requirements.txt"
$REMOTE_DIR   = "/home/ubuntu/heyvaani"

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Hey Vaani -- AWS EC2 Deployment Script" -ForegroundColor Cyan
Write-Host "  Target: $EC2_USER@$EC2_IP" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Test SSH connectivity
Write-Host "[1/6] Testing SSH connectivity to EC2..." -ForegroundColor Yellow
$sshTest = ssh -i $KEY_FILE -o "StrictHostKeyChecking=no" -o "ConnectTimeout=10" "${EC2_USER}@${EC2_IP}" "echo OK" 2>&1
if ($sshTest -ne "OK") {
    Write-Host "SSH failed. Output: $sshTest" -ForegroundColor Red
    Write-Host "Check: Is EC2 running? Is port 22 open in Security Group?" -ForegroundColor Red
    exit 1
}
Write-Host "SSH connected!" -ForegroundColor Green

# Step 2: Setup remote environment
Write-Host "[2/6] Setting up remote environment..." -ForegroundColor Yellow
$setupCmd = @"
if [ ! -f /swapfile ]; then sudo fallocate -l 2G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile && echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab; fi
sudo apt-get update -qq && sudo apt-get install -y -qq python3-pip python3-venv ffmpeg 2>/dev/null
mkdir -p $REMOTE_DIR
if [ ! -d $REMOTE_DIR/venv ]; then python3 -m venv $REMOTE_DIR/venv; fi
echo SETUP_OK
"@
$result = ssh -i $KEY_FILE -o "StrictHostKeyChecking=no" "${EC2_USER}@${EC2_IP}" $setupCmd
Write-Host "Remote environment ready!" -ForegroundColor Green

# Step 3: Upload server.py
Write-Host "[3/6] Uploading server.py to EC2..." -ForegroundColor Yellow
scp -i $KEY_FILE -o "StrictHostKeyChecking=no" $SERVER_PY "${EC2_USER}@${EC2_IP}:${REMOTE_DIR}/server.py"
if ($LASTEXITCODE -ne 0) { Write-Host "SCP failed!" -ForegroundColor Red; exit 1 }
if (Test-Path $REQUIREMENTS) {
    scp -i $KEY_FILE -o "StrictHostKeyChecking=no" $REQUIREMENTS "${EC2_USER}@${EC2_IP}:${REMOTE_DIR}/requirements.txt"
}
Write-Host "Files uploaded!" -ForegroundColor Green

# Step 4: Install Python packages
Write-Host "[4/6] Installing Python packages on EC2 (takes 3-5 min first time)..." -ForegroundColor Yellow
$pipCmd = "source $REMOTE_DIR/venv/bin/activate && pip install --quiet --upgrade pip && pip install --quiet faster-whisper numpy soundfile requests && echo PIP_OK"
ssh -i $KEY_FILE -o "StrictHostKeyChecking=no" "${EC2_USER}@${EC2_IP}" $pipCmd
Write-Host "Packages installed!" -ForegroundColor Green

# Step 5: Create and enable systemd service
Write-Host "[5/6] Installing systemd service..." -ForegroundColor Yellow
$serviceContent = "[Unit]`nDescription=Hey Vaani ASR Server`nAfter=network.target`n`n[Service]`nUser=ubuntu`nWorkingDirectory=$REMOTE_DIR`nExecStart=$REMOTE_DIR/venv/bin/python3 $REMOTE_DIR/server.py --asr whisper --whisper-model base`nRestart=always`nRestartSec=5`n`n[Install]`nWantedBy=multi-user.target"
$svcCmd = "echo '$serviceContent' | sudo tee /etc/systemd/system/heyvaani.service > /dev/null && sudo systemctl daemon-reload && sudo systemctl enable heyvaani && sudo systemctl restart heyvaani && sleep 3 && sudo systemctl is-active heyvaani"
ssh -i $KEY_FILE -o "StrictHostKeyChecking=no" "${EC2_USER}@${EC2_IP}" $svcCmd
Write-Host "Systemd service started!" -ForegroundColor Green

# Step 6: Health check
Write-Host "[6/6] Testing server health endpoint..." -ForegroundColor Yellow
Start-Sleep -Seconds 8
try {
    $health = Invoke-WebRequest -Uri "http://${EC2_IP}/api/health" -TimeoutSec 15 -ErrorAction Stop
    Write-Host "Server responding at http://${EC2_IP}/api/health" -ForegroundColor Green
} catch {
    Write-Host "Server not yet on port 80 -- check AWS Security Group has port 80 open to 0.0.0.0/0" -ForegroundColor Yellow
    Write-Host "Also run: ssh -i $KEY_FILE ${EC2_USER}@${EC2_IP} 'sudo journalctl -u heyvaani -n 30'" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  DEPLOYMENT COMPLETE!" -ForegroundColor Green
Write-Host "  EC2 Server:  http://$EC2_IP" -ForegroundColor White
Write-Host "  Dashboard:   http://$EC2_IP/dashboard" -ForegroundColor White
Write-Host ""
Write-Host "  NEXT: Update ESP32 to point at $EC2_IP" -ForegroundColor Yellow
Write-Host "  Option A: Use captive portal -- power-cycle ESP32," -ForegroundColor White
Write-Host "            connect to 'HeyVaani-Setup', enter IP $EC2_IP" -ForegroundColor White
Write-Host "  Option B: Re-flash firmware with hardcoded IP $EC2_IP" -ForegroundColor White
Write-Host "============================================================" -ForegroundColor Green
