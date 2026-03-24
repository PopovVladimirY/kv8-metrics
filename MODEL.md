# MODEL -- Anomaly Detection and ML Inference on KV8 Telemetry

A showcase demonstrating KV8 as the data spine for a complete ML lifecycle:
offline model training on recorded telemetry, followed by live, low-latency
anomaly scoring injected back into the same Kafka bus.

---

## 1. Problem Statement

The kv8feeder emits three time-synchronized 1000 Hz feeds from an aerial
platform:

| Kafka Topic          | Payload  | Key fields                                               |
|----------------------|----------|----------------------------------------------------------|
| `Aerial/Navigation`  | 58 B     | lat_deg, lon_deg, alt_m, velocity_ms, nav_status         |
| `Aerial/Attitude`    | 80 B     | orientation (quaternion), angular_rate, accel            |
| `Aerial/Motors`      | 20 B     | battery_v, motor1-4_rpm                                  |

At 1000 Hz the platform generates ~158 KB/s of dense, structured binary data.
A typical one-hour flight produces roughly 500 MB of compressed Parquet and
captures rich multi-modal dynamics that are ideal for learning normal behaviour
and detecting departures from it.

Failure modes worth detecting include:

- **Motor degradation** -- one or more motor RPMs diverging from the commanded
  set point or oscillating outside expected variance bands.
- **Battery stress** -- voltage sag under load faster than the baseline curve,
  predicting insufficient range.
- **Navigation anomaly** -- GPS fix quality dropping, HDOP spiking, barometric
  and GPS altitude disagreements, or velocity inconsistencies.
- **Attitude instability** -- oscillatory roll/pitch sequences, angular
  acceleration spikes, or quaternion drift.
- **Flight envelope exceedance** -- combined state (speed + attitude + altitude)
  entering regions never visited in nominal training flights.

---

## 2. Use Case Narrative

A fleet operator records all telemetry to a time-series store during normal
flights. Each night a training job re-fits anomaly models on the accumulated
data. Models are exported to ONNX and deployed as a lightweight C++ consumer
(`kv8infer`) that runs beside the live system. Every 50 ms `kv8infer` publishes
an anomaly score frame to a dedicated Kafka topic. kv8scope displays the score
as a live overlay alongside raw waveforms. A threshold breach triggers a webhook
that posts an alert to the operations dashboard before any human notices a
problem in the raw signals.

---

## 3. Pipeline Overview

```
=== OFFLINE TRAINING ===

  Kafka  (Aerial/Navigation, Aerial/Attitude, Aerial/Motors)
      |
      v [kv8recorder -- C++ consumer, writes Parquet]
  Parquet files (per-flight, per-topic, partitioned by date)
      |
      v [Python notebook / training script]
  Feature extraction
      |
      +-- Isolation Forest  (scikit-learn)
      +-- LSTM Autoencoder  (PyTorch)
      +-- Statistical baseline (rolling mean/std per channel)
      |
      v [ONNX export (torch.onnx / sklearn-onnx)]
  models/
    anomaly_isolation_forest.onnx
    anomaly_lstm_ae.onnx
      |
      v [validation on held-out flight recordings]
  Precision / Recall metrics, threshold calibration
      |
      v [model artefacts deployed to kv8infer config directory]


=== LIVE INFERENCE ===

  Kafka  (Aerial/Navigation, Aerial/Attitude, Aerial/Motors)
      |
      v [librdkafka consumer, full 1000 Hz]
  +-----------------------------------------------+
  |  kv8infer  (C++ process)                      |
  |                                               |
  |  KafkaReader    -- one thread per topic,      |
  |                    SpscRingBuffer handoff      |
  |  FrameAssembler -- timestamp-aligned merge,   |
  |                    drops orphan frames         |
  |  FeatureExtractor -- 50 ms sliding window,    |
  |                      ~50 sample ring           |
  |                      per-channel mean, std,   |
  |                      delta, kurtosis          |
  |  OnnxEngine     -- ONNX Runtime C++ API,      |
  |                    batched on the window end   |
  |  AnomalyScorer  -- weighted ensemble of both  |
  |                    models, hysteresis filter   |
  +-----------------------------------------------+
      |
      v [binary KV8 UDT frame]
  Kafka  Aerial/AnomalyScore   (20 Hz)
      |
      +----> kv8scope  (live waveform overlay)
      +----> webhook / alerting sidecar
```

---

## 4. Offline Training Pipeline

### 4.1  Recording

A new tool `kv8recorder` (mirrors the structure of `kv8zoom`) consumes all three
Aerial topics at full rate and appends rows to per-flight Parquet files using the
Apache Arrow C++ library (header-only columnar writer is sufficient here).  Each
row is one merged frame keyed by the KV8 timestamp.

