# Goemon64: instruction-level verification of the stick×basis composition (func_801CE870 + helpers)

Goal: verify the one remaining inference in the v10 patch design — if a patch rotates ONLY the
Camera eye−look_at delta by θ inside `func_801CE4D0` (sp+0x4C/50/54, before the normalize at
0x801CE558), does the player's movement direction rotate by exactly θ, with no other
camera-derived input to the composition?

**VERDICT: YES (qualified).** Exact 1:1 on flat ground (up to the game's own 10-bit yaw
quantization); on slopes the result is exactly what the game would compute for a camera whose
pose is genuinely rotated by θ about world-Y (coherent by construction), because the basis is the
movement system's ONLY camera input. Qualifications in §7.

Evidence files (this scratchpad): `mv_801CE870.c`, `chk_801CE4D0.c`, `chk_801CE3F0.c`,
`hlp_8001D394_1DF94.c`, `hlp_8001D460_1E060.c`, `hlp_8001D5B8_1E1B8.c`, `hlp_8001D718_1E318.c`,
`hlp_8001D898_1E498.c`, `hlp_800040A8_4CA8.c`, `hlp_80003F30_4B30.c`, `hlp_80003CC8_48C8.c`,
`hlp_80003D28_4928.c`, `hlp_80003A94_4694.c`, `x_801D6A98.c`, `x_801D628C.c`, `ctlscan.awk`.
CONFIRMED = direct instruction evidence quoted; INFERRED = marked.

Conventions: player = `*(0x801FC604)`; ctl = `*(player+0x5C)` = 0x8020C860; basis Vec3f at
ctl+0x6C/70/74 (0x8020C8CC); rec = `*(0x8020CA2C)` = processed analog-stick record
(+0xC mag, +0x10/+0x14 planar components). Angle unit: 10-bit binang, 0x400 = full turn
(CONFIRMED: `func_800040A8` = `(atan2raw+0x20)>>6 & 0x3FF`, 0x800040BC-C4; `func_80003F30`
masks its input `andi 0x3FF` at 0x80003F30 and indexes a quarter-wave float table at 0x8006xxxx).

---

## 1. Call-chain plumbing (CONFIRMED)

`func_801CE3F0` (entry): a0=player, a1=**float** speed (sw a1 → lwc1 f4, 0x801CE3FC/0x801CE430).
- Zeroes out-vector player+0x74/78/7C (0x801CE404-40C) — the final movement vector lives
  **inline in the player object at +0x74**, and a1 of the whole chain points there
  (0x801CE49C `addiu a1,a0,0x74`).
- Gate 1: `objA = player->0x64; if (objA->0x5E & 2) return 0x400` (0x801CE400/410/418) —
  a status BYTE on the camera-controller object, movement suppressed entirely; not a pose read.
- Gate 2: rec+0xC == 0 && !(ctl+0x0 & 1) → clear cut-flag `*(0x801FC624)+0xAB`, clear ctl+0x78,
  return 0x400 (0x801CE434-474).
- Passes a2 = −rec10/mag, a3 = rec14/mag (0x801CE47C-494), stack+0x10 = speed, to CE4D0.

`func_801CE4D0` tail (0x801CE810-858) calls `func_801CE870` with:
- a0 = player, a1 = &player+0x74 (out), a2 = &player+0x84, a3 = ctl+0x6C (live basis),
- caller-stack +0x10 = &basis_copy (Vec3f copied from ctl+0x6C/70/74 to sp+0x68/6C/70 at
  0x801CE810-858 — the just-stored/blended basis), +0x14 = −rec10/mag, +0x18 = rec14/mag,
  +0x1C = speed float.
In CE870's frame these appear at sp+0x80/0x84/0x88/0x8C.

## 2. The helpers are pure math (CONFIRMED)

All eight callees read ONLY their pointer/register arguments plus constant data
(epsilon doubles at `lui 0x8008`, sine table at `lui 0x8006`). No 0x8020/0x8021 globals, no
Camera chain, no side effects beyond their output pointers:

