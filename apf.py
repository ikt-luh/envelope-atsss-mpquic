"""
apf.py

Implements APF-hosted endpoints:
  - POST   /atsss-t/sub
  - POST   /atsss-t/session
  - GET    /atsss-t/session/{sessionID}
  - DELETE /atsss-t/session/{sessionID}
  - POST   /atsss-cu/sub
  - POST   /atsss-cu/ratio/{aueID}   (aueID encoded as "ip:port")
    + also supports /atsss-cu/ratio/{ip}/{port} as a convenience alias
  - POST   /atsss-cp/sub

Notes:
  - aueID/aneID are always tuples (ip, port) in request bodies.
  - callback URLs are full URLs and may use different (callback) ports than aueID/aneID ports.
  - APF triggers callbacks using the stored callback URLs:
      * Vertical registry callback after /atsss-t/sub and on registry changes
      * ANE callback first (must succeed), then AUE callback (must succeed) after session acceptance
"""

from __future__ import annotations

import asyncio
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Literal, Optional
from uuid import uuid4

import httpx
from urllib.parse import urlparse
from fastapi import FastAPI, HTTPException, Path, status
from pydantic import BaseModel, ConfigDict, Field, HttpUrl, conint


# -----------------------------------------------------------------------------
# App
# -----------------------------------------------------------------------------
app = FastAPI(
    title="ATSSS APF (doc-aligned)",
    version="1.0.2",
    description="APF implementing ATSSS-T / ATSSS-Cu / ATSSS-Cp + callbacks.",
)


# -----------------------------------------------------------------------------
# Models (aligned with your YAML)
# -----------------------------------------------------------------------------

class SubAck(BaseModel):
    model_config = ConfigDict(extra="forbid")
    status: Literal["subscribed"] = "subscribed"

class EndpointId(BaseModel):
    model_config = ConfigDict(extra="forbid")
    ip: str
    port: int = Field(ge=1, le=65535)

    def key(self) -> str:
        return f"{self.ip}:{self.port}"


# ---------- /atsss-t/sub ----------
class AtsssTSubRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    serviceID: str = Field(..., description="Identifier of the service provided by the vertical")
    callback: HttpUrl = Field(..., description="Callback URL used to notify the vertical about registrations")


class AtsssTRegistryCallback(BaseModel):
    model_config = ConfigDict(extra="forbid")
    aueList: List[EndpointId]
    aneList: List[EndpointId]


# ---------- /atsss-t/session ----------
class FlowFilter(BaseModel):
    model_config = ConfigDict(extra="forbid")
    aueID: EndpointId = Field(..., description="Identifier of the AUE (e.g., IP:port of the UE)")
    aneID: EndpointId = Field(..., description="Identifier of the ANE (e.g., IP:port of the ANE)")
    protocol: str = Field(..., description="Underlying transport protocol (e.g., TCP, UDP)")


class SplitRatio(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)
    gpp3: conint(ge=0, le=100) = Field(..., alias="3GPP", description="Percentage sent over 3GPP")
    n3gpp: conint(ge=0, le=100) = Field(..., alias="N3GPP", description="Percentage sent over non-3GPP")

    def total(self) -> int:
        return int(self.gpp3) + int(self.n3gpp)

class SteeringPolicy(BaseModel):
    model_config = ConfigDict(extra="forbid")
    method: Literal["minRTT", "load-balancing", "selective-duplication"] = Field(..., description='Traffic steering method (e.g., "minRTT" or "load-balancing")')
    splitRatio: Optional[SplitRatio] = Field(..., description="Traffic distribution across accesses")


class AtsssTSessionCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    serviceID: str = Field(..., description="Unique identifier of the requesting vertical service")
    callbackUri: Optional[HttpUrl] = Field(
        None,
        description="Optional callback URI for async session updates; if provided, response can be sent asynchronously",
    )
    flowFilter: FlowFilter = Field(..., description="Traffic flow criteria")
    steeringPolicy: SteeringPolicy = Field(..., description="Defines the traffic steering policy for multi-connectivity")
    duration: conint(gt=1) = Field(..., description="Session duration in seconds")


class AtsssTSessionCreateResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")
    sessionID: str = Field(..., description="Unique identifier of the instantiated ATSSS session")
    serviceID: str = Field(..., description="Identifier of the requesting vertical service")
    status: Literal["accepted", "rejected", "deleted"] = Field(..., description="Status of the request/session, e.g., accepted, rejected, or deleted")

# ---------- /atsss-t/session/{sessionID} ----------

class AccessNetwork(BaseModel):
    model_config = ConfigDict(extra="forbid")
    accessId: str = Field(..., description="Unique identifier for the access network connection")
    accessType: conint(ge=0, le=255) = Field(
        ...,
        description="Numeric value (0..255) for access type (aligned with ETSI MEC 015 MtsCapabilityInfo)",
    )


class AtsssTSessionDetails(BaseModel):
    model_config = ConfigDict(extra="forbid")
    sessionID: str
    serviceID: str
    startTime: datetime
    status: Literal["active", "failed", "ongoing"]
    accessNetworks: List[AccessNetwork]
    steeringPolicy: SteeringPolicy = Field(..., description="Current steering policy for the session")
    duration: conint(gt=1) = Field(..., description="Session duration in seconds")


# ---- ATSSS-Cu ----
class AtsssCuSubRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    aueID: EndpointId
    serviceID: str
    callback: HttpUrl


class RatioUpdateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    ratio: int = Field(ge=0, le=100)


class AtsssCuCallbackPayload(BaseModel):
    model_config = ConfigDict(extra="forbid")
    sessionID: str
    serviceID: str
    startTime: datetime
    status: Literal["active", "failed", "ongoing"]
    steeringPolicy: SteeringPolicy
    duration: int
    aneID: EndpointId

# ---- ATSSS-Cp ----
class AtsssCpSubRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    aneID: EndpointId
    serviceID: str
    callback: HttpUrl


class AtsssCpCallbackPayload(BaseModel):
    model_config = ConfigDict(extra="forbid")
    sessionID: str
    serviceID: str
    startTime: datetime
    status: Literal["active", "failed", "ongoing"]
    steeringPolicy: SteeringPolicy
    duration: int
    aueID: EndpointId

class AtsssCpRatioResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")
    aueID: EndpointId
    sessionID: str
    method: Literal["minRTT", "load-balancing", "selective-duplication"]
    splitRatio: SplitRatio
    ratio3GPP: int = Field(ge=0, le=100, description="Percentage of traffic over 3GPP (same as splitRatio['3GPP'])")



# -----------------------------------------------------------------------------
# In-memory state
# -----------------------------------------------------------------------------
# serviceID -> { "ip:port" -> callbackUrl }
AUE_SUBS: Dict[str, Dict[str, str]] = {}
ANE_SUBS: Dict[str, Dict[str, str]] = {}

# serviceID -> [vertical callbackUrl]
VERTICAL_SUBS: Dict[str, List[str]] = {}

# Track which AUE/ANE are currently used by an active session
IN_SESSION_AUE: Dict[str, set[str]] = {}
IN_SESSION_ANE: Dict[str, set[str]] = {}

# Session record storage
class _SessionRecord(BaseModel):
    model_config = ConfigDict(extra="forbid")
    details: AtsssTSessionDetails
    flow: FlowFilter
    callbackUri: Optional[str] = None
    expiresAt: datetime


SESSIONS: Dict[str, _SessionRecord] = {}


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
def now_utc() -> datetime:
    return datetime.now(timezone.utc)


async def post_json(url: str, payload: Any, timeout_s: float = 3.0) -> None:
    """Fire-and-forget callback sender (best for registry updates and non-critical notifications)."""
    async with httpx.AsyncClient(timeout=timeout_s) as client:
        try:
            resp = await client.post(url, json=payload)
            resp.raise_for_status()
        except Exception as e:
            print(f"[APF] Callback POST failed to {url}: {e}")


async def post_json_checked(url: str, payload: Any, headers: dict | None = None, timeout_s: float = 15.0) -> bool:
    """
    Checked callback sender:
      - returns True only if HTTP response is 2xx
      - returns False on timeout/connection errors or non-2xx responses
    """
    async with httpx.AsyncClient(timeout=timeout_s) as client:
        try:
            resp = await client.post(url, json=payload, headers=headers)
            print(f"[APF] Callback POST to {url} returned status {resp.status_code}")
            resp.raise_for_status()
            return True
        except Exception as e:
            print(f"[APF] Callback POST failed to {url}: {e}")
            return False


