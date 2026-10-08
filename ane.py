import os
import subprocess
from typing import Dict, Optional, List

import requests
from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel
from datetime import datetime

try:
    from datetime import UTC
except ImportError:  # fix for Python < 3.11
    from datetime import timezone
    UTC = timezone.utc

import locker

# -----------------------------------------------------------------------------
# Env / Config
# -----------------------------------------------------------------------------
APF_BASE_URL = os.getenv("APF_BASE_URL", "http://192.168.108.187:8100").rstrip("/")
SERVICE_ID = os.getenv("SERVICE_ID", "ATSSS")

ANE_ID = {
    "ip": os.getenv("ANE_IP", "192.168.108.187"),
    "port": int(os.getenv("ANE_PORT", "12345")),
}
CALLBACK_URL = os.getenv("ANE_CALLBACK_URL", "")

# ANE binary + args (move hardcoding here)
ATSSS_ANE_BIN = os.getenv("ATSSS_ANE_BIN", "./atsss_project/build/atsss_ane")
ATSSS_ANE_PORTS = os.getenv("ATSSS_ANE_PORTS", "4443,4444")
ATSSS_CERT = os.getenv("ATSSS_CERT", "./atsss_project/build/server_cert.pem")
ATSSS_KEY = os.getenv("ATSSS_KEY", "./atsss_project/build/server_key.pem")

# Mode IDs understood by atsss_ane binary
ATSSS_RTT_MODE_ID = os.getenv("ATSSS_RTT_MODE_ID", "1")
ATSSS_LB_MODE_ID = os.getenv("ATSSS_LB_MODE_ID", "2")  # load-balancing
ATSSS_SD_MODE_ID = os.getenv("ATSSS_SD_MODE_ID", "3")  # selective-duplication

# Misc params
ATSSS_GOBACKN = os.getenv("ATSSS_GOBACKN", "0")
ATSSS_SD_PARAM = os.getenv("ATSSS_SD_PARAM", "1000000")  # used for selective-duplication
ATSSS_RTT_PARAM = os.getenv("ATSSS_RTT_PARAM", "1000000") # used for minRTT
RATIO_SCALE_DIV = int(os.getenv("RATIO_SCALE_DIV", "10"))  # your old logic: 30% -> 3, 70% -> 7

# -----------------------------------------------------------------------------
# App state
# -----------------------------------------------------------------------------
app = FastAPI(title="ANE (ATSSS-Cp client)")

active_sessions: Dict[str, dict] = {}
active_processes: List[subprocess.Popen] = []
stored_ratio: Optional[int] = None
last_decision_time = locker.LockedValue(None)


# -----------------------------------------------------------------------------
# Models
# -----------------------------------------------------------------------------
class RatioUpdate(BaseModel):
    ratio: int


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
def subscribe_to_apf() -> None:
    if not APF_BASE_URL:
        raise RuntimeError("APF_BASE_URL is not set")
    if not CALLBACK_URL:
        raise RuntimeError("ANE_CALLBACK_URL is not set")

    payload = {
        "aneID": ANE_ID,
        "serviceID": SERVICE_ID,
        "callback": CALLBACK_URL,
    }
    r = requests.post(f"{APF_BASE_URL}/atsss-cp/sub", json=payload, timeout=5.0)
    r.raise_for_status()
    print("[ANE] Subscribed to APF")


def stop_all_processes() -> None:
    # Stop only the processes started by this service
    for p in active_processes:
        try:
            p.terminate()
        except Exception:
            pass

    for p in active_processes:
        try:
            p.wait(timeout=5)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass

    active_processes.clear()


def _start_atsss_ane_load_balancing(split_ratio: dict) -> None:
    # split_ratio looks like {"3GPP": 30, "N3GPP": 70}
    try:
        gpp = int(split_ratio.get("3GPP", 50))
        n3 = int(split_ratio.get("N3GPP", 50))
    except Exception:
        gpp, n3 = 50, 50

    # keep your old convention: 30 -> 3, 70 -> 7
    fiveg = gpp // max(RATIO_SCALE_DIV, 1)
    nonfiveg = n3 // max(RATIO_SCALE_DIV, 1)

    print("\n=== ATSSS START (ANE / load-balancing) ===")
    print(f"Ports: {ATSSS_ANE_PORTS}")
    print(f"SplitRatio: 3GPP={gpp} N3GPP={n3}  =>  shares={fiveg},{nonfiveg}")
    print("==========================================\n")

    cmd = [
        ATSSS_ANE_BIN,
        ATSSS_ANE_PORTS,
        ATSSS_CERT,
        ATSSS_KEY,
        ATSSS_LB_MODE_ID,
        f"{fiveg},{nonfiveg}",
        ATSSS_GOBACKN,
    ]
    p = subprocess.Popen(cmd)
    active_processes.append(p)

