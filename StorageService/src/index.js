import express from "express";
import dotenv from "dotenv";
import { createNatsConnection, subscribeNATS } from "./nats.js";
import { createMongoClient } from "./mongo.js";

dotenv.config();

// ---- ENV promenljive ----
const NATS_SERVER = process.env.NATS_SERVER;
const MONGO_URL = process.env.MONGO_URL;
const MONGO_DB = process.env.MONGO_DB;

const RAW_COLLECTION = process.env.RAW_COLLECTION;
const AVG_COLLECTION = process.env.AVG_COLLECTION;
const SEC_COLLECTION = process.env.SEC_COLLECTION;
const COMMANDS_COLLECTION = process.env.COMMANDS_COLLECTION;

const SUBJECT_RAW = process.env.SUBJECT_RAW;               // sensor.raw.>
const SUBJECT_AVG = process.env.SUBJECT_AVG;               // sensor.avg.>
const SUBJECT_SECURITY = process.env.SUBJECT_SECURITY;     // sensor.security.>

const PORT = parseInt(process.env.PORT ?? "4000", 10);

// ---- globalne promenljive za konekcije ----
let nc;      // NATS connection
let client;  // Mongo client
let rawCol;
let avgCol;
let secCol;
let cmdCol;

// helper da sigurno napravimo Date
function toDateOrNull(value) {
  if (!value) return null;
  const d = new Date(value);
  return isNaN(d.getTime()) ? null : d;
}

