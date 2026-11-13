import json
import os
import secrets
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Literal, Optional

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException, Query, Response
from pydantic import BaseModel, Field
from pymongo import DESCENDING, MongoClient
from pymongo.errors import DuplicateKeyError

load_dotenv()

MONGODB_URI = os.environ["MONGODB_URI"]
MQTT_NAME = os.environ["MQTT_NAME"]
BROKER_HOST = "135.232.203.144"
BROKER_PORT = 1883
SETTINGS_TOPIC = f"ecse3038/{MQTT_NAME}/fan/settings"

mongo = MongoClient(MONGODB_URI)
db = mongo["ecse3038_test2"]
fans = db["fans"]
readings = db["readings"]
settings = db["settings"]

fans.create_index("mac", unique=True)
fans.create_index("key", unique=True)
settings.create_index("mac", unique=True)
readings.create_index([("time", DESCENDING)])

mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)


@asynccontextmanager
async def lifespan(app: FastAPI):
    try:
        mqtt_client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
        mqtt_client.loop_start()
    except Exception as e:
        print("MQTT connect failed:", e)
    yield
    mqtt_client.loop_stop()
    mqtt_client.disconnect()


app = FastAPI(lifespan=lifespan)


# ---------- models ----------
class FanIn(BaseModel):
    mac: str
    room: str


class ReadingIn(BaseModel):
    mac: str
    humidity: float


class SettingsIn(BaseModel):
    mode: Literal["auto", "on", "off"]
    max_humidity: float = Field(ge=0, le=100)


# ---------- A1: fans ----------
@app.post("/fans", status_code=201)
def create_fan(fan: FanIn):
    key = secrets.token_urlsafe(16)
    try:
        fans.insert_one({"mac": fan.mac, "room": fan.room, "key": key})
    except DuplicateKeyError:
        raise HTTPException(status_code=409, detail="Fan already exists")
    return {"mac": fan.mac, "room": fan.room, "key": key}


@app.get("/fans")
def list_fans():
    return list(fans.find({}, {"_id": 0, "key": 0}))


@app.get("/fans/{mac}")
def get_fan(mac: str):
    fan = fans.find_one({"mac": mac}, {"_id": 0, "key": 0})
    if fan is None:
        raise HTTPException(status_code=404, detail="Fan not found")
    return fan


# ---------- A2: readings ----------
@app.post("/readings", status_code=201)
def create_reading(
    reading: ReadingIn,
    x_api_key: Optional[str] = Header(default=None),
):
    # 401: missing or never-issued key
    if not x_api_key:
        raise HTTPException(status_code=401, detail="Missing API key")
    owner = fans.find_one({"key": x_api_key})
    if owner is None:
        raise HTTPException(status_code=401, detail="Unknown API key")

    # 403: valid key, but for another MAC
    if owner["mac"] != reading.mac:
        raise HTTPException(status_code=403, detail="Key does not belong to this MAC")

    # 422: humidity out of range (checked after auth so 401/403 win)
    if reading.humidity < 0 or reading.humidity > 100:
        raise HTTPException(status_code=422, detail="humidity must be 0 to 100")

    doc = {
        "mac": reading.mac,
        "humidity": reading.humidity,
        "time": datetime.now(timezone.utc),
    }
    readings.insert_one(doc)
    return {
        "mac": doc["mac"],
        "humidity": doc["humidity"],
        "time": doc["time"].isoformat(),
    }


# ---------- A3: list readings ----------
@app.get("/readings")
def list_readings(
    mac: Optional[str] = None,
    limit: int = Query(default=10, ge=1, le=100),
):
    query = {"mac": mac} if mac else {}
    cursor = readings.find(query, {"_id": 0}).sort("time", DESCENDING).limit(limit)
    out = []
    for r in cursor:
        r["time"] = r["time"].isoformat()
        out.append(r)
    return out


# ---------- A4: settings ----------
@app.put("/fans/{mac}/settings")
def put_settings(mac: str, body: SettingsIn, response: Response):
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="Fan not found")

    result = settings.update_one(
        {"mac": mac},
        {"$set": {"mode": body.mode, "max_humidity": body.max_humidity}},
        upsert=True,
    )
    response.status_code = 201 if result.upserted_id is not None else 200

    payload = json.dumps({"mode": body.mode, "max_humidity": float(body.max_humidity)})
    mqtt_client.publish(SETTINGS_TOPIC, payload, qos=1, retain=True)

    return {"mac": mac, "mode": body.mode, "max_humidity": body.max_humidity}


# ---------- A5: delete ----------
@app.delete("/fans/{mac}", status_code=204)
def delete_fan(mac: str, x_api_key: Optional[str] = Header(default=None)):
    if not x_api_key:
        raise HTTPException(status_code=401, detail="Missing API key")
    owner = fans.find_one({"key": x_api_key})
    if owner is None:
        raise HTTPException(status_code=401, detail="Unknown API key")

    target = fans.find_one({"mac": mac})
    if target is None:
        raise HTTPException(status_code=404, detail="Fan not found")

    if owner["mac"] != mac:
        raise HTTPException(status_code=403, detail="Not this fan's key")

    fans.delete_one({"mac": mac})
    settings.delete_one({"mac": mac})
    return Response(status_code=204)