No lossy decimation -- the full 1000 Hz record is preserved.  A one-hour flight
at 29 fields @ 8 bytes each fits in roughly 500 MB compressed.

### 4.2  Feature Engineering

Raw fields alone are sufficient as model inputs, but derived features
dramatically improve signal quality:

| Feature group         | Derived columns                                           |
|-----------------------|-----------------------------------------------------------|
| Velocity magnitude    | `\|v\|` = sqrt(vx^2 + vy^2 + vz^2)                       |
| Euler angles          | roll, pitch, yaw from the orientation quaternion          |
| Angular rate norms    | `\|omega\|`                                               |
| Motor imbalance       | max(RPMs) - min(RPMs), std(RPMs)                          |
| Battery rate of change| d(battery_v)/dt                                           |
| GPS quality delta     | HDOP change rate, alt_gps - baro_alt diff                 |
| Rolling statistics    | per-channel mean, std, min, max over 1 s window (1000 pt) |

A Python script (`scripts/feature_engineering.py`) reads Parquet, computes these
columns, and writes a feature Parquet file consumed by the training scripts.

### 4.3  Model Selection and Training

Two complementary algorithms cover different anomaly shapes:

#### Isolation Forest

- **Why**: unsupervised, no labelled anomalies needed, works well on
  mixed-distribution tabular data, extremely fast to train and export.
- **Input**: one row = one merged frame (all raw + derived features, ~40 dims).
- **Training**: scikit-learn `IsolationForest(n_estimators=200, contamination=0.01)`.
- **Output**: anomaly score in [-1, 1]; values near +1 are inliers, -1 outliers.
- **Export**: `sklearn-onnx` converts the fitted estimator to ONNX opset 17.

#### LSTM Autoencoder

- **Why**: captures temporal structure and multi-step correlations that pointwise
  models miss (e.g., an oscillation building over 200 ms before a fault).
- **Input**: sliding window of W=50 time steps x 40 features (50 ms @ 1000 Hz).
- **Architecture**:
  ```
  Encoder: LSTM(40 -> 64) -> LSTM(64 -> 32)
  Latent:  Linear(32 -> 16)
  Decoder: LSTM(16 -> 32) -> LSTM(32 -> 64) -> Linear(64 -> 40)
  ```
- **Training**: unsupervised reconstruction; loss = MSE on nominal-only flights.
  PyTorch, Adam, cosine annealing LR, 100 epochs on a modern GPU.
- **Anomaly score**: reconstruction error (mean squared error) on the latest window.
- **Export**: `torch.onnx.export` with dynamic batch axis.

#### Threshold Calibration

After training, evaluate both models on a held-out set of known-nominal and
known-anomaly flights (inject synthetic faults: a step-drop in one motor RPM,
GPS fix forced to 0, sudden attitude spike).  Choose thresholds that achieve
>95% recall on injected faults at <1% false positive rate.  Store thresholds in
`kv8infer`'s JSON config alongside the ONNX model paths.

### 4.4  Tooling

| Task               | Tool / Library                         |
|--------------------|----------------------------------------|
| Kafka consumption  | librdkafka (C++)                       |
| Parquet writing    | Apache Arrow C++ (recording side)      |
| Data exploration   | Python, pandas, matplotlib             |
| Feature eng.       | Python, numpy                          |
| IF training        | scikit-learn                           |
| LSTM training      | PyTorch                                |
| ONNX export        | sklearn-onnx, torch.onnx               |
| Experiment tracking| MLflow (optional, lightweight)         |
| Validation metrics | scikit-learn (classification_report)   |

No cloud dependency is required -- all training runs on a single workstation
or modest GPU server.

---

## 5. Live Inference Pipeline

### 5.1  kv8infer Architecture

`kv8infer` is structured identically to `kv8zoom` (see `ZOOM_IMPLEMENTATION.md`)
to keep the codebase consistent.

```
tools/
  kv8infer/
    CMakeLists.txt
    main.cpp                   -- loads config, starts inference loop
    Config.h / Config.cpp      -- JSON config (broker, topics, model paths,
                                  thresholds, window size)
    KafkaReader.h / .cpp       -- librdkafka consumer, one thread per topic
                                  (reusable from kv8zoom)
    FrameAssembler.h / .cpp    -- timestamp-aligned merge of nav+att+mot
    FeatureExtractor.h / .cpp  -- rolling 50-sample window, computes ~40 features
    OnnxEngine.h / .cpp        -- ONNX Runtime C++ API wrapper
                                  loads both models at startup
                                  runs inference on each window completion
    AnomalyScorer.h / .cpp     -- combines IF score + LSTM reconstruction error
                                  applies hysteresis filter
                                  emits AnomalyFrame struct
    KafkaWriter.h / .cpp       -- librdkafka producer, publishes to
                                  Aerial/AnomalyScore at 20 Hz
    AnomalyFeed.h              -- KV8 UDT definition for the score frame
```