def default_access_networks() -> List[AccessNetwork]:
    # Matches the example shown in the document (Wi-Fi 6 and 5G).
    return [
        AccessNetwork(accessId="wifi6", accessType=14),
        AccessNetwork(accessId="5g", accessType=33),
    ]


def ensure_split_ratio(policy: SteeringPolicy) -> SteeringPolicy:
    """If load-balancing without splitRatio, default to 50/50."""
    if policy.method == "load-balancing" and policy.splitRatio is None:
        policy.splitRatio = SplitRatio(**{"3GPP": 50, "N3GPP": 50})
    return policy


def registry_snapshot(service_id: str) -> AtsssTRegistryCallback:
    """Only entities registered for this service that are NOT part of a session yet."""
    aues = AUE_SUBS.get(service_id, {})
    anes = ANE_SUBS.get(service_id, {})
    in_aue = IN_SESSION_AUE.get(service_id, set())
    in_ane = IN_SESSION_ANE.get(service_id, set())

    aue_list: List[EndpointId] = []
    for k in aues.keys():
        if k in in_aue:
            continue
        ip, port_s = k.rsplit(":", 1)
        aue_list.append(EndpointId(ip=ip, port=int(port_s)))

    ane_list: List[EndpointId] = []
    for k in anes.keys():
        if k in in_ane:
            continue
        ip, port_s = k.rsplit(":", 1)
        ane_list.append(EndpointId(ip=ip, port=int(port_s)))

    return AtsssTRegistryCallback(aueList=aue_list, aneList=ane_list)


def notify_vertical_registry(service_id: str) -> None:
    """Push registry snapshot to all subscribed vertical callbacks for a service."""
    callbacks = VERTICAL_SUBS.get(service_id, [])
    if not callbacks:
        return
    payload = registry_snapshot(service_id).model_dump()
    for cb in callbacks:
        asyncio.create_task(post_json(cb, payload))


async def expire_sessions_loop() -> None:
    """Auto-expire sessions after a fixed time (duration)."""
    while True:
        try:
            now = now_utc()
            expired = [sid for sid, rec in SESSIONS.items() if rec.expiresAt <= now]
            for sid in expired:
                await _delete_session_internal(sid, expired=True)
        except Exception as e:
            print(f"[APF] expiry loop error: {e}")
        await asyncio.sleep(1.0)


@app.on_event("startup")
async def _startup() -> None:
    asyncio.create_task(expire_sessions_loop())


async def _delete_session_internal(session_id: str, expired: bool = False) -> None:
    """Free AUE/ANE, remove session, notify vertical registry."""
    rec = SESSIONS.get(session_id)
    if not rec:
        return

    service_id = rec.details.serviceID
    aue_key = rec.flow.aueID.key()
    ane_key = rec.flow.aneID.key()

    # Free endpoints
    IN_SESSION_AUE.setdefault(service_id, set()).discard(aue_key)
    IN_SESSION_ANE.setdefault(service_id, set()).discard(ane_key)

    # Remove session
    del SESSIONS[session_id]

    # Notify vertical that endpoints are available again
    notify_vertical_registry(service_id)


# -----------------------------------------------------------------------------
# ATSSS-T endpoints (CAM App Server/Vertical -> APF)
# -----------------------------------------------------------------------------
@app.post("/atsss-t/sub", response_model=SubAck, status_code=201)
async def atsss_t_sub(req: AtsssTSubRequest) -> SubAck:
    # Sender: CAM App Server (Vertical) -> Receiver: APF
    VERTICAL_SUBS.setdefault(req.serviceID, [])
    VERTICAL_SUBS[req.serviceID].append(str(req.callback))

    # Send immediate snapshot (useful; doc says "upon changes", but immediate snapshot is typical)
    asyncio.create_task(post_json(str(req.callback), registry_snapshot(req.serviceID).model_dump()))
    return SubAck()


