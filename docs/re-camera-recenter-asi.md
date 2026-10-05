# RE — Driving-camera "snap back to forward" (CAM-A)

Target: `Mercenaries2.exe`, 53,482,288 bytes, md5 `857b3387d54774a32c1328effb5de4d4`,
ImageBase `0x400000`, flat-mapped so `VA = file offset + 0x400000`.
Static, read-only. All VAs verified against raw bytes (capstone output re-checked).

---

## 0. Which class is the DRIVING camera

The vehicle/seat free-look camera is **CAM-A**:
* vtable `0x00BD2060` — installed at `0x00715D44  mov dword ptr [edi], 0xbd2060`
* ctor `0x00715D20`, `ret 0x3C` (15 float / dword args), object size `0x6D0`
* allocated + populated by the camera factory `0x006A59D0`;
  arg build `0x006A652E..0x006A65A5`, ctor call `0x006A65A5 → 0x00715D20`

Its own per-frame slot 4 is `0x00717A80` → `0x00716E30` (`ret 8`, `[ebp+8]`=obj, `[ebp+0xC]`=dt)
→ shared base `0x0070FD40`.

## 1. The record that feeds it (data-driven source of truth)

Deserializer `0x0065E1D0`, record size `0x50` (`mov eax,0x50; ret` at `0x00640250`).
Commits into a hash map at `0x0065E3B6  mov edi,0x17BCE20 / 0x0065E3BB call 0x64A600`
(not into container `0xDF7B88`/`0xDF7C08`; that map is read only by CAM-A arms of the factory).

Every parameter is fetched **once at load** via `GetSettingFloat 0x00656320`
(`edx` = name ptr, default at `[esp]`, result in `xmm0`) and lands in a fixed record slot.

| call VA | name ptr | name | record slot | exe default |
|---|---|---|---|---|
| `0x0065E1EB` | `0x00BC9D24` | StickLength | +0x00 | 7.5 (`[0x00D2D87C]`) |
| `0x0065E204` | `0x00BC9D30` | StickLengthAtMaxFovSpeed | +0x04 | -1.0 (`[0x00DFDB5C]`) |
| `0x0065E21D` | `0x00BC9D4C` | FocusOffset (Vec3, `0x00656610`) | +0x08..0x14 | — |
| **`0x0065E250`** | **`0x00BC9D58`** | **YawResetTime** | **+0x14** | **3.25 (`[0x00BEB6B4]`)** |
| `0x0065E269` | `0x00BC9D68` | DefaultPitch | +0x18 | -0.25 (`[0x00BEAC14]`) |
| **`0x0065E282`** | **`0x00BC8B70`** | **SpringStrength** | **+0x1C** | **2.0 (`[0x00B92874]`)** |
| **`0x0065E297`** | **`0x00BC9D78`** | **SpringDampening** | **+0x20** | **1.0 (`fld1`)** |
| `0x0065E2B0` … | | FirstPerson*/Fov | +0x24..0x4C | |

Factory→ctor→object (all four "hot" fields), verified byte-for-byte:

```
0x006A6538  movss xmm0,[esi+0x14]            ; record YawResetTime
0x006A6540  mulss xmm0,[0x00BEB5B4]          ;  * (1/12)   <-- 12 Hz -> seconds
0x006A6589  movss [esp],xmm0                 ; arg5
...
0x00715DBA  movss xmm0,[esp+0x24]            ; arg5
0x00715DC0  movss [edi+0x668],xmm0           ; ** CAM-A +0x668 = YawResetTime/12 (seconds)
0x00715D96  movss [edi+0x65C],xmm0           ; ** +0x65C = SpringStrength   (default 2.0)
0x00715DA4  movss [edi+0x660],xmm0           ; ** +0x660 = SpringDampening  (default 1.0)
0x00715DB2  movss [edi+0x664],xmm0           ;     +0x664 = DefaultPitch    (default -0.25 rad)
```

`[0x00BEB5B4]` = `0x41200000`? No — actual value read: `0.083333336` = 1/12. So the record value is
in **twelfths of a second**; stock YawResetTime 3.25 → live `+0x668 = 0.2708 s`.

## 2. Live per-frame object fields — the recenter state machine

All of the following are in `0x00717780` (called every frame from `0x00716E30`).
`esi` = CAM-A object, xmm5 = dt (`[ebp+8]`).

### 2a. Countdown / delay

