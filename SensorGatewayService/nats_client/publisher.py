import json
import threading
import asyncio
import queue
from nats.aio.client import Client as NATS

class NatsPublisher:
  def __init__(self, servers):
    self.servers = servers
    self._loop = None
    self._thread = None
    self._nats_conn = None
    self._queue = queue.Queue()
    self._stop_event = threading.Event()

  def start(self):
    self._thread = threading.Thread(target=self._run_loop, daemon=True)
    self._thread.start()

  def _run_loop(self):
    self._loop = asyncio.new_event_loop()
    asyncio.set_event_loop(self._loop)
    self._loop.run_until_complete(self._async_main())

  async def _async_main(self):
    self._nats_conn = NATS()
    await self._nats_conn.connect(servers=self.servers)
    print("[nats] Connected.", flush=True)

    try:
      while not self._stop_event.is_set():
        try:
          subject, payload = self._queue.get(timeout=0.2)
        except queue.Empty:
          await asyncio.sleep(0.05)
          continue
        data = json.dumps(payload).encode("utf-8")
        await self._nats_conn.publish(subject, data)
    finally:
      await self._nats_conn.drain()
      await self._nats_conn.close()
      print("[nats] Closed.", flush=True)

  def publish(self, subject: str, payload: dict):
    self._queue.put((subject, payload))

  def stop(self):
    self._stop_event.set()
    if self._loop:
      asyncio.run_coroutine_threadsafe(asyncio.sleep(0), self._loop)
      self._thread.join(timeout=3)