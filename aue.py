import os
import time
import subprocess
from typing import Dict, Optional

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

APF_BASE_URL = os.getenv("APF_BASE_URL", "http://192.168.108.187:8100").rstrip("/")
AUE_ID = {
    "ip": os.getenv("AUE_IP", "192.168.108.184"),
    "port": 12345
}
SERVICE_ID = os.getenv("SERVICE_ID", "ATSSS")
CALLBACK_URL = os.getenv("AUE_CALLBACK_URL", "http://192.168.108.184:8101/callback")
AUE_ID_EXTRACTED = f"{AUE_ID['ip']}%3A{AUE_ID['port']}"


AUE_FIFO_OUTPUT = os.getenv("AUE_FIFO_OUTPUT", "/tmp/fifo_output")
AUE_FIFO_INPUT = os.getenv("AUE_FIFO_INPUT", "/tmp/fifo_input")

# N3IWUE trigger (host-local service)
TRIGGER_URL = os.getenv("TRIGGER_URL", "http://127.0.0.1:8999/start-n3iwue")
DELETE_URL = os.getenv("DELETE_URL", "http://127.0.0.1:8999/stop-n3iwue")
CONFIG_URL = os.getenv("CONFIG_URL", "http://127.0.0.1:8999/configure-gre")

# GRE polling
GRE_IFACE = os.getenv("GRE_IFACE", "gretun-id-2-1")
GRE_IP_PREFIX = os.getenv("GRE_IP_PREFIX", "10.45.0.")
GRE_WAIT_TIMEOUT_S = float(os.getenv("GRE_WAIT_TIMEOUT_S", "15"))
GRE_WAIT_INTERVAL_S = float(os.getenv("GRE_WAIT_INTERVAL_S", "1.0"))

# Behavior toggles
SKIP_N3IWUE = os.getenv("SKIP_N3IWUE", "0") == "1"
STRICT_N3IWUE = os.getenv("STRICT_N3IWUE", "0") == "1"  # if True, fail callback on trigger/GRE issues

# UE process command config
ATSSS_UE_BIN = os.getenv("ATSSS_UE_BIN", "./atsss_project/build/atsss_ue")
ATSSS_SERVER_LIST = os.getenv("ATSSS_SERVER_LIST", "")          # e.g. "192.168.108.187:4443,192.168.108.187:4444"
ATSSS_DEST_APP_SERVER = os.getenv("ATSSS_DEST_APP_SERVER", "")  # e.g. "192.168.108.186:4445"
ATSSS_IFACES = os.getenv("ATSSS_IFACES", "wwan0,gretun-id-2-1")  # e.g. "wwan0,gretun-id-2-1"

ATSSS_RTT_MODE_ID = os.getenv("ATSSS_RTT_MODE_ID", "1")
ATSSS_LB_MODE_ID = os.getenv("ATSSS_LB_MODE_ID", "2")
ATSSS_SD_MODE_ID = os.getenv("ATSSS_SD_MODE_ID", "3")

ATSSS_NUM_ITER = os.getenv("ATSSS_NUM_ITER", "1000")
ATSSS_GOBACKN = os.getenv("ATSSS_GOBACKN", "0")
ATSSS_INTERVAL_US = os.getenv("ATSSS_INTERVAL_US", "100000")
ATSSS_SD_PARAM = os.getenv("ATSSS_SD_PARAM", "100000")
ATSSS_RTT_PARAM = os.getenv("ATSSS_RTT_PARAM", "100000")

STOP_OLD_ON_NEW_SESSION = os.getenv("STOP_OLD_ON_NEW_SESSION", "1") == "1"
RATIO_SCALE_DIV = int(os.getenv("RATIO_SCALE_DIV", "10"))



# -----------------------------------------------------------------------------
# Globals
# -----------------------------------------------------------------------------
stored_ratio: Optional[int] = None
last_decision_time = locker.LockedValue(None)

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def ensure_fifo(path):
    if not os.path.exists(path):
        os.mkfifo(path)