```
0x0071779C  comiss xmm0,[esi+0x64C]          ; xmm0 = 1e-4      -> "timer already ~0 ?"
0x007177A5  movss  xmm0,[esi+0x614]          ;  pitch rate
0x007177B4  comiss xmm1,[0x00B92B58]         ;  squared vs 0.1
0x007177B9  movss  xmm0,[esi+0x610]          ;  yaw rate
0x007177C8  comiss xmm1,[0x00B92B58]         ;  squared vs 0.1
0x007177CD  mov    byte [esi+0x6C9],bl       ;  clear "armed"
0x007177D3  mov    byte [esi+0x6CB],bl       ;  clear "recentre now"
0x007177E4  movss  [esi+0x64C],xmm0          ;  timer := 0     (fast path, no recentre)
0x007177EC  mov    byte [esi+0x6CB],1
...
0x00717800  movss  xmm1,[esi+0x64C]          ;  read timer
0x00717808  subss  xmm1,xmm5                 ;  -= dt          (max(0,.) via 0x71780C/0x717811)
0x00717814  movss  [esi+0x614],xmm0          ;  kill pitch rate
0x0071781C  movss  [esi+0x610],xmm0          ;  kill yaw rate
0x00717824  movss  [esi+0x640],xmm0          ;  kill pitch vel
0x0071782C  movss  [esi+0x644],xmm0          ;  kill yaw vel
0x00717834  mov    byte [esi+0x6CB],1        ;  "recentre" flag
```

Re-arm (called on **any** look input, or when the vehicle is spinning faster than 2 rad/s
at `0x007178E3  comiss xmm4,[0x00B92874=2.0]`):

```
0x00717923  movss xmm1,[esi+0x66C]           ; 0.5 s floor
0x0071792B  addss xmm1,[esi+0x668]           ; + YawResetTime/12      <-- THE DELAY
0x00717933  movss [esi+0x64C],xmm1           ; timer := 0.5 + delay
```

So `+0x668` **is the delay**, and it is only ever written by the ctor at `0x00715DC0`
(byte-exact-encoding scan for `f3 0f 11 87 68 06 00 00` finds exactly two hits, one is the
ctor; the other is CAM-E's `0x00722CFD` which is a different object layout).
`+0x64C` is written every frame — do NOT patch that.

### 2b. Blend (smoothness)

```
0x00717193  movss xmm0,[esi+0x64C]           ; timer
0x0071719B  divss xmm0,[esi+0x66C]           ; / 0.5           <-- SMOOTHNESS WINDOW
0x007171A6..0x007171CE clamp xmm0 to [0..1]  -> [esp+0x14]  = "k"
0x007171CE  cmp  byte [esi+0x6CA],0
0x007171D7  movss [esp+0x14],xmm2            ; if 6CA==1 -> k = 0 (instant snap)
...
0x007172B4  cmp  byte [esi+0x6CB],0          ; "recentre" mode?
   clear (0x007172ED, free-look integrate):
0x007172ED    addss xmm0,[esi+0x620]
0x00717312    movss [esi+0x620],xmm0         ; clamp to [-1.2 .. 0.25]
   set   (0x007172BD, exponential recentre):
0x007172BD    movss xmm0,[esp+0x14]          ; k
0x007172CB    subss xmm1,xmm0                ; 1-k
0x007172CF    mulss xmm0,[esi+0x620]         ; k*current
0x007172D7    mulss xmm1,[esi+0x664]         ; (1-k)*DefaultPitch
0x007172DF    addss xmm1,xmm0
0x007172E3    movss [esi+0x620],xmm1         ; write
```

The blend is **linear in the timer**, not exponential: k goes `1 → 0` over
`[+0x66C] = 0.5` seconds. That is why the last 0.5 s feels abrupt: the whole correction is
compressed into that fixed window, and `[+0x66C]` is written **only in the ctor**
(`0x00715DD0  movss [edi+0x66C],[0x00BBB99C=0.5]`) and is a live, per-frame readable field.

### 2c. Spring (a separate smoothing the same class runs)

```
0x00716BAC  fld  dword ptr [esi+0x660]       ; SpringDampening (stock 1.0)
0x00716BB2  mulss xmm0,[esi+0x65C]           ; SpringStrength   (stock 2.0)
0x00716BBA  fstp dword ptr [esi+0x62C]       ; live copy
0x00716BC0  movss [esi+0x628],xmm0           ; live copy
```

`+0x65C / +0x660` are ctor-only writers (`0x00715D96 / 0x00715DA4`, byte-scan confirms);
`+0x628 / +0x62C` are rewritten every frame from them and are consumed by the
shared-base spring at `0x0070FD40`. So retuning `+0x65C/+0x660` **is** sufficient — no need to
write the derived ones.

## 3. Delay vs smoothness — decoupled, in different fields

| knob | live field | stock value | written by | read by |
|---|---|---|---|---|
| DELAY before recenter starts | `obj+0x668` | `YawResetTime/12 = 0.2708 s` | ctor `0x00715DC0` (record) | `0x0071792B addss` (re-arm) |
| SMOOTHNESS of the return sweep | `obj+0x66C` | `0.5 s` | ctor `0x00715DD0` (hard-coded const) | `0x0071719B divss`, `0x007178F4 comiss`, `0x00717923 movss` |
| Recentre blend target (pitch) | `obj+0x664` | `-0.25 rad` | ctor `0x00715DB2` | `0x007172D7 mulss` |
| Spring stiffness | `obj+0x65C` | `2.0` | ctor `0x00715D96` | `0x00716BB2 mulss` |
| Spring damping | `obj+0x660` | `1.0` | ctor `0x00715DA4` | `0x00716BAC fld` |
| Kill switch (instant snap) | `obj+0x6CA` byte | 0 | `0x0071791F`, event `0x5D4C76BF→0x00717B7E` | `0x007171CE`, `0x007177D9` |

**Two independent scalars.** Delay is the extra hold on top of a fixed 0.5 s blend; the
blend duration is `obj+0x66C` and is *not* a data-driven name — it's compiled in from
`[0x00BBB99C]`. So the "no input for N s then recenter" timer is real (`obj+0x64C`,
decremented at `0x00717808`) and its arm value is `obj+0x66C + obj+0x668`.

## 4. Slider guidance

* **Slider "Recenter delay"** → write `float` at `+0x668` on every live CAM-A object.
  Code accepts any non-negative float (used only in `addss xmm1,[+0x668]`).
  Range: **0.0 – 5.0** (seconds). Stock ≈ 0.2708.
  If you want to expose it as "YawResetTime", divide by 12 first.
* **Slider "Recenter smoothness / duration"** → write `float` at `+0x66C`.
  Read as a divisor and as a compare threshold, so must be > 0.
  Range: **0.5 – 6.0** seconds (stock 0.5). Values below `dt·2` will produce
  `k = timer/tick > 1`, but `0x007171A6..0x007171CE` clamps to `[0,1]`, so it degrades safely.
* **Slider "Spring"** → write `+0x65C` and `+0x660`.
  Range: strength **0.5 – 20.0** (stock 2.0), damping **0.1 – 10.0** (stock 1.0).
  These feed the shared spring at `+0x628/+0x62C` each frame — no separate patch needed.

**Do not** write `+0x64C` (per-frame countdown, `0x007177E4 / 0x00717911 / 0x00717933`).
**Do not** try to retune through the GetSettingFloat store — post-load writes to the store are
inert (see prior memory note); you must write the object field.

## 5. Explicitly NOT PROVEN (do not ship against these)

* **Where the YAW re-centring back onto the vehicle heading actually happens.**
  Everything traced here is on `obj+0x620` (blended toward `+0x664 = DefaultPitch`, i.e. a
  *pitch* recentre) and on the `+0x64C` timer which is armed/checked in the same block.
  The timer's name (`YawResetTime`) and its single shared use strongly suggest both axes
  read `k` from `[esp+0x14]`, but I did not find a symmetric `mulss/addss` block for yaw
  inside `0x00717130` (the block at `0x0071721F/0x00717275` computes a *pitch* increment
  from `[esi+0x14]` and `[+0x644]` and stores xmm0 which then only feeds `[+0x620]`).
  A plausible continuation is inside the shared base `0x0070FD40` or the caller
  `0x00716E30` block that runs between the timer state machine and the final quaternion
  build; I did not confirm it byte-for-byte.
* **`+0x66C` has no data-driven source.** It is only ever written by
  `0x00715DD0 movss [edi+0x66C],[0x00BBB99C]`. To slow the sweep you either write
  `obj+0x66C` per frame, or patch the `[0x00BBB99C]` constant (affects anything else
  that reads it — I did not enumerate those readers).
* **CAM-A vs. other classes while driving.** I confirmed CAM-A is constructed from the
  vehicle record that has `FirstPersonOffset`/`FirstPersonReverseOffset` (i.e. a driving rig),
  and the SEAT container `0xDF7B88` uses a different record with
  `YawLag/PitchLag/RollLag` (unhandled here). I did **not** prove which live camera object
  the game binds while the player is *just driving without aiming*; that requires a runtime
  or a link from the "state = in vehicle" branch of the camera manager to
  `0x00715D20`.
* **`0x0065E2E3` (`0x0065E32F`, …) blocks past record offset +0x20** (FirstPersonPitch,
  FirstPersonReverseOffset, FirstPersonReversePitch, DefaultFov, MaxFov, FovMaxSpeed) —
  mapped from prior session but not re-verified in this pass.
* **`+0x640`, `+0x644`** are per-frame velocity accumulators written at `0x007162C7/0x007162CF`;
  they are zeroed on recentre (`0x00717824/0x0071782C`) and used as free-look rates at
  `0x00717282/0x007172A8`. **Not** tunable — the store is per-frame.
