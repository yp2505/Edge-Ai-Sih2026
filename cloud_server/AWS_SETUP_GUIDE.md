# Hey Vaani — AWS Cloud Server Setup Guide

Complete step-by-step guide to deploy the Hey Vaani ASR server on AWS EC2.

---

## What You Will Set Up

```
ESP32 (microphone) ──► EC2 Server (server.py) ──► Amazon Transcribe (ASR)
                              │
                              ▼
                    Intent Handler (rule-based)
                              │
                              ▼
                    JSON response back to ESP32
```

---

## Prerequisites

- A laptop/PC with internet connection
- A valid debit/credit card (for AWS identity verification — you won't be charged)
- Basic terminal knowledge

---

## STEP 1: Create AWS Account

1. Open [https://aws.amazon.com](https://aws.amazon.com)
2. Click **"Create an AWS Account"**
3. Enter: email, password, account name (e.g. `heyvaani`)
4. Choose **"Personal"** account type
5. Enter card details (identity verification only — not charged)
6. Verify phone number via OTP
7. Choose **"Basic support — Free"**
8. ✅ Account created

---

## STEP 2: Launch a Free EC2 Instance

1. Log into [https://console.aws.amazon.com](https://console.aws.amazon.com)
2. Search bar → type **EC2** → click it
3. Click **"Launch Instance"** (orange button)
4. Fill in:

| Field | Value |
|---|---|
| **Name** | `heyvaani-server` |
| **AMI** | Ubuntu Server 22.04 LTS — must say ✅ Free tier eligible |
| **Instance type** | `t2.micro` ← **must be exactly this** |
| **Key pair** | Create new → Name: `heyvaani-key` → RSA → .pem → Download |

> ⚠️ **Save the .pem file** — you cannot download it again!

5. Under **Network settings** → tick ✅ **"Allow SSH traffic from Anywhere"**
6. Storage: leave default (8 GB)
7. Click **"Launch Instance"**
8. Wait ~2 minutes

---

## STEP 3: Open Port 5000 (Firewall)

1. **EC2 → Instances** → click your instance
2. Scroll down → **Security** tab
3. Click the Security Group link (e.g. `sg-0abc123...`)
4. **Edit inbound rules → Add rule** (twice):

| Type | Port | Source |
|---|---|---|
| Custom TCP | `5000` | `0.0.0.0/0` |
| Custom TCP | `8080` | `0.0.0.0/0` |

5. Click **Save rules**

---

## STEP 4: Get Your EC2 Public IP

1. **EC2 → Instances** → click your instance
2. Find **"Public IPv4 address"** (e.g. `3.109.XX.XX`)
3. Copy this IP — needed everywhere below

---

## STEP 5: SSH Into Your Server

Open terminal on your laptop:

```bash
# Fix key file permissions (required on Linux/Mac)
chmod 400 ~/Downloads/heyvaani-key.pem

# Connect to EC2
ssh -i ~/Downloads/heyvaani-key.pem ubuntu@3.109.XX.XX
```

If asked "Are you sure you want to continue?" → type `yes`

You'll see: `ubuntu@ip-xxx:~$` — you're connected!

---

## STEP 6: Install Everything on EC2

Run these in order inside the SSH session:

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install tools
sudo apt install -y python3-pip python3-venv git ffmpeg screen awscli

# Create 2GB swap (CRITICAL — only 1GB RAM on t2.micro)
sudo fallocate -l 2G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# Python environment
python3 -m venv ~/venv
source ~/venv/bin/activate

# Install Python packages
pip install "amazon-transcribe[awscrt]" boto3 numpy soundfile
```

---

## STEP 7: Set Up Amazon Transcribe (IAM Keys)

### A. Create IAM User (on AWS Console)

1. **AWS Console → IAM → Users → Create User**
2. Username: `heyvaani-transcribe`
3. Next → **Attach policies directly**
4. Search and tick: `AmazonTranscribeFullAccess`
5. Create User
6. Click the user → **Security credentials** tab
7. **Create access key** → choose **"Other"** → Next
8. **Download .csv file** (has your keys)

### B. Configure Keys on EC2

Back in SSH terminal:

```bash
aws configure
```

Enter:
```
AWS Access Key ID:     [paste from CSV]
AWS Secret Access Key: [paste from CSV]
Default region name:   ap-south-1
Default output format: json
```

Test it:
```bash
aws sts get-caller-identity
# Should print your account ID ✅
```

---

## STEP 8: Copy server.py to EC2

Run from **your laptop** (not EC2):

```bash
scp -i ~/Downloads/heyvaani-key.pem \
    /path/to/server.py \
    ubuntu@3.109.XX.XX:~/
```

---

## STEP 9: Run the Server

In SSH terminal:

```bash
screen -S heyvaani          # start persistent session
source ~/venv/bin/activate
python3 server.py --asr aws # run with Amazon Transcribe
```

You should see:
```
🎙️  Hey Vaani ASR Server
🗣️  ASR backend: Amazon Transcribe (ap-south-1, en-IN)
✅  Listening on 0.0.0.0:5000
```

**Detach** (keeps running): `Ctrl+A` then `D`  
**Reattach later**: `screen -r heyvaani`

---

## STEP 10: Make Server Start on Reboot

```bash
sudo nano /etc/systemd/system/heyvaani.service
```

Paste:
```ini
[Unit]
Description=Hey Vaani ASR Server
After=network.target

[Service]
User=ubuntu
WorkingDirectory=/home/ubuntu
ExecStart=/home/ubuntu/venv/bin/python3 /home/ubuntu/server.py --asr aws
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Save (Ctrl+X → Y → Enter):
```bash
sudo systemctl daemon-reload
sudo systemctl enable heyvaani
sudo systemctl start heyvaani
sudo systemctl status heyvaani   # check it's running
```

---

## STEP 11: Point ESP32 at EC2

1. Power cycle the ESP32 to trigger captive portal
2. Connect your phone to WiFi: **HeyVaani-Setup**
3. Open browser → `192.168.4.1`
4. Enter:
   - WiFi SSID: your network name
   - WiFi Password: your password
   - Server IP: `3.109.XX.XX` (your EC2 IP)
5. Submit → ESP32 reboots and connects ✅

---

## STEP 12: Set Billing Alert

1. **AWS Console → Billing → Billing Preferences**
2. Tick: **"Receive Free Tier Usage Alerts"**
3. Enter email → Save

You'll get warnings before any charge happens.

---

## Quick Reference Commands

```bash
# SSH in
ssh -i ~/Downloads/heyvaani-key.pem ubuntu@3.109.XX.XX

# Reattach server session
screen -r heyvaani

# Check server health
curl http://3.109.XX.XX:8080/api/health

# View live logs (systemd)
sudo journalctl -u heyvaani -f

# Restart server
sudo systemctl restart heyvaani
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Permission denied (publickey)` | `chmod 400 heyvaani-key.pem` |
| Can't reach port 5000 | Check Security Group inbound rules (Step 3) |
| `UnrecognizedClientException` | Re-run `aws configure` with correct keys |
| `AccessDeniedException` | IAM user needs `AmazonTranscribeFullAccess` |
| `amazon_transcribe not found` | `pip install "amazon-transcribe[awscrt]"` |
| Server crashes | `sudo journalctl -u heyvaani -n 50` |

---

## Free Tier Limits

| Resource | Free Limit | Your Usage |
|---|---|---|
| EC2 t2.micro | 750 hrs/month | ~720 hrs/month ✅ |
| Amazon Transcribe | 60 min/month | ~1,800 detections/month ✅ |
| Data transfer out | 100 GB/month | Far less ✅ |
| EBS Storage | 30 GB | 8 GB ✅ |
| **Duration** | **12 months** | After 12 months ~$8.50/month |

---

*Hey Vaani SIH 2026 — AWS Deployment Guide v1.0*