def gre_has_ip(ifname: str, ip_prefix: str) -> bool:
    res = subprocess.run(
        ["ip", "-o", "-4", "addr", "show", "dev", ifname],
        capture_output=True,
        text=True,
    )
    return res.returncode == 0 and ip_prefix in res.stdout


def aue_id_encoded() -> str:
    # aueID in APF ratio endpoint is encoded as "ip:port" with ':' URL-escaped as %3A
    return f"{AUE_ID['ip']}%3A{AUE_ID['port']}"


def build_ue_cmd(mode_id: str, mode_param: str) -> list[str]:
    if not ATSSS_SERVER_LIST or not ATSSS_DEST_APP_SERVER:
        raise RuntimeError(
            "Missing ATSSS_SERVER_LIST or ATSSS_DEST_APP_SERVER env. "
            "Set them in .env to remove hardcoding."
        )

    # atsss_ue usage (based on your earlier line):
    # atsss_ue <server_list> <destination app server> <ifaces> <mode> <mode params> <num iter> <gobackn> <interval> <fifo_out> <fifo_in>
    return [
        ATSSS_UE_BIN,
        ATSSS_SERVER_LIST,
        ATSSS_DEST_APP_SERVER,
        ATSSS_IFACES,
        str(mode_id),
        str(mode_param),
        ATSSS_NUM_ITER,
        ATSSS_GOBACKN,
        ATSSS_INTERVAL_US,
        AUE_FIFO_OUTPUT,
        AUE_FIFO_INPUT,
    ]

active_sessions: Dict[str, dict] = {}
active_processes: list[subprocess.Popen] = []


app = FastAPI(title="AUE (ATSSS-Cu client)")


class RatioUpdate(BaseModel):
    ratio: int


def subscribe_to_apf() -> None:
    payload = {
        "aueID": AUE_ID,
        "serviceID": SERVICE_ID,
        "callback": CALLBACK_URL,
    }
    r = requests.post(f"{APF_BASE_URL}/atsss-cu/sub", json=payload, timeout=5.0)
    r.raise_for_status()
    print("[AUE] Subscribed to APF")


def stop_all_processes() -> None:
    for p in active_processes:
        try:
            p.terminate()
            p.wait(timeout=5)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass
    active_processes.clear()

def maybe_trigger_n3iwue_and_wait_gre() -> dict:
    """
    Returns a dict with diagnostic info. Does NOT raise unless STRICT_N3IWUE=1.
    """
    info = {"triggered": False, "gre_ready": False, "gre_iface": GRE_IFACE}

    if SKIP_N3IWUE:
        print("[AUE] SKIP_N3IWUE=1: skipping trigger + GRE wait")
        info["skipped"] = True
        return info

    # Trigger
    try:
        r = requests.post(TRIGGER_URL, timeout=2.0)
        r.raise_for_status()
        info["triggered"] = True
    except Exception as e:
        msg = f"[AUE] n3iwue trigger failed: {e!r}"
        print(msg)
        if STRICT_N3IWUE:
            raise HTTPException(status_code=502, detail=msg)
        info["error"] = msg
        return info
    
    #Wait for GRE IP
    deadline = time.monotonic() + GRE_WAIT_TIMEOUT_S
    while time.monotonic() < deadline:
        if gre_has_ip(GRE_IFACE, GRE_IP_PREFIX):
            info["gre_ready"] = True
            return info
        time.sleep(GRE_WAIT_INTERVAL_S)

    msg = f"[AUE] GRE not ready within {GRE_WAIT_TIMEOUT_S}s on {GRE_IFACE} (prefix {GRE_IP_PREFIX})"
    print(msg)
    if STRICT_N3IWUE:
        raise HTTPException(status_code=502, detail=msg)
    info["error"] = msg
    return info