- `func_8001D394(v)`: normalize Vec3f in place (guards |v|²∈{0,1}).
- `func_8001D460(v, out, b)`: **world → b-frame** coordinate change. With
  h=√(b.x²+b.z²): out = { (v.x·b.z − v.z·b.x)/h,  v.y·h − b.y·(v.x·b.x+v.z·b.z)/h,  v·b } =
  v expressed in the orthonormal frame {side=(b.z,0,−b.x)/h, up=b×side, fwd=b}. Returns 0
  (with an axis-swap fallback) when b is vertical (h < eps, 0x8001D4A8).
- `func_8001D898(v, out, b)`: same construction but referenced to the (x,y) plane —
  h₂=√(b.x²+b.y²), sign s=±1 by sign of b.y: out = { s·(v.x·b.y − v.y·b.x)/h₂, v·b,
  s·(v.z·h₂ − b.z·(v.x·b.x+v.y·b.y)/h₂) }. Degenerate when b ∥ z.
- `func_8001D718(u, out, b)`: **exact transpose (inverse) of D898** — maps a vector expressed
  in b's frame back to world. Verified term-by-term (out.x at 0x8001D7EC-81C = u.y·b.x +
  s·(u.x·b.y − u.z·b.z·b.x)/h₂, etc. = Mᵀu).
- `func_8001D5B8(a, out, c)`: weighted combine used only on CE4D0's blend path.
- `func_800040A8`: atan2 → 10-bit binang. `func_80003F30(ang, out)`: sin/cos pair via table
  (writes one to *out, returns the other in f0). `func_80003CC8/80003D28`: single trig lookups.
  `func_80003A94`: raw fixed-point atan2 core.

## 3. func_801CE870 composition, instruction-traced (CONFIRMED)

Let n = player+0x84 (normalized IN PLACE at entry, 0x801CE888 jal D394), b = basis,
sx=−rec10/mag, sz=rec14/mag, spd = speed float.

1. `θ_stick = A(−sx, sz) = A(rec10/mag, rec14/mag)` — 0x801CE890-8A0 (neg.s f12 in delay slot),
   result → sp+0x30. **Pure stick.**
2. `D898(&basis_copy, out898, n)` — 0x801CE8AC. out898 = b in n's frame; components
   out898.x (sp+0x64) and out898.z (sp+0x6C) span the plane ⊥ n.
   If √(out898.x²+out898.z²) < eps (b ∥ n; camera looking along the normal):
   final = −θ_stick (0x801CE8C0-908). Else `θ_a = A(−out898.x, out898.z)` (0x801CE910)
   = **azimuth of the camera basis about n** — the projection of camera-forward onto the
   ground plane. This is the ONLY place the camera direction enters the yaw.
3. `D460(n, out460, b)` — 0x801CE924, out460 = n in the camera-basis frame.
   If degenerate (v0==0) or planar mag < eps: final = −(θ_stick+θ_a) (0x801CE92C-9B8).
   Else `θ_b = A(out460.x, out460.y)` (0x801CE990) — tilt of n around the camera axis —
   and **final yaw = θ_b − θ_stick − θ_a** (0x801CE99C-9AC), masked `andi 0x3FF` (0x801CE9C8).
4. Local vector u = { F30ret(yaw)·spd, 0, F30out(yaw)·spd } built at sp+0x40/44/48
   (0x801CE9D4-A10; y forced 0 → in-plane).
5. Final yaw halfword → **ctl+0x8** (0x801CEA44 `sh v1,0x8(t1)`, t1=*(player+0x5C));
   sentinel 0x400 when sx==sz==0 (0x801CEA14-A3C).
6. Override: if ctl+0x0 bit0 (0x801CEA4C-50), u is REBUILT from forced angle ctl+0x2 and
   multiplier ctl+0x4 (0x801CEA5C-A88) — **camera ignored entirely** on this path.
7. `D718(u, player+0x74, n)` (0x801CEAB0-B8): map u from n's frame to world → movement vector.

**No read of the Camera chain in CE870/CE3F0**: exhaustive scan of all 148 files for the
segmented-pointer mask idiom (`lui 0x8FFF`/`ori 0xFFFE`, the only way any code reaches the
render Camera struct) shows that within the base-exe player/movement cluster the ONLY user is
`func_801CE4D0` (funcs_29.c). CD310/CE3F0/CE870/CC4C0/CC000/CEAD4/CF960/CFE24/D0C64 never mask
a Camera pointer. All other 0x8FFF users are the camera-controller cluster (0x801D6xxx.),
view/render code, and per-area overlays. [CONFIRMED]

## 4. What CE4D0 stores (task item) — CONFIRMED, with one correction

- ctl+0x6C/70/74 ← normalize(eye−at) (fast path 0x801CE644-654) or angle-blend toward it
  (0x801CE670-7FC, via D460 relative geometry + (v1−1)/v1 exponential approach + D5B8 + D394).
- **ctl+0x6A ← STICK yaw, not camera yaw** (correction to the task premise): sp+0x44 is
  `A(f12=rec+0x14, f14=rec+0x10)` computed at 0x801CE548-554 from `*(0x8020CA2C)` — the analog
  record — and stored at 0x801CE660. It is last-frame stick-direction memory for the
  snap-vs-blend hysteresis (0x801CE61C-638: |Δ+5| mod 0x400 < 0xB → snap). Camera-independent;
  rotating the basis does not touch it and needs no compensation.
- ctl+0x78 ← blend counter (set 0x4F on cut flag 0x801CE58C, cleared 0x801CE66C,
  decremented 0x801CE80C).
- Nothing else. The raw sp+0x4C delta is consumed only by: D394 normalize (0x801CE558),
  D460 blend geometry (0x801CE568), fast-path copy, D5B8 blend arg (0x801CE7F0). No pitch or
  other camera scalar is stored anywhere.

## 5. Complete consumer map of ctl+0x6A / ctl+0x6C (CONFIRMED)

Method: (a) zero direct-address references exist — exhaustive grep for lui-0x8021 lo16s
−0x3736/−0x3734/−0x3730/−0x372C/−0x3728/−0x3798/−0x379E/−0x379C/−0x37A0 over all files: no hits
(the two −0x37A0/−0x379C lines in funcs_87.c are lui 0x8016 — different global). (b) dataflow
scan (`ctlscan.awk`): every base register loaded from `lw $r,0x5C(...)` and dereferenced at a
ctl offset within 10 instructions, across all 148 files.

ctl+0x6A (stick-yaw memory):
- `func_801CE4D0` — lhu 0x801CE61C (hysteresis compare), sh 0x801CE660 (update).
- `func_801CC000` — sh zero 0x801CC23C (state reset).
- Nothing else. (The many 0x6A hits in 0x801D6xxx/0x801D7xxx are the CAMERA object's own +0x6A
  yaw field — e.g. `func_801D6A98` writes camera yaw = −0x4000 − heading(ctl+0xA4) to ITS
  object at 0x801D6AE0, base = a0 = camera obj, x_801D6A98.c — a different struct.)

ctl+0x6C/70/74 (movement basis):
- `func_801CE4D0` — writer (fast/blend paths).
- `func_801CE870` — sole reader-for-use (a3 + stack copy).
- `func_801CC000` — zero-reset (0x801CC244-260); `func_801CC4C0` — conditional zero-reset
  gated by halfword *(0x8020CA64) (0x801CCE7C-94). Both overwritten by CE4D0 next frame.
- Nothing else reads the basis. It feeds movement only.

ctl+0x8 (CE870's output yaw): read by the movement-state handler family
(func_801DFF70, 801E026C, 801E2740, 801E2800, 801E2D74, 801E3D04, 801DFE44, 801DDC38, ...)
which also read/write the override fields ctl+0x2/ctl+0x4 and heading ctl+0xA4. None of these
appears in the 0x8FFF Camera-reader list → everything downstream of the resolver inherits the
rotated basis consistently. [CONFIRMED for the list; INFERRED that the family is the
walk/run/jump state machine.]

## 6. player+0x84 ("n") provenance — camera-independent [strong INFERRED]

n is the frame vector for the whole composition (surface-normal-like; the second CD310 call
site saves player+0x84/88/8C, substitutes the unit-Y vector, restores after — 0x801CDE5C-EB8).
Writers found: `func_801CC4C0` copies global Vec3f 0x8020CA70/74/78 → player+0x84/88/8C
(0x801CCB58-68, a0 built at 0x801CCA3C/50) and resets it with +0x88=1.0 (0x801CC9C0-DC,
0x801CC948-950); 0x8020CA70's writers are `func_801CF960` (movement-driver callee,
0x801CFDE8-F8), `func_801CFE24` (copies player+0xB4/B8/BC fields), and CC4C0's own
(0,1,0)-reset (0x801CC91C→0x801CC978/97C + 0x801CC9E4). All are movement/collision-cluster
code; none masks a Camera pointer (§3 scan). No camera provenance found.

## 7. The 1:1 argument and qualifications

Rotation R = R_Y(θ) applied to the eye−at delta at sp+0x4C/50/54 (after 0x801CE544, before the
normalize call at 0x801CE558; the atan2 in between at 0x801CE54C is stick-only, so ordering
against it is irrelevant). normalize∘R = R∘normalize, so ctl+0x6C becomes exactly the basis of
a camera rotated by θ (fast path; blend path converges to it, see Q4).

Flat ground (n = (0,1,0), h₂=1): D898's frame is the identity → out898 = b, θ_a = A(−b.x,b.z)
shifts by exactly θ under R_Y; out460 = (0, h', b.y) with h' = horizontal norm of b →
θ_b = A(0,h') invariant under R_Y; D718 = identity. Final yaw shifts by exactly −θ and the
world vector is the θ-rotated original. **Exact 1:1.**

Q1 — quantization: the yaw pipeline is 10-bit (0x400/turn, `andi 0x3FF` at 0x801CE9C8; trig
table indexed by the same grid). Movement rotation lands on that grid (≤ ~0.18° rounding);
the render-side rotation at 80017D8C is float-exact. Sub-perceptual mismatch, inherent to the
game's own movement (it always quantizes).

Q2 — slopes (n ≠ up): movement = azimuth of b about n composed with stick; rotating b about
world-Y shifts that azimuth by ≈θ (exactly θ only for vertical n), and θ_b also moves. But this
is numerically identical to the game's own output for a camera truly at the rotated pose —
view and movement stay coherent as long as the view patch applies the same R_Y(θ) to the
camera orientation. "1:1 vs the ideal world-yaw" is approximate on slopes; "1:1 vs a genuinely
rotated camera" is exact.

Q3 — override path (ctl+0x0 bit0, forced angle ctl+0x2 × ctl+0x4): direction taken verbatim,
camera ignored (0x801CEA4C-A88). Scripted/forced moves neither need nor receive rotation —
consistent, since they ignore the real camera too.

Q4 — blend transient: for ≤0x4F frames after a camera-cut flag (*(0x801FC624)+0xAB), the basis
lags the (rotated) target exactly as it lags any real cut. The snap-vs-blend decision itself
uses only stick data (rec mag < 0.3 via double at 0x8020A970, and ctl+0x6A stick-yaw delta),
so the patch cannot spuriously trigger or suppress blending.

Q5 — gates that bypass the basis entirely: objA->0x5E & 2, rec mag 0, speed 0 → early return
with sentinel 0x400 (no movement). Unaffected.

Q6 — feedback via the game's own camera (dynamic, not a correctness issue): rotated movement
rotates heading ctl+0xA4 (written by CC4C0/CBD88); the camera-controller cluster
(e.g. func_801D6A98: camera yaw target = −0x4000 − ctl+0xA4) follows the heading, so the real
camera swings behind the new direction over time — the same convergence already observed with
v9. The per-frame movement mapping above remains exact throughout.
