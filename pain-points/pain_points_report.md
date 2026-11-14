# Developer Pain Point Report

## 🚨 Top High Risk Functions (Complex + Untested)

- **[`resumeTask()`](https://github.com/ruxven/bjorne_agent/blob/d4664c260e87bdeb0104125920116d4598920ab6/src/main.cpp#L401-L479)** in `src/main.cpp`
  - Score: **10100**
  - Complexity: **15** (above threshold value: 15)
  - Branch Coverage: **0.0%** (below threshold value: 80)
  - Line Coverage: **0.0%** (below threshold value: 80)
  - Critical findings: ``

## 🔒 Top Security Risk Functions (Prioritized by coverage)

- **[`bjorne_agent::platform::installShutdownHandlers()`](https://github.com/ruxven/bjorne_agent/blob/d4664c260e87bdeb0104125920116d4598920ab6/src/platform/Shutdown.h#L25-L30)** in `src/platform/Shutdown.h`
  - Score: **20000**
  - Branch Coverage: **0.0%**
  - Line Coverage: **0.0%**
  - Critical findings: `cert-err33-c, cert-err33-c`