@app.post("/callback")
def atsss_rule_update(request: Request, data: dict):
    # 1) Empty body => delete/control path
    if not data:
        event = request.headers.get("X-ATSSS-Event", "")
        if event != "deleted":
            raise HTTPException(status_code=400, detail="Empty payload without delete event header")

        session_id = request.headers.get("X-ATSSS-SessionID", "")
        if not session_id:
            raise HTTPException(status_code=400, detail="Missing X-ATSSS-SessionID header")

        print("\n=== ATSSS DELETE RECEIVED (EMPTY BODY) ===")
        print(f"Session: {session_id}")
        print("========================================\n")

        active_sessions.pop(session_id, None)
        if STOP_OLD_ON_NEW_SESSION:
            stop_all_processes()

        # Optional: stop n3iwue
        if not SKIP_N3IWUE:
            try:
                requests.post(DELETE_URL, timeout=2.0)
            except Exception as e:
                print(f"[AUE] stop n3iwue failed (ignored): {e!r}")

        return {"status": "deleted", "sessionID": session_id}

    session_id = data.get("sessionID", "")
    steering_policy = data.get("steeringPolicy", {}) or {}
    method = steering_policy.get("method", "")

    if not session_id:
        raise HTTPException(status_code=400, detail="Missing sessionID in callback payload")

    if STOP_OLD_ON_NEW_SESSION:
        stop_all_processes()

    diag = maybe_trigger_n3iwue_and_wait_gre()

    print("\n=== ATSSS START ===")
    print(f"Session: {session_id}")
    print(f"ANE ID: {data.get('aneID')}")
    print(f"Steering policy: {steering_policy}")
    print(f"diag: {diag}")
    print("====================\n")

    if(steering_policy['method'] == 'load-balancing'):
        split_ratio = steering_policy['splitRatio']
        fiveg = split_ratio.get('3GPP')//10
        nonfiveg = split_ratio.get('N3GPP')//10
    try:
        if method == "load-balancing":
            sr = steering_policy.get("splitRatio", {}) or {}
            # Your old behavior: convert 0..100 to 0..10 shares by //10
            fiveg = int(sr.get("3GPP", 50)) // max(RATIO_SCALE_DIV, 1)
            nonfiveg = int(sr.get("N3GPP", 50)) // max(RATIO_SCALE_DIV, 1)
            mode_param = f"{fiveg},{nonfiveg}"
            cmd = build_ue_cmd(ATSSS_LB_MODE_ID, mode_param)

        elif method == "selective-duplication":
             cmd = build_ue_cmd(ATSSS_SD_MODE_ID, ATSSS_SD_PARAM)

        elif method == "minRTT":
             cmd = build_ue_cmd(ATSSS_RTT_MODE_ID, ATSSS_RTT_PARAM)

        else:
            raise HTTPException(status_code=400, detail=f"Unsupported steering method: {method}")
        p = subprocess.Popen(cmd)
        active_processes.append(p)
        active_sessions[session_id] = data
        print(f"[AUE] Started UE process: {' '.join(cmd)}")
        return {"status": "active", "sessionID": session_id, "method": method, "diag": diag}

    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Failed to start UE process: {e!r}")

@app.post("/ratio")
def update_split_ratio(req: RatioUpdate):

    global last_decision_time
    last_decision_time.unlock()

    global stored_ratio
    stored_ratio = req.ratio
    print(f"[AUE] Stored new ratio: {req.ratio}")

    r = requests.post(
        f"{APF_BASE_URL}/atsss-cu/ratio/{AUE_ID_EXTRACTED}",
        json={"ratio": req.ratio},
    )
    r.raise_for_status()

    print(f"Requested new split ratio: {req.ratio}% 3GPP")
    return {"status": "requested"}


@app.get("/ratio")
def get_ratio():
    global last_decision_time
    timestamp = datetime.now(UTC).isoformat(timespec="milliseconds")
    timestamp = timestamp.replace("+00:00", "Z")
    last_decision_time.value = timestamp
    # If nothing stored yet, return a default
    return {"ratio": stored_ratio if stored_ratio is not None else 50}

@app.get("/rl-timestamp")
def get_timestamp():
    global last_decision_time
    # if a ratio has not been requested yet, it returns 0
    return {"timestamp": last_decision_time.value if last_decision_time.value is not None else "NOT_SET"}


@app.on_event("startup")
def startup_event():
    ensure_fifo(AUE_FIFO_OUTPUT)
    ensure_fifo(AUE_FIFO_INPUT)
    subscribe_to_apf()