@app.post("/atsss-t/session", response_model=AtsssTSessionCreateResponse, status_code=201)
async def atsss_t_create_session(req: AtsssTSessionCreateRequest) -> AtsssTSessionCreateResponse:
    # Sender: CAM App Server (Vertical) -> Receiver: APF

    # Basic validation consistent with the prompt: split ratio should sum to 100
    sr = req.steeringPolicy.splitRatio
    if sr.total() != 100:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail='splitRatio must sum to 100 across "3GPP" and "N3GPP".',
        )

    service_id = req.serviceID
    session_id = f"sess-{uuid4().hex[:12]}"

    aue_key = req.flowFilter.aueID.key()
    ane_key = req.flowFilter.aneID.key()

    # Feasibility: must be registered + not already in a session
    aue_registered = aue_key in AUE_SUBS.get(service_id, {})
    ane_registered = ane_key in ANE_SUBS.get(service_id, {})
    aue_free = aue_key not in IN_SESSION_AUE.get(service_id, set())
    ane_free = ane_key not in IN_SESSION_ANE.get(service_id, set())

    feasible = aue_registered and ane_registered and aue_free and ane_free
    if not feasible:
        return AtsssTSessionCreateResponse(sessionID=session_id, serviceID=service_id, status="rejected")

    policy = ensure_split_ratio(req.steeringPolicy)
    start = now_utc()

    details = AtsssTSessionDetails(
        sessionID=session_id,
        serviceID=service_id,
        startTime=start,
        status="active",
        accessNetworks=default_access_networks(),
        steeringPolicy=policy,
        duration=req.duration,
    )

    expires_at = start + timedelta(seconds=req.duration)

    # Create session record first (so GET works immediately), but we will ROLLBACK if callbacks fail.
    SESSIONS[session_id] = _SessionRecord(
        details=details,
        flow=req.flowFilter,
        callbackUri=str(req.callbackUri) if req.callbackUri else None,
        expiresAt=expires_at,
    )

    # Mark in-session so they disappear from registry lists
    IN_SESSION_AUE.setdefault(service_id, set()).add(aue_key)
    IN_SESSION_ANE.setdefault(service_id, set()).add(ane_key)

    # Notify vertical registry subscribers about change (AUE/ANE now "busy")
    notify_vertical_registry(service_id)

    # Callback URLs
    aue_cb = AUE_SUBS[service_id][aue_key]
    ane_cb = ANE_SUBS[service_id][ane_key]

    # Build callback payloads
    ane_payload = AtsssCpCallbackPayload(
        sessionID=session_id,
        serviceID=service_id,
        startTime=start,
        status="active",
        steeringPolicy=policy,
        duration=req.duration,
        aueID=req.flowFilter.aueID,
    ).model_dump(mode="json", by_alias=True)


    aue_payload = AtsssCuCallbackPayload(
        sessionID=session_id,
        serviceID=service_id,
        startTime=start,
        status="active",
        steeringPolicy=policy,
        duration=req.duration,
        aneID=req.flowFilter.aneID,
    ).model_dump(mode="json", by_alias=True)


    # -----------------------------
    # NEW BEHAVIOR:
    #  1) POST ANE callback and require success
    #  2) POST AUE callback and require success
    #  If either fails -> rollback + reject
    # -----------------------------
    ane_ok = await post_json_checked(ane_cb, ane_payload)
    if not ane_ok:
        await _delete_session_internal(session_id, expired=False)
        return AtsssTSessionCreateResponse(sessionID=session_id, serviceID=service_id, status="rejected")

    aue_ok = await post_json_checked(aue_cb, aue_payload)
    if not aue_ok:
        await _delete_session_internal(session_id, expired=False)
        return AtsssTSessionCreateResponse(sessionID=session_id, serviceID=service_id, status="rejected")

    # Optional: callbackUri sends payload identical to GET session details
    if req.callbackUri:
        asyncio.create_task(post_json(str(req.callbackUri), details.model_dump(mode="json", by_alias=True)))


    return AtsssTSessionCreateResponse(sessionID=session_id, serviceID=service_id, status="accepted")


