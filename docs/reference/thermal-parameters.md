# HBF thermal parameters: derivation and sources

> Status: Current
> Last reviewed: 2026-10-23

The HBF thermal model ([physical model](model.md#thermal-reliability-and-evidence-limits))
is one lumped RC node per stack: media operations deposit energy, the node decays
toward the package boundary, and the Base-die firmware drives the four OCP HBF
v0.7.0 section 9.2 modes from the node temperature. The model's mechanics follow
the standard; its numbers do not, because no HBF product publishes them. This
page records where every number in the shipped profiles comes from, how far it
can be trusted, and what the numbers imply before any simulation is run.
`configs/parameter-provenance.json` carries the same sources per configuration
key.

## Values

| Key | Value | Basis | Grade |
|---|---|---|---|
| `hbf-thermal-read-energy-pj-per-bit` | 8.0 | Measured on a hybrid-bonded 1 Tb flash die (ISSCC 2025), as quoted by FlashAccel | literature-derived |
| `hbf-thermal-program-energy-pj-per-bit` | 150.0 | SLC data-sheet die current over the profile's 75 µs program, scaled onto the read anchor | literature-derived |
| `hbf-thermal-erase-energy-uj-per-block` | 131.0 | SLC data-sheet die current over the profile's 2 ms erase, scaled onto the read anchor | literature-derived |
| `hbf-thermal-resistance-c-per-w` | 2.0 | Hybrid-bonded HBM stack junction-to-case 1.5–2.0 C/W plus TIM, lid and cold plate | literature-derived |
| `hbf-thermal-capacitance-j-per-c` | 0.3 | Silicon, base die and mold of a 16-high stack within the 775 µm height budget | literature-derived |
| `hbf-thermal-static-power-w` | 2.0 | Base-die PHY, ECC and controller; unpublished | exploratory |
| `hbf-thermal-ambient-c` | 45 | ASHRAE W45 facility-water supply limit, the coolant-side reference | exploratory |
| `hbf-thermal-neighbor-heat-c` | 0 (overlays 10/20/25) | Package coupling to the GPU and HBM, the experiment variable | exploratory |
| `hbf-thermal-throttle-c` / `release-c` / `severe-c` / `shutdown-c` | 85 / 80 / 95 / 105 | Implementation-specific in OCP; in the range of the NVIDIA H100 qualification temperatures and the OCP 105 C junction maximum | exploratory |

The time constant R × C is 0.6 s. The light budget derived from these values,
(LTT − boundary) / R − static, is 18 W per stack behind a 45 C boundary, 13 W at
55 C, 8 W at 65 C and 3 W at 75 C.

## Energy per operation

**Read, 8 pJ per payload bit.** FlashAccel (arXiv 2607.10186) reports that
"measurements from a real hybrid bonding Flash prototype still show a read
energy of 8 pJ/bit", citing the Kioxia/Sandisk 1 Tb 3b/cell CBA die of ISSCC
2025 paper 30.2 (Yanagidaira et al.). Kioxia's own summary of that paper states a
29% read-energy improvement and a 4.8 Gb/s interface without an absolute figure,
so the value is graded literature-derived rather than vendor-published. Two
independent estimates bracket it. The die-current method on a Micron SLC NAND
data sheet (MT29F4G08AAA: ICC1 = 25 mA typical at 3.3 V while the die is busy)
applied to this profile family's 4 µs read of a 4 KiB page gives
25 mA × 3.3 V × 4 µs / 32,768 bit = 10.1 pJ/bit. SlugLab's HBFSim (arXiv
2609.09800) assumes 84 pJ per byte, 10.5 pJ/bit, plus 5 W idle per stack. For
scale, SK hynix (IEDM 2023) puts HBM2E at about 4.3 pJ/bit with HBM3E 20% lower:
a flash read costs two to three times an HBM read per bit.

**Program, 150 pJ per payload bit; erase, 131 µJ per 1 MiB block.** The same
data sheet lists the program and erase currents equal to the read current
(ICC2 = ICC3 = 25 mA typical), so energy scales with busy time. Over the
profile's 75 µs program of a 4 KiB page that is 6.2 µJ, 189 pJ/bit; over its
2 ms block erase it is 165 µJ. Both are scaled by the single factor
8 / 10.1 = 0.79 that maps the data-sheet read onto the measured anchor, giving
150 pJ/bit and 131 µJ. The ratio program : read per bit is therefore the ratio of
busy times, 75 µs : 4 µs. The previous values (6, 60, 150) had no source.

**Static, 2 W per stack.** NAND standby is negligible (the data sheet's CMOS
standby current is 10 µA typical per die, 0.5 mW for sixteen dies); the static
term is the Base-die PHYs, ECC and controller, for which nothing is published.
SlugLab assumes 5 W idle. The value stays an assumption with a 2–5 W range.

## Resistance and heat capacity

**R = 2.0 C/W.** A 2025 review of 2.5D/3D thermal management (MDPI Eng 6(12):373)
puts hybrid-bonded HBM stacks at about 1.5–2.0 C/W junction-to-case and
experimentally characterized micro-bump 3D DRAM cubes at about 3 C/W; a review
of hybrid-bonded HBM thermals (MDPI Electronics 14(13):2682) reports 22.8–47%
lower stack resistance than micro-bump stacks. SK hynix (IEDM 2023) attributes
the stack resistance to the die gap-fill and the stacking method. The lumped
node decays toward the coolant-side boundary, so the stack's junction-to-case
value gains 0.2–0.5 C/W of TIM, lid and cold plate: 1.7–2.5 C/W, of which 2.0 is
taken. The previous 2.5 was the upper end of that band. A coupled 3D-ICE package
model of GPU, HBM and HBF (SlugLab) needs 105.9 W of HBF power to reach the
105 C limit, an effective stack-to-sink resistance well under 1 C/W, which is the
optimistic end of the sensitivity range (1.0 / 2.0 / 3.0).

**C = 0.3 J/C.** Sandisk's fact sheet gives first-generation HBF the HBM4
footprint and stack height with 16-die stacking, and OCP grade 2 allows 16 dies.
JEDEC's HBM4 height budget is 775 µm for 12- and 16-high stacks, which leaves a
16-high stack about 40 µm per layer (die plus bond) once the base die and bumps
are taken out; the HBM footprint is about 10.5 × 12.0 mm, so a core die is about
1.1 cm². Sixteen such layers are 0.074 cm³ of silicon, 0.12 J/C at
1.63 J/(cm³·K); a 100 µm base die adds 0.018 J/C; the mold, underfill and the
stack's share of lid and TIM bring the node to 0.2–0.4 J/C, of which 0.3 is
taken. The previous 3.0 J/C corresponded to about a cubic centimetre of copper
per stack. With R = 2.0 C/W the time constant is 0.6 s: a stack that starts
serving reaches its thermal steady state within a few seconds, so windows of
seconds no longer depend on the start state as the 7.5 s constant made them.

## Boundary temperature

The node decays toward `ambient + neighbor heat + static × R`. The 45 C ambient
is the ASHRAE W45 facility-water supply limit, the coolant-side reference of a
liquid-cooled package. What the HBF stack actually sees is set by its package
neighbors, and that coupling is the experiment variable, `hbf-thermal-neighbor-heat-c`
(overlays `thermal-boundary-55c`, `65c`, `70c`):

- an NVIDIA H100 PCIe package is qualified at 87 C (GPU average) and 95 C (HBM),
  with software slowdown at the reported limit and hardware slowdown 2 C beyond;
- measured H100 dies under a sustained stress test run at 41–50 C on
  direct-to-chip liquid cooling fed by 20 C chilled water and at 54–72 C on air
  (arXiv 2507.16781);
- in SlugLab's coupled 3D-ICE package, the HBF layer next to the interposer
  tracks the compute die (91.6 C against a 90 C compute-die limit) while the top
  layer stays near the sink at 47.2 C: a single node per stack can represent the
  hottest die only through its boundary term.

A boundary of 45 C is therefore a thermally isolated stack on warm water; 55–65 C
is a stack next to a liquid-cooled GPU under load; 75 C approaches an air-cooled
package. The controller rejects an idle steady state (boundary + static × R) at
or above RTT, which caps the boundary below 76 C with the shipped values.

## What the numbers imply

Per stack, 256 planes reading 4 KiB pages every 4 µs peak at 262 GB/s, which at
8 pJ/bit is 16.8 W of media power; programs peak at 14 GB/s per stack and, at
150 pJ/bit, the same 16.8 W. Against the derived light budget:

| Boundary | Light budget per stack | Sustainable reads per stack | Equilibrium at peak reads |
|---|---|---|---|
| 45 C | 18 W | 281 GB/s (above the array peak) | 82.6 C, below LTT |
| 55 C | 13 W | 203 GB/s | 92.6 C |
| 65 C | 8 W | 125 GB/s | 102.6 C |
| 75 C | 3 W | 47 GB/s | 112.6 C |

So an isolated stack on 45 C water never throttles on reads alone, whatever the
load; behind a boundary of 55 C and above, sustained reads near the array peak
are paced, and the pacing deepens quickly with the boundary. Decode-dominated
LLM serving reads HBF at 100–240 GB/s per stack in the shipped studies, which
lands exactly in that transition. The thresholds themselves are implementation
choices that a product would set against its own boundary; the sensitivity to
move is the boundary, not the thresholds.

## HBM stacks and the package coupling

The HBF node above decays toward a boundary that the configuration fixes. In
a package the boundary is set by the neighbors: the HBM stacks, which dissipate
the bytes they serve, and the GPU die. The reviewed literature establishes the
mechanism and its weight but publishes no conductances, so the HBM node is
derived like the HBF one and the coupling is an explicit, swept assumption.

**HBM node (`hbm-thermal-*`, enabled in the profiles as telemetry).**

| Key | Value | Basis |
|---|---|---|
| read and write energy | 3.4 pJ per payload bit each | SK hynix (IEDM 2023): HBM2E about 4.3 pJ/bit, HBM3E 20% lower; NVIDIA's HPCA 2017 split of an HBM access (1.48 pJ/b toggle-independent column energy, 2.85 pJ/b data path at 50% toggle of which 0.54 pJ/b I/O, 5.7 pJ/b at 100% toggle) shows reads and writes share the data path and about nine tenths of the energy stays inside the stack |
| resistance, capacitance | 2.0 C/W, 0.25 J/C | the HBF derivation applied to a 12-high DRAM stack (hybrid-bonded junction-to-case 1.5-2.0 C/W plus TIM and cold plate; silicon, base die and mold within the 775 µm budget) |
| static power | 3 W | Base-die PHY, logic and refresh, unpublished; SlugLab assumes 5 W idle; 2-5 W range |
| refresh-doubling and limit temperatures | 85 C, 95 C | the DRAM convention of doubled refresh above 85 C (JESD238 section 6.6 carries the trip points); 95 C is the HBM qualification temperature of the H100 package |

The node reports its temperatures and the stack-time above both thresholds;
HBM timing is never changed by it.

**Package network (`package-thermal-*`, disabled in the profiles; overlays
`package-coupling-liquid-cooled` and `package-cascaded-hbf`).** The GPU die,
every HBM stack and every HBF stack keep their own path to the coolant and are
joined by symmetric conductances per node-class pair, so the continuous
network conserves energy. At each update instant (1 ms) a stack receives the
environment its neighbors impose, 1/R_eff = 1/R + Σg and
T_eff = R_eff (T_amb/R + Σ g T_j), and relaxes toward it with the per-device
closed forms until the next update; holding the neighbors fixed within the
interval is the one discretization, and 1 ms is small against the coupled
time constants (R_eff C: 0.20 s for an HBF stack and 0.17 s for an HBM stack
in the 4H4F profile, 0.9 s for the GPU). The light budget follows the current
environment, floored by `hbf-thermal-minimum-light-budget-w`.

The network boots at its idle steady state: every stack at static power, the
GPU at its configured power, solved as the linear system before the first
operation. That is the state a powered package rests in, and the one the word
"idle" means for a coupled run; booting the stacks at their isolated idle
while the GPU sits at its loaded steady state would open every run with a
warm-up transient of about a second. In the 4H4F profile with the overlay
defaults the idle network rests at 60.2 C (HBF), 60.8 C (HBM) and 77.9 C
(GPU) behind 45 C water, against isolated idles of 49, 51 and 80 C; the HBF
boundary is 58.8 C and the light budget 37.3 W. A ceiling start
(`thermal-steady-state-start`) still pins the HBF stacks at LTT, and the HBM
stacks and the GPU boot at the steady state around them. The HBM node has no
serving-state start of its own, so a window that begins serving at once sees
the HBM stacks warm from that idle toward their serving equilibrium over about
R_eff C ≈ 0.2 s; windows of seconds carry that transient in their first
fraction. A package whose idle rests at or above LTT boots its HBF stacks in
Light mode with the floor budget; one that rests at or above STT is refused,
because the stack would assert CATTRIP before serving a command.

One coolant per package: `hbf-thermal-ambient-c` and `hbm-thermal-ambient-c`
must equal `package-thermal-ambient-c` (a coolant sweep changes all three),
and `hbf-thermal-neighbor-heat-c` must be 0, because the network supplies the
neighbors and a declared heat would count them twice. The engine refuses both
conflicts, so the `thermal-boundary-55c/65c/70c` overlays do not combine with
the package overlays: declare the neighbors or model them. What the sources
support:

- memory-side temperature cannot be predicted from memory power alone: it
  depends on the thermal field of the neighboring compute die and the package
  boundary (review of multiscale 3D-IC thermal modeling, citing Kim et al.,
  ITHERM 2022, on HBM in 2.5D Si-interposer systems);
- GPU coupling is the dominant factor in HBM thermal stress, and a conductive
  plate through the molding compound lowers the HBM junction by 10.3 C (Son et
  al., KAIST, NEMO 2020); spacing GPU and HBM chiplets lowers a 2.5D system by
  4 C in a HotSpot model (TAP-2.5D, Boston University);
- the D2D PHY in the HBM base die is a major heat source: Samsung adds a Heat
  Path Block over it and SK hynix stacks cooling elements on it (Computex
  2026), which is the heat every byte through the HBM interface deposits;
- a die-to-die link costs about 0.3 pJ/bit per end on an advanced package
  (ISSCC 2025, 0.29 pJ/b UCIe link on CoWoS);
- FlashAccel integrates HBF either beside the GPU (CLI) or cascaded through the
  HBM base die (CSI); "HBF Sucks?" shows that a near-tier-first hierarchy hands
  HBF a write-heavy stream (write/read 1.14-4.90×).

Defaults: GPU 700 W behind 0.05 C/W and 20 J/C (a 1 s time constant, booting
at its steady state); HBF-GPU and HBM-GPU 0.3 W/C, HBF-HBM, HBF-HBF and HBM-HBM
0.1 W/C per pair. These are first-order lid-spreading estimates (a 1.5 mm copper
lid under direct liquid cooling spreads heat over about 5 mm) and the quantity
to sweep: 0.05-1 W/C. In the 4H4F profile they put an idle HBF stack at a
boundary of 58.8 C with the GPU at its steady state, inside the 55-65 C band
where the decode sweeps start pacing; 55 C and 65 C water move it to 68.8 C
and 78.8 C (light budgets 22.3 W and 7.3 W).

**Peer against Tiered.** No placement-specific rule is needed. A Tiered
placement writes every byte it misses in HBM into the HBM cache and reads it
back, so its HBM stacks dissipate the hit traffic plus the fills plus the
re-reads and their heat rises with HBF misses, which the network passes on to
the HBF boundary; a Peer placement reads HBF directly, its HBM heat is the hit
traffic alone, and more HBF traffic means less HBM heat. In the 45 C decode
cells of the 128K Llama-3.1-8B study, Tiered LRU moved 3.01 bytes through
HBM plus HBF per logical byte against 1.00 for Peer and Tiered bypass, and its
HBM stacks carried 433 GB/s and 11.8 W against 353 GB/s and 9.6 W for Peer KV
first on 4H4F. The cascaded topology adds a second difference that applies to
both placements: every HBF payload byte also heats the HBM base die it hangs
behind.

**A calibration caveat on R.** Two independent 3D-ICE package models of an HBF
stack imply an effective stack-to-sink resistance well under 1 C/W (SlugLab:
105.9 W to reach 105 C; "HBF Sucks?": 53.7 W at the 80 C limit), against the
2.0 C/W taken here from junction-to-case reviews plus TIM. With 0.7 C/W every
derived budget triples and read-only decode would not pace below a boundary
of about 70 C. The value stays in the sensitivity range until a package
measurement settles it.

## Sources

- FlashAccel: Leveraging High-Bandwidth Flash (HBF) for High-Throughput LLM
  Inference, arXiv 2607.10186 (2026).
- K. Yanagidaira et al., A 1Tb 3b/cell 3D-Flash Memory with a 29%-Improved-Energy-
  Efficiency Read Operation and 4.8Gb/s Power-Isolated Low-Tapped-Termination
  I/Os, ISSCC 2025, paper 30.2; Kioxia's summary at
  kioxia.com/en-jp/rd/technology/topics/topics-83.html.
- Micron, NAND Flash Memory MT29F4G08AAA/MT29F8G08BAA/MT29F8G08DAA/MT29F16G08FAA
  data sheet, Rev. B (2007): DC and operating characteristics.
- HBFSim: Fast and Faithful Simulation of High-Bandwidth Flash Under Real GPU
  Execution, arXiv 2609.09800 (SlugLab, 2026).
- Thermal Management Challenges in 2.5D and 3D Chiplet Integration: A Review on
  Architecture–Cooling Co-Design, MDPI Eng 6(12):373 (2025).
- Thermal Issues Related to Hybrid Bonding of 3D-Stacked High Bandwidth Memory: A
  Comprehensive Review, MDPI Electronics 14(13):2682 (2025).
- SK hynix, Advanced Packaging Technologies in Memory, IEDM 2023 short course
  handout 15-6.
- NVIDIA H100 PCIe GPU Product Brief, PB-11133-001_v02, Table 5.
- Cooling Matters: Benchmarking Large Language Models and Vision-Language Models
  on Liquid-Cooled Versus Air-Cooled H100 GPU Systems, arXiv 2507.16781 (2025).
- Sandisk HBF Fact Sheet (2025); trade reports of the JEDEC HBM4 775 µm height
  budget and the HBM footprint (Tom's Hardware, 2025).
- OCP HBF v0.7.0, sections 9.1–9.2 (junction range, thermal modes, TTTEMP grid).
- N. Chatterjee et al., Architecting an Energy-Efficient DRAM System for GPUs,
  HPCA 2017 (HBM access-energy split).
- A Review of Multiscale Thermal Modeling in Heterogeneous 3D ICs, arXiv
  2604.03290 (2026), citing T. Kim et al., ITHERM 2022, and K. Son et al.,
  IEEE NEMO 2020 (through-mold plate, 10.3 C).
- Y. Ma et al., TAP-2.5D: A Thermally-Aware Chiplet Placement Methodology for
  2.5D Systems (Boston University).
- TrendForce and Tom's Hardware reports of Computex 2026 (Samsung Heat Path
  Block, SK hynix iHBM on the HBM base-die D2D PHY).
- D. Turker Melek et al., A 0.29 pJ/b 5.27 Tb/s/mm UCIe Advanced Package Link
  in 3 nm FinFET with 2.5-D CoWoS Packaging, ISSCC 2025.
- HBF Sucks? A Full-Stack Characterization of High-Bandwidth Flash for
  KV-Centric LLM Serving, arXiv 2608.11668 (2026).