async function main() {
  console.log("[storage] Starting StorageService ...");

  console.log("[storage] Config:");
  console.log(`  NATS_SERVER=${NATS_SERVER}`);
  console.log(`  MONGO_URL=${MONGO_URL}`);
  console.log(`  MONGO_DB=${MONGO_DB}`);
  console.log(`  RAW_COLLECTION=${RAW_COLLECTION}`);
  console.log(`  AVG_COLLECTION=${AVG_COLLECTION}`);
  console.log(`  SEC_COLLECTION=${SEC_COLLECTION}`);
  console.log(`  COMMANDS_COLLECTION=${COMMANDS_COLLECTION}`);
  console.log(`  SUBJECT_RAW=${SUBJECT_RAW}`);
  console.log(`  SUBJECT_AVG=${SUBJECT_AVG}`);
  console.log(`  SUBJECT_SECURITY=${SUBJECT_SECURITY}`);
  console.log(`  PORT=${PORT}`);

  // 1) NATS konekcija
  nc = await createNatsConnection(NATS_SERVER);

  // 2) Mongo konekcija
  client = await createMongoClient(MONGO_URL);
  const db = client.db(MONGO_DB);

  rawCol = db.collection(RAW_COLLECTION);
  avgCol = db.collection(AVG_COLLECTION);
  secCol = db.collection(SEC_COLLECTION);
  cmdCol = db.collection(COMMANDS_COLLECTION);

  // 3) Sub na RAW podatke (sensor.raw.<sensor_name>)
  subscribeNATS(nc, SUBJECT_RAW, async (msg) => {
    try {
      const doc = {
        ts_server: toDateOrNull(msg.ts_server), // timeField za raw_data
        metadata: {
          sensor_name: msg.sensor_name,
          house_id: msg.house_id,
          user_id: msg.user_id ?? null,
          state: msg.state ?? null,
        },
        temperature: msg.temperature,
        humidity: msg.humidity,
        pressure: msg.pressure,
        millis: msg.millis,
      };

      await rawCol.insertOne(doc);

      console.log("[storage][RAW] inserted:", {
        sensor: doc.metadata.sensor_name,
        ts: doc.ts_server?.toISOString?.() ?? null,
        temp: doc.temperature,
        hum: doc.humidity,
        press: doc.pressure,
      });
    } catch (err) {
      console.error("[storage][RAW] insert error:", err);
    }
  });

  // 4) Sub na AVG podatke (sensor.avg.<sensor_name>)
  subscribeNATS(nc, SUBJECT_AVG, async (msg) => {
    try {
      const doc = {
        // timeField za avg_data je t_end
        t_end: toDateOrNull(msg.t_end),
        t_start: toDateOrNull(msg.t_start),
        metadata: {
          sensor_name: msg.sensor_name,
          house_id: msg.house_id,
          window: msg.window, // { type, size_sec }
        },
        avg: msg.avg,     // { temperature, humidity, pressure }
        count: msg.count,
      };

      await avgCol.insertOne(doc);

      console.log("[storage][AVG] inserted:", {
        sensor: doc.metadata.sensor_name,
        t_end: doc.t_end?.toISOString?.() ?? null,
        avg: doc.avg,
        count: doc.count,
      });
    } catch (err) {
      console.error("[storage][AVG] insert error:", err);
    }
  });

  // 5) Sub na SECURITY evente (sensor.security.<sensor_name>)
  subscribeNATS(nc, SUBJECT_SECURITY, async (msg) => {
    try {
      const doc = {
        ts_server: toDateOrNull(msg.ts_server), // timeField za security_events
        metadata: {
          sensor_name: msg.sensor_name,
          house_id: msg.house_id,
          user_id: msg.user_id ?? null,
        },
        event_type: msg.event_type,
        prev_state: msg.prev_state,
        new_state: msg.new_state,
        reason: msg.reason,
        failed_attempts: msg.failed_attempts,
        millis: msg.millis,
        // ostalo ako postoji (možeš kasnije da proširiš)
      };

      await secCol.insertOne(doc);

      console.log("[storage][SEC] inserted:", {
        sensor: doc.metadata.sensor_name,
        ts: doc.ts_server?.toISOString?.() ?? null,
        type: doc.event_type,
        from: doc.prev_state,
        to: doc.new_state,
      });
    } catch (err) {
      console.error("[storage][SEC] insert error:", err);
    }
  });

  // 6) HTTP API za health check (i kasnije za Grafanu)
  const app = express();

  // --------- Helperi za API (Grafana) ---------

  // parsira limit query param; ako je loš ili nema, vrati default
  function parseLimit(qLimit, defaultValue) {
    const n = parseInt(qLimit ?? "", 10);
    if (Number.isNaN(n) || n <= 0) return defaultValue;
    return n;
  }

  // zajednički filter za sensor_name / house_id
  function buildCommonQuery(req) {
    const query = {};

    const sensorName = req.query.sensor_name || req.query.sensor;
    const houseId = req.query.house_id || req.query.house;

    if (sensorName) {
      query["metadata.sensor_name"] = sensorName;
    }
    if (houseId) {
      query["metadata.house_id"] = houseId;
    }

    return query;
  }

  app.get("/", (req, res) => {
    res.json({ status: "ok" });
  });

  // ovde ćemo kasnije dodati /api/raw, /api/avg, /api/security za Grafanu

  // --------- RAW data: /api/raw/latest ---------
  // Primer: /api/raw/latest?limit=100&sensor_name=MKR1010_WiFi&house_id=house_1
  app.get("/api/raw/latest", async (req, res) => {
    try {
      const limit = parseLimit(req.query.limit, 100);
      const query = buildCommonQuery(req);

      const docs = await rawCol
        .find(query)
        .sort({ ts_server: -1 })
        .limit(limit)
        .toArray();

      res.json(docs);
    } catch (err) {
      console.error("[storage][API][RAW] error:", err);
      res.status(500).json({ error: "Internal server error" });
    }
  });

  // --------- AVG data: /api/avg/latest ---------
  // Primer: /api/avg/latest?limit=200&sensor_name=MKR1010_WiFi&house_id=house_1
  app.get("/api/avg/latest", async (req, res) => {
    try {
      const limit = parseLimit(req.query.limit, 200);
      const query = buildCommonQuery(req);

      const docs = await avgCol
        .find(query)
        .sort({ t_end: -1 })       // koristimo t_end kao "kraj prozora"
        .limit(limit)
        .toArray();

      res.json(docs);
    } catch (err) {
      console.error("[storage][API][AVG] error:", err);
      res.status(500).json({ error: "Internal server error" });
    }
  });

  // --------- SECURITY events: /api/security/latest ---------
  // Primer: /api/security/latest?limit=50&sensor_name=MKR1010_WiFi&house_id=house_1
  app.get("/api/security/latest", async (req, res) => {
    try {
      const limit = parseLimit(req.query.limit, 50);
      const query = buildCommonQuery(req);

      const docs = await secCol
        .find(query)
        .sort({ ts_server: -1 })   // najnoviji eventovi
        .limit(limit)
        .toArray();

      res.json(docs);
    } catch (err) {
      console.error("[storage][API][SEC] error:", err);
      res.status(500).json({ error: "Internal server error" });
    }
  });

  app.get("/api/commands/latest", async (req, res) => {
    try {
      const limit = parseInt(req.query.limit, 50);

      const docs = await cmdCol
        .find({})
        .sort({ ts_server: -1 })   // poslednje komande prve
        .limit(limit)
        .toArray();

      res.json(docs);
    } catch (err) {
      console.error("[storage] /api/commands/latest error:", err);
      res.status(500).json({ error: "Internal server error" });
    }
  });

  app.listen(PORT, () => {
    console.log(`[storage] HTTP API listening on http://localhost:${PORT}`);
  });

  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}

async function stop() {
  console.log("\n[storage] Stopping ...");
  try {
    if (nc) await nc.drain();
  } catch {}
  try {
    if (client) await client.close();
  } catch {}
  console.log("[storage] Bye.");
  process.exit(0);
}

main().catch((err) => {
  console.error("[storage] Fatal:", err);
  process.exit(1);
});