@app.get("/atsss-t/session/{sessionID}", response_model=AtsssTSessionDetails)
async def atsss_t_get_session(sessionID: str) -> AtsssTSessionDetails:
    # Sender: CAM App Server (Vertical) -> Receiver: APF
    rec = SESSIONS.get(sessionID)
    if not rec:
        raise HTTPException(status_code=404, detail="Session not found")
    return rec.details


@app.delete("/atsss-t/session/{sessionID}", response_model=AtsssTSessionCreateResponse)
async def atsss_t_delete_session(sessionID: str) -> AtsssTSessionCreateResponse:
    rec = SESSIONS.get(sessionID)
    if not rec:
        raise HTTPException(status_code=404, detail="Session not found")

    service_id = rec.details.serviceID
    aue_key = rec.flow.aueID.key()
    ane_key = rec.flow.aneID.key()   # <--- ADD THIS

    aue_cb = AUE_SUBS.get(service_id, {}).get(aue_key)
    if not aue_cb:
        raise HTTPException(status_code=409, detail="AUE callback not registered for this session")

    ane_cb = ANE_SUBS.get(service_id, {}).get(ane_key)  # <--- ADD THIS

    headers = {
        "X-ATSSS-Event": "deleted",
        "X-ATSSS-SessionID": sessionID,
        "X-ATSSS-ServiceID": service_id,
    }

    # AUE is required
    aue_ok = await post_json_checked(aue_cb, payload={}, headers=headers)
    if not aue_ok:
        raise HTTPException(status_code=502, detail="Failed to notify AUE of session deletion")

    # ANE is best-effort
    if ane_cb:
        ane_ok = await post_json_checked(ane_cb, payload={}, headers=headers)
        if not ane_ok:
            print(f"[APF] WARN: Failed to notify ANE of session deletion for {sessionID} ({ane_cb})")
    else:
        print(f"[APF] WARN: ANE callback not registered for this session: service={service_id} ane={ane_key}")

    await _delete_session_internal(sessionID, expired=False)
    return AtsssTSessionCreateResponse(sessionID=sessionID, serviceID=service_id, status="deleted")



# -----------------------------------------------------------------------------
# ATSSS-Cu endpoints (AUE -> APF)
# -----------------------------------------------------------------------------
@app.post("/atsss-cu/sub", response_model=SubAck, status_code=201)
async def atsss_cu_sub(req: AtsssCuSubRequest) -> SubAck:
    # Sender: AUE -> Receiver: APF
    AUE_SUBS.setdefault(req.serviceID, {})
    AUE_SUBS[req.serviceID][req.aueID.key()] = str(req.callback)

    # Registry changed -> notify vertical subscribers
    notify_vertical_registry(req.serviceID)
    return SubAck()


def _parse_aue_id_from_str(aue_id: str) -> EndpointId:
    """
    Parse aueID from "ip:port" string used in /atsss-cu/ratio/{aueID}.
    (Your YAML uses EndpointId schema for path, but HTTP paths are strings—this is the practical encoding.)
    """
    try:
        ip, port_s = aue_id.rsplit(":", 1)
        return EndpointId(ip=ip, port=int(port_s))
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid aueID format (expected ip:port): {e}")


@app.post("/atsss-cu/ratio/{aueID}")
async def atsss_cu_ratio_encoded(
    aueID: str = Path(..., description='Encoded AUE id as "ip:port" (e.g., "10.0.0.5:12345")'),
    body: RatioUpdateRequest = ...,
) -> Dict[str, str]:
    # Sender: AUE -> Receiver: APF
    aue = _parse_aue_id_from_str(aueID)
    return await _apply_ratio_update(aue, body)


@app.post("/atsss-cu/ratio/{ip}/{port}")
async def atsss_cu_ratio_split(
    ip: str,
    port: int = Path(..., ge=1, le=65535),
    body: RatioUpdateRequest = ...,
) -> Dict[str, str]:
    # Convenience alias (not in YAML): /atsss-cu/ratio/{ip}/{port}
    aue = EndpointId(ip=ip, port=port)
    return await _apply_ratio_update(aue, body)


