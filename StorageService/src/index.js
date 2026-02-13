import express from "express";
import dotenv from "dotenv";
import { createNatsConnection, subscribeNATS } from "./nats.js";
import { createMongoClient } from "./mongo.js";

dotenv.config();

const NATS_SERVER = process.env.NATS_SERVER;
const MONGO_URL = process.env.MONGO_URL;
const MONGO_DB = process.env.MONGO_DB;

const RAW_COLLECTION = process.env.RAW_COLLECTION;
const AVG_COLLECTION = process.env.AVG_COLLECTION;
const SEC_COLLECTION = process.env.SEC_COLLECTION;
const COMMANDS_COLLECTION = process.env.COMMANDS_COLLECTION;

const SUBJECT_RAW = process.env.SUBJECT_RAW;             
const SUBJECT_AVG = process.env.SUBJECT_AVG;               
const SUBJECT_SECURITY = process.env.SUBJECT_SECURITY;     

const PORT = parseInt(process.env.PORT ?? "4000", 10);


let nc;      
let client;  
let rawCol;
let avgCol;
let secCol;
let cmdCol;

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

  nc = await createNatsConnection(NATS_SERVER);

  client = await createMongoClient(MONGO_URL);
  const db = client.db(MONGO_DB);

  rawCol = db.collection(RAW_COLLECTION);
  avgCol = db.collection(AVG_COLLECTION);
  secCol = db.collection(SEC_COLLECTION);
  cmdCol = db.collection(COMMANDS_COLLECTION);

  subscribeNATS(nc, SUBJECT_RAW, async (msg) => {
    try {
      const doc = {
        ts_server: toDateOrNull(msg.ts_server), 
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

  subscribeNATS(nc, SUBJECT_AVG, async (msg) => {
    try {
      const doc = {
        t_end: toDateOrNull(msg.t_end),
        t_start: toDateOrNull(msg.t_start),
        metadata: {
          sensor_name: msg.sensor_name,
          house_id: msg.house_id,
          window: msg.window, 
        },
        avg: msg.avg,   
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

  subscribeNATS(nc, SUBJECT_SECURITY, async (msg) => {
    try {
      const doc = {
        ts_server: toDateOrNull(msg.ts_server),
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

  // HTTP API za Grafanu
  const app = express();

  // parsira limit query param; ako je loš ili nema, vrati default
  function parseLimit(qLimit, defaultValue) {
    const n = parseInt(qLimit ?? "", 10);
    if (Number.isNaN(n) || n <= 0) return defaultValue;
    return n;
  }

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
        .sort({ t_end: -1 })
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
        .sort({ ts_server: -1 })  
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
        .sort({ ts_server: -1 })  
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
