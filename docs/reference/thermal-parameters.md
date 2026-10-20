# HBF thermal parameters: derivation and sources

> Status: Current
> Last reviewed: 2026-10-20

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
