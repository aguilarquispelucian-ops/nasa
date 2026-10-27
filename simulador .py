import time
import random
import urllib.request
import json

SUPABASE_URL = "https://ntbrfedcrblamjfujryf.supabase.co/rest/v1/ground_truth"
API_KEY = "sb_publishable_AM2Rw2pH3qJjRyW9oZw1Nw_BD9xsYF_"

NODES = [
    {"node_id": "NODE-PER-01", "aoi_id": "per", "moist": (75.0, 96.0), "vib": (0.005, 0.020), "regime": "LAMINA_AGUA_ARROZAL"},
    {"node_id": "NODE-VEN-01", "aoi_id": "ven", "moist": (38.0, 62.0), "vib": (0.180, 0.550), "regime": "DEFORMACION_SISMICA"},
    {"node_id": "NODE-CTL-01", "aoi_id": "ctl", "moist": (5.0, 9.5),   "vib": (0.001, 0.004), "regime": "HIPERARIDO_ESTABLE"}
]

print("==========================================================")
print(" SIMULADOR IOT TERRAPASS -> SUPABASE REALTIME EN VIVO")
print("==========================================================")

while True:
    node = random.choice(NODES)
    moist = round(random.uniform(*node["moist"]), 2)
    eps = round(4.0 + moist * 0.34, 1)
    vib = round(random.uniform(*node["vib"]), 3)
    water = round(max(0.0, (moist - 70.0) * 1.1), 1)

    payload = {
        "node_id": node["node_id"],
        "aoi_id": node["aoi_id"],
        "soil_moisture_pct": moist,
        "water_level_cm": water,
        "dielectric_eps": eps,
        "vibration_g": vib,
        "local_regime": node["regime"]
    }

    req = urllib.request.Request(
        SUPABASE_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "apikey": API_KEY,
            "Authorization": f"Bearer {API_KEY}",
            "Content-Type": "application/json",
            "Prefer": "return=minimal"
        },
        method="POST"
    )

    try:
        with urllib.request.urlopen(req) as resp:
            print(f"[{time.strftime('%H:%M:%S')}] OK ({resp.status}) -> {node['node_id']} ({node['aoi_id']}) | Humedad: {moist}% | Eps: {eps} | Vib: {vib}g")
    except Exception as e:
        print(f"[ERROR] {e}")

    time.sleep(5)