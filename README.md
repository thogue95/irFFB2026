# irFFB2026

**Enhanced Force Feedback for iRacing**  
irFFB2026 is a nearly complete rewrite and major evolution of irFFB, delivering dramatically more realistic and connected steering feedback through a modern implementation of the Pacejka Magic Formula for self-aligning torque (SAT) and vertical load effects.

### Project Lineage & Credits

This repository is a **fork** of the original irFFB project by **nlp80**.

- Original repository: [https://github.com/nlp80/irFFB](https://github.com/nlp80/irFFB)  
- All foundational credit for irFFB goes to **nlp80** and early contributors.  
- irFFB2022 (previous major update) diverged significantly with new architecture.  
- irFFB2026 continues that path with a full rewrite of the FFB engine, focusing on realism, latency, and usability.

irFFB2026 is released under the same **GNU General Public License v3** as the original. See the [LICENSE](LICENSE) file for full details.

### Core Innovation: Pacejka Magic Formula Implementation

The heart of irFFB2026 is a new FFB model based on the **Pacejka Magic Formula** — a widely respected tire physics model used in real-world automotive engineering and racing simulations.

Key aspects of the implementation:

- **Self-Aligning Torque (SAT)**: Calculates realistic self-aligning forces from tire slip angle, load, and camber using Pacejka coefficients (digressive behavior via Q_DZ2 = -0.2f, nominal load FZ_NOM = 3000 N, etc.). This provides natural feedback for oversteer/understeer, grip loss, and cornering limits.
- **Vertical Load Effects**: Models up-and-down forces and suspension movement (shock velocities, delta Fz per axle) using a vertical damping term (CZ_BASE = 2000 Ns/m) and bumpsLevel scaling. This lets drivers feel body roll, weight transfer, curb compression, and terrain changes in a lifelike way.
- **Integration Approach**: Pacejka outputs are summed as additional torque layered on top of iRacing’s native FFB signal. The result is interpolated at high rate (360/720 Hz) with double-buffering to avoid tearing, gentle spike filtering to prevent harsh snaps, and impact reduction during high-G events.
- **Result**: Much richer detail than stock iRacing FFB — clearer push/loose feel, earlier slip detection, and more connected curb/roll sensations — while maintaining low latency in Game modes.

### Key Features

- **Ultra-low latency Game modes** (360 Hz & 720 Hz) — vJoy optional, precise timing via sleepSpinUntil and NtDelayExecution
- **Auto Tune** — Automatically raises Max Force to eliminate clipping (learns stable values over clean laps)
- **SimHub integration** — In-car button bindings (Max Force, FFB Effects, Damping, Bumps, Auto Tune toggle) + overlays for clipping %, oversteer/understeer intensity
- **Impact force reduction** — Attenuates FFB during high-G spikes
- **Simplified UI** — Easier setup, quick tips, per-car/track settings saved automatically
- **Reliability improvements** — Robust DirectInput reacquire, lower CPU usage, no vJoy required in irFFB modes
