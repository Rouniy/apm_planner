# FFT Setup and Advanced Tools FFT

The Qt port implements the MP10 `ConfigFFTView` and independent
`ConfigFFTWindow`, not an alias to the time-frequency Spectrogram tool.
`SETUP → FFT Setup` follows ESP8266 Setup and consumes `displayFFTSetup`.
Advanced Tools opens a fresh modeless 1000×700 window for every FFT click;
the fixed 24-entry main TOOLS menu is unchanged.

## Functional scope

- Three reference parameters: `INS_LOG_BAT_CNT`, `INS_LOG_BAT_MASK`,
  `LOG_BITMASK`. Packaged/versioned metadata supplies titles, ranges,
  increments and bit choices. Missing parameters remain `n/a`; bitmasks use
  real checkable menus rather than MP10's unsuitable 32..4096 slider.
- The parameter adapter pins the selected target generation and physical
  vehicle instance. It publishes committed snapshots or all three successful
  exact reads, serializes owned typed writes and waits for matching ACKs.
  Stale heartbeat, arming, target changes, uncertain writes and route loss
  prevent further writes. Offline analysis is independent of parameter state.
- Single-vehicle exact reservations use a separate route policy from Swarm:
  supported serial/TCP/UDP-client and one learned UDP peer. The selected
  target, link epoch and UDP peer revision are revalidated. Multi-peer UDP
  does not qualify; the existing Swarm policy is unchanged.
- ASCII `.log` and binary `.bin` parsers stream all six ACC and six GYR batch
  instances, or IMU/IMU2/IMU3 fallback when no complete usable batch exists.
  One total 2,097,152-sample budget covers retained sensor data and pending
  packets. Sixteen pending headers bound incomplete-batch accumulation;
  oldest incomplete batches are evicted without discarding completed data.
- Bins 4..14, Start Freq 0..1000 Hz, magnitude/dB, every available XYZ curve,
  per-series sample rates and a strongest-gyro notch suggestion. The
  suggestion is informational and never writes a filter automatically.
- Workers copy their inputs, poll cancellation and are joined on destruction.
  File pickers and asynchronous completions are guarded against page/window
  deletion. Existing Spectrogram remains a separate tool.

## Deliberate differences from MP10

The Hann normalization is exactly `(4/N)*0.5*(1-cos(2*pi*i/N))`, with no DC
subtraction. Magnitudes, or per-window dB values, are averaged in the plotted
domain. The port fixes the reference's dropped last/only complete FFT window
and wrong divisor, and uses fractional frequency bins instead of integer
division/truncated sample rates. Below-Start-Freq zero bins remain reference
behavior. Exact-zero dB values remain in the data, while the initial plot
shows a useful 160 dB span below the peak; pan/zoom can inspect lower values.

Batch packets correlate by N and fixed 32-sample sequence geometry, including
out-of-order/short-tail packets. Only complete time-contiguous samples form an
FFT window; unlike MP10, separated bursts are not silently concatenated. An
insufficient-window message suggests lowering Bins or raising
`INS_LOG_BAT_CNT`. IMU fallback uses median timestamp intervals and splits
large gaps, rather than the reference's slowly converging EMA. Dominant-rate
selection is bounded/cancellable; minority-rate samples are not averaged
onto a different frequency grid.

## Verification and remaining gates

Final checkpoint: full Qt 5 build with required Qt audio and **213/213 CTest
targets pass** (`build-fifth.log`, `tests-verified.log`). Real X11 verifies
`setup-spectrum-verified.png` and `window-spectrum-verified.png`, CNT
1024→1056 and MASK 1→3 matching `WRITE ACK` records, all three exact refresh
reads in `peer.log`, and application exit 0 with the FFT window open
(`app-verified.log`). No real flight controller was commanded. The synthetic
peer was stopped afterward. An earlier full suite of 212 passing tests did
not cover the subsequently discovered production-route/shutdown defects.

Evidence for this slice is stored in `/tmp/apm-fft.T1Aeqm/`. Independent Claude
reviews c180/c181/c182 were exchanged through TCP 4096; final policy review
reports REVIEW-OK. Core/parser tests cover
normalization, complete-window averaging, fractional bins, all six batch
instances, Unicode paths, binary data, incomplete-batch eviction, input bounds
and cancellation. UI/adapter tests exercise metadata bitmasks, write ACK and
retirement, late committed snapshots, offline workers and modeless ownership.
The production route audit independently checks 53 MP10 pages against 45 Qt
factories (44 mapped plus QML Plugins), nine explicit gaps, three groups and
the actual FFT action creating two independent windows.

Real X11 is required in addition to these tests: both entry points, usable
spectra, parameter changes against a synthetic peer, and clean application
shutdown. The first live run caught the too-strict Swarm parameter-route gate
and a pre-existing firmware-page/header teardown bug; both are fixed and remain
regression gates, not be hidden by the earlier passing unit suite.

Remaining strict-parity gates: representative physical flight logs/hardware,
native Windows/macOS runs and reference screenshot comparison. Mixed-rate
discard diagnostics, out-of-range metadata presentation and malformed/NaN
window reporting can be improved without declaring the current port complete.
Further low-priority follow-ups: generic single-vehicle route error wording,
retry firmware-header binding if a page is first shown outside MainWindow,
and explicit serial/two-peer/unsettled-generation runtime matrices.
The original raw ACC1..4/GYR1..4 FFT path and `INS_LOG_BAT_OPT` editor are not
part of the current MP10 FFT page and are not silently claimed as implemented.