### 5.2  AnomalyFrame UDT

Published to `Aerial/AnomalyScore` at 20 Hz (one per 50 ms inference window):

| Field            | Type | Description                                      |
|------------------|------|--------------------------------------------------|
| score_if         | f32  | Isolation Forest anomaly score (0.0 = normal)    |
| score_lstm       | f32  | LSTM reconstruction error (normalised)           |
| score_ensemble   | f32  | Weighted combination (primary alerting signal)   |
| alert            | u8   | 0 = nominal, 1 = warning, 2 = critical           |
| dominant_channel | u8   | Index of the feature with highest residual       |
| window_ts_us     | u64  | Timestamp of the last sample in the window (us)  |

Total payload: 19 bytes.  Compatible with existing KV8 UDT machinery.

### 5.3  Latency Budget

At 1000 Hz input the FrameAssembler completes a merged frame every 1 ms.  The
FeatureExtractor advances its rolling window on each frame without a copy (ring
buffer pointer arithmetic only).  Every 50 frames (50 ms) it triggers inference:

| Stage                      | Typical latency       |
|----------------------------|-----------------------|
| Kafka consume + deserialise| < 0.5 ms / frame      |
| Frame assembly             | < 0.1 ms              |
| Feature extraction (50 pt) | < 0.5 ms              |
| IF ONNX inference          | < 1 ms  (CPU)         |
| LSTM ONNX inference        | < 3 ms  (CPU, window) |
| Kafka publish              | < 1 ms                |
| **Total end-to-end**       | **< 6 ms**            |

End-to-end latency from sensor event to anomaly score on the bus is well under
10 ms, which is fast enough for actuation-level alerting.

### 5.4  Dependencies

ONNX Runtime ships a self-contained C++ shared library with no transitive
runtime dependencies beyond the C++ standard library.  It is fetched via CMake
`FetchContent` from the official GitHub release tarball (pre-built binary,
no build step required).  No Python, no Boost, no additional ML framework is
pulled into the production binary.

```cmake
FetchContent_Declare(onnxruntime
    URL      https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-linux-x64-1.19.2.tgz
    URL_HASH SHA256=<hash>
)
```

On Windows, the equivalent `.zip` release is used.  The CMake target exposes
`onnxruntime::onnxruntime` as an imported shared library.

---

## 6. kv8scope Integration

kv8scope gains a new `AnomalyPanel` that subscribes to `Aerial/AnomalyScore`:

- **Ensemble score waveform** rendered in the main waveform area alongside raw
  motor RPM and attitude channels.
- **Alert banner** at the top of the scope window when `alert >= 1`, colour-
  coded yellow (warning) / red (critical).
- **Dominant channel label** showing which raw signal contributed most to the
  current anomaly score, guiding the operator's attention.
- **Threshold lines** drawn as horizontal rules on the score waveform, matching
  the calibrated thresholds from config.

No changes to the existing Kafka consumer or UDT machinery are needed -- the
anomaly feed is just another KV8 UDT topic.

---

## 7. Workflow Summary

```
1. Fly ten nominal flights with kv8feeder / real hardware.
   kv8recorder saves full 1000 Hz Parquet to disk.

2. Run feature_engineering.py to produce feature Parquet.

3. Train Isolation Forest and LSTM Autoencoder on nominal data.
   Export both to ONNX.  Calibrate thresholds on synthetic fault set.

4. Deploy ONNX files to kv8infer config directory.
   Start kv8infer alongside existing Kafka + kv8feeder stack.

5. kv8scope AnomalyPanel shows live anomaly score.
   Webhook sidecar forwards alert=2 events to operations dashboard.

6. After each new batch of flights, re-run training to adapt to fleet
   changes (battery aging, motor wear).  Hot-swap ONNX models by sending
   SIGHUP to kv8infer -- it reloads models without pipeline restart.
```

---

## 8. Extension Paths

- **Fleet-level model**: train on data from multiple platforms simultaneously;
  detect cross-platform correlated anomalies (shared airspace interference,
  firmware regression).
- **Causal attribution**: SHAP values computed in Python post-hoc on flagged
  windows to explain which sensor combination triggered the alert.
- **Federated training**: each platform records locally; models are aggregated
  using FedAvg without raw data leaving the edge node.
- **Supervised fine-tuning**: once labelled anomaly events accumulate, fine-tune
  the LSTM encoder head as a binary classifier using those labels, improving
  precision without retraining the full autoencoder.
- **Sim-to-real**: pre-train on synthetic flights from a physics simulator
  (e.g., Gazebo), then fine-tune on a small real-flight corpus to reduce the
  amount of real data needed before deployment.
