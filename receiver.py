from fastapi import FastAPI, Body
from typing import Any, Dict, List

app = FastAPI(title="Callback Receiver")
MAILBOX: List[Dict[str, Any]] = []

@app.post("/{path:path}")
async def catch_all(path: str, body: Any = Body(...)):
    MAILBOX.append({"path": "/" + path, "body": body})
    return {"status": "accepted"}

@app.get("/_mailbox")
async def mailbox():
    return MAILBOX

@app.delete("/_mailbox")
async def clear_mailbox():
    MAILBOX.clear()
    return {"status": "cleared"}
