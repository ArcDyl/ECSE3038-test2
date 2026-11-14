# ECSE3038 Test 2: The bathroom fan

An IoT system that runs a bathroom extractor fan when the air gets too damp.

- **Board:** an ESP32 (simulated in Wokwi) with a DHT22 humidity sensor and an LED that stands for the fan. It POSTs a humidity reading to the API every 10 seconds.
- **API:** a FastAPI app backed by MongoDB Atlas. It stores fans and readings, and lets someone set a fan's mode and humidity limit.
- **MQTT:** when settings are changed on the API, it publishes them (retained) to the class broker. The board subscribes, drives the LED, and reports its state back.

```
ESP32 (Wokwi) --POST /readings + X-API-Key--> API --> MongoDB Atlas
ESP32 <------- ecse3038/<name>/fan/settings --- API (PUT /fans/{mac}/settings)
ESP32 --> ecse3038/<name>/fan/state, ecse3038/<name>/status
```

## Repository layout

```
api/     FastAPI app (app.py, requirements.txt)
board/   PlatformIO project for the ESP32 (src/main.cpp, diagram.json, wokwi.toml)
```

## API endpoints

| Route | Purpose |
|---|---|
| `POST /fans` | Register a fan `{"mac", "room"}`. Returns a new API key (201). 409 if the MAC exists. |
| `GET /fans`, `GET /fans/{mac}` | List fans / get one (keys are never shown). |
| `POST /readings` | Add a reading `{"mac", "humidity"}` with the `X-API-Key` header. |
| `GET /readings` | Newest first. Optional `?mac=` and `?limit=` (1 to 100, default 10). |
| `PUT /fans/{mac}/settings` | Set `{"mode": "auto"\|"on"\|"off", "max_humidity": 0-100}` and publish to MQTT. |
| `DELETE /fans/{mac}` | Remove a fan. Needs that fan's own key in `X-API-Key`. |

## How to run the API

Requires Python 3 and a MongoDB Atlas cluster.

```bash
git clone https://github.com/<your-username>/ECSE3038-test2.git
cd ECSE3038-test2/api
python3 -m venv venv
source venv/bin/activate          # Windows: venv\Scripts\activate
python -m pip install -r requirements.txt
```

Create `api/.env` (this file is never committed):

```
MONGODB_URI=mongodb+srv://<user>:<password>@<cluster-host>/?retryWrites=true&w=majority
MQTT_NAME=<name>
```

`MQTT_NAME` is lowercase letters and digits and sets the topic prefix `ecse3038/<name>/...`.

Start the server:

```bash
uvicorn app:app --host 127.0.0.1 --port 8000
```

Interactive docs are at `/docs`.

## How to run the board

Requires VS Code with the PlatformIO and Wokwi extensions.

1. Open the `ECSE3038-test2` folder in VS Code (so the `board` project is loaded).
2. In `board/src/main.cpp`, set `MQTT_NAME` (the same as in `api/.env`) and `API_URL` (your API's `/readings` address).
3. Create `board/include/secrets.h` (never committed) containing:
   ```cpp
   #define DEVICE_KEY ""
   ```
4. Build the project with PlatformIO, then start the Wokwi simulation.
5. Copy the MAC address from the Serial Monitor. The first POST returns 401, which is expected.
6. Register the board with `POST /fans` using that MAC and a room name. Copy the `key` from the response.
7. Put the key between the quotes in `board/include/secrets.h`, then build and start Wokwi again.

### Trying it out

The DHT22 starts at 55% humidity. Set the fan's behaviour with:

```bash
curl -X PUT http://<api-host>/fans/<MAC>/settings \
  -H "Content-Type: application/json" \
  -d '{"mode": "auto", "max_humidity": 65}'
```

Then click the sensor in Wokwi and drag the humidity above 65. The LED turns on, and `on` is published on `ecse3038/<name>/fan/state`. Watch all topics by subscribing to `ecse3038/<name>/#` on the class broker.

## MQTT topics

| Topic | Written by | Retained | Carries |
|---|---|---|---|
| `ecse3038/<name>/fan/settings` | API | yes | `{"mode": "auto", "max_humidity": 65.0}` |
| `ecse3038/<name>/fan/state` | board | yes | `on` or `off` |
| `ecse3038/<name>/status` | board and broker (last will) | yes | `online` or `offline` |

## Secrets

`.env`, `board/include/secrets.h`, `.pio/` and `venv/` are listed in `.gitignore` and are never committed.