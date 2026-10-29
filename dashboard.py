# Dashboard SafeGas - Google Colab
# !pip install paho-mqtt plotly pandas
import json, time, ssl
import paho.mqtt.client as mqtt
import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots

HOST = "GANTI-DENGAN-HOST.s1.eu.hivemq.cloud"
PORT = 8883
USER = "GANTI_USER_DASHBOARD"
PASS = "GANTI_PASSWORD_DASHBOARD"
DEVICE = "safegas01"
DURATION_S = 60          # lama pengambilan data

rows, events = [], []

def on_connect(c, u, f, rc, props=None):
    print("Connected, rc =", rc)
    c.subscribe(f"safegas/{DEVICE}/#")

def on_message(c, u, msg):
    try:
        p = json.loads(msg.payload.decode())
    except Exception:
        print(msg.topic, msg.payload); return
    if msg.topic.endswith("/telemetry"):
        rows.append(p)
    elif msg.topic.endswith("/event"):
        events.append(p); print("EVENT:", p["uptime_s"], p["msg"])

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
c.username_pw_set(USER, PASS)
c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
c.on_connect, c.on_message = on_connect, on_message
c.connect(HOST, PORT)
c.loop_start()
time.sleep(DURATION_S)
c.loop_stop()

df = pd.DataFrame(rows)
fig = make_subplots(rows=2, cols=1, shared_xaxes=True,
                    subplot_titles=("Kadar gas (ADC)", "Status katup"))
fig.add_trace(go.Scatter(x=df.uptime_s, y=df.adc, name="ADC avg"), 1, 1)
fig.add_trace(go.Scatter(x=df.uptime_s, y=df.adc_raw, name="ADC raw", opacity=.4), 1, 1)
fig.add_hline(y=int(df.threshold.iloc[0]), line_dash="dash", line_color="orange", row=1, col=1)
fig.add_trace(go.Scatter(x=df.uptime_s, y=(df.valve == "OPEN").astype(int),
                         name="Katup (1=buka)", line_shape="hv"), 2, 1)
for e in events:
    fig.add_vline(x=e["uptime_s"], line_width=1, line_dash="dot")
fig.update_layout(title="SafeGas Monitoring", height=600)
fig.show()

# Kirim perintah reset dari dashboard:
# c.reconnect(); c.publish(f"safegas/{DEVICE}/cmd", "reset")