def _start_atsss_ane_selective_duplication(split_ratio: dict) -> None:
    # split_ratio looks like {"3GPP": 30, "N3GPP": 70}
    try:
        gpp = int(split_ratio.get("3GPP", 50))
        n3 = int(split_ratio.get("N3GPP", 50))
    except Exception:
        gpp, n3 = 50, 50

    # keep your old convention: 30 -> 3, 70 -> 7
    fiveg = gpp // max(RATIO_SCALE_DIV, 1)
    nonfiveg = n3 // max(RATIO_SCALE_DIV, 1)

    print("\n=== ATSSS START (ANE / selective-duplication) ===")
    print(f"Ports: {ATSSS_ANE_PORTS}")
    print(f"SD param: {ATSSS_SD_PARAM}")
    print("=================================================\n")

    cmd = [
        ATSSS_ANE_BIN,
        ATSSS_ANE_PORTS,
        ATSSS_CERT,
        ATSSS_KEY,
        ATSSS_SD_MODE_ID,
        ATSSS_SD_PARAM,
        ATSSS_GOBACKN,
    ]

    p = subprocess.Popen(cmd)
    active_processes.append(p)

def _start_atsss_ane_minRTT(split_ratio: dict) -> None:
    # split_ratio looks like {"3GPP": 30, "N3GPP": 70}
    try:
        gpp = int(split_ratio.get("3GPP", 50))
        n3 = int(split_ratio.get("N3GPP", 50))
    except Exception:
        gpp, n3 = 50, 50

    # keep your old convention: 30 -> 3, 70 -> 7
    fiveg = gpp // max(RATIO_SCALE_DIV, 1)
    nonfiveg = n3 // max(RATIO_SCALE_DIV, 1)

    print("\n=== ATSSS START (ANE / minRTT) ===")
    print(f"Ports: {ATSSS_ANE_PORTS}")
    print(f"SD param: {ATSSS_RTT_PARAM}")
    print("=================================================\n")

    cmd = [
        ATSSS_ANE_BIN,
        ATSSS_ANE_PORTS,
        ATSSS_CERT,
        ATSSS_KEY,
        ATSSS_RTT_MODE_ID,
        ATSSS_RTT_PARAM,
        ATSSS_GOBACKN,
    ]

    p = subprocess.Popen(cmd)
    active_processes.append(p)

# -----------------------------------------------------------------------------
# API
# -----------------------------------------------------------------------------
@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/ratio")
def update_ratio(req: RatioUpdate):
    global last_decision_time
    last_decision_time.unlock()
    
    global stored_ratio
    stored_ratio = req.ratio

    print(f"[ANE] Stored new ratio: {req.ratio}")
    return {"status": "stored", "ratio": req.ratio}


@app.get("/ratio")
def get_ratio():
    global last_decision_time
    timestamp = datetime.now(UTC).isoformat(timespec="milliseconds")
    timestamp = timestamp.replace("+00:00", "Z")
    # If nothing stored yet, return a default
    return {"ratio": stored_ratio if stored_ratio is not None else 50}

@app.get("/rl-timestamp")
def get_timestamp():
    global last_decision_time
    # if a ratio has not been requested yet, it returns 0
    return {"timestamp": last_decision_time.value if last_decision_time.value is not None else 0}


@app.post("/callback")
def atsss_rule_update(request: Request, data: dict):
    # -----------------------------
    # DELETE path: empty body + headers
    # -----------------------------
    if not data:  # covers None and {}
        event = request.headers.get("X-ATSSS-Event", "")
        if event != "deleted":
            raise HTTPException(status_code=400, detail="Empty payload without delete event header")

        session_id = request.headers.get("X-ATSSS-SessionID", "")
        if not session_id:
            raise HTTPException(status_code=400, detail="Missing X-ATSSS-SessionID header for delete")

        print("\n=== ATSSS DELETE RECEIVED (ANE) ===")
        print(f"Session: {session_id}")
        print("==================================\n")

        # cleanup local state + stop started processes
        active_sessions.pop(session_id, None)
        stop_all_processes()

        return {"status": "deleted", "sessionID": session_id}

    # -----------------------------
    # START/UPDATE path: normal JSON body
    # -----------------------------
    session_id = data.get("sessionID")
    if not session_id:
        raise HTTPException(status_code=400, detail="Missing sessionID in callback payload")

    active_sessions[session_id] = data
    steering_policy = data.get("steeringPolicy", {})
    method = steering_policy.get("method")

    # Stop previous started process(es) before starting a new one for same session
    # (optional, but avoids duplicates on repeated callbacks)
    stop_all_processes()

    if method == "load-balancing":
        split_ratio = steering_policy.get("splitRatio") or {}
        _start_atsss_ane_load_balancing(split_ratio)
        return {"status": "ok", "sessionID": session_id}

    if method == "selective-duplication":
        split_ratio = steering_policy.get("splitRatio") or {}
        _start_atsss_ane_selective_duplication(split_ratio)
        return {"status": "ok", "sessionID": session_id}

    if method == "minRTT":
        split_ratio = steering_policy.get("splitRatio") or {}
        _start_atsss_ane_minRTT(split_ratio)
        return {"status": "ok", "sessionID": session_id}


    raise HTTPException(status_code=400, detail=f"Unsupported steeringPolicy.method: {method}")


@app.on_event("startup")
def startup_event():
    subscribe_to_apf()
