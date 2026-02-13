import json
import socket
import threading
import paho.mqtt.client as mqtt


class MqttWorker:
  """
  Jednostavan MQTT subscriber worker:
  - povezuje se na host:port
  - subscribe na 1 topic
  - svaki JSON payload šalje u on_measure(dict)
  """

  def __init__(self, host, port, topic, on_measure, client_id=None):
    self.host = host
    self.port = port
    self.topic = topic
    self.on_measure = on_measure

    # unikatan client_id (po defaultu na osnovu topica)
    cid = client_id or f"sensor-gateway-{topic}".replace("/", "_")
    self.client = mqtt.Client(client_id=cid)

    self.client.on_connect = self._on_connect
    self.client.on_message = self._on_message
    self.client.on_disconnect = self._on_disconnect

    self._started = False
    self._lock = threading.Lock()

  # === MQTT callbacks ===

  def _on_connect(self, client, userdata, flags, rc):
    if rc == 0:
      print(f"[mqtt] Connected to {self.host}:{self.port}", flush=True)
      client.subscribe(self.topic)
      print(f"[mqtt] Subscribed to {self.topic}", flush=True)
    else:
      print(f"[mqtt] Connect failed, rc={rc}", flush=True)

  def _on_message(self, client, userdata, msg):
    try:
      payload = json.loads(msg.payload.decode("utf-8"))
    
      self.on_measure(payload)

      print(f"[mqtt] Payload from {msg.topic}: {payload}", flush=True)
    except Exception as e:
      print(f"[mqtt] Bad message ({e}). Topic={msg.topic}", flush=True)

  def _on_disconnect(self, client, userdata, rc):
    print(f"[mqtt] Disconnected, rc={rc}", flush=True)

  # === lifecycle ===

  def start(self):
    with self._lock:
      if self._started:
        return
      try:
        print(f"[mqtt] Connecting to {self.host}:{self.port} ...", flush=True)
        self.client.connect(self.host, self.port, keepalive=60)
      except socket.error as e:
        print(f"[mqtt] Connect error: {e}", flush=True)
      self.client.loop_start()
      self._started = True

  def stop(self):
    with self._lock:
      if not self._started:
        return
      print("[mqtt] Stopping loop...", flush=True)
      self.client.disconnect()
      self.client.loop_stop()
      self._started = False
