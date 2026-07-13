# rtr_latency — render-to-render / present-pipeline latency harness

Pure-software DXGI flip-model harness. Measures, per frame, over QPC + GPU timestamps:

1. **Frame interval + jitter** — QPC delta between `Present()` calls (stddev = determinism)
2. **GPU render time** — D3D11 timestamp queries
3. **Present→flip latency** — `DXGI_FRAME_STATISTICS.SyncQPCTime`

Outputs `rtr_latency.csv` + a summary with mean / median / p99 / max / jitter(sd).

## Build via GitHub Actions (no local compiler needed)

1. Create a new repo on GitHub (empty, no README).
2. Upload these files, preserving the layout:
   ```
   src/rtr_latency.cpp
   .github/workflows/build.yml
   README.md
   ```
   (Web UI: "Add file" → "Upload files" → drag the folder in. The path
   `.github/workflows/build.yml` must be exact or the workflow won't run.)
3. The push triggers the build automatically. Open the **Actions** tab →
   the latest run → wait for the green check.
4. Download the compiled binary from that run's **Artifacts** section:
   `rtr_latency-exe` → unzip → `rtr_latency.exe`.

If it didn't auto-run: Actions tab → select "build" → "Run workflow"
(that's the `workflow_dispatch` trigger).

## Run (on the target machine)

```
rtr_latency.exe 5000            :: uncapped — pipeline floor
rtr_latency.exe 5000 --vsync    :: locked to the panel — jitter around fixed cadence
rtr_latency.exe 5000 --busy 200 :: load the GPU into a realistic frame time
```

## A/B a tweak

1. `rtr_latency.exe 5000 --vsync` → save `rtr_latency.csv` as `before.csv`
2. apply one change (e.g. GPU IRQ affinity), reboot
3. rerun → `after.csv`
4. compare **p99 and jitter(sd)** of the frame interval, not mean FPS.

Presentation-pipeline latency, not click-to-photon. Answers "does this config
make the present path more deterministic?"