async def _apply_ratio_update(aue: EndpointId, body: RatioUpdateRequest) -> Dict[str, str]:
    """
    Apply ratio update to active load-balancing sessions for this AUE.
    Then forward updated rule to ANE via its callback URL (described in your document).
    """
    updated_any = False
    aue_key = aue.key()

    # Find sessions where this AUE participates and policy is load-balancing
    for sid, rec in list(SESSIONS.items()):
        if rec.flow.aueID.key() != aue_key:
            continue
        if rec.details.steeringPolicy.method != "load-balancing":
            continue

        rec.details.steeringPolicy.splitRatio = SplitRatio(**{"3GPP": body.ratio, "N3GPP": 100 - body.ratio})
        SESSIONS[sid] = rec
        updated_any = True

        service_id = rec.details.serviceID
        ane_key = rec.flow.aneID.key()

        payload = {"ratio": body.ratio}
        ane_cb = ANE_SUBS.get(service_id, {}).get(ane_key)
        parsed = urlparse(ane_cb)
        ane_ip_port = parsed.netloc
        asyncio.create_task(post_json(f"http://{ane_ip_port}/ratio", payload))
        # If vertical provided callbackUri, send GET-identical session details
        if rec.callbackUri:
            asyncio.create_task(post_json(rec.callbackUri, rec.details.model_dump(by_alias=True)))

    if not updated_any:
        return {"status": "ok", "message": "no active load-balancing session for this AUE"}

    return {"status": "ok"}


# -----------------------------------------------------------------------------
# ATSSS-Cp endpoints (ANE -> APF)
# -----------------------------------------------------------------------------
@app.post("/atsss-cp/sub", response_model=SubAck, status_code=201)
async def atsss_cp_sub(req: AtsssCpSubRequest) -> SubAck:
    # Sender: ANE -> Receiver: APF
    ANE_SUBS.setdefault(req.serviceID, {})
    ANE_SUBS[req.serviceID][req.aneID.key()] = str(req.callback)

    # Registry changed -> notify vertical subscribers
    notify_vertical_registry(req.serviceID)
    return SubAck()

@app.get("/atsss-cp/ratio/{aueID}", response_model=AtsssCpRatioResponse)
async def atsss_cp_get_ratio(
    aueID: str = Path(..., description='Encoded AUE id as "ip:port" (e.g., "10.0.0.5:12345")')
) -> AtsssCpRatioResponse:
    # Sender: ANE -> Receiver: APF
    aue = _parse_aue_id_from_str(aueID)
    aue_key = aue.key()

    # Find the active session for this AUE (in this simplified model, at most one should exist)
    for rec in SESSIONS.values():
        if rec.flow.aueID.key() != aue_key:
            continue
        if rec.details.status != "active":
            continue

        policy = rec.details.steeringPolicy
        if policy.method != "load-balancing" or policy.splitRatio is None:
            raise HTTPException(
                status_code=409,
                detail="No ratio available: session is not in load-balancing mode.",
            )

        return AtsssCpRatioResponse(
            aueID=rec.flow.aueID,
            sessionID=rec.details.sessionID,
            method=policy.method,
            splitRatio=policy.splitRatio,
            ratio3GPP=policy.splitRatio.gpp3,
        )

    raise HTTPException(status_code=404, detail="No active session found for this AUE.")


@app.get("/debug/state")
def debug_state():
    return {
        "AUE_SUBS": AUE_SUBS,
        "ANE_SUBS": ANE_SUBS,
        "IN_SESSION_AUE": {k: list(v) for k, v in IN_SESSION_AUE.items()},
        "IN_SESSION_ANE": {k: list(v) for k, v in IN_SESSION_ANE.items()},
        "SESSIONS": {sid: {
            "serviceID": rec.details.serviceID,
            "aue": rec.flowFilter.aueID.key(),
            "ane": rec.flowFilter.aneID.key(),
            "expiresAt": rec.expiresAt.isoformat(),
            "method": rec.details.steeringPolicy.method
        } for sid, rec in SESSIONS.items()},
    }

# -----------------------------------------------------------------------------
# Run:
#   pip install fastapi uvicorn httpx "pydantic>=2" requests
#   uvicorn apf:app --host 0.0.0.0 --port 8081
# -----------------------------------------------------------------------------
