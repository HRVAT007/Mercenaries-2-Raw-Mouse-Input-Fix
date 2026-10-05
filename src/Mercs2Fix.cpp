// Mercs2Fix.asi - Mercenaries 2: World in Flames PC fix
// Drop next to Mercenaries2.exe (requires Ultimate ASI Loader dinput8.dll)
// Configuration lives in Mercs2Fix.ini beside this file; every behaviour below
// has its own switch there, and the risky ones default off.
//
// Features:
//   - Borderless: forces the D3D9 device windowed (FullScreen_RefreshRateInHz=0)
//     and sizes the window to the monitor, giving true borderless at whatever
//     resolution the game itself was started at.
//   - Camera and aim behaviour, all patched at byte-verified sites in the exe:
//     mounted-weapon turn rate and command response, the crouch/slide pitch lock,
//     the car camera's re-centre, the aim-assist snap, and the lock-on hold.
//   - Shadow map resolution and cast distance.  These need the shader tool
//     (tools/shadow_res.py) as well as the ini, because the atlas size is baked
//     into data/shader3.bin.
//   - Settings panel on INSERT for everything worth changing while playing.
//   - Mercs2Fix.log beside this file.

#define WIN32_LEAN_AND_MEAN
// The version the log banner and the INSERT panel header both print, so neither can go stale on
// its own again (the banner said v62 through v64, and the panel has said v60 since v60).
#define MF_VERSION "76"

#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <intrin.h>

#define LOG_FILE "Mercs2Fix.log"
#define INI_FILE "Mercs2Fix.ini"
#define GAME_PID_WAIT_MS 60000

// ---------------- Logging ----------------
static CRITICAL_SECTION g_logCs;
static char g_moduleDir[MAX_PATH] = {0};

static void Log(const char* fmt, ...) {
    EnterCriticalSection(&g_logCs);
    char path[MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%s\\%s", g_moduleDir, LOG_FILE);
    FILE* f = fopen(path, "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------- Config ----------------
struct Config {
    bool borderless;
    bool forceWindowedD3D;
    bool vsyncOff;
    bool aimAssistDisable;
    // Camera-behavior block layout, VERIFIED from the loader at 0x65EAF0 (which
    // names each field as it fills the 56-byte struct inserted into map
    // 0x17BCEC0), and confirmed by the live values matching the code defaults
    // for Fov/CrouchFov/CameraShakeScale/AimDisruptScale:
    //   +00 HorizontalAimAssistClose  +04 HorizontalAimAssistFar
    //   +08 VerticalAimAssistClose    +0C VerticalAimAssistFar
    //   +10 YawSpeed                  +14 PitchSpeed
    //   +18 CrouchYawSpeed            +1C CrouchPitchSpeed
    //   +20 RotationSpeed             +24 AimAssist
    //   +28 Fov                       +2C CrouchFov
    //   +30 CameraShakeScale          +34 AimDisruptScale
    // v11/v12 zeroed +04..+10 under an off-by-four guess, which included
    // YawSpeed -> the camera could not rotate and firing appeared dead.
    int  aimZeroFields;       // zero the four aim floats at +00..+0C
    int  aimZeroMode;         // zero the AimAssist field at +24
    int  aimWatchdog;         // re-apply to the live map every 200ms
    int  aimPatchAtInsert;    // sanitize the block before the game caches it
    int  insertLogging;       // read-only INSERT logging in the 0x64A600 hook
    float aimMinYawPitch;     // floor for +10..+1C (removes mounted-weapon damping); 0=off
    int  aimDefaults;         // legacy/inert
    int  aimStub;             // legacy/inert
    // v34: live aim-assist call-site patches from static analysis.
    // NOP the E8 call at RVA 0x8F445F inside the per-frame camera update
    // (0x8F4380) that invokes the aim-assist pass when flag bit 4 is set.
    int  aimNopCallSite;      // 1 = NOP the 5-byte call at 0x8F445F    // Diagnostic: log calls to the candidate live aim-assist function 0x8F3050.
    // Installs an inline hook that increments a counter and logs periodically.
    int  aimDiagHook;         // 1 = install diagnostic hook at 0x8F3050
    // v41: patches on the addresses proven by the runtime RE
    // (re-notes/B-camera-runtime.md).  Every one is ini-gated and off except
    // the cone, which is a single .data float.
    int  aimConeDisable;      // [AimAssist] ConeDisable   - strength selector, off by design
    int  aimNopStore;         // [AimAssist] NopAssistStore - drop the assist yaw store
    int  aimKillWeight;       // [AimAssist] KillWeight    - NOP the stickiness store
    int  turretScaleOne;      // [Turret] MountedScaleOne  - 0.25 -> 1.0 operand
    int  turretNeverManned;   // [Turret] NeverManned      - flag store 1 -> 0
    // v43: the two mounted dampeners that were still untouched.  Manned camera
    // motion is slowed by THREE separate mechanisms, all keyed on obj+0x7DD;
    // MountedScaleOne only ever defeated the third one.
    int  turretNormalParams;  // [Turret] ForceNormalParams - mounted 14-float set -> normal set
    int  turretNormalSmooth;  // [Turret] ForceNormalSmooth - mounted smoothing coefficient -> normal
    // v44: shadow atlas size.  The map is created from literal immediates only, so this
    // is a scale factor over 15 separate operands that MUST move together.
    int  shadowMapScale;      // [Shadow] MapSizeScale  - 1 = untouched, 2 = 2048x8192
    // v49: shadow CAST distance.  ShadowBaseDistance is a real float at
    // [[0x00DFC2F8]+0x2BC0] with exactly one consumer in all of .text (0x00859322),
    // which builds the four LOD caster bands as base * 3^i.  Scaled at the consumer,
    // so the shipped value is never written and the default 1.0 leaves the byte stream
    // untouched.
    float shadowBaseScale;    // [Shadow] BaseDistanceScale
    // v61: the water tile height record.  Both publishers divide by a per-tile height span with
    // no zero test, and a zero span makes the published scalar +-Inf or NaN, which is what the
    // "water disappears / vertical sheets of triangles" symptom is made of.  See
    // ApplyWaterHspGuard for the two verified sites and the guard's register budget.
    int   waterGuardHsp;      // [Water] GuardFlatHeightTiles
    // v62: the read-only fade-divide tap.  On by default because it writes nothing to the game -
    // it only folds maxima into our own globals - and because the v61 guard's own counters came
    // back negative, so the next clue has to come from measurement.
    int   waterTapFade;       // [Water] TapFadeDivides
    // v64: read-only tap on the ONE divide that reaches the water shaders every frame.
    // See ApplyProjNearFarTap.  Same rule as the fade tap: it measures, it never substitutes.
    int   waterTapProj;       // [Water] TapProjNearFar
    // v66: read-only tap on the ONE geometry number the CPU still ships to the water patch every
    // frame: the 72-byte subdivision grid frame (Corner0/DeltaU/DeltaV1/DeltaV2/DeltaUV).  Every
    // divide that could produce an absurd height has now been measured and cleared, the GPU-side
    // water surfaces are fixed-size, and the ocean's per-vertex Y is computed in vs_3_0 from this
    // frame - so the frame itself is the last unmeasured input, and the only mechanism found that
    // can depend on view angle and resolution at once.  See ApplyWaterGridFrameTap.
    int   waterTapGrid;       // [Water] TapGridFrame
    // v69: the LOD measurement.  Raising the draw distance makes buildings and big props show up as
    // empty space, and the only gate found that drops an object completely is the 16-bit mask word at
    // object+idx*36+0x352 - see ApplyLodResultTap for the verified pipeline.  Read-only.
    int   lodTapResult;       // [Lod] TapResult
    // v69: the matching fix, off by default so this build measures first.  -1 = no detour installed;
    // 0..22 = rewrite the lod byte before the mask is built, forcing every object to at most that LOD.
    // v70: with the LOD gate cleared, the only remaining way for NEARBY geometry to leave the screen
    // is the clip volume, so read the near/far pair every camera pass actually produces.  Read-only:
    // nothing in the image derives a near plane from [View] Distance, which is a static claim, and a
    // number settles it.  See ApplyProjParamsTap for the verified tail site.
    int   projTap;            // [View] TapProjParams
    // v71: the shared draw-submit arena holds 8192 batch records and the game RETURNS without
    // submitting when it is full (0x008546AF -> 0x008548DF).  Counting that gate is the only way to
    // tell a capacity loss from a numeric ceiling, and both are read-only.  See ApplyBatchGateTap.
    int   batchTap;           // [View] TapBatchGate
    // v74: the symptom is view-angle dependent and pops hard, so it is a branch fed by the view
    // transform, not a distance band.  The skinned submit has exactly one such branch; this counts
    // it.  Read-only.  See ApplySkinFadeTap.
    int   skinFadeTap;        // [Lod] TapSkinFade
    // v45: the reticle HOLD, which is NOT the v42 assist.  A separate state machine at
    // 0x0071D0A0 (one call site, 0x0071DE2B) acquires a target and overwrites the yaw
    // accumulator obj+0x618 with the clamped lock value; free aim only ADDs to that same
    // accumulator afterwards (0x0071DE59), so discarding the lock's store leaves the mouse
    // path completely intact.
    int  lockOnHoldNop;       // [AimAssist] NopLockOnHold
    // v45: button prompts.  0x014A3818 is a plain global input-mode (0 = keyboard+mouse,
    // 1 = pad, 2 = pad with XInput) written by exactly four absolute stores, two of which
    // claim the pad.  Zeroing those two immediates keeps every reader and the whole
    // detection state machine running and just makes it land on the KB&M prompt set - which
    // the exe already has art for (pad->keyboard sprite remap table at 0x005BB953).
    int  keyboardPrompts;     // [Prompts] KeyboardIcons
    // v58: the crouch/slide PITCH gate.  Inside HUMAN's slot-4 update (0x0071DC40) a branch
    // at 0x0071DE11 sends the frame to one of two camera paths:
    //     jne 0x0071DE90   when the state byte is set   -> yaw-only path
    //     fall through 0x0071DE13                       -> the normal free-aim path
    // The byte that decides it was verified at the instruction level for v58: 0x0071DDB0 calls
    // 0x0071D790, 0x0071DDB5 loads the result with `mov al,[esp+0x17]`, and that same al is still
    // live at the gate's `test al,al` (0x0071DDE0..0x0071DE0B is pure SSE on xmm0/xmm1/xmm2, which
    // cannot write al).  What 0x0071D790 reads to fill it was not traced, so no source field is
    // claimed here.
    // The frame's pitch delta is written to [esp+0x1c] and the ONLY consumer of that slot is
    // 0x0071DE69 `addss xmm0,[esp+0x1c]`, on the fall-through side; the taken side consumes
    // [esp+0x18] (yaw) at 0x0071DE90 and re-derives pitch from the subject's own matrix
    // instead.  That asymmetry is the reported symptom exactly: yaw keeps working, pitch does
    // not, and it returns a moment later because it is a state flag, not a timer.
    // NOP-ing the two-byte jne forces the free-aim path always.  It is init-only and NOT
    // menu-togglable: the branch is on the per-frame hot path, and writing two bytes there at
    // runtime could be read half-applied by the game as `nop` + `jg <garbage>`.
    int  freePitchCrouch;     // [Camera] FreePitchInCrouch
    // v59: the VEHICLE camera's re-centre.  CAM-A (vtable 0x00BD2060) keeps the wait before it
    // starts swinging back at +0x668 and the blend window it swings over at +0x66C, and both are
    // written only inside its ctor - so these two scales are applied there, which makes them
    // "next vehicle you enter" rather than live.  See ApplyVehicleRecentre.
    float vehDelayScale;      // [Vehicle] ResetDelayScale
    float vehBlendScale;      // [Vehicle] ResetBlendScale
    // v56: THE turret limiter, found by measurement and verified at the byte level.
    // The turret camera is CAM-E and its per-frame aim delta is
    //     delta = [obj+0x668] * [obj+0x648] * dt
    // with [obj+0x668] = [obj+0x66C] = 1.5 rad/s = 85.9 deg/s, a ctor-only value read
    // straight out of game data (never changes during play).  So the turret can never
    // turn faster than ~86 deg/s no matter how hard the mouse is pushed - a 180 degree
    // turn takes at least 2.1 s.  This scales that constant.  See ApplyTurretTurnRate.
    float turretTurnRate;     // [Turret] TurnRateScale
    // v62: a FLOOR on the aim rate, not a multiplier.  CAM-E's rate constant is not a constant at
    // all - the ctor copies it out of a per-mount parameter record, and the CAMREC census read base
    // rates from 0.35 up to 1.5 rad/s across 44 records.  A fixed multiplier therefore cannot
    // equalise mounts: the 105mm artillery's 0.35 x 3.0 = 1.05 rad/s is still slower than the
    // turret's STOCK 1.5, which is exactly the "same slow aim, different weapon" report.  The cave
    // now computes max(base * TurnRateScale, MinRateRad), so every mount is lifted to at least the
    // speed the light turret already ships and the light turret is unchanged (its base is 1.5, and
    // max(1.5 * scale, 1.5) == 1.5 * scale for any scale >= 1).
    float turretMinRate;      // [Turret] MinRateRad
    // v62: CAM-B is the only other class in the family with CAM-E's limiter shape - verified
    // byte-exact at 0x007197AD, where the per-frame yaw delta is [ebx+0x658] * [ebx+0x640] * dt,
    // preceded by the same shared lag advancer call 0x0070D4C0.  Its rate lives at +0x658, NOT
    // +0x668, which is why the CAM-E patches could never have reached a CAM-B-served mount.
    // Off by default: no capture has yet shown a live CAM-B owning a slow aim, so this is the
    // instrumented option rather than a shipped behaviour change.
    int   camBRateOn;         // [Turret] PatchCamBTurnRate
    // v57: the FIRST-ORDER LAG on the aim command, which is the limiter v56 did not touch.
    // See ApplyTurretCommandLag.  turretCmdBias is 1 - turretCmdResponse, precomputed so the
    // cave can express X' = X*K + (1-K) with two aligned float reads.
    float turretCmdResponse;
    float turretCmdBias;
    // v58: the in-game menu.  menuEnabled both creates the overlay and arms the optional
    // detours at their inert value, which is what makes their sliders live.
    int   menuEnabled;          // [Menu] Enabled
    int   menuSave;             // [Menu] SaveToIni  - write changed values back
    // v58: the far clip plane, live.  [0x00DFC348] holds the int the game read from
    // Mercs2.ini [Render] ViewDistance at startup and the camera re-reads it every update
    // (0x007141EF/0x007141F4), with exactly one consumer, so writing it is the same lever as
    // the ini key without the restart.  0 = leave the game's own value.
    int   viewDistance;         // [View] Distance
    // v39: generic loader-read interception.  See the GSF block for why the
    // load-time call sites are the only upstream point for these values.
    int  setEnabled;          // [Settings] Enabled  - install the stubs
    int  setLogWatch;         // [Settings] LogWatch - log watched reads + values
    int  setHookAll;          // [Settings] HookAll  - hook all 669 named sites
    int  setPatchSites;       // v39 site stubs; v39 proved the sites never fire
    int  setLogHashNames;     // log each watched name as it is hashed
    // Seat/turret records committed through 0x649180 (containers 0xDF7B88,
    // 0xDF7C08, 0xDF7C88).  v20 proved the camera-behavior hash map is NOT the
    // source of aim assist or mounted dampening; these containers are.  These
    // builds are read-only dumpers: they record raw dwords so the name->slot
    // mapping is confirmed from real values before any write ships.
    int   seatLogging;     // dump seat records as they are committed
    int   seatMaxRecords;  // per-container cap (a capture burst ignores it)
    // v22 evidence: the game re-commits seat records ~30/s during play, so a
    // capture-window install loses nothing while keeping the detour out of
    // normal gameplay entirely.  That matters because 0x649180 is a shared
    // engine commit path (~190 call sites) and the seat hook is the only
    // structural change between v20 (mouse worked in game) and v21 (it did not).
    int   seatDefer;       // 1 = arm the detour only while capturing
    int   seatZeroFarAim;  // 1 = write 0.0 to +54 (FarAimAssist) on 0xDF7C08 records
    float seatCloseAimMin; // floor for +50 (CloseAimAssist); 0=off, 0.2=min shipped value
    float seatYawInertiaMax; // ceiling for +34 (YawNoInputInertia); 0=off, 1.0=safe shipped value
};
static Config g_cfg;

static int IniInt(const char* sec, const char* key, int def) {
    char path[MAX_PATH]; _snprintf(path, sizeof(path) - 1, "%s\\%s", g_moduleDir, INI_FILE);
    return (int)GetPrivateProfileIntA(sec, key, def, path);
}
static float IniFloat(const char* sec, const char* key, float def) {
    char buf[64] = {0}; char path[MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%s\\%s", g_moduleDir, INI_FILE);
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), path);
    if (!buf[0]) return def;
    return (float)atof(buf);
}
static void LoadConfig() {
    g_cfg.borderless        = IniInt("Borderless", "Enabled", 1) != 0;
    g_cfg.forceWindowedD3D  = IniInt("Borderless", "ForceWindowedD3D", 1) != 0;
    g_cfg.vsyncOff          = IniInt("Borderless", "VsyncOff", 0) != 0;
    // v75 RELEASE DEFAULTS.  Every dev-time switch that defaulted ON in earlier builds now
    // defaults OFF: they were measurement probes, and shipping a slim config would hand those
    // paths back to players.  Defaults match the validated live config; every key still works
    // and the annotated docs/Mercs2Fix.annotated.ini explains what each one re-enables.
    g_cfg.aimAssistDisable  = IniInt("AimAssist",  "Disable", 1) != 0;
    g_cfg.aimZeroFields     = IniInt("AimAssist",  "ZeroFields", 0);   // v20: works, but changes nothing audible
    g_cfg.aimZeroMode       = IniInt("AimAssist",  "ZeroAimMode", 0);   // proven to kill camera rotation
    g_cfg.aimWatchdog       = IniInt("AimAssist",  "Watchdog", 0);
    g_cfg.aimPatchAtInsert  = IniInt("AimAssist",  "PatchAtInsert", 0);
    g_cfg.insertLogging     = IniInt("AimAssist",  "InsertLogging", 0);
    g_cfg.aimMinYawPitch    = IniFloat("Camera",   "MinYawPitch", 0.0f);
    g_cfg.aimDefaults       = IniInt("AimAssist",  "DefaultPatches", 0);
    g_cfg.aimStub           = IniInt("AimAssist",  "CallSiteStubs", 0);
    g_cfg.aimNopCallSite    = IniInt("AimAssist",  "NopCameraCall", 0);
    g_cfg.aimDiagHook       = IniInt("AimAssist",  "DiagHook", 0);
    g_cfg.aimConeDisable    = IniInt("AimAssist",  "ConeDisable", 0) != 0;
    g_cfg.aimNopStore       = IniInt("AimAssist",  "NopAssistStore", 1) != 0;
    g_cfg.aimKillWeight     = IniInt("AimAssist",  "KillWeight", 0) != 0;
    g_cfg.turretScaleOne    = IniInt("Turret",     "MountedScaleOne", 1) != 0;
    g_cfg.turretNeverManned = IniInt("Turret",     "NeverManned", 0) != 0;
    g_cfg.turretNormalParams = IniInt("Turret",    "ForceNormalParams", 1) != 0;
    g_cfg.turretNormalSmooth = IniInt("Turret",    "ForceNormalSmooth", 1) != 0;
    {
        int s = IniInt("Shadow", "MapSizeScale", 1);
        g_cfg.shadowMapScale = (s == 2 || s == 4 || s == 8) ? s : 1;
        float f = IniFloat("Shadow", "BaseDistanceScale", 1.0f);
        // Anything outside [1,8] is treated as "off": a scale below 1 would shrink cast
        // distance, which nobody asked for, and a large one just stops casting shadows.
        g_cfg.shadowBaseScale = (f >= 1.0f && f <= 8.0f) ? f : 1.0f;
    }
    // On by default: this is a NaN guard, not a behaviour change.  It can only alter a tile whose
    // division did not produce a finite number, and it counts every time it does, so one run with
    // the capture key pressed tells us whether the theory is right before anything else is tried.
    // Also on by default, and also a measurement rather than a behaviour change: the fade tap only
    // reads, so the worst it can do is cost a few cycles per render-state write.  It is the only
    // way the next capture answers the water question with numbers instead of another guess.
    // v64: same argument as the fade tap.  The 2026-10-03 batch-ring measurement killed the
    // 16-bit vertex cursor (peak 3159 of 62464) and the 8192-record silent drop (peak 3856), so
    // the only water-path arithmetic still standing is the per-frame far/(far-near) that becomes
    // the projection z-row and is uploaded into the water shaders' `projectParams`.
    // v66: every divide on the water path has now been measured and cleared, and the GPU-side
    // water surfaces are hardcoded 128x128, so the only geometry number the CPU still ships to the
    // water patch each frame is the 72-byte grid frame.  It is the last unmeasured input and the
    // only mechanism found that can vary with both view angle and resolution.  Read-only.
    {
        // Off is "the key is absent or out of range", NOT "-1": GetPrivateProfileIntA skips a leading
        // '-' and would read MaxLod=-1 as 1, silently arming the clamp.  99 can never be a real LOD
        // index, so it is a safe absent-value sentinel and any 0..22 is taken literally.
    }
    g_cfg.lockOnHoldNop     = IniInt("AimAssist",  "NopLockOnHold", 1) != 0;
    g_cfg.keyboardPrompts   = IniInt("Prompts",    "KeyboardIcons", 0) != 0;
    {
        // Stock is 1.0 (inert - nothing is written).  Anything outside [1,10] is treated as
        // off: below 1 would make the turret slower than shipped, which nobody asked for,
        // and a large one makes it unaimable rather than responsive.
        float r = IniFloat("Turret", "TurnRateScale", 1.0f);
        g_cfg.turretTurnRate = (r >= 1.0f && r <= 10.0f) ? r : 1.0f;
    }
    {
        // v62: the artillery fix.  Default is 1.5 rad/s = the light turret's own shipped rate, so
        // the floor cannot make any mount slower than shipped and cannot change the light turret
        // at all - it only lifts mounts whose record carries a slower base.  0 removes the floor.
        float r = IniFloat("Turret", "MinRateRad", 1.5f);
        g_cfg.turretMinRate = (r >= 0.0f && r <= 10.0f) ? r : 1.5f;
    }
    g_cfg.camBRateOn        = IniInt("Turret",     "PatchCamBTurnRate", 0) != 0;
    {
        // Stock is 1.0 (inert - nothing is written).  The upper bound is not arbitrary: with
        // the measured shipped inertia of 0.90 the transform X' = 1-(1-X)*K reaches exactly 0
        // at K=10, i.e. the command becomes the target with no lag at all.  Past that X' goes
        // negative, the lag turns into an overshoot and the aim would oscillate, so larger
        // values are refused rather than clamped - the cave also carries a maxss against 0.0
        // so a surprise shipped inertia cannot make it unstable.
        float r = IniFloat("Turret", "CommandResponse", 1.0f);
        g_cfg.turretCmdResponse = (r >= 1.0f && r <= 10.0f) ? r : 1.0f;
        g_cfg.turretCmdBias = 1.0f - g_cfg.turretCmdResponse;
    }
    g_cfg.menuEnabled       = IniInt("Menu", "Enabled", 1) != 0;
    g_cfg.menuSave          = IniInt("Menu", "SaveToIni", 1) != 0;
    // 0 means "do not touch what the game read from Mercs2.ini [Render] ViewDistance".  The
    // ceiling here is depth precision, not the data type: near is a hard 0.2, so the depth
    // ratio grows with the far plane and past a point distant faces start fighting for the
    // same z values.  600 is the top of the range that has been driven without a report of
    // that; anything larger is refused rather than clamped.
    {
        int v = IniInt("View", "Distance", 0);
        g_cfg.viewDistance = (v >= 0 && v <= 600) ? v : 0;
    }
    // Default ON: the lock is a pure loss of control (vertical aim is thrown away while the
    // crouch/slide state byte is set) and removing it is a 2-byte length-preserving NOP of a
    // branch that has been verified against the shipped exe.  Set to 0 to get it back.
    g_cfg.freePitchCrouch   = IniInt("Camera",     "FreePitchInCrouch", 1) != 0;
    {
        // Both are pure multipliers on CAM-A's ctor constants, so 1.0 is exactly "shipped".
        // The shipped values are 0.2708 s of wait and a 0.500 s blend; the range below lets the
        // delay reach ~2.2 s and the blend ~4 s, which is as far as the re-centre can be dragged
        // before it starts fighting the next look input instead of smoothing out.  Outside
        // [1,8] is treated as off rather than clamped, same rule as the shadow scale.
        float d = IniFloat("Vehicle", "ResetDelayScale", 1.0f);
        g_cfg.vehDelayScale = (d >= 1.0f && d <= 8.0f) ? d : 1.0f;
        float b = IniFloat("Vehicle", "ResetBlendScale", 1.0f);
        g_cfg.vehBlendScale = (b >= 1.0f && b <= 8.0f) ? b : 1.0f;
    }
    g_cfg.setEnabled        = IniInt("Settings",   "Enabled", 0);
    g_cfg.setLogWatch       = IniInt("Settings",   "LogWatch", 0);
    g_cfg.setHookAll        = IniInt("Settings",   "HookAll", 0);
    g_cfg.setPatchSites     = IniInt("Settings",   "PatchSites", 0);
    g_cfg.setLogHashNames   = IniInt("Settings",   "LogHashNames", 0);
    g_cfg.seatLogging       = IniInt("Seat",       "Logging", 0);
    g_cfg.seatMaxRecords    = IniInt("Seat",       "MaxRecords", 24);
    g_cfg.seatDefer         = IniInt("Seat",       "DeferToCapture", 0);
    g_cfg.seatZeroFarAim    = IniInt("Seat",       "ZeroFarAimAssist", 0);
    g_cfg.seatCloseAimMin   = (float)IniFloat("Seat", "CloseAimAssistFloor", 0.0f);
    g_cfg.seatYawInertiaMax = (float)IniFloat("Seat", "YawNoInputInertiaCeiling", 0.0f);
}

// ---------------- Vtable hook ----------------
struct VHook { void** vtable; int index; void* original; };
static bool HookVtable(void** vtable, int index, void* hook, VHook* out) {
    if (!vtable || !hook || !out) return false;
    void** slot = &vtable[index];
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    out->vtable = vtable; out->index = index; out->original = *slot;
    *slot = hook;
    DWORD tmp; VirtualProtect(slot, sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

// ---------------- IAT hook ----------------
static void* HookIAT(HMODULE mod, const char* dllName, const char* funcName, void* hook) {
    if (!mod) return nullptr;
    auto dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) return nullptr;
    auto desc = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + importDir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* name = (const char*)((BYTE*)mod + desc->Name);
        if (_stricmp(name, dllName) != 0) continue;
        IMAGE_THUNK_DATA* origThunk = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)((BYTE*)mod + desc->OriginalFirstThunk) : nullptr;
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)((BYTE*)mod + desc->FirstThunk);
        for (; thunk->u1.Function; ++thunk, (origThunk ? ++origThunk : origThunk)) {
            if (!origThunk) break;
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto imp = (IMAGE_IMPORT_BY_NAME*)((BYTE*)mod + origThunk->u1.AddressOfData);
            const char* fName = (const char*)imp->Name;
            if (strcmp(fName, funcName) == 0) {
                DWORD old;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    void* original = (void*)(uintptr_t)thunk->u1.Function;
                    thunk->u1.Function = (DWORD)(uintptr_t)hook;
                    DWORD tmp; VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &tmp);
                    FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(void*));
                    return original;
                }
            }
        }
    }
    return nullptr;
}

// ---------------- Code patch helper ----------------
static bool PatchBytes(void* addr, const BYTE* newBytes, size_t len, BYTE* origOut) {
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    if (origOut) memcpy(origOut, addr, len);
    memcpy(addr, newBytes, len);
    DWORD tmp; VirtualProtect(addr, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

// ---------------- Aim assist disable ----------------
// Pattern at each registration site: D9 E8 (fld1) ... BA <name_va> (mov edx, name)
// We patch the fld1 to fldz (D9 EE) so the registered default becomes 0.0f.
// Offsets verified from v1 log + byte dumps. All are RVAs in .text.
struct AimPatch { DWORD rva; const char* name; };
static const AimPatch g_aimPatches[] = {
    { 0x25E7CE, "CloseAimAssist"          },  // fld1 2 bytes before BA at 0x25E7D0
    { 0x25E7E3, "FarAimAssist"            },  // fld1 2 bytes before BA at 0x25E7E5
    { 0x25EAF9, "HorizontalAimAssistClose"},  // prologue fld1 of function B
    { 0x25EB0C, "HorizontalAimAssistFar"  },  // fld1 2 bytes before BA at 0x25EB0E
    { 0x25EB21, "VerticalAimAssistClose"  },  // fld1 2 bytes before BA at 0x25EB23
    { 0x25EB36, "VerticalAimAssistFar"    },  // fld1 2 bytes before BA at 0x25EB38
};
static BYTE g_aimOrigBytes[_countof(g_aimPatches)][2];
static bool g_aimPatched[_countof(g_aimPatches)] = {false};

static void ApplyAimAssistPatches() {
    HMODULE base = GetModuleHandleW(nullptr);
    const BYTE fldz[2] = { 0xD9, 0xEE };
    int ok = 0;
    for (int i = 0; i < _countof(g_aimPatches); ++i) {
        BYTE* addr = (BYTE*)base + g_aimPatches[i].rva;
        // Verify we're looking at fld1 (D9 E8) before patching
        if (addr[0] != 0xD9 || addr[1] != 0xE8) {
            Log("AimAssist: %s @ RVA 0x%06X - unexpected bytes %02X %02X (expected D9 E8), skipped",
                g_aimPatches[i].name, g_aimPatches[i].rva, addr[0], addr[1]);
            continue;
        }
        if (PatchBytes(addr, fldz, 2, g_aimOrigBytes[i])) {
            g_aimPatched[i] = true;
            ++ok;
            Log("AimAssist: %s @ RVA 0x%06X patched fld1->fldz (orig %02X %02X)",
                g_aimPatches[i].name, g_aimPatches[i].rva,
                g_aimOrigBytes[i][0], g_aimOrigBytes[i][1]);
        } else {
            Log("AimAssist: %s @ RVA 0x%06X - VirtualProtect failed", g_aimPatches[i].name, g_aimPatches[i].rva);
        }
    }
    Log("AimAssist: %d/%d patches applied", ok, (int)_countof(g_aimPatches));
}

static void RevertAimAssistPatches() {
    HMODULE base = GetModuleHandleW(nullptr);
    for (int i = 0; i < _countof(g_aimPatches); ++i) {
        if (g_aimPatched[i]) {
            PatchBytes((BYTE*)base + g_aimPatches[i].rva, g_aimOrigBytes[i], 2, nullptr);
            g_aimPatched[i] = false;
        }
    }
}

// ---------------- Aim assist runtime zeroing ----------------
// The fldz patches above only change defaults; the game reads actual values
// from a settings bytecode stream via GetSettingFloat (RVA 0x256320,
// name in edx, default float at [esp+4], returns xmm0, cdecl stack).
// We redirect the E8 call at each AimAssist read site to a codecave stub:
//   mov eax,[esp+4] / push eax / call orig / add esp,4 / xorps xmm0,xmm0 / inc hits / ret
// The original still runs (stream cursor stays aligned) but the caller gets 0.0f.
static const DWORD AIM_GETTER_RVA = 0x256320;
struct AimSite { DWORD rva; const char* name; };
static const AimSite g_aimSites[] = {
    { 0x25E7DE, "CloseAimAssist"          },
    { 0x25E7F3, "FarAimAssist"            },
    { 0x25EB07, "HorizontalAimAssistClose"},
    { 0x25EB1C, "HorizontalAimAssistFar"  },
    { 0x25EB31, "VerticalAimAssistClose"  },
    { 0x25EB46, "VerticalAimAssistFar"    },
};
static BYTE g_aimSiteOrig[_countof(g_aimSites)][5];
static bool g_aimSitePatched[_countof(g_aimSites)] = {false};
static volatile LONG g_aimStubHits = 0;
static BYTE* g_aimStub = nullptr;

static void ApplyAimAssistStub() {
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* getter = (BYTE*)base + AIM_GETTER_RVA;

    g_aimStub = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_aimStub) { Log("AimStub: VirtualAlloc failed"); return; }
    int n = 0;
    g_aimStub[n++] = 0x8B; g_aimStub[n++] = 0x44; g_aimStub[n++] = 0x24; g_aimStub[n++] = 0x04; // mov eax,[esp+4]
    g_aimStub[n++] = 0x50;                                                                      // push eax
    g_aimStub[n++] = 0xE8;                                                                      // call getter
    *(DWORD*)(g_aimStub + n) = (DWORD)(getter - (g_aimStub + n + 4)); n += 4;
    g_aimStub[n++] = 0x83; g_aimStub[n++] = 0xC4; g_aimStub[n++] = 0x04;                        // add esp,4
    g_aimStub[n++] = 0x0F; g_aimStub[n++] = 0x57; g_aimStub[n++] = 0xC0;                        // xorps xmm0,xmm0
    g_aimStub[n++] = 0xFF; g_aimStub[n++] = 0x05;                                               // inc [hits]
    *(DWORD*)(g_aimStub + n) = (DWORD)&g_aimStubHits; n += 4;
    g_aimStub[n++] = 0xC3;                                                                      // ret

    int ok = 0;
    for (int i = 0; i < _countof(g_aimSites); ++i) {
        BYTE* site = (BYTE*)base + g_aimSites[i].rva;
        if (site[0] != 0xE8) {
            Log("AimStub: %s @ RVA 0x%06X - byte %02X (expected E8), skipped",
                g_aimSites[i].name, g_aimSites[i].rva, site[0]);
            continue;
        }
        DWORD target = (DWORD)(site + 5) + *(DWORD*)(site + 1);
        if (target != (DWORD)getter) {
            Log("AimStub: %s @ RVA 0x%06X - call target 0x%08X != getter 0x%08X, skipped",
                g_aimSites[i].name, g_aimSites[i].rva, target, (DWORD)getter);
            continue;
        }
        BYTE newCall[5]; newCall[0] = 0xE8;
        *(DWORD*)(newCall + 1) = (DWORD)(g_aimStub - (site + 5));
        if (PatchBytes(site, newCall, 5, g_aimSiteOrig[i])) {
            g_aimSitePatched[i] = true;
            ++ok;
        } else {
            Log("AimStub: %s @ RVA 0x%06X - VirtualProtect failed", g_aimSites[i].name, g_aimSites[i].rva);
        }
    }
    Log("AimStub: %d/%d call sites redirected to stub %p (getter %p)",
        ok, (int)_countof(g_aimSites), g_aimStub, getter);
}

static void RevertAimAssistStub() {
    HMODULE base = GetModuleHandleW(nullptr);
    for (int i = 0; i < _countof(g_aimSites); ++i) {
        if (g_aimSitePatched[i]) {
            PatchBytes((BYTE*)base + g_aimSites[i].rva, g_aimSiteOrig[i], 5, nullptr);
            g_aimSitePatched[i] = false;
        }
    }
}

// ---------------- v39: name-driven loader override ----------------
// GetSettingFloat (VA 0x656320) is how the game reads every named parameter
// out of the packed data files.  Its disassembly settles the architecture:
//   push ecx/ebx/esi/edi ; call 0x824270        ; EDX = name -> EAX = hash
//   mov edx,[0xEDC6E0]                          ; count of descriptors
//   mov esi,[0xEDC6DC]                          ; descriptor array, 16-byte entries
//   cmp [esi+4],eax / je found                  ; linear hash compare
//   found: movzx eax,[esi+0xC] ; add [0xEDC6C8]; value offset inside the blob
//          mov ecx,[0xEDC6D8] ; call [ecx_vtbl+0x1C](offset,0,1) ; stream seek
//          mov esi,[esi] -> type switch, read from the stream cursor [obj+0x10]
// So values are streamed OUT OF THE FILE, once, while a record loads, and
// [0xEDC6D8]/[0xEDC6DC] are NULL the rest of the time (confirmed live: the v38
// capture read them as 0).  That is the reason every container write in
// v20-v36 was invisible: the container we could see was not the copy the
// objects were built from, and after the load the source table is gone.
// The one upstream point that covers every downstream copy is this call.
// Every read site carries the setting name in EDX via `BA <name_va>`, so a
// scan of .text recovers a call-site -> name map (offline: 780 sites, 669
// named, including CloseAimAssist, YawNoInputInertia, TimeForAimState...).
// Each interesting site is redirected to a stub that runs the original call
// and hands (value, site) to a handler which logs it and may replace it.
static const DWORD GSF_RVA = 0x256320;
#define GSF_MAX_SITES 1200
#define GSF_NAME_LEN 40
#define GSF_STUB_SIZE 48
#define GSF_MAX_OVERRIDES 32

struct GsfSite {
    DWORD site;
    char  name[GSF_NAME_LEN];
    volatile LONG hits;
    float firstValue;
    bool  patched;
};
static GsfSite g_gsf[GSF_MAX_SITES];
static int g_gsfCount = 0;
static BYTE* g_gsfStubMem = nullptr;
static int g_gsfStubUsed = 0;

struct GsfOverride { char name[GSF_NAME_LEN]; float value; };
static GsfOverride g_gsfOv[GSF_MAX_OVERRIDES];
static int g_gsfOvCount = 0;
static BYTE g_gOrig[GSF_MAX_SITES][5];
static volatile LONG g_gsfCalls = 0;

static bool GsfNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool GsfReadName(DWORD va, char* out) {
    if (va < 0x00401000 || va > 0x03600000) return false;
    const unsigned char* p = (const unsigned char*)va;
    int i = 0;
    while (i < GSF_NAME_LEN - 1 && GsfNameChar((char)p[i])) ++i;
    if (i < 3 || p[i] != 0) return false;
    memcpy(out, p, i + 1);
    return true;
}

static bool GsfWatched(const char* n) {
    static const char* sub[] = { "aim", "assist", "inertia", "yaw", "pitch",
                                 "damp", "lag", "crouch", "sandpaper", "rotationspeed",
                                 "disrupt", "fov", "sens" };
    char low[GSF_NAME_LEN]; int i = 0;
    for (; i < GSF_NAME_LEN - 1 && n[i]; ++i)
        low[i] = (n[i] >= 'A' && n[i] <= 'Z') ? (char)(n[i] + 32) : n[i];
    low[i] = 0;
    for (int k = 0; k < (int)_countof(sub); ++k)
        if (strstr(low, sub[k])) return true;
    return false;
}

static bool GsfFindOverride(const char* n, float* out) {
    for (int i = 0; i < g_gsfOvCount; ++i)
        if (!strcmp(g_gsfOv[i].name, n)) { *out = g_gsfOv[i].value; return true; }
    return false;
}

// cdecl: the stub pushes (idx) then (value ptr), so the pointer is arg1.
static void GsfHandlerInner(float* val, int idx);

static void __cdecl GsfHandler(float* val, int idx) {
    __try { GsfHandlerInner(val, idx); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void GsfHandlerInner(float* val, int idx) {
    if (idx < 0 || idx >= g_gsfCount || !val) return;
    GsfSite* s = &g_gsf[idx];
    LONG h = InterlockedIncrement(&s->hits);
    InterlockedIncrement(&g_gsfCalls);
    if (h == 1) s->firstValue = *val;
    if (g_cfg.setLogWatch && h <= 3)
        Log("SREAD  %-28s = %-.7g  site=%08X hit=%d", s->name, *val, s->site, (int)h);
    float ov;
    if (GsfFindOverride(s->name, &ov)) {
        *val = ov;
        if (g_cfg.setLogWatch && h <= 3)
            Log("SWRITE %-28s -> %-.7g  site=%08X", s->name, ov, s->site);
    }
}

static bool GsfReserved(const char* k) {
    static const char* r[] = { "Enabled", "LogWatch", "HookAll", "PatchSites", "LogHashNames" };
    for (int i = 0; i < (int)_countof(r); ++i)
        if (!_strnicmp(k, r[i], strlen(r[i])) && (k[strlen(r[i])] == '=' || k[strlen(r[i])] == 0))
            return true;
    return false;
}

static void GsfLoadOverrides() {
    char path[MAX_PATH]; _snprintf(path, sizeof(path) - 1, "%s\\%s", g_moduleDir, INI_FILE);
    static char buf[8192];
    DWORD n = GetPrivateProfileSectionA("Settings", buf, sizeof(buf), path);
    char* p = buf;
    while (p < buf + n) {
        char* eq = strchr(p, '=');
        if (eq && !GsfReserved(p)) {
            *eq = 0;
            if (g_gsfOvCount < GSF_MAX_OVERRIDES && strlen(p) < GSF_NAME_LEN) {
                strcpy(g_gsfOv[g_gsfOvCount].name, p);
                g_gsfOv[g_gsfOvCount].value = (float)atof(eq + 1);
                ++g_gsfOvCount;
                Log("SETTINGS override %s = %-.7g", p, g_gsfOv[g_gsfOvCount - 1].value);
            }
        }
        p += strlen(p) + 1;
    }
}

static int GsfAddSite(DWORD site, const char* name) {
    if (g_gsfCount >= GSF_MAX_SITES) return -1;
    GsfSite* s = &g_gsf[g_gsfCount];
    s->site = site; s->hits = 0; s->firstValue = 0.0f; s->patched = false;
    strcpy(s->name, name);
    return g_gsfCount++;
}

// Emit a per-site stub: run the original call, then route the returned xmm0
// through GsfHandler.  The caller's stack argument (the float default) sits
// at [esp+4] when we issue the call, exactly as at the original site, so the
// getter's [esp+0x14] arithmetic still resolves.
static bool GsfBuildStub(int idx, BYTE* site, BYTE* getter) {
    if (g_gsfStubUsed >= GSF_MAX_SITES) return false;
    BYTE* st = g_gsfStubMem + (size_t)g_gsfStubUsed * GSF_STUB_SIZE;
    int n = 0;
    st[n++] = 0xE8;                                          // call getter
    *(DWORD*)(st + n) = (DWORD)(getter - (st + n + 4)); n += 4;
    st[n++] = 0x83; st[n++] = 0xEC; st[n++] = 0x04;          // sub esp,4
    st[n++] = 0xF3; st[n++] = 0x0F; st[n++] = 0x11; st[n++] = 0x04; st[n++] = 0x24; // movss [esp],xmm0
    st[n++] = 0x89; st[n++] = 0xE0;                          // mov eax,esp
    st[n++] = 0x68; *(DWORD*)(st + n) = (DWORD)idx; n += 4;  // push idx
    st[n++] = 0x50;                                          // push eax
    st[n++] = 0xE8;                                          // call GsfHandler
    *(DWORD*)(st + n) = (DWORD)((BYTE*)&GsfHandler - (st + n + 4)); n += 4;
    st[n++] = 0x83; st[n++] = 0xC4; st[n++] = 0x08;          // add esp,8
    st[n++] = 0xF3; st[n++] = 0x0F; st[n++] = 0x10; st[n++] = 0x04; st[n++] = 0x24; // movss xmm0,[esp]
    st[n++] = 0x83; st[n++] = 0xC4; st[n++] = 0x04;          // add esp,4
    st[n++] = 0xE9;                                          // jmp site+5
    *(DWORD*)(st + n) = (DWORD)(site + 5 - (st + n + 4)); n += 4;
    if (n > GSF_STUB_SIZE) return false;
    ++g_gsfStubUsed;
    BYTE jmp[5]; jmp[0] = 0xE9;
    *(DWORD*)(jmp + 1) = (DWORD)(st - (site + 5));
    if (!PatchBytes(site, jmp, 5, g_gOrig[idx])) return false;
    g_gsf[idx].patched = true;
    return true;
}

static void GsfInit() {
    GsfLoadOverrides();
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    BYTE* getter = base + GSF_RVA;
    DWORD total = 0, named = 0;
    if (g_cfg.setEnabled) {
        g_gsfStubMem = (BYTE*)VirtualAlloc(nullptr, (size_t)GSF_MAX_SITES * GSF_STUB_SIZE,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_gsfStubMem) { Log("GSF: VirtualAlloc for stubs failed"); g_cfg.setEnabled = 0; }
    }
    __try {
        IMAGE_DOS_HEADER* dh = (IMAGE_DOS_HEADER*)base;
        if (dh->e_magic != IMAGE_DOS_SIGNATURE) { Log("GSF: bad DOS header"); return; }
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dh->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) { Log("GSF: bad NT header"); return; }
        IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
        for (int si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sh) {
            if (!(sh->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            BYTE* sec = base + sh->VirtualAddress;
            DWORD len = sh->Misc.VirtualSize;
            if (len > sh->SizeOfRawData) len = sh->SizeOfRawData;
            for (DWORD i = 0; i + 5 <= len; ++i) {
                if (sec[i] != 0xE8) continue;
                if ((DWORD)(sec + i + 5 + *(DWORD*)(sec + i + 1)) != (DWORD)getter) continue;
                ++total;
                BYTE* site = sec + i;
                char nm[GSF_NAME_LEN]; bool got = false;
                for (int back = 5; back <= 32 && !got; ++back) {
                    BYTE* q = site - back;
                    if (q < sec) break;
                    if (*q == 0xBA) { got = GsfReadName(*(DWORD*)(q + 1), nm); if (got) break; }
                }
                if (!got) continue;
                ++named;
                int idx = GsfAddSite((DWORD)site, nm);
                if (idx < 0) continue;
                float dummy;
                bool want = g_cfg.setHookAll || GsfWatched(nm) || GsfFindOverride(nm, &dummy);
                // v39 proved these 155 per-call-site stubs never fire during a
                // load, so they are off by default; v40 hooks the getter itself.
                if (g_cfg.setEnabled && g_cfg.setPatchSites && want) {
                    if (!GsfBuildStub(idx, site, getter))
                        Log("GSF: stub build failed for %s @%08X", nm, (DWORD)site);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("GSF: access violation while scanning (sites found so far: %d)", g_gsfCount);
    }
    Log("GSF: %u call sites to %p, %d named, %d patched (logWatch=%d hookAll=%d)",
        total, getter, named, g_gsfStubUsed, g_cfg.setLogWatch, g_cfg.setHookAll);
}

static void GsfRevert() {
    for (int i = 0; i < g_gsfCount; ++i)
        if (g_gsf[i].patched) PatchBytes((void*)g_gsf[i].site, g_gOrig[i], 5, nullptr);
}

static void GsfSummary() {
    int shown = 0;
    for (int i = 0; i < g_gsfCount; ++i) {
        if (!g_gsf[i].patched || !g_gsf[i].hits) continue;
        if (shown++ > 120) break;
        Log("CAPTURE GSF: %-28s hits=%d first=%-.7g%s",
            g_gsf[i].name, (int)g_gsf[i].hits, g_gsf[i].firstValue,
            g_gsfOvCount ? " (overridden)" : "");
    }
    Log("CAPTURE GSF: %d named reads total, %d stubs live", shown, g_gsfStubUsed);
}

// ---------------- v40: getter instrumentation ----------------
// v39 patched 155 call sites and the level loaded WITHOUT tripping one
// (capture showed 1076 seat commits and the camera map filling 0 -> 44 at the
// same second, so the load definitely ran after our init).  Guessing sites is
// therefore the wrong granularity: instrument the getter itself.  Three hooks,
// all with branch-free stolen bytes, cover every read in the game:
//
//  A. 0x82427A  `jmp 0x4BDFB6` inside HashName(0x824270).  EDX still holds the
//     name pointer and [esp] still holds the getter's return address, so this
//     tells us WHICH name is being hashed and WHO asked.  HashName touches no
//     stack arguments, so pushad/popad is safe here.
//     The hash is FNV-1a, lowercased, seed at *[0x245D6D8], prime 0x01000193,
//     final xor 0x2A + one more multiply.
//  B. 0x65639C  `5F 5E 5B 59 C3` (pop edi/esi/ebx/ecx/ret) on the type-7 float
//     exit: xmm0 holds the value streamed from the data file.  The stolen bytes
//     contain no branches, so the stub re-executes them and returns directly.
//  C. 0x656355  the identical byte sequence on the not-found exit, where xmm0
//     holds the caller's default - proves a parameter is absent from a record.
#define GSA_HASH_RVA   0x42427A      // +0x82427A
#define GSA_FLOATRET_RVA 0x25639C
#define GSA_DEFRET_RVA   0x256355
#define GSA_GETTER_HASHRET 0x00656329 // RA seen from hook A when the getter called it

static volatile LONG g_haTotal = 0;       // names hashed (all callers)
static volatile LONG g_haGetter = 0;      // names hashed by GetSettingFloat
static volatile LONG g_valReads = 0;      // float values returned from the file
static volatile LONG g_defReads = 0;      // defaults returned (parameter absent)
static DWORD g_gsaGetterLo = 0, g_gsaGetterHi = 0;  // the getter block, for RA matching
static char  g_curName[GSF_NAME_LEN] = "";
static DWORD g_curNameRa = 0;
static BYTE  g_haOrig[5], g_valOrig[5], g_defOrig[5];
static bool  g_haOn = false, g_valOn = false, g_defOn = false;
static BYTE* g_gsaCode = nullptr;
#define GSA_MAX_STUBS 16
static int g_gsaUsed = 0;

struct GsaSeen { char name[GSF_NAME_LEN]; LONG count; float first; float min, max; };
static GsaSeen g_gsa[400];
static int g_gsaCount = 0;
static int g_gsaLogged = 0;

static int GsaNameSlot(const char* n) {
    for (int i = 0; i < g_gsaCount; ++i)
        if (!strcmp(g_gsa[i].name, n)) return i;
    if (g_gsaCount >= 400) return -1;
    int i = g_gsaCount++;
    strcpy(g_gsa[i].name, n);
    g_gsa[i].count = 0; g_gsa[i].first = 0.0f;
    g_gsa[i].min = 1e30f; g_gsa[i].max = -1e30f;
    return i;
}

static void GsaHashNoteInner(const char* name, DWORD ra);

// Hook A sits at 0x82427A, before the loop advances EDX, so EDX is still the
// name pointer and [esp] is still the caller's return address (verified against
// the disassembly).  GsfReadName rejects anything that is not a NUL-terminated
// identifier, and the whole body is SEH-guarded for other callers.
static void __cdecl GsaHashNote(const char* name, DWORD ra) {
    __try { GsaHashNoteInner(name, ra); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void GsaHashNoteInner(const char* name, DWORD ra) {
    InterlockedIncrement(&g_haTotal);
    bool fromGetter = (ra >= g_gsaGetterLo && ra < g_gsaGetterHi);
    if (fromGetter) InterlockedIncrement(&g_haGetter);
    // Only the float getter's own hash calls may name the values that hooks B
    // and C report, so anything else clears the context instead of poisoning it.
    if (!GsfReadName((DWORD)name, g_curName) || !fromGetter) { g_curName[0] = 0; return; }
    g_curNameRa = ra;
    if (!g_cfg.setLogHashNames) return;
    if (!GsfWatched(name)) return;
    int slot = GsaNameSlot(name);
    if (slot < 0) return;
    LONG c = InterlockedIncrement(&g_gsa[slot].count);
    if (c <= 2 && g_gsaLogged < 400) {
        ++g_gsaLogged;
        Log("GSHAPE %-26s ra=%08X%s", name, ra, fromGetter ? " (getter)" : "");
    }
}

// Runs on the load-time hot path, once per value the getter returns.  The name
// table is a linear scan, so only watched/overridden names are tracked at all;
// the two global counters still see every read.
static void GsaValueCommon(float* v, bool fromFile) {
    if (fromFile) InterlockedIncrement(&g_valReads);
    else InterlockedIncrement(&g_defReads);
    if (!g_curName[0] || !v) return;
    float ov;
    bool tracked = GsfFindOverride(g_curName, &ov) || (g_cfg.setLogWatch && GsfWatched(g_curName));
    if (!tracked) return;
    int slot = GsaNameSlot(g_curName);
    if (slot < 0) return;
    LONG c = InterlockedIncrement(&g_gsa[slot].count);
    if (c == 1) g_gsa[slot].first = *v;
    if (*v < g_gsa[slot].min) g_gsa[slot].min = *v;
    if (*v > g_gsa[slot].max) g_gsa[slot].max = *v;
    if (GsfFindOverride(g_curName, &ov)) {
        *v = ov;
        if (g_cfg.setLogWatch && c <= 3) Log("SWRITE %-26s -> %-.7g", g_curName, ov);
        return;
    }
    if (g_cfg.setLogWatch && c <= 3 && g_gsaLogged < 900) {
        ++g_gsaLogged;
        Log("GVALUE %-26s = %-.7g  hits=%d %s", g_curName, *v, (int)c,
            fromFile ? "from-file" : "DEFAULT(absent)");
    }
}

static void __cdecl GsaValueNote(float* v) {
    __try { GsaValueCommon(v, true); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void __cdecl GsaDefaultNote(float* v) {
    __try { GsaValueCommon(v, false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Emit a stub at *pp (writes into g_gsaCode) and install the E9 at site.
// kind: 0 = hash note, 1 = value note, 2 = default note.
static bool GsaInstall(int kind, BYTE* site, const BYTE* expect, int expectLen, DWORD resume) {
    for (int k = 0; k < expectLen; ++k)
        if (site[k] != expect[k]) {
            Log("GSA: site %08X byte %02X != expected %02X, skipped", (DWORD)site, site[k], expect[k]);
            return false;
        }
    BYTE* st = g_gsaCode;
    if (g_gsaUsed >= GSA_MAX_STUBS) return false;
    g_gsaCode += GSF_STUB_SIZE;
    int n = 0;
    st[n++] = 0x60;                                          // pushad
    if (kind == 0) {
        st[n++] = 0x8B; st[n++] = 0x44; st[n++] = 0x24; st[n++] = 0x20; // mov eax,[esp+0x20] = RA
        st[n++] = 0x50;                                      // push eax
        st[n++] = 0x52;                                      // push edx (name)
        st[n++] = 0xE8; *(DWORD*)(st + n) = (DWORD)((BYTE*)&GsaHashNote - (st + n + 4)); n += 4;
        st[n++] = 0x83; st[n++] = 0xC4; st[n++] = 0x08;      // add esp,8
        st[n++] = 0x61;                                      // popad
        st[n++] = 0xE9; *(DWORD*)(st + n) = (DWORD)((BYTE*)resume - (st + n + 4)); n += 4;
    } else {
        void* fn = (kind == 1) ? (void*)&GsaValueNote : (void*)&GsaDefaultNote;
        st[n++] = 0x83; st[n++] = 0xEC; st[n++] = 0x08;      // sub esp,8
        st[n++] = 0xF3; st[n++] = 0x0F; st[n++] = 0x11; st[n++] = 0x04; st[n++] = 0x24; // movss [esp],xmm0
        st[n++] = 0x54;                                      // push esp  (float ptr)
        st[n++] = 0xE8; *(DWORD*)(st + n) = (DWORD)((BYTE*)fn - (st + n + 4)); n += 4;
        st[n++] = 0x83; st[n++] = 0xC4; st[n++] = 0x04;      // add esp,4
        st[n++] = 0xF3; st[n++] = 0x0F; st[n++] = 0x10; st[n++] = 0x04; st[n++] = 0x24; // movss xmm0,[esp]
        st[n++] = 0x83; st[n++] = 0xC4; st[n++] = 0x08;      // add esp,8
        st[n++] = 0x61;                                      // popad
        for (int k = 0; k < 5; ++k) st[n++] = expect[k];     // pop edi/esi/ebx/ecx / ret
    }
    if (n > GSF_STUB_SIZE) return false;
    BYTE jmp[5]; jmp[0] = 0xE9;
    *(DWORD*)(jmp + 1) = (DWORD)(st - (site + 5));
    BYTE* orig = (kind == 0) ? g_haOrig : (kind == 1) ? g_valOrig : g_defOrig;
    if (!PatchBytes(site, jmp, 5, orig)) return false;
    if (kind == 0) g_haOn = true; else if (kind == 1) g_valOn = true; else g_defOn = true;
    Log("GSA: hook %d installed at %08X (resume %08X, stub %p)", kind, (DWORD)site, resume, st);
    (void)resume;
    return true;
}

static void GsaInit() {
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    g_gsaGetterLo = (DWORD)(base + GSF_RVA);
    g_gsaGetterHi = g_gsaGetterLo + 0x200;   // the whole float-getter body
    g_gsaCode = (BYTE*)VirtualAlloc(nullptr, 16 * GSF_STUB_SIZE,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_gsaCode) { Log("GSA: VirtualAlloc failed"); return; }
    static const BYTE haBytes[5] = { 0xE9, 0x37, 0x9D, 0xC9, 0xFF };
    static const BYTE retBytes[5] = { 0x5F, 0x5E, 0x5B, 0x59, 0xC3 };
    // Hook A replaces `jmp 0x4BDFB6`, the veneer that loads the FNV seed, so the
    // stub has to continue into that veneer.
    GsaInstall(0, base + GSA_HASH_RVA, haBytes, 5, (DWORD)(base + 0xBDFB6));
    GsaInstall(1, base + GSA_FLOATRET_RVA, retBytes, 5, 0);
    GsaInstall(2, base + GSA_DEFRET_RVA, retBytes, 5, 0);
    DWORD seed = 0;
    __try { seed = *(const DWORD*)(base + 0x205D6D8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("GSA: getter block %08X..%08X, FNV seed (0x245D6D8) = %08X, hash hook resume %08X",
        g_gsaGetterLo, g_gsaGetterHi, seed, (DWORD)(base + 0xBDFB6));
}

static void GsaRevert() {
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    if (g_haOn) PatchBytes(base + GSA_HASH_RVA, g_haOrig, 5, nullptr);
    if (g_valOn) PatchBytes(base + GSA_FLOATRET_RVA, g_valOrig, 5, nullptr);
    if (g_defOn) PatchBytes(base + GSA_DEFRET_RVA, g_defOrig, 5, nullptr);
}

// Did our .text patches survive?  A SecuROM integrity pass would restore the
// original bytes and silently disarm everything we installed.
static void GsaVerify() {
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    int alive = 0;
    for (int i = 0; i < g_gsfCount; ++i)
        if (g_gsf[i].patched && *(BYTE*)g_gsf[i].site == 0xE9) ++alive;
    Log("CAPTURE GSA: names hashed=%ld, by getter=%ld, file values=%ld, defaults=%ld, last name=%s (ra=%08X)",
        g_haTotal, g_haGetter, g_valReads, g_defReads, g_curName, g_curNameRa);
    Log("CAPTURE GSA: hook bytes still installed: A=%d B=%d C=%d; v39 site stubs alive %d/%d",
        g_haOn && *(BYTE*)(base + GSA_HASH_RVA) == 0xE9,
        g_valOn && *(BYTE*)(base + GSA_FLOATRET_RVA) == 0xE9,
        g_defOn && *(BYTE*)(base + GSA_DEFRET_RVA) == 0xE9,
        alive, g_gsfStubUsed);
    int shown = 0;
    for (int i = 0; i < g_gsaCount && shown < 60; ++i) {
        if (!g_gsa[i].count) continue;
        ++shown;
        Log("CAPTURE GSA: %-26s hits=%d first=%-.7g range[%-.7g..%-.7g]",
            g_gsa[i].name, (int)g_gsa[i].count, g_gsa[i].first,
            g_gsa[i].min > 1e29f ? 0.0f : g_gsa[i].min,
            g_gsa[i].max < -1e29f ? 0.0f : g_gsa[i].max);
    }
}

// ---------------- v42: runtime camera patches ----------------
// Static RE (re-notes/B-camera-runtime.md) found the per-frame consumer, and with it the
// reason all eleven data writes did nothing: HumanCameraModifier +0x20/+0x24 are ENUM
// INDICES into a hard-coded 3-row constant table (obj+0xA08, filled by ctor 0x0071EB10),
// so the record's aim-assist floats only ever nudge which row is picked.  Aim assist is one
// routine, 0x0071D5B0, called from exactly one site (0x0071DC28) whose return ADDS a
// correction onto the mouse yaw and stores it back at 0x0071DC32.
// v43 closed the assist question: 0x0071D5B0 stores to nothing but its own stack frame,
// the blend weight obj+0x780 and the small struct at obj+0x784, it has exactly one call
// site, and obj+0x780 is read by no code outside 0x0071D5B0 itself.  With the yaw store
// NOPped the camera assist therefore has zero remaining path to the camera - so whatever
// snap is still visible does not come from this routine.
#define CAM_CONE_DATARVA      0x9FDCB4   // [0x00DFDCB4] = 0.98  (strength SELECTOR, not a gate)
#define CAM_STORE_RVA         0x31DC32   // movss [yawOut],xmm0 : the assist's only output
#define CAM_WEIGHT_STRVA      0x31D729   // movss [esi+0x780],xmm0 : smoothed stickiness
#define CAM_SCALE_LD_STRVA    0x31E557   // movss xmm0,[0x00BBC7EC] : 0.25 while manned
#define CAM_FLAG_STRVA        0x31E9DD   // mov byte [ebx+0x7DD],1  : manned/mode flag
#define CAM_PARAMSEL1_JNEVA   0x31DAE3   // jne over lea esi,[ebx+0x620] : first 14-float set
#define CAM_PARAMSEL2_JNEVA   0x31DAFE   // jne over lea esi,[ebx+0x658] : second 14-float set
#define CAM_SMOOTHSEL_JEVA    0x31BE50   // je to [ebp+0x9F0] : smoothing coefficient
// v45.  0x0071D556 is the lock-on state machine's ONLY yaw sink:
//   0x0071D54B push ecx / 0x0071D54C movss [esp],xmm0 / 0x0071D551 call 0x00822CB0
//   0x0071D556 fstp dword ptr [esi+0x618]   <- overwrite the yaw accumulator with the lock
// It must not simply be NOPped: `fstp` is what POPs ST(0), and this runs every frame, so
// removing it without a replacement store leaks the FPU stack and faults within a second.
// The 6 bytes are therefore rewritten to `fstp dword ptr [<abs>]` - same length, same pop,
// same flags, and the value lands in a private float of ours instead of the camera.
#define CAM_LOCKON_STORE_RVA  0x31D556
// v45 button prompts.  Both instructions are `C7 05 18 38 4A 01 <imm32>` - an absolute
// store to the input-mode global at 0x014A3818 - and only the immediate differs, so the
// patch is two immediate rewrites and touches no control flow at all.
#define PROMPT_MODE1_RVA      0x0FB13D   // mov dword [0x014A3818],1  (pad)
#define PROMPT_MODE2_RVA      0x0FB150   // mov dword [0x014A3818],2  (pad + XInput)
static bool  g_coneOn = false;
static float g_coneOrig = 0.0f;
static BYTE  g_weightOrig[8], g_scaleOrig[8], g_flagOrig[7];
static bool  g_weightOn = false, g_scaleOn = false, g_flagOn = false;
static BYTE  g_paramOrig[4], g_smoothOrig[2];
static bool  g_paramOn = false, g_smoothOn = false;
static BYTE  g_bypassOrig[4];
static bool  g_bypassOn = false;
static BYTE  g_lockOnOrig[6];
static bool  g_lockOnOn = false;
static float g_fpuSink = 0.0f;   // discard target for the redirected lock-on fstp
static BYTE  g_promptOrig[8];
static bool  g_promptOn = false;

// The five bytes at 0x0071DC32 are the assist's only visible output, verbatim from
// the shipped exe:
//   E8 83 F9 FF FF   call 0x0071D5B0          ; aim assist; returns (its own delta + yaw)
//   8B 55 18         mov  edx,[ebp+0x18]      ; the yaw output pointer
//   5F 5E            pop  edi / pop esi
//   F3 0F 11 02      mov  [edx],xmm0          ; <<< yaw := assist result
// The assist does not gate the yaw, it ADDs to it: inside 0x0071D5B0 the last
// arithmetic before the return is `addss xmm0,[esp+0x38]`, i.e. the caller's yaw
// plus the assist delta.  So the whole effect lives in ONE instruction - the store
// at 0x0071DC32 - and NOPping those 4 bytes leaves the yaw exactly as the caller
// already wrote it at 0x0071DBC6 (`movss [[ebp+0x18]],xmm0`).
// v41 instead rewrote the call itself, and that crashed the game 7s into a save
// load.  The reason is the callee's `ret 0x14`: those 20 arg bytes are released by
// the RETURN, and the epilogue `pop edi / pop esi` at 0x0071DC30 depends on esp
// being back at the pre-args value.  A pass-through keeps esp 0x14 bytes low, so
// the two pops read the argument floats into edi/esi and the caller continues with
// clobbered saved registers.  The fix has to be the store: it cannot disturb esp,
// the stack, or any register, and it drops exactly the assist's contribution.
static void ApplyAssistStoreNop() {
    BYTE* site = (BYTE*)GetModuleHandleW(nullptr) + CAM_STORE_RVA;
    static const BYTE want[9] = {
        0x8B, 0x55, 0x18, 0x5F, 0x5E, 0xF3, 0x0F, 0x11, 0x02 };
    static const BYTE repl[4] = { 0x90, 0x90, 0x90, 0x90 };
    __try {
        if (memcmp(site - 5, want, 9)) {
            Log("CamPatch: store NOP skipped, 0x0071DC2D = %02X %02X %02X %02X %02X %02X %02X %02X %02X (expected 8B 55 18 5F 5E F3 0F 11 02)",
                site[-5], site[-4], site[-3], site[-2], site[-1], site[0], site[1], site[2], site[3]);
            return;
        }
        g_bypassOn = PatchBytes(site, repl, 4, g_bypassOrig);
        Log("CamPatch: aim-assist yaw store NOPped at 0x0071DC32 (%s)", g_bypassOn ? "applied" : "protect failed");
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("CamPatch: store NOP write faulted"); }
}

static void ApplyCameraRuntimePatches() {
    HMODULE base = GetModuleHandleW(nullptr);

    if (g_cfg.aimAssistDisable && g_cfg.aimNopStore) ApplyAssistStoreNop();

    // Not a gate: 0x0071D657 compares the second rate argument against 0.98 only to
    // pick WHICH bundle strength the assist uses - above 0.98 it takes f4 (0.2, the
    // gentle "already on target" case), below it f3/f5 (0.7/0.8).  Writing 2.0
    // therefore makes the assist pull HARDER, so this stays off unless we want to
    // prove the strength table is what the feel comes from.
    if (g_cfg.aimAssistDisable && g_cfg.aimConeDisable) {
        __try {
            float* p = (float*)((BYTE*)base + CAM_CONE_DATARVA);
            g_coneOrig = *p;
            Log("CamPatch: assist cone [0x00DFDCB4] = %-.7g (expect 0.98)", *p);
            float v = 2.0f;
            g_coneOn = PatchBytes(p, (const BYTE*)&v, 4, nullptr);
            Log("CamPatch: cone -> %-.7g (%s)", v, g_coneOn ? "applied" : "VirtualProtect failed");
        } __except (EXCEPTION_EXECUTE_HANDLER) { Log("CamPatch: cone write faulted"); }
    }

    // Mounted dampening, mechanism 3 of 3: 0x0071DC40 scales obj+0x414/0x418 by 0.25 while
    // obj+0x7DD is set.  Swap the operand for the 1.0 already in .rdata - same instruction
    // length, and the byte check doubles as a rebase guard.
    if (g_cfg.turretScaleOne) {
        BYTE* site = (BYTE*)base + CAM_SCALE_LD_STRVA;
        static const BYTE want[8] = { 0xF3, 0x0F, 0x10, 0x05, 0xEC, 0xC7, 0xBB, 0x00 };
        static const BYTE repl[8] = { 0xF3, 0x0F, 0x10, 0x05, 0x64, 0xB6, 0xB9, 0x00 };
        if (!memcmp(site, want, 8)) {
            g_scaleOn = PatchBytes(site, repl, 8, g_scaleOrig);
            Log("CamPatch: mounted scale 0.25 -> 1.0 at 0x0071E557 (%s)", g_scaleOn ? "applied" : "protect failed");
        } else {
            Log("CamPatch: 0x0071E557 bytes %02X %02X %02X %02X (expected F3 0F 10 05), scale patch skipped",
                site[0], site[1], site[2], site[3]);
        }
    }

    // Mounted dampening, mechanisms 1 and 2 (v43).  0x0071DAD5 reads obj+0x7DD and then
    // selects between two 14-float parameter sets with `lea esi,[ebx+0x690]` (manned) or
    // `lea esi,[ebx+0x620]` (normal), the second group being 0x6C8/0x658, and `rep movsd
    // ecx=0xE` copies the chosen set onto the stack where 0x0071DB0C onwards uses it as the
    // turn-rate multipliers.  The manned lea executes unconditionally and the two `jne`s only
    // skip the normal lea, so NOPping both leaves the normal sets selected while leaving the
    // flag itself - and everything else that reads it - untouched.  Mechanism 2 is the same
    // shape in 0x0071BE30: `cmp byte [ebp+0x7DD],0 / je 0x0071BE5C` picks the ring-buffer
    // smoothing coefficient at [ebp+0x9F4] (manned) instead of [ebp+0x9F0] (normal); making
    // that branch unconditional takes the normal one.  v42 proved that mechanism 3 alone (the
    // explicit x0.25) does NOT fix the sluggish turret, which is what put these two next.
    if (g_cfg.turretNormalParams) {
        static const BYTE want1[8] = { 0x8D,0xB3,0x90,0x06,0x00,0x00,0x75,0x06 };
        static const BYTE want2[8] = { 0x8D,0xB3,0xC8,0x06,0x00,0x00,0x75,0x06 };
        static const BYTE nops[2]  = { 0x90, 0x90 };
        BYTE* j1 = (BYTE*)base + CAM_PARAMSEL1_JNEVA;
        BYTE* j2 = (BYTE*)base + CAM_PARAMSEL2_JNEVA;
        if (!memcmp(j1 - 6, want1, 8) && !memcmp(j2 - 6, want2, 8)) {
            bool a = PatchBytes(j1, nops, 2, g_paramOrig);
            bool b = PatchBytes(j2, nops, 2, g_paramOrig + 2);
            g_paramOn = a && b;
            Log("CamPatch: mounted parameter sets -> normal at 0x0071DAE3/0x0071DAFE (%s)",
                g_paramOn ? "applied" : "protect failed");
        } else {
            Log("CamPatch: param-select bytes mismatch (0x0071DADD=%02X.., 0x0071DAF8=%02X..), skipped",
                j1[-6], j2[-6]);
        }
    }
    if (g_cfg.turretNormalSmooth) {
        BYTE* site = (BYTE*)base + CAM_SMOOTHSEL_JEVA;
        static const BYTE want[2] = { 0x74, 0x0A };
        static const BYTE repl[2] = { 0xEB, 0x0A };   // same displacement, unconditional
        if (!memcmp(site, want, 2)) {
            g_smoothOn = PatchBytes(site, repl, 2, g_smoothOrig);
            Log("CamPatch: mounted smoothing coefficient -> normal at 0x0071BE50 (%s)",
                g_smoothOn ? "applied" : "protect failed");
        } else {
            Log("CamPatch: 0x0071BE50 bytes %02X %02X (expected 74 0A), smoothing patch skipped",
                site[0], site[1]);
        }
    }

    // Stickiness store NOP: obj+0x780 never rises off 0.0, so the blend weight stays inert.
    // Off by default - the cone edit is the cleaner test of the same hypothesis.
    if (g_cfg.aimKillWeight) {
        BYTE* site = (BYTE*)base + CAM_WEIGHT_STRVA;
        static const BYTE want[8] = { 0xF3, 0x0F, 0x11, 0x86, 0x80, 0x07, 0x00, 0x00 };
        static const BYTE nops[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        if (!memcmp(site, want, 8)) {
            g_weightOn = PatchBytes(site, nops, 8, g_weightOrig);
            Log("CamPatch: assist weight store NOPped at 0x0071D729 (%s)", g_weightOn ? "applied" : "protect failed");
        } else {
            Log("CamPatch: 0x0071D729 bytes %02X %02X %02X %02X (expected F3 0F 11 86), weight patch skipped",
                site[0], site[1], site[2], site[3]);
        }
    }

    // Diagnostic that removes all THREE mounted mechanisms at once, because all of them key
    // on obj+0x7DD.  Off by default: it also changes aim-assist mode dependence and the
    // mounted parameter sets, so it is a test, not a fix.
    if (g_cfg.turretNeverManned) {
        BYTE* site = (BYTE*)base + CAM_FLAG_STRVA;
        static const BYTE want[7] = { 0xC6, 0x83, 0xDD, 0x07, 0x00, 0x00, 0x01 };
        static const BYTE repl[7] = { 0xC6, 0x83, 0xDD, 0x07, 0x00, 0x00, 0x00 };
        if (!memcmp(site, want, 7)) {
            g_flagOn = PatchBytes(site, repl, 7, g_flagOrig);
            Log("CamPatch: manned flag now stored as 0 at 0x0071E9DD (%s)", g_flagOn ? "applied" : "protect failed");
        } else {
            Log("CamPatch: 0x0071E9DD mismatch, flag patch skipped");
        }
    }
    Log("CamPatch: nopStore=%d cone=%d killWeight=%d scaleOne=%d neverManned=%d normalParams=%d normalSmooth=%d",
        g_bypassOn, g_coneOn, g_weightOn, g_scaleOn, g_flagOn, g_paramOn, g_smoothOn);
}

// v45: the reticle hold that survived v42.  0x0071D0A0 is a lock-on state machine with one
// call site (0x0071DE2B, inside the per-frame update 0x0071DC40 and after the assist block
// that v42 killed).  Its phase int at obj+0x974 and hold weights at obj+0x984/0x988 are what
// make the crosshair grab an enemy, and the single store at 0x0071D556 is the only place that
// decision reaches the yaw accumulator obj+0x618.  Free aim is unaffected because it enters
// later, as `addss` into that same accumulator at 0x0071DE59, so discarding the lock's write
// removes the grab without touching the mouse.
static void ApplyLockOnHoldDiscard() {
    BYTE* site = (BYTE*)GetModuleHandleW(nullptr) + CAM_LOCKON_STORE_RVA;
    static const BYTE want[6] = { 0xD9, 0x9E, 0x18, 0x06, 0x00, 0x00 };  // fstp [esi+0x618]
    BYTE repl[6] = { 0xD9, 0x1D, 0, 0, 0, 0 };                           // fstp [<abs>]
    DWORD sink = (DWORD)(uintptr_t)&g_fpuSink;
    repl[2] = (BYTE)(sink);        repl[3] = (BYTE)(sink >> 8);
    repl[4] = (BYTE)(sink >> 16);  repl[5] = (BYTE)(sink >> 24);
    __try {
        if (memcmp(site, want, 6)) {
            Log("CamPatch: 0x0071D556 = %02X %02X %02X %02X %02X %02X (expected D9 9E 18 06 00 00), lock-on discard skipped",
                site[0], site[1], site[2], site[3], site[4], site[5]);
            return;
        }
        g_lockOnOn = PatchBytes(site, repl, 6, g_lockOnOrig);
        Log("CamPatch: lock-on yaw overwrite redirected to a discard float at 0x0071D556 (%s)",
            g_lockOnOn ? "applied" : "protect failed");
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("CamPatch: lock-on discard write faulted"); }
}

// v45: the game draws console-controller glyphs on keyboard+mouse because a global input mode
// (0x014A3818: 0 = KB&M, 1 = pad, 2 = pad with XInput) latches to the pad as soon as any
// XInput byte moves, and stays there via the sticky byte at 0x014A381C.  Both pad values are
// written by absolute stores at 0x004FB13D and 0x004FB150 whose ONLY difference is the
// immediate, so rewriting both immediates to 0 leaves the detection state machine, every
// reader and the controller bindings themselves intact while the mode can never leave KB&M.
// The keyboard art is already shipped - the exe holds a pad->keyboard sprite remap table at
// 0x005BB953 and a prompt-text remap at 0x00626081, both gated on "XInput present AND mode
// == 0", which is precisely the state this patch produces while the pad stays plugged in.
static void ApplyKeyboardPrompts() {
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* p1 = (BYTE*)base + PROMPT_MODE1_RVA;
    BYTE* p2 = (BYTE*)base + PROMPT_MODE2_RVA;
    static const BYTE want1[10] = { 0xC7,0x05,0x18,0x38,0x4A,0x01,0x01,0x00,0x00,0x00 };
    static const BYTE want2[10] = { 0xC7,0x05,0x18,0x38,0x4A,0x01,0x02,0x00,0x00,0x00 };
    static const BYTE zero[4]   = { 0x00,0x00,0x00,0x00 };
    __try {
        if (memcmp(p1, want1, 10) || memcmp(p2, want2, 10)) {
            Log("Prompt: 0x004FB13D/0x004FB150 bytes mismatch (%02X..%02X / %02X..%02X), skipped",
                p1[0], p1[9], p2[0], p2[9]);
            return;
        }
        bool a = PatchBytes(p1 + 6, zero, 4, g_promptOrig);
        bool b = PatchBytes(p2 + 6, zero, 4, g_promptOrig + 4);
        g_promptOn = a && b;
        Log("Prompt: input-mode stores forced to 0 = keyboard+mouse at 0x004FB13D/0x004FB150 (%s)",
            g_promptOn ? "applied" : "protect failed");
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("Prompt: write faulted"); }
}

static void RevertCameraRuntimePatches() {
    HMODULE base = GetModuleHandleW(nullptr);
    if (g_bypassOn) { PatchBytes((BYTE*)base + CAM_STORE_RVA, g_bypassOrig, 4, nullptr); g_bypassOn = false; }
    if (g_coneOn) {
        float v = g_coneOrig;
        PatchBytes((BYTE*)base + CAM_CONE_DATARVA, (const BYTE*)&v, 4, nullptr);
        g_coneOn = false;
    }
    if (g_weightOn) { PatchBytes((BYTE*)base + CAM_WEIGHT_STRVA, g_weightOrig, 8, nullptr); g_weightOn = false; }
    if (g_scaleOn)  { PatchBytes((BYTE*)base + CAM_SCALE_LD_STRVA, g_scaleOrig, 8, nullptr); g_scaleOn = false; }
    if (g_flagOn)   { PatchBytes((BYTE*)base + CAM_FLAG_STRVA, g_flagOrig, 7, nullptr); g_flagOn = false; }
    if (g_paramOn) {
        PatchBytes((BYTE*)base + CAM_PARAMSEL1_JNEVA, g_paramOrig, 2, nullptr);
        PatchBytes((BYTE*)base + CAM_PARAMSEL2_JNEVA, g_paramOrig + 2, 2, nullptr);
        g_paramOn = false;
    }
    if (g_smoothOn) { PatchBytes((BYTE*)base + CAM_SMOOTHSEL_JEVA, g_smoothOrig, 2, nullptr); g_smoothOn = false; }
    if (g_lockOnOn) { PatchBytes((BYTE*)base + CAM_LOCKON_STORE_RVA, g_lockOnOrig, 6, nullptr); g_lockOnOn = false; }
    if (g_promptOn) {
        static const BYTE one[4] = { 0x01,0,0,0 }, two[4] = { 0x02,0,0,0 };
        PatchBytes((BYTE*)base + PROMPT_MODE1_RVA + 6, one, 4, nullptr);
        PatchBytes((BYTE*)base + PROMPT_MODE2_RVA + 6, two, 4, nullptr);
        g_promptOn = false;
    }
}

// ---------------- v44: shadow atlas size ----------------
// There is no config key, script name or wad entry for shadow resolution: the map is
// created from literal immediates.  0x00755D93 (owner of the PgLtiRendererShadowPc.cpp /
// ShadowMapCombined|ColorTex|Tex|Surf name strings) pushes width 0x400 and height 0x1000,
// i.e. a 1024x4096 strip holding four 1024x1024 cascades, at five create sites, and the
// same subsystem then bakes the geometry into the per-cascade clear rects at
// 0x0075611B..0x00756187 (rect left/top/right/bottom fields +0x4408..+0x4444, built from
// mov eax,0x400 / mov edx,0x800 / mov edx,0xC00 / mov [esi+0x4444],0x1000) and the
// per-cascade width/height at 0x007562B5.  Scaling therefore means 15 operands, and a
// partial scale gives cascades that no longer tile the atlas, so this is all-or-nothing:
// every operand is verified against the shipped value first and nothing is written if any
// one of them mismatches.
struct ShadowSizeSite { DWORD rva; int off; DWORD expect; const char* what; };
static const ShadowSizeSite g_shadowSites[] = {
    { 0x355DF5, 1, 0x1000, "strip height 0" }, { 0x355DFA, 1, 0x400, "strip width 0" },
    { 0x355E83, 1, 0x1000, "strip height 1" }, { 0x355E88, 1, 0x400, "strip width 1" },
    { 0x355F82, 1, 0x1000, "strip height 2" }, { 0x355F87, 1, 0x400, "strip width 2" },
    { 0x356064, 1, 0x1000, "strip height 3" }, { 0x356069, 1, 0x400, "strip width 3" },
    { 0x35608C, 1, 0x1000, "strip height 4" }, { 0x35609D, 1, 0x400, "strip width 4" },
    { 0x35612F, 1, 0x400,  "cascade size / rect right" },
    { 0x35614C, 1, 0x800,  "rect top/bottom 1" },
    { 0x356169, 1, 0xC00,  "rect top/bottom 2" },
    { 0x356187, 6, 0x1000, "rect bottom 3" },
    { 0x3562B5, 1, 0x400,  "per-cascade w+h" },
};
// v60: the other half of the atlas, which lives in a DATA file.
//
// The 15 immediates above only decide how big the render target is.  What reads it is the PCF
// filter compiled into data\shader3.bin, and that shader carries its geometry as a baked
// constant: every one of the 315 shadow pixel shaders opens with a def_c float4 of
//
//     (0.5/W, 0.5/H, W, H)          shipped: (1/2048, 1/8192, 1024, 4096)
//
// plus two tap-offset vectors of the same family.  Measured on the shipped file: each of those
// three 16-byte vectors occurs EXACTLY 315 times, every occurrence lands on a def_c record
// payload (zero misaligned hits), and each atlas vector pairs with exactly one of each tap
// vector inside the next 200 bytes.  A whole-exe scan finds zero copies of all three, so there
// is no runtime table that could override them - the file is the only place they exist.
//
// Consequence, and the reason this build refuses rather than patches: if the exe grows the
// atlas and the shader still believes it is 1024 wide, the taps land two texels apart and the
// bilinear weights run at half the texel rate, so the kernel no longer matches its own grid and
// the shadows band and streak along every cascade edge.  That is worse than shipping, not
// better.  Both halves have to move together, so the exe half is gated on the file having
// already been re-written by shadow_res.py, which is the only thing allowed to touch it.
static int g_shaderScale = 0;      // 0 = unknown / unreadable / mixed
static int g_atlasScaleLive = 1;   // the scale the exe immediates actually carry now

// The four components scale in opposite directions (the texel halves shrink, the dimensions
// grow), and every shipped value is dyadic, so any power of two is bit-exact in both
// directions - which is why the offered scales are 1, 2, 4 and 8.  x8 is a boot test, not a
// known-good setting: it wants an 8192x32768 depth strip, and D3D9 caps texture height at
// 8192/16384 on most hardware, so the allocation can fail outright at startup.
static void ShadowAtlasBytes(int scale, BYTE* out) {
    const float base[4] = { 1.0f / 2048.0f, 1.0f / 8192.0f, 1024.0f, 4096.0f };
    float v[4];
    v[0] = base[0] / (float)scale;
    v[1] = base[1] / (float)scale;
    v[2] = base[2] * (float)scale;
    v[3] = base[3] * (float)scale;
    memcpy(out, v, sizeof(v));
}

static int ShadowCount(const BYTE* buf, DWORD len, const BYTE* pat) {
    int n = 0;
    for (DWORD i = 0; i + 16 <= len; ++i)
        if (buf[i] == pat[0] && buf[i + 1] == pat[1] && !memcmp(buf + i, pat, 16)) ++n;
    return n;
}

// Read data\shader3.bin and say which atlas scale it is baked at.  Exactly one of 1, 2, 4 or 8 can
// have hits, because going from one to the other rewrites the bytes; more than one means the
// file was edited by something else or a write was interrupted, and 0 means no idea.
static int ShadowProbeShader() {
    char path[MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%s\\data\\shader3.bin", g_moduleDir);
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("ShadowShader: cannot open %s (%lu) - the atlas scale is unknown, so x>1 is refused",
            path, GetLastError());
        return 0;
    }
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == 0 || sz == INVALID_FILE_SIZE || sz > (16u << 20)) {
        Log("ShadowShader: %u bytes is not a shader3.bin - x>1 is refused", sz);
        CloseHandle(h);
        return 0;
    }
    BYTE* buf = (BYTE*)VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = buf != nullptr;
    if (ok) {
        for (DWORD done = 0; done < sz; ) {
            DWORD want = sz - done, n = 0;
            if (!ReadFile(h, buf + done, want, &n, nullptr) || n == 0) { ok = false; break; }
            done += n;
        }
    }
    CloseHandle(h);
    if (!ok) {
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        Log("ShadowShader: %s could not be read - x>1 is refused", path);
        return 0;
    }
    int found = 0, mixed = 0;
    for (int s = 1; s <= 8; s <<= 1) {
        BYTE pat[16];
        ShadowAtlasBytes(s, pat);
        int n = ShadowCount(buf, sz, pat);
        if (n) { found = s; ++mixed; Log("ShadowShader: %d hits of the x%d atlas constant "
            "%02X %02X %02X %02X ...", n, s, pat[0], pat[1], pat[2], pat[3]); }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    if (mixed != 1) {
        Log("ShadowShader: %d different scales present in one file, so it is not a clean x1/x2/x4/x8 "
            "- x>1 is refused.  Run 'python tools\\shadow_res.py restore'.", mixed);
        return 0;
    }
    return found;
}

static void ApplyShadowMapScale() {
    int scale = g_cfg.shadowMapScale;
    if (scale <= 1) {
        if (g_shaderScale > 1)
            Log("ShadowScale: data\\shader3.bin is baked at x%d while [Shadow] MapSizeScale=%d, so "
                "the PCF filter believes the atlas is %dx%d and the game is allocating it %dx%d. "
                "Either set MapSizeScale=%d or run 'python tools\\shadow_res.py restore'.",
                g_shaderScale, scale, 1024 * g_shaderScale, 4096 * g_shaderScale, 1024, 4096,
                g_shaderScale);
        return;
    }
    if (g_shaderScale != scale) {
        Log("ShadowScale: x%d NOT applied.  The 15 atlas immediates are one half of this; the "
            "other half is the texel constant baked into data\\shader3.bin, which is currently at "
            "x%d.  Scaling only the render target leaves the PCF taps two texels apart in a map "
            "twice as wide, which bands and streaks every cascade edge - worse than stock.  With "
            "the game CLOSED, run:  python tools\\shadow_res.py set %d",
            scale, g_shaderScale, scale);
        return;
    }
    HMODULE base = GetModuleHandleW(nullptr);
    const int n = (int)(sizeof(g_shadowSites) / sizeof(g_shadowSites[0]));
    int bad = 0;
    __try {
        for (int i = 0; i < n; ++i) {
            const ShadowSizeSite* s = &g_shadowSites[i];
            const BYTE* p = (const BYTE*)base + s->rva;
            DWORD cur; memcpy(&cur, p + s->off, 4);
            if (cur != s->expect) {
                Log("ShadowScale: 0x%08X (%s) holds %u, expected %u - NOTHING written",
                    (DWORD)(uintptr_t)p, s->what, cur, s->expect);
                ++bad;
            }
        }
        if (bad) return;
        int done = 0;
        for (int i = 0; i < n; ++i) {
            const ShadowSizeSite* s = &g_shadowSites[i];
            BYTE* p = (BYTE*)base + s->rva;
            DWORD nv = s->expect * (DWORD)scale;
            if (PatchBytes(p + s->off, (const BYTE*)&nv, 4, nullptr)) ++done;
        }
        Log("ShadowScale: x%d on %d/%d immediates - atlas %ux%u -> %ux%u, cascade %ux%u, and "
            "data\\shader3.bin was confirmed to carry the matching (0.5/W, 0.5/H, W, H) constant, "
            "so the PCF taps stay exactly one texel apart.",
            scale, done, n, 1024u, 4096u, 1024u * (unsigned)scale, 4096u * (unsigned)scale,
            1024u * (unsigned)scale, 1024u * (unsigned)scale);
        if (done == n) g_atlasScaleLive = scale;
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("ShadowScale: write faulted, scale aborted"); }
}

// ---------------- v49: shadow CAST distance ----------------
// ShadowBaseDistance is a real float, stored once per level load into [[0x00DFC2F8]+0x2BC0] by
// the property setter at 0x005B04C1 (`F3 0F 11 80 C0 2B 00 00` = movss [eax+0x2bc0],xmm0) and
// read by EXACTLY ONE consumer in all of .text: 0x00859322 `movss xmm4,[esi+0x2bc0]` inside the
// band builder 0x00858F33, which turns it into four caster bands as base * 3^i.  Re-read the loop
// byte by byte for v58 and it is exactly that: 0x0085932A..0x0085934D is a binary exponentiation
// of xmm6 (= [0x00BA8990] = 3.0, loaded at 0x008592FB) to |i|, 0x0085935F `mulss xmm0,xmm4` scales
// it by our xmm4, and 0x00859363 stores it and walks edx by 4 for i=0..3.  The bands are 1/3/9/27
// times the stored base, NOT a cumulative 1/4/13/40 as one research pass claimed - the `addss
// xmm0,xmm2 / movaps xmm2,xmm0` pair at 0x00859370 sits AFTER the store and xmm2 is never read
// inside the loop, so it feeds code outside, not the array.  Everything else
// in the file that mentions the displacement is coincidence (a `call` and a `jmp` operand) or the
// getter.  So one site controls how far out shadow casters are drawn, and it is a multiply.
//
// The scale is applied AT THE CONSUMER, not to the stored value, for two reasons: the game may
// have loaded the level before this DLL runs, and leaving the stored float alone keeps the game's
// own state honest - reverting the ini key reverts the effect with nothing to repair.
//
// Shape: the site is 8 bytes, so it takes a 5-byte E9 plus 3 NOPs, and the cave replays the
// original load, multiplies, and returns to site+8.  MOVSS and MULSS do not write EFLAGS, and the
// instruction after the site (0x0085932A `0F 28 C6` = movaps xmm0,xmm6) needs none either, while
// the `test eax,eax` two bytes before it feeds a `jg` further down - so the flags that the site
// straddles survive untouched.  esi, eax and ecx are all preserved: the cave reads esi and writes
// only xmm4.
#define SHADOW_BASE_SITE_RVA 0x459322   // VA 0x00859322
static const BYTE g_shadowBaseOrig[8] = { 0xF3, 0x0F, 0x10, 0xA6, 0xC0, 0x2B, 0x00, 0x00 };
static bool g_shadowBasePatched = false;

static void ApplyShadowBaseScale() {
    // With the menu armed the cave is installed even at 1.0 so the slider has something to
    // drive; multiplying by exactly 1.0f is a bit-exact no-op on a float.
    if (g_cfg.shadowBaseScale == 1.0f && !g_cfg.menuEnabled) return;
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* site = (BYTE*)base + SHADOW_BASE_SITE_RVA;
    __try {
        if (memcmp(site, g_shadowBaseOrig, sizeof(g_shadowBaseOrig)) != 0) {
            Log("ShadowBase: consumer 0x00859322 does not hold F3 0F 10 A6 C0 2B 00 00 - NOTHING written");
            return;
        }
        // Report the value the shipped data actually loaded, so the effect of the scale is a
        // number the user can check rather than an abstraction.
        float stored = 0.0f;
        const DWORD* viewport = (const DWORD*)(((BYTE*)base) + 0x9FC2F8);   // VA 0x00DFC2F8
        if (*viewport) stored = *(const float*)((const BYTE*)*viewport + 0x2BC0);
        // A zero here is NOT proof the lever is dead: the multiply happens at draw time on
        // whatever the game loads then, while this probe runs at init, when the renderer
        // object may not exist yet.  It does mean the banner below cannot quote real bands,
        // so the capture key re-reads the same slot mid-play (CAPTURE shadowbase).
        if (stored == 0.0f)
            Log("ShadowBase: probe read 0 from the renderer slot at init (object not up yet, "
                "or this is not the distance field) - bands unknown until a capture.");

        BYTE* cave = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!cave) { Log("ShadowBase: cave allocation failed"); return; }
        memcpy(cave, g_shadowBaseOrig, 8);                                   // movss xmm4,[esi+0x2bc0]
        // mulss xmm4, dword ptr [abs]: F3 0F 59 <modrm 0x24 = reg4/rm100> <sib 0x25 = disp32> disp32.
        // (0x3D would decode as reg=xmm7, rm=ebp+disp32 - it clobbers a live register and reads
        // garbage, which is exactly the kind of bug a hand-written stub earns.)
        cave[8] = 0xF3; cave[9] = 0x0F; cave[10] = 0x59; cave[11] = 0x24; cave[12] = 0x25;
        DWORD addr = (DWORD)(uintptr_t)&g_cfg.shadowBaseScale;
        memcpy(cave + 13, &addr, 4);
        cave[17] = 0xE9;                                                     // jmp site+8
        LONG back = (LONG)((DWORD_PTR)(site + 8) - (DWORD_PTR)(cave + 22));
        memcpy(cave + 18, &back, 4);

        BYTE patch[8];
        patch[0] = 0xE9;
        LONG fwd = (LONG)((DWORD_PTR)cave - (DWORD_PTR)(site + 5));
        memcpy(patch + 1, &fwd, 4);
        patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90;
        if (!PatchBytes(site, patch, sizeof(patch), nullptr)) {
            Log("ShadowBase: VirtualProtect on the consumer failed");
            return;
        }
        g_shadowBasePatched = true;
        Log("ShadowBase: x%.2f at the single consumer 0x00859322 - shipped value was %-.7g, so the four "
            "caster bands go from %-.5g/%-.5g/%-.5g/%-.5g to %-.5g/%-.5g/%-.5g/%-.5g. Shadows reaching "
            "further costs fill rate; set back to 1.0 to disable.",
            g_cfg.shadowBaseScale, stored,
            stored, stored * 3.0f, stored * 9.0f, stored * 27.0f,
            stored * g_cfg.shadowBaseScale, stored * 3.0f * g_cfg.shadowBaseScale,
            stored * 9.0f * g_cfg.shadowBaseScale, stored * 27.0f * g_cfg.shadowBaseScale);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ShadowBase: faulted, cast-distance scale aborted");
    }
}

// v58: the crouch/slide PITCH LOCK.  User report: "not being able to move camera up and down
// during/moments after slide".  Verified against the shipped exe (md5
// 857b3387d54774a32c1328effb5de4d4), inside HUMAN's vtable slot 4 (0x0071DC40):
//
//   0x0071DDB0  call 0x0071D790            ; fills a state byte at [esp+0x17]
//   0x0071DDB5  mov  al, [esp+0x17]
//   0x0071DE09  test al, al                ; same al, still live - the gate
//   0x0071DE11  jne  0x0071DE90            ; 75 7D, state set => LOCKED path
//
// The two paths differ in exactly one respect, and it is the reported symptom:
//
//   fall-through (0x0071DE13) - calls 0x0071D0A0, then
//       0x0071DE30  movss xmm0, [esp+0x18]        ; yaw delta
//       0x0071DE55  addss xmm1, xmm0              ; -> [ebx+0x618]
//       0x0071DE62  movups xmm0, [ebx+0x61c]
//       0x0071DE69  addss xmm0, [esp+0x1c]        ; PITCH delta is consumed
//   taken (0x0071DE90) -
//       0x0071DE90  movss xmm1, [esp+0x18]        ; yaw only
//       0x0071DEB1  movss xmm2, [ebx+0x618]
//       0x0071DEB9  addss xmm2, xmm1
//       0x0071DEBD  movss [ebx+0x618], xmm2       ; [esp+0x1c] never read: pitch dropped
//
// Both rejoin at 0x0071DFAA.  So the locked branch is a pure "discard vertical aim while the
// state byte is set".  NOP-ing the jne makes the free-aim branch unconditional.
//
// Why init-only and NOT menu-togglable: this is on the per-frame hot path, and the patch is two
// bytes.  A writer cannot make a 2-byte store atomic with respect to a thread that is fetching
// those bytes as instructions - the game can decode the half-applied pair as `nop` + `jg
// <displacement>` and branch to garbage.  Restoring at init (or not patching at all) is the only
// safe shape, so the switch is read once at startup and a restart is required to change it.
#define PITCH_GATE_RVA 0x31DE11   // VA 0x0071DE11
static const BYTE g_pitchGateOrig[2] = { 0x75, 0x7D };
static const BYTE g_pitchGateNop[2]  = { 0x90, 0x90 };
static int g_pitchGateOff = 0;      // 1 = the lock has been removed this run

static void ApplyFreePitchGate() {
    if (!g_cfg.freePitchCrouch) {
        Log("PitchGate: [Camera] FreePitchInCrouch=0, so the shipped crouch/slide pitch lock at "
            "0x0071DE11 is left in place - vertical aim stays disabled while that state is set.");
        return;
    }
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* site = (BYTE*)base + PITCH_GATE_RVA;
    __try {
        if (memcmp(site, g_pitchGateOrig, sizeof(g_pitchGateOrig)) != 0) {
            Log("PitchGate: 0x0071DE11 holds %02X %02X, not the expected 75 7D branch - NOTHING "
                "written.  This exe is not the one the patch was verified against.", site[0], site[1]);
            return;
        }
        // 0x0071DE11 is a rel8 conditional jump, so its 2 bytes are replaced by 2 NOPs of the
        // same length and every branch target in the function is still exactly where the
        // compiler put it.  No re-layout, no relocation, nothing else can point into the site.
        if (!PatchBytes(site, g_pitchGateNop, sizeof(g_pitchGateNop), nullptr)) {
            Log("PitchGate: VirtualProtect on 0x0071DE11 failed, lock left in place");
            return;
        }
        g_pitchGateOff = 1;
        Log("PitchGate: NOP-patched the branch at 0x0071DE11, so the pitch delta at [esp+0x1c] is "
            "always applied (0x0071DE69) instead of being dropped by the locked path at "
            "0x0071DE90.  Look up/down during crouch, slide and recovery.  Restart with "
            "[Camera] FreePitchInCrouch=0 to get the shipped behaviour back.");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("PitchGate: read/write at 0x0071DE11 faulted, nothing changed");
    }
}

// v59: the VEHICLE camera's re-centre.  User report: "car camera re centres very fast and
// sharply - make it do with bigger delay and with more smoothnes instead of being very abrupt".
//
// The class is CAM-A, vtable 0x00BD2060, ctor 0x00715D20 (verified: 0x00715D41 `c7 07 60 20 bd 00`
// = mov [edi],0x00BD2060 with edi = the object, since the factory's `call 0x00715D20` at
// 0x006A65A5 is what builds it).  What it is built FROM decides whether this is the car camera at
// all, and the answer is in the shipped property-name pool: the record the factory reads for this
// ctor is deserialized by 0x0065E1D0, whose only properties are StickLength,
// StickLengthAtMaxFovSpeed, FocusOffset, YawResetTime, DefaultPitch, SpringDampening,
// SpringStrength, FirstPersonOffset, FirstPersonPitch, FirstPersonReverseOffset and
// FirstPersonReversePitch.  A camera record with a stick length, a spring, a yaw reset time and
// first-person forward/reverse seats is a vehicle chase camera; the on-foot camera (0x00BD2100)
// and the turret (0x00BD2200) are built from other records.
//
// Two of its fields are exactly the two things the report names, and both are written in the ctor:
//
//   0x00715DBA  movss xmm0,[esp+0x24]        ; YawResetTime, already divided by 12 by the
//   0x00715DC0  movss [edi+0x668],xmm0       ; factory at 0x006A6540 (x [0x00BEB5B4]=0.08333)
//   0x00715DC8  movss xmm0,[0x00BBB99C]      ; 0.5, compiled in - NOT data-driven
//   0x00715DD0  movss [edi+0x66C],xmm0
//
// and one method consumes them as the re-centre timer (0x00717780, reached from 0x00716E30):
//
//   while look input is live, re-arm:   0x00717923 movss xmm1,[+0x66C]
//                                       0x0071792B addss xmm1,[+0x668]
//                                       0x00717933 movss [+0x64C],xmm1
//   otherwise count down:               0x00717800 movss xmm1,[+0x64C]
//                                       0x00717808 subss xmm1,xmm5   (xmm5 = dt), floored at 0
//   blend weight:                       0x00717193 movss xmm0,[+0x64C]
//                                       0x0071719B divss xmm0,[+0x66C]
//                                       0x007171A6..0x007171C8  k = clamp(that, 0, 1)
//   applied once, at:                   0x007172BB movss xmm0,[esp+0x14]  (k)
//                                       0x007172C3 movss xmm1,1.0 ; subss xmm1,xmm0  (1-k)
//                                       0x007172CB mulss xmm0,[+0x620] ; 0x007172D3 mulss xmm1,[+0x664]
//                                       0x007172DB addss ; 0x007172E3 movss [+0x620],...
//
// So the countdown starts at window+delay, which makes k = 1 (nothing blended away) until the
// delay has passed, and then walks 1 -> 0 across the window while the field is lerped toward its
// default.  **+0x668 is the delay and +0x66C is the duration of the blend** - the two halves of
// the complaint, and they are independent: raising only the delay postpones the snap without
// softening it, which is the shape of the shipped 0.271 s delay + hard 0.5 s window.
//
// +0x66C is a DIVISOR (0x0071719B), so it must never reach 0; the scale multiplies 0.5 upward,
// which cannot do that, and the clamp at 0x007171A6 means a small k degrades safely rather than
// dividing anything else.  k is read at exactly ONE site in this function (a scan of
// 0x00717100..0x00717620 for the [esp+0x14] slot finds one write stream and one read), so this
// blend is the pitch/DefaultPitch one; the yaw half of the state machine was not pinned down, and
// the honest claim is that both halves run off the SAME countdown [+0x64C], which is what the
// delay term gates.
//
// Shape: same as the shadow and turret-rate caves.  Each site is the 8-byte store, replaced by
// E9 + 3 NOPs; the cave does `mulss xmm0,[abs scale]` (the encoding is copied from a real one in
// the exe, 0x00716AF6), replays the store, and returns to site+8.  xmm0 is the value being stored,
// edi is the object and is not touched, MOVSS/MULSS write no EFLAGS, and the neighbours on both
// sides are plain SSE loads/stores that read none.  A .text-wide scan of every rel8/rel32 branch
// and call found ZERO targets landing inside either 8-byte window.
//
// It is a CONSTRUCTOR patch, so the change applies to the next vehicle you enter - the same
// semantics as [Turret] CommandResponse, and the reason the panel row says so.
#define VEH_DELAY_SITE_RVA 0x315DC0   // VA 0x00715DC0  movss [edi+0x668],xmm0
#define VEH_BLEND_SITE_RVA 0x315DD0   // VA 0x00715DD0  movss [edi+0x66C],xmm0
static const BYTE g_vehDelayOrig[8] = { 0xF3, 0x0F, 0x11, 0x87, 0x68, 0x06, 0x00, 0x00 };
static const BYTE g_vehBlendOrig[8] = { 0xF3, 0x0F, 0x11, 0x87, 0x6C, 0x06, 0x00, 0x00 };
static bool g_vehDelayOn = false, g_vehBlendOn = false;

// Installs one multiply-ahead-of-store cave.  Returns false without writing if the site does not
// hold the bytes the patch was verified against.
static bool InstallScaleCave(DWORD siteRva, const BYTE* expect, float* scale, const char* what) {
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* site = (BYTE*)base + siteRva;
    if (memcmp(site, expect, 8) != 0) {
        Log("%s: 0x%08X does not hold the expected store - NOTHING written", what,
            (DWORD)(uintptr_t)site);
        return false;
    }
    BYTE* cave = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) { Log("%s: cave allocation failed", what); return false; }
    // mulss xmm0, dword ptr [disp32] : F3 0F 59 05 <disp32>.  The field being scaled lives in
    // g_cfg, so the menu changes it for the NEXT construction without any code being rewritten.
    cave[0] = 0xF3; cave[1] = 0x0F; cave[2] = 0x59; cave[3] = 0x05;
    DWORD addr = (DWORD)(uintptr_t)scale;
    memcpy(cave + 4, &addr, 4);
    memcpy(cave + 8, expect, 8);                       // the original store
    cave[16] = 0xE9;                                    // jmp site+8
    LONG back = (LONG)((DWORD_PTR)(site + 8) - (DWORD_PTR)(cave + 21));
    memcpy(cave + 17, &back, 4);
    BYTE patch[8];
    patch[0] = 0xE9;
    LONG fwd = (LONG)((DWORD_PTR)cave - (DWORD_PTR)(site + 5));
    memcpy(patch + 1, &fwd, 4);
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90;
    if (!PatchBytes(site, patch, sizeof(patch), nullptr)) {
        Log("%s: VirtualProtect on 0x%08X failed", what, (DWORD)(uintptr_t)site);
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }
    Log("%s: cave installed at CAM-A's ctor store 0x%08X (scale x%.2f, applied at construction).",
        what, (DWORD)(uintptr_t)site, (double)*scale);
    return true;
}

static void ApplyVehicleRecentre() {
    // With the menu armed the caves install at 1.0 so the two sliders have something to drive;
    // x1.0f is a bit-exact no-op on a float.
    bool needDelay = (g_cfg.vehDelayScale != 1.0f) || g_cfg.menuEnabled;
    bool needBlend = (g_cfg.vehBlendScale != 1.0f) || g_cfg.menuEnabled;
    if (!needDelay && !needBlend) {
        Log("CarCam: both [Vehicle] scales are 1.0 and the menu is off, so CAM-A's ctor is "
            "untouched - the vehicle camera keeps the shipped 0.271 s delay and 0.500 s blend.");
        return;
    }
    __try {
        if (needDelay)
            g_vehDelayOn = InstallScaleCave(VEH_DELAY_SITE_RVA, g_vehDelayOrig,
                                            &g_cfg.vehDelayScale, "CarCam delay");
        if (needBlend)
            g_vehBlendOn = InstallScaleCave(VEH_BLEND_SITE_RVA, g_vehBlendOrig,
                                            &g_cfg.vehBlendScale, "CarCam blend");
        // The banner numbers are quoted from the shipped data, not invented: the delay slot is
        // YawResetTime 3.25 divided by 12 ([0x00BEB6B4] and [0x00BEB5B4], both read from the
        // exe), and the blend window is the compiled-in 0.5 at [0x00BBB99C].
        if (g_vehDelayOn || g_vehBlendOn)
            Log("CarCam: shipped values are YawResetTime 3.25 / 12 = 0.2708 s delay plus a 0.500 s "
                "blend window, i.e. the camera holds for a quarter second and then takes half a "
                "second to settle - now %.3f s delay and %.3f s blend for the NEXT vehicle you "
                "enter.", 0.2708333 * g_cfg.vehDelayScale, 0.5 * g_cfg.vehBlendScale);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CarCam: read/write faulted, CAM-A untouched");
    }
}

// v56: THE turret speed limit.  Found by external measurement and then verified at the byte
// level against the shipped exe (md5 857b3387d54774a32c1328effb5de4d4) and against a live
// ReadProcessMemory of the paused game, so nothing here is inferred.
//
// The turret is served by camera class CAM-E: vtable 0x00BD2200, written at 0x00722C85 inside
// ctor 0x00722C70, whose only caller is the camera factory 0x006A59D0.  **No HUMAN object is
// allocated at all while mounted** - run 3 caught +0x000 change away from 0x00BD2200 and back
// again exactly when the user dismounted and remounted - which is why every patch from v42 to
// v55, all of them aimed at the HUMAN class, logged "applied" and changed nothing.
//
// CAM-E's per-frame update is vtable slot 4 ([0x00BD2200+0x10] = 0x00723140), and inside it:
//
//   0x0072328B  movss xmm1, [ebx+0x668]   ; yaw rate, rad/s.  Live value 1.5 exactly.
//   0x00723293  mulss xmm1, [ebx+0x648]   ; yaw aim state - the OUTPUT of a lag filter, see v57
//   0x0072329B  movss xmm0, [ebp+8]       ; x dt
//   0x007232A5  mulss xmm1, xmm0
//   0x007232AE  movss [esp+0x1C], xmm1    ; -> this frame's yaw delta
//   0x007232B4  movss xmm1, [ebx+0x66C]   ; pitch rate, rad/s.  Live value 1.5 exactly.
//   0x007232BC  mulss xmm1, [ebx+0x630]
//   0x007232CD  mulss xmm1, xmm0
//   0x007232D6  movss [esp+0x20], xmm1    ; -> this frame's pitch delta
//
// 1.5 rad/s is 85.94 deg/s, so a 180 degree turn cannot take less than 2.09 s however hard the
// mouse is pushed.  Measured over a live 30 s sweep the view peaked at 0.4931 deg/frame
// (72 deg/s, i.e. 84% of that ceiling) and the mean rate stayed flat at 11.9-17.8 deg/s while
// the mouse input ranged over 900x - a rate limit, not a clamp on the input.  The raw input
// really is unbounded: +0x010 reached +-3881 with atMax 0.0%.
//
// CORRECTION, established after v56 shipped: the claim that "+0x648 has a flat histogram with a
// smooth tail and no pile-up, so nothing upstream saturates" was WRONG, and that reading is
// exactly why v56 only helped partly.  +0x648 is not an input factor at all - it is the STATE of
// an exponential lag filter, and a lag filter's value only reaches its ceiling if the input is
// held long enough, so a broad, pile-up-free histogram is the symptom of the saturation, not
// evidence against it.  The saturating clamp is one instruction earlier: 0x0072302D scales the
// raw input by 0.25 and 0x0072304A/0x00723054 clamp the result to +-1.  See the v57 block below
// for the full mechanism.
// This also EXPLAINS why v54's saturation clamp at 0x006999B4 was exonerated by direct
// measurement: it was never the limiter.
//
// +0x668/+0x66C are written ONLY in the ctor, at 0x00722CFD/0x00722D0B, from stack args the
// factory fills out of a loaded parameter record - there is no constant in the exe to enlarge
// in place, so this scales the read.  Other camera classes reuse the same offsets for their own
// purposes - CAM-B's ctor stores floats to [+0x668]/[+0x66C] at 0x00719013/0x00719021 and
// CAM-A's at 0x00715DC0/0x00715DD0 (see the v59 block) - and patching CAM-E's two sites cannot
// reach any of them.  CAM-E's own target-tracking slew at 0x00723D92 also reads
// [+0x668] and is DELIBERATELY left at the shipped rate: that path is the sticky-over-an-enemy
// behaviour the user wants gone, not sped up.
//
// Shape: each site is 8 bytes, so it takes a 5-byte E9 plus 3 NOPs, and the cave replays the
// original load, multiplies, and jumps back to site+8.  xmm1 is a fresh load at both sites so
// nothing is clobbered; MOVSS/MULSS write no EFLAGS and neither neighbour reads any; ebx and
// ebp are untouched.  A brute-force scan of every rel8/rel32 branch and call in .text found
// ZERO targets inside either 8-byte window.  The scale lives in g_cfg so the capture key can
// change it at runtime and the next frame picks it up.
#define TURRET_RATE_YAW_RVA   0x32328B   // VA 0x0072328B
#define TURRET_RATE_PITCH_RVA 0x3232B4   // VA 0x007232B4
static const BYTE g_turretYawOrig[8]   = { 0xF3, 0x0F, 0x10, 0x8B, 0x68, 0x06, 0x00, 0x00 };
static const BYTE g_turretPitchOrig[8] = { 0xF3, 0x0F, 0x10, 0x8B, 0x6C, 0x06, 0x00, 0x00 };
static bool g_turretYawOn = false, g_turretPitchOn = false;

static void ApplyTurretTurnRate() {
    // Same arming rule as the shadow cave: installed at 1.0 when the menu is on, because the
    // cave multiplies by g_cfg.turretTurnRate every frame and 1.5f * 1.0f is the shipped 1.5f.
    // v62 adds a second reason to install even with the scale at 1.0: the floor is the artillery
    // fix, and it is what makes a slow mount match the light turret without touching the turret.
    if (g_cfg.turretTurnRate == 1.0f && g_cfg.turretMinRate == 0.0f && !g_cfg.menuEnabled) return;
    HMODULE base = GetModuleHandleW(nullptr);
    struct { DWORD rva; const BYTE* orig; bool* done; const char* axis; } sites[2] = {
        { TURRET_RATE_YAW_RVA,   g_turretYawOrig,   &g_turretYawOn,   "yaw"   },
        { TURRET_RATE_PITCH_RVA, g_turretPitchOrig, &g_turretPitchOn, "pitch" },
    };
    for (int i = 0; i < 2; ++i) {
        BYTE* site = (BYTE*)base + sites[i].rva;
        __try {
            // Check all 8 bytes: the two sites differ only in the displacement byte, and the
            // wrong one would still "work" while scaling the wrong axis.
            if (memcmp(site, sites[i].orig, 8) != 0) {
                Log("TurretRate: %s site VA 0x%08X does not hold %02X %02X %02X %02X %02X %02X %02X %02X "
                    "(found %02X %02X %02X %02X %02X %02X %02X %02X) - NOTHING written",
                    sites[i].axis, 0x400000 + sites[i].rva,
                    sites[i].orig[0], sites[i].orig[1], sites[i].orig[2], sites[i].orig[3],
                    sites[i].orig[4], sites[i].orig[5], sites[i].orig[6], sites[i].orig[7],
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6], site[7]);
                continue;
            }
            BYTE* cave = (BYTE*)VirtualAlloc(nullptr, 40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!cave) { Log("TurretRate: cave allocation failed for %s", sites[i].axis); continue; }
            memcpy(cave, sites[i].orig, 8);                    // replay movss xmm1,[ebx+0x668/0x66C]
            // mulss xmm1, dword ptr [abs] = F3 0F 59 0D <abs32>.  ModRM 0x0D is mod 00,
            // reg 001 (xmm1), rm 101 (disp32), and needs NO SIB - unlike xmm4, which is
            // reg 100 and does.  Getting that wrong is how a stub ends up clobbering a live
            // register, so it is spelled out here.
            cave[8] = 0xF3; cave[9] = 0x0F; cave[10] = 0x59; cave[11] = 0x0D;
            DWORD addr = (DWORD)(uintptr_t)&g_cfg.turretTurnRate;
            memcpy(cave + 12, &addr, 4);
            // v62: maxss xmm1, dword ptr [abs] = F3 0F 5F 0D <abs32>, the same ModRM 0x0D for
            // xmm1.  This is the floor that fixes the mounts whose parameter record carries a
            // slower base rate than the light turret - see the MinRateRad comment.  MAXSS's
            // documented NaN rule works in our favour here: if either operand is NaN the result
            // is the SECOND operand, and the second operand is our global, so a NaN arriving from
            // a corrupt mount record cannot poison the floor - it is discarded.
            cave[16] = 0xF3; cave[17] = 0x0F; cave[18] = 0x5F; cave[19] = 0x0D;
            DWORD floorAddr = (DWORD)(uintptr_t)&g_cfg.turretMinRate;
            memcpy(cave + 20, &floorAddr, 4);
            cave[24] = 0xE9;                                   // jmp site+8
            LONG back = (LONG)((DWORD_PTR)(site + 8) - (DWORD_PTR)(cave + 29));
            memcpy(cave + 25, &back, 4);

            BYTE patch[8];
            patch[0] = 0xE9;
            LONG fwd = (LONG)((DWORD_PTR)cave - (DWORD_PTR)(site + 5));
            memcpy(patch + 1, &fwd, 4);
            patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90;
            if (!PatchBytes(site, patch, sizeof(patch), nullptr)) {
                Log("TurretRate: VirtualProtect failed on the %s site", sites[i].axis);
                continue;
            }
            *sites[i].done = true;
            Log("TurretRate: %s rate x%.2f with floor %.3f rad/s at VA 0x%08X (cave %p, 33 bytes: "
                "replay load, mulss scale, maxss floor, jmp back).  Shipped CAM-E rate is 1.5 rad/s "
                "= 85.94 deg/s, so the ceiling becomes %.1f deg/s and a 180 degree turn takes "
                "%.2f s instead of 2.09 s.  TurnRateScale=1.0 removes the multiply, "
                "MinRateRad=0.0 removes the floor.",
                sites[i].axis, g_cfg.turretTurnRate, g_cfg.turretMinRate,
                0x400000 + sites[i].rva, cave,
                85.9437f * g_cfg.turretTurnRate, 3.14159265f / (1.5f * g_cfg.turretTurnRate));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("TurretRate: faulted on the %s site, left untouched", sites[i].axis);
        }
    }
}

// ---------------- v62: CAM-B's twin of the CAM-E aim-rate limiter ----------------
// Agent A's strict linear sweep of all five classes' slot-4 updates, byte-reverified here, found
// exactly ONE other class with CAM-E's `rate * command * dt` shape:
//
//   0x007197A8  E8 13 3D FF FF   call 0x0070D4C0                    ; the SAME shared lag advancer
//   0x007197AD  F3 0F 10 8B 58 06 00 00   movss xmm1,[ebx+0x658]    <<< RATE  (8 bytes)
//   0x007197B5  F3 0F 59 8B 40 06 00 00   mulss xmm1,[ebx+0x640]    <- command factor
//   0x007197BD  F3 0F 59 4D 08            mulss xmm1,[ebp+8]        <- dt
//   0x007197C2  F3 0F 10 05 2C AA BE 00   movss xmm0,-0.0
//   0x007197D2  F3 0F 5C C1               subss xmm0,xmm1           ; -delta
//   0x007197D6  F3 0F 58 83 14 06 00 00   addss xmm0,[ebx+0x614]    ; accumulate
//   0x007197F8  F3 0F 11 83 14 06 00 00   movss [ebx+0x614],xmm0    ; after a clamp to +64C..+648
//
// The rate is CAM-B's +0x658, NOT +0x668: its ctor copies it from a per-record arg at
// 0x00719075 (`movss [edi+0x658],xmm0`, fed by `movss xmm0,[esp+0x20]` at 0x00719061).  This is
// the trap in this offset family again - CAM-A's +0x668/+0x66C are a hold/blend pair and CAM-E's
// are its rate, so a patch aimed at "the rate constant" has to name the class it reads it through.
// The sweep also proves the CAM-E sites are read only by CAM-E (`[reg+0x668]` loads exist at
// 0x0072272E/0x0072328B/0x00723D92, all inside CAM-E's own cluster), so nothing shipped so far
// could ever have reached a CAM-B-served mount.
//
// Not shipped on by default: no capture has yet shown CAM-B owning a slow aim.  The 105mm
// artillery is served by a live CAM-E (real instance with 0.38-quantised input in the v60 dump),
// which is what the floor above fixes; CAM-B is the remaining candidate for the next mount that
// behaves the same way, and one ini key turns this on without a rebuild of the patch.
//
// Shape identical to the CAM-E cave on purpose: the site is exactly 8 bytes, so E9+3NOP, and the
// cave replays the load, multiplies by the scale, applies the floor and jumps back to site+8.
// xmm1 is the destination of the replayed movss, so it is ours to extend; ebx and ebp are not
// touched; MOVSS/MULSS/MAXSS write no EFLAGS and the next instruction is a fresh MOVSS.
#define CAMB_RATE_YAW_RVA 0x3197AD   // VA 0x007197AD
static const BYTE g_camBRateOrig[8] = { 0xF3, 0x0F, 0x10, 0x8B, 0x58, 0x06, 0x00, 0x00 };
static bool g_camBRateOn = false;

static void ApplyCamBTurnRate() {
    if (!g_cfg.camBRateOn) return;
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* site = (BYTE*)base + CAMB_RATE_YAW_RVA;
    __try {
        if (memcmp(site, g_camBRateOrig, 8) != 0) {
            Log("CamBRate: yaw site VA 0x%08X does not hold %02X %02X %02X %02X %02X %02X %02X %02X "
                "(found %02X %02X %02X %02X %02X %02X %02X %02X) - NOTHING written",
                0x400000 + CAMB_RATE_YAW_RVA,
                g_camBRateOrig[0], g_camBRateOrig[1], g_camBRateOrig[2], g_camBRateOrig[3],
                g_camBRateOrig[4], g_camBRateOrig[5], g_camBRateOrig[6], g_camBRateOrig[7],
                site[0], site[1], site[2], site[3], site[4], site[5], site[6], site[7]);
            return;
        }
        BYTE* cave = (BYTE*)VirtualAlloc(nullptr, 40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!cave) { Log("CamBRate: cave allocation failed"); return; }
        memcpy(cave, g_camBRateOrig, 8);
        cave[8] = 0xF3; cave[9] = 0x0F; cave[10] = 0x59; cave[11] = 0x0D;
        DWORD addr = (DWORD)(uintptr_t)&g_cfg.turretTurnRate;
        memcpy(cave + 12, &addr, 4);
        cave[16] = 0xF3; cave[17] = 0x0F; cave[18] = 0x5F; cave[19] = 0x0D;
        DWORD floorAddr = (DWORD)(uintptr_t)&g_cfg.turretMinRate;
        memcpy(cave + 20, &floorAddr, 4);
        cave[24] = 0xE9;
        LONG back = (LONG)((DWORD_PTR)(site + 8) - (DWORD_PTR)(cave + 29));
        memcpy(cave + 25, &back, 4);
        BYTE patch[8];
        patch[0] = 0xE9;
        LONG fwd = (LONG)((DWORD_PTR)cave - (DWORD_PTR)(site + 5));
        memcpy(patch + 1, &fwd, 4);
        patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90;
        if (!PatchBytes(site, patch, sizeof(patch), nullptr)) {
            Log("CamBRate: VirtualProtect failed on the CAM-B yaw site");
            return;
        }
        g_camBRateOn = true;
        Log("CamBRate: CAM-B +0x658 yaw rate x%.2f floored at %.3f rad/s at VA 0x%08X (cave %p).  "
            "CAM-B PITCH has no rate*command*dt product (its 0x00719689 path uses subss/divss/"
            "comiss), so pitch on a CAM-B mount is NOT covered by this and stays as shipped.",
            g_cfg.turretTurnRate, g_cfg.turretMinRate, 0x400000 + CAMB_RATE_YAW_RVA, cave);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CamBRate: faulted on the CAM-B site, left untouched");
    }
}

// Turning this OFF does not rewrite the two sites back.  The cave multiplies by
// g_cfg.turretTurnRate every frame, so storing 1.0f there is a single aligned 4-byte write -
// atomic against the game thread - and 1.5f * 1.0f is exactly 1.5f in IEEE-754, i.e. the
// shipped value bit for bit.  Restoring the original 8 bytes instead would mean writing live
// code that the game may be executing mid-write, for no behavioural gain.
static void KillTurretTurnRate() {
    if (!g_turretYawOn && !g_turretPitchOn && !g_camBRateOn) return;
    g_cfg.turretTurnRate = 1.0f;
    // v62: the floor travels with the scale, because "shipped" means both of them off.  MAXSS
    // against 0.0f is inert for every mount record rate seen so far (the census ran 0.35..1.5,
    // all positive), so this restores the shipped behaviour exactly on the same one aligned
    // dword-per-slot basis as the scale.
    g_cfg.turretMinRate = 0.0f;
    Log("TurretRate: scale set to 1.00 and floor to 0.000 - yaw and pitch deltas are the shipped "
        "rate again, detours left installed and multiplying by exactly 1.0.  The CAM-B cave reads "
        "the same two globals, so %s covered by this too.",
        g_camBRateOn ? "PatchCamBTurnRate=1 is" : "no CAM-B patch is installed, and");
}

// v57 - THE ACTUAL LIMITER.  v56 doubled [+0x668] and the user's verdict was "better but not
// fully fixed, if you try to go left right fast turret aim will just stay in single spot".  A
// rate ceiling cannot do that; a term going to ZERO can, and this is that term.
//
// CAM-E's aim command [+0x648] is not a copy of the mouse.  It is the state slot of a 24-byte
// exponential track embedded in the camera at +0x638 (yaw) and +0x620 (pitch):
//
//   track+0x00 / +0x04   inertia while the input is NEW     <- CameraTurret.YawInputInertia
//   track+0x08           inertia while the input CONTINUES  <- CameraTurret.YawNoInputInertia
//   track+0x0C/0x0D      flags, 0 from the ctor
//   track+0x10           THE STATE, i.e. [+0x648] itself
//   track+0x14           "was advancing last frame"
//
// Every frame 0x00722EB0 calls the shared advancer 0x0070D4C0 four times (yaw/pitch, twice
// each) and that advancer's only store to track+0x10 is 0x0070D582, whose value comes from
// 0x0070D78E = `P + (V-P)*(1-X)*dt*60`.  So the command closes on its target by
// (1-X)*dt*60 of the gap per frame.  V is `-clamp([+0x010]*0.25, -1, +1)` - the mouse counts
// scaled by 0.25 (0x0072302D) and then clamped at +-1 (0x0072304A/0x00723054), so a hard flick
// and a gentle one both produce |V| = 1 and the command can never grow with the input.
//
// That is confirmed against the run-4 dump by INVERTING the equation, because it is the one
// thing in this chain that is not a guess:
//
//     X = 1 - (P_new - P) / ((V - P) * dt * 60)
//
// solved per frame from measured P ([+0x648]), measured input ([+0x010]) and the measured dt
// between the command's own transitions: 2706 frames, X = 0.955 (p10..p90 0.934..0.960) while
// the command grows and 0.915 while it shrinks, i.e. the live shipped values are 0.95 and 0.90,
// matching the exe's own defaults at 0x00DFDDC4 and 0x00BB3E44.  At the measured 139 fps
// (dt*60 = 0.43) that is 2.2% of the gap closed per frame, giving a per-frame command step of
// 0.022 - which is exactly the 0.017..0.022 band the fast-sweep frames showed and the 0.032
// cliff the slew census found.  Reversing the mouse therefore takes ~20 frames to walk the
// command back through zero, and on a quick left-right the command never leaves the middle:
// mean |P| falls 0.55 -> 0.16.  v56 scaled a ceiling that was never reached then, which is why
// it helped sustained turns and did nothing for sweeps.
//
// SHAPE: the six inertia floats are written ONLY in the ctor (0x00722E4B..0x00722E85 - a scan
// of CAM-E's whole vtable cluster and of the factory finds no other writer), so scaling them
// once at construction is durable and costs nothing per frame.  The hook goes at the ctor's
// single exit - the body 0x00722C70..0x00722E9C contains no `ret` at all - which is
// 0x00722E9D `8B C7` mov eax,edi / `5F` pop edi / `C2 60 00` ret 0x60.  Five bytes of E9 cover
// the first five of those six; the cave replays all six and never returns to the site, so the
// orphaned `00` byte left at 0x00722EA2 is unreachable.  A scan of .text for rel32 branches and
// of the whole image for absolute dwords found ZERO references into 0x00722E9D..0x00722EA3.
// edi is the freshly built camera and is dead after `mov eax,edi`; xmm0 is scratch, the ctor's
// last SSE use is `movss [edi+0x61c],xmm0` at 0x00722E95 and SSE registers are caller-saved, so
// the cave clobbers nothing the factory still wants (it reads only eax back).
//
// Deliberately NOT the advancer call sites.  Replacing `call 0x70d4c0` at 0x00723078 with
// `movss [esi+0x10],xmm1` would pin P to V exactly, but the helper ends `ret 8` and cleans the
// 8-byte argument slot the caller reserved with `sub esp,8` at 0x00723062 - so deleting the
// call leaves esp 8 bytes too low for the rest of 0x00722EB0, whose next `[esp]` writes land on
// the saved edi that `pop edi` then loads.  It would also overwrite the arc-limit zero the same
// function just wrote at 0x00722F92, which is what keeps the turret honest at its arc ends.
// Scaling the inertia instead keeps the whole shipped algorithm - arc zero, dead-zone reset,
// dt sub-stepping at 1/60 - and only changes how fast it closes.
#define TURRET_CTOR_TAIL_RVA 0x322E9D   // VA 0x00722E9D
static const BYTE g_cmdTailOrig[6] = { 0x8B, 0xC7, 0x5F, 0xC2, 0x60, 0x00 };
static bool g_cmdLagOn = false;

// The six floats, in the order the ctor writes them.  Both tracks carry the with-input value
// twice (+0x638 and +0x63C, +0x620 and +0x624) because the advancer reads +0x00 on a rising
// edge and +0x04 when the target and the state have opposite signs; leaving either at 0.95
// would leave half of the sweep unresponsive.
static const DWORD kCmdFields[6] = { 0x620, 0x624, 0x628, 0x638, 0x63C, 0x640 };

// The floor the cave applies after scaling, so a field the shipped data leaves at 0 stays at 0
// instead of being dragged negative by the (1-K) term.
static const float g_cmdZero = 0.0f;

static void ApplyTurretCommandLag() {
    // Installed at K=1 as well when the menu is armed: the cave computes X' = X*1 + 0, which
    // reproduces the shipped inertia bit for bit, and without it the menu slider could only
    // take effect after a restart.
    if (g_cfg.turretCmdResponse == 1.0f && !g_cfg.menuEnabled) return;
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* site = (BYTE*)base + TURRET_CTOR_TAIL_RVA;
    __try {
        if (memcmp(site, g_cmdTailOrig, 6) != 0) {
            Log("CmdLag: ctor tail VA 0x%08X does not hold %02X %02X %02X %02X %02X %02X "
                "(found %02X %02X %02X %02X %02X %02X) - NOTHING written",
                0x400000 + TURRET_CTOR_TAIL_RVA,
                g_cmdTailOrig[0], g_cmdTailOrig[1], g_cmdTailOrig[2], g_cmdTailOrig[3],
                g_cmdTailOrig[4], g_cmdTailOrig[5],
                site[0], site[1], site[2], site[3], site[4], site[5]);
            return;
        }
        BYTE* cave = (BYTE*)VirtualAlloc(nullptr, 512, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
        if (!cave) { Log("CmdLag: cave allocation failed"); return; }
        // X' = X*K + (1-K), floored at 0.0.  Per field:
        //   F3 0F 10 87 <disp32>  movss xmm0,[edi+d]   movss reg000, m32
        //   F3 0F 59 05 <abs32>   mulss xmm0,[K]       mod=00 rm=101 -> disp32, NO SIB
        //   F3 0F 58 05 <abs32>   addss xmm0,[1-K]
        //   F3 0F 5F 05 <abs32>   maxss xmm0,[0.0]     keeps the coefficient in [0,1)
        //   F3 0F 11 87 <disp32>  movss [edi+d],xmm0
        const DWORD kAbs = (DWORD)(uintptr_t)&g_cfg.turretCmdResponse;
        const DWORD bAbs = (DWORD)(uintptr_t)&g_cfg.turretCmdBias;
        const DWORD zAbs = (DWORD)(uintptr_t)&g_cmdZero;
        size_t o = 0;
        for (int i = 0; i < 6; ++i) {
            memcpy(cave + o, "\xF3\x0F\x10\x87", 4); o += 4;
            memcpy(cave + o, &kCmdFields[i], 4); o += 4;
            memcpy(cave + o, "\xF3\x0F\x59\x05", 4); o += 4;
            memcpy(cave + o, &kAbs, 4); o += 4;
            memcpy(cave + o, "\xF3\x0F\x58\x05", 4); o += 4;
            memcpy(cave + o, &bAbs, 4); o += 4;
            memcpy(cave + o, "\xF3\x0F\x5F\x05", 4); o += 4;
            memcpy(cave + o, &zAbs, 4); o += 4;
            memcpy(cave + o, "\xF3\x0F\x11\x87", 4); o += 4;
            memcpy(cave + o, &kCmdFields[i], 4); o += 4;
        }
        memcpy(cave + o, g_cmdTailOrig, 6); o += 6;   // mov eax,edi; pop edi; ret 0x60

        BYTE patch[5];
        patch[0] = 0xE9;
        LONG fwd = (LONG)((DWORD_PTR)cave - (DWORD_PTR)(site + 5));
        memcpy(patch + 1, &fwd, 4);
        if (!PatchBytes(site, patch, sizeof(patch), nullptr)) {
            Log("CmdLag: VirtualProtect failed on the ctor tail");
            return;
        }
        g_cmdLagOn = true;
        // X' = max(0, 0.95*K + (1-K)) for the yaw track, and the same for 0.90 (pitch).  The
        // advancer closes (1-X)*dt*60 of the remaining gap each frame, so at the frame scale the
        // camera actually ran at while being measured (139 fps) the percentages below are the
        // directly observable behaviour: shipped 2.2% of the gap per frame, K=6 -> 12.9%.
        const float frameScale = 60.0f / 139.0f;
        float xNew = 0.95f * g_cfg.turretCmdResponse + g_cfg.turretCmdBias;
        float xPitch = 0.90f * g_cfg.turretCmdResponse + g_cfg.turretCmdBias;
        if (xNew < 0.0f) xNew = 0.0f;
        if (xPitch < 0.0f) xPitch = 0.0f;
        float closeNew = (1.0f - xNew) * frameScale;
        float closeOld = (1.0f - 0.95f) * frameScale;
        // Time constant = seconds for the gap to fall to 1/e, i.e. -1/(fps * ln(1-close)).
        float tauNew = (closeNew > 0.0f && closeNew < 1.0f)
                     ? -1.0f / (139.0f * logf(1.0f - closeNew)) : 0.0f;
        float tauOld = -1.0f / (139.0f * logf(1.0f - closeOld));
        Log("CmdLag: CAM-E ctor tail VA 0x%08X -> cave %p (%u bytes).  Inertia 0.95 -> %.4f "
            "(yaw), 0.90 -> %.4f (pitch), so the aim command closes %.1f%% of the gap per frame "
            "instead of the shipped %.1f%%: time constant %.3f s instead of %.3f s.  Applies to "
            "the NEXT mount - the camera is built when you get in, not while you drive.",
            0x400000 + TURRET_CTOR_TAIL_RVA, cave, (unsigned)o,
            xNew, xPitch, closeNew * 100.0f, closeOld * 100.0f, tauNew, tauOld);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CmdLag: faulted on the ctor tail, left untouched");
    }
}

// Unlike the rate scale this one cannot be undone by changing a float, because the transform
// runs at MOUNT time, not per frame: a camera already built keeps its scaled coefficients.
// Setting K to 1.0 makes the cave an identity for every FUTURE mount and the log says so, and
// the user gets told to dismount and remount (or reload) to get the shipped feel back.
static void KillTurretCommandLag() {
    if (!g_cmdLagOn) return;
    g_cfg.turretCmdResponse = 1.0f;
    g_cfg.turretCmdBias = 0.0f;
    Log("CmdLag: K set to 1.00 (identity).  Cameras mounted from now on keep the shipped "
        "0.95/0.90 inertia - the one you are sitting in keeps the scaled value until you "
        "dismount and remount.");
}

// Read-only: the shipped values behind the two distance levers, so a later write is
// against a measured number rather than an assumed one.
// ---------------- v34: live aim-assist call-site NOP ----------------
// Static analysis identified RVA 0x8F445F as an E8 call inside the per-frame
// camera update (0x8F4380) that invokes the aim-assist pass when flag bit 4
// is set.  NOPping this 5-byte call disables the entire aim-assist pass
// regardless of what data it reads.  Gated by NopCameraCall=1.
static const DWORD AIM_CALL_NOP_RVA = 0x8F445F;
static BYTE g_aimCallOrig[5] = {0};
static bool g_aimCallNopped = false;

static void ApplyAimCallNop() {
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* addr = (BYTE*)base + AIM_CALL_NOP_RVA;
    if (addr[0] != 0xE8) {
        Log("AimCallNop: RVA 0x%06X byte %02X (expected E8), skipped", AIM_CALL_NOP_RVA, addr[0]);
        return;
    }
    DWORD target = (DWORD)(addr + 5) + *(DWORD*)(addr + 1);
    Log("AimCallNop: call target at RVA 0x%06X -> 0x%08X", AIM_CALL_NOP_RVA, target);
    const BYTE nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    if (PatchBytes(addr, nops, 5, g_aimCallOrig)) {
        g_aimCallNopped = true;
        Log("AimCallNop: 5-byte NOP applied at RVA 0x%06X (orig E8 %02X%02X%02X%02X)",
            AIM_CALL_NOP_RVA, g_aimCallOrig[1], g_aimCallOrig[2], g_aimCallOrig[3], g_aimCallOrig[4]);
    } else {
        Log("AimCallNop: VirtualProtect failed at RVA 0x%06X", AIM_CALL_NOP_RVA);
    }
}

static void RevertAimCallNop() {
    if (!g_aimCallNopped) return;
    HMODULE base = GetModuleHandleW(nullptr);
    PatchBytes((BYTE*)base + AIM_CALL_NOP_RVA, g_aimCallOrig, 5, nullptr);
    g_aimCallNopped = false;
}

// ---------------- v34: diagnostic hook at candidate aim function 0x8F3050 ----------------
// Inline hook that counts calls and logs every Nth invocation so we can confirm
// whether this function runs during turret gameplay.  Does NOT alter behavior.
static const DWORD AIM_DIAG_RVA = 0x8F3050;
static volatile LONG g_aimDiagCalls = 0;
static BYTE g_aimDiagOrig[5] = {0};
static bool g_aimDiagHooked = false;
static BYTE* g_aimDiagTrampoline = nullptr;

static void __stdcall AimDiagLog() {
    LONG n = InterlockedIncrement(&g_aimDiagCalls);
    if (n <= 10 || (n % 500) == 0) {
        Log("AimDiag: 0x8F3050 called #%d", n);
    }
}

static void ApplyAimDiagHook() {
    HMODULE base = GetModuleHandleW(nullptr);
    BYTE* target = (BYTE*)base + AIM_DIAG_RVA;

    g_aimDiagTrampoline = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_aimDiagTrampoline) { Log("AimDiag: VirtualAlloc failed"); return; }

    int n = 0;
    // pushad to preserve all registers (this is x86 cdecl/stdcall context)
    g_aimDiagTrampoline[n++] = 0x60; // pushad
    // call AimDiagLog (stdcall, no stack cleanup needed from callee side)
    g_aimDiagTrampoline[n++] = 0xE8;
    *(DWORD*)(g_aimDiagTrampoline + n) = (DWORD)((BYTE*)AimDiagLog - (g_aimDiagTrampoline + n + 4));
    n += 4;
    g_aimDiagTrampoline[n++] = 0x61; // popad
    // execute the original 5 bytes we stole
    memcpy(g_aimDiagTrampoline + n, target, 5);
    n += 5;
    // jmp back to target+5
    g_aimDiagTrampoline[n++] = 0xE9;
    *(DWORD*)(g_aimDiagTrampoline + n) = (DWORD)((target + 5) - (g_aimDiagTrampoline + n + 4));
    n += 4;

    // patch the target: jmp to trampoline
    BYTE jmpRel[5]; jmpRel[0] = 0xE9;
    *(DWORD*)(jmpRel + 1) = (DWORD)(g_aimDiagTrampoline - (target + 5));
    if (PatchBytes(target, jmpRel, 5, g_aimDiagOrig)) {
        g_aimDiagHooked = true;
        Log("AimDiag: inline hook installed at RVA 0x%06X, trampoline %p",
            AIM_DIAG_RVA, g_aimDiagTrampoline);
    } else {
        Log("AimDiag: VirtualProtect failed at RVA 0x%06X", AIM_DIAG_RVA);
    }
}

static void RevertAimDiagHook() {
    if (!g_aimDiagHooked) return;
    HMODULE base = GetModuleHandleW(nullptr);
    PatchBytes((BYTE*)base + AIM_DIAG_RVA, g_aimDiagOrig, 5, nullptr);
    g_aimDiagHooked = false;
    if (g_aimDiagTrampoline) {
        VirtualFree(g_aimDiagTrampoline, 0, MEM_RELEASE);
        g_aimDiagTrampoline = nullptr;
    }
    Log("AimDiag: hook reverted, total calls=%d", (int)g_aimDiagCalls);
}

// ---------------- Shared window / logging state ----------------
static HMODULE g_hSelf = nullptr;          // this .asi module (set in DllMain)
static HWND g_hGameWnd = nullptr;          // real game window, once GameWindowThread finds it
// Timestamp (GetTickCount) until which the extra read-only dumps are active.
static volatile LONG g_captureUntil = 0;
// Menus show the cursor, gameplay hides it.
static bool CursorVisible() {
    CURSORINFO ci = {sizeof(ci)};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}
// WM_ACTIVATE / SETFOCUS / KILLFOCUS transitions, timestamped for the focus log lines.
static volatile LONG g_activateEvents = 0;
// Set by the D3D9 hooks once the device is genuinely windowed; borderless waits for it.
static volatile LONG g_d3dWindowed = 0;

// ---------------- Borderless window ----------------
static WNDPROC g_realGameWndProc = nullptr;
static HMONITOR g_hMonitor = nullptr;
static volatile LONG g_borderlessApplied = 0;

struct EnumCtx { DWORD pid; HWND best; LONG bestArea; };
static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    EnumCtx* c = (EnumCtx*)lp;
    DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_CHILD) return TRUE;
    // Skip dialog boxes (error dialogs use class #32770) and error windows
    wchar_t cls[64] = {0}; wchar_t title[128] = {0};
    GetClassNameW(h, cls, 64); GetWindowTextW(h, title, 128);
    if (wcscmp(cls, L"#32770") == 0) return TRUE;
    if (wcsstr(title, L"Error") || wcsstr(title, L"error")) return TRUE;
    RECT r; if (!GetWindowRect(h, &r)) return TRUE;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}

static HWND FindGameWindow() {
    EnumCtx ctx = { GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(EnumProc, (LPARAM)&ctx);
    return ctx.best;
}

static void ApplyBorderless(HWND h) {
    if (!h) return;
    if (!g_d3dWindowed) {
        // Device is fullscreen-exclusive; styling the host window fights D3D
        // and produces the white-screen artifact. Skip.
        return;
    }
    g_hMonitor = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    RECT mr;
    if (GetMonitorInfoW(g_hMonitor, &mi)) mr = mi.rcMonitor;
    else SystemParametersInfo(SPI_GETWORKAREA, 0, &mr, 0);

    LONG style = GetWindowLongW(h, GWL_STYLE);
    LONG exStyle = GetWindowLongW(h, GWL_EXSTYLE);
    LONG origStyle = style, origEx = exStyle;
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_TABSTOP);
    style |= WS_POPUP | WS_VISIBLE;
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongW(h, GWL_STYLE, style);
    SetWindowLongW(h, GWL_EXSTYLE, exStyle);
    SetWindowPos(h, HWND_TOP,
                 mr.left, mr.top,
                 mr.right - mr.left, mr.bottom - mr.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    Log("Borderless: applied to %p (style %08lX->%08lX, ex %08lX->%08lX), monitor %dx%d @ (%d,%d)",
        h, origStyle, style, origEx, exStyle,
        mr.right - mr.left, mr.bottom - mr.top, mr.left, mr.top);
    InterlockedExchange(&g_borderlessApplied, 1);
}

// ---------------- Game window subclass ----------------
static void ServiceDeferredSeatHook();   // defined with the seat recorder below

// v58: the settings panel lives below this function, but the WndProc needs it: while the panel is
// open its own navigation keys are eaten here so that pressing Left/Right to move a slider does
// not also steer the player.  Both are declared here and defined in the menu module.
static bool MenuOpenNow();
static bool MenuOwnsKey(int vk);
static volatile LONG g_menuKeyEaten = 0;

static LRESULT CALLBACK hkGameWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ServiceDeferredSeatHook();
    // v58: panel keys first, before the switch, so a swallowed key cannot be counted as game
    // input by the probes below either.
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) &&
        MenuOpenNow() && MenuOwnsKey((int)w)) {
        // Safe to drop: the panel reads its own keys with GetAsyncKeyState on a separate thread,
        // so suppressing the queued message cannot starve the trigger.  Only the panel's keys are
        // affected, and only while it is open.
        InterlockedIncrement(&g_menuKeyEaten);
        return 0;
    }
    switch (m) {
    case WM_NCCALCSIZE:
        if (g_cfg.borderless && w == TRUE) return 0;
        break;
    case WM_GETMINMAXINFO:
        if (g_cfg.borderless && g_hMonitor) {
            MINMAXINFO* mmi = (MINMAXINFO*)l;
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfoW(g_hMonitor, &mi)) {
                int ww = mi.rcMonitor.right - mi.rcMonitor.left;
                int hh = mi.rcMonitor.bottom - mi.rcMonitor.top;
                mmi->ptMaxPosition.x = 0; mmi->ptMaxPosition.y = 0;
                mmi->ptMaxSize.x = ww;    mmi->ptMaxSize.y = hh;
                mmi->ptMaxTrackSize.x = ww; mmi->ptMaxTrackSize.y = hh;
            }
            return 0;
        }
        break;
    case WM_DISPLAYCHANGE:
        if (g_cfg.borderless) ApplyBorderless(h);
        break;
    case WM_NCHITTEST:
        if (g_cfg.borderless) return HTCLIENT;
        break;
    case WM_ACTIVATE: {
        // Re-assert borderless on activation in case the game reset styles
        const LONG n = InterlockedIncrement(&g_activateEvents);
        Log("FOCUS #%ld WM_ACTIVATE code=%d fgIsGame=%d hidden=%d",
            n, (int)LOWORD(w),
            (GetForegroundWindow() == h) ? 1 : 0, CursorVisible() ? 0 : 1);
        if (g_cfg.borderless && LOWORD(w) != WA_INACTIVE) {
            Sleep(50);
            ApplyBorderless(h);
        }
        break;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS:
        Log("FOCUS #%ld %s fgIsGame=%d hidden=%d",
            InterlockedIncrement(&g_activateEvents),
            (m == WM_SETFOCUS) ? "SETFOCUS" : "KILLFOCUS",
            (GetForegroundWindow() == h) ? 1 : 0, CursorVisible() ? 0 : 1);
        break;
    }
    return CallWindowProcW(g_realGameWndProc, h, m, w, l);
}

static DWORD WINAPI GameWindowThread(LPVOID) {
    DWORD deadline = GetTickCount() + GAME_PID_WAIT_MS;
    HWND h = nullptr;
    while ((LONG)(deadline - GetTickCount()) > 0) {
        h = FindGameWindow();
        if (h) break;
        Sleep(150);
    }
    if (!h) { Log("GameWindow: no top-level window found for PID %u within %d ms",
                  GetCurrentProcessId(), GAME_PID_WAIT_MS); return 1; }

    g_hGameWnd = h;
    wchar_t cls[256] = {0}; wchar_t title[256] = {0};
    GetClassNameW(h, cls, 256); GetWindowTextW(h, title, 256);
    Log("GameWindow: found %p class='%S' title='%S'", h, cls, title);

    // Subclass
    g_realGameWndProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)hkGameWndProc);
    Log("GameWindow: subclassed WndProc (real=%p)", g_realGameWndProc);

    // Apply borderless after a short settle, then again later (game may resize)
    if (g_cfg.borderless) {
        Sleep(500); ApplyBorderless(h);
        Sleep(1500); ApplyBorderless(h);
        Sleep(3000); ApplyBorderless(h);
    }
    return 0;
}

// ---------------- D3D9 hooks (force windowed for true borderless) ----------------
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 g_realDirect3DCreate9 = nullptr;

typedef HRESULT (WINAPI *PFN_D3DCreateDevice)(void*, UINT, D3DDEVTYPE, HWND, DWORD,
                                              D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static PFN_D3DCreateDevice g_realD3DCreateDevice = nullptr;
static VHook g_vhD3DCreateDevice = {nullptr, 0, nullptr};

typedef HRESULT (WINAPI *PFN_D3DReset)(void*, D3DPRESENT_PARAMETERS*);
static PFN_D3DReset g_realD3DReset = nullptr;
static VHook g_vhD3DReset = {nullptr, 0, nullptr};
static HRESULT WINAPI hkD3DReset(void* self, D3DPRESENT_PARAMETERS* pp);

// Track whether the D3D device is genuinely windowed. Borderless window
// styling must only run in that case; styling a fullscreen-exclusive device's
// host window causes the white-screen-at-start artifact.

static void GetDesktopSize(UINT* w, UINT* h) {
    DEVMODEW dm = {0}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        *w = dm.dmPelsWidth; *h = dm.dmPelsHeight;
    } else {
        *w = (UINT)GetSystemMetrics(SM_CXSCREEN);
        *h = (UINT)GetSystemMetrics(SM_CYSCREEN);
    }
}

// Ladder of windowed configs. Index 0 = minimal change; later indices add more
// windowed-safety. We try each until CreateDevice/Reset succeeds.
enum WindowedStrategy { WS_MINIMAL = 0, WS_SAFE_DISCARD, WS_SAFE_FLIP, WS_COUNT };
static const char* kStrategyName[WS_COUNT] = { "MINIMAL", "SAFE_DISCARD", "SAFE_FLIP" };

static void ApplyWindowedStrategy(D3DPRESENT_PARAMETERS* pp, int strat) {
    UINT dw, dh; GetDesktopSize(&dw, &dh);
    pp->Windowed = TRUE;
    // D3D9 requires FullScreen_RefreshRateInHz == 0 when Windowed == TRUE.
    // Leaving the game's fullscreen value here is what caused 0x8876086C.
    pp->FullScreen_RefreshRateInHz = 0;
    switch (strat) {
    case WS_MINIMAL:
        // Only flip the flag; keep the game's backbuffer/format/multisample/swap
        break;
    case WS_SAFE_DISCARD:
        pp->BackBufferWidth = dw;
        pp->BackBufferHeight = dh;
        pp->BackBufferFormat = D3DFMT_UNKNOWN;
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
        break;
    case WS_SAFE_FLIP:
        pp->BackBufferWidth = dw;
        pp->BackBufferHeight = dh;
        pp->BackBufferFormat = D3DFMT_UNKNOWN;
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        pp->SwapEffect = D3DSWAPEFFECT_FLIP;
        pp->BackBufferCount = 1;
        break;
    }
}

static void FixPresentParams(D3DPRESENT_PARAMETERS* pp, const char* who) {
    if (!pp) return;
    BOOL wasWindowed = pp->Windowed;
    UINT wasInterval = pp->PresentationInterval;
    UINT wasW = pp->BackBufferWidth, wasH = pp->BackBufferHeight;
    if (g_cfg.forceWindowedD3D) ApplyWindowedStrategy(pp, WS_MINIMAL);
    if (g_cfg.vsyncOff) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    Log("D3D9 %s: Windowed %d->%d, Interval 0x%X->0x%X, BackBuffer %ux%u->%ux%u, hFocus=%p",
        who, wasWindowed, pp->Windowed, wasInterval, pp->PresentationInterval,
        wasW, wasH, pp->BackBufferWidth, pp->BackBufferHeight, pp->hDeviceWindow);
}

static HRESULT WINAPI hkD3DCreateDevice(void* self, UINT adapter, D3DDEVTYPE type,
                                        HWND hFocus, DWORD flags,
                                        D3DPRESENT_PARAMETERS* pp,
                                        IDirect3DDevice9** outDev) {
    D3DPRESENT_PARAMETERS orig = *pp;
    HRESULT hr = D3DERR_INVALIDCALL;
    int used = -1;

    if (g_cfg.forceWindowedD3D) {
        for (int s = 0; s < WS_COUNT; ++s) {
            *pp = orig;
            ApplyWindowedStrategy(pp, s);
            if (g_cfg.vsyncOff) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            hr = g_realD3DCreateDevice(self, adapter, type, hFocus, flags, pp, outDev);
            Log("D3D9 CreateDevice strategy %s -> 0x%08X", kStrategyName[s], (unsigned)hr);
            if (SUCCEEDED(hr)) { used = s; break; }
        }
    }
    if (FAILED(hr)) {
        *pp = orig;
        hr = g_realD3DCreateDevice(self, adapter, type, hFocus, flags, pp, outDev);
        Log("D3D9 CreateDevice fullscreen fallback -> 0x%08X", (unsigned)hr);
    }

    if (SUCCEEDED(hr)) {
        InterlockedExchange(&g_d3dWindowed, (used >= 0) ? 1 : 0);
        Log("D3D9 CreateDevice OK (windowed=%d strategy=%s) BackBuffer %ux%u fmt=%d swap=%d multi=%d",
            (int)g_d3dWindowed, used >= 0 ? kStrategyName[used] : "FULLSCREEN",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
            pp->SwapEffect, pp->MultiSampleType);
    }

    if (SUCCEEDED(hr) && outDev && *outDev) {
        void** vtable = *(void***)(*outDev);
        if (!g_vhD3DReset.vtable) {
            // IDirect3DDevice9::Reset is vtable index 16
            if (HookVtable(vtable, 16, (void*)&hkD3DReset, &g_vhD3DReset)) {
                g_realD3DReset = (PFN_D3DReset)g_vhD3DReset.original;
                Log("D3D9: hooked IDirect3DDevice9::Reset (vtable %p, orig %p)",
                    vtable, g_realD3DReset);
            } else {
                Log("D3D9: Reset vtable hook FAILED");
            }
        }
    }
    return hr;
}

static HRESULT WINAPI hkD3DReset(void* self, D3DPRESENT_PARAMETERS* pp) {
    D3DPRESENT_PARAMETERS orig = *pp;
    HRESULT hr = D3DERR_INVALIDCALL;
    int used = -1;

    if (g_cfg.forceWindowedD3D) {
        for (int s = 0; s < WS_COUNT; ++s) {
            *pp = orig;
            ApplyWindowedStrategy(pp, s);
            if (g_cfg.vsyncOff) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            hr = g_realD3DReset(self, pp);
            if (SUCCEEDED(hr)) { used = s; break; }
        }
    }
    if (FAILED(hr)) {
        *pp = orig;
        hr = g_realD3DReset(self, pp);
    }
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&g_d3dWindowed, (used >= 0) ? 1 : 0);
        Log("D3D9 Reset OK (windowed=%d strategy=%s)", (int)g_d3dWindowed,
            used >= 0 ? kStrategyName[used] : "FULLSCREEN");
    }
    // Re-assert borderless only if we are genuinely windowed
    if (g_cfg.borderless && g_d3dWindowed && g_hGameWnd) {
        Sleep(100);
        ApplyBorderless(g_hGameWnd);
    }
    return hr;
}

static IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdkVersion) {
    IDirect3D9* d3d = g_realDirect3DCreate9(sdkVersion);
    if (d3d) {
        void** vtable = *(void***)d3d;
        if (!g_vhD3DCreateDevice.vtable) {
            // IDirect3D9::CreateDevice is vtable index 16
            if (HookVtable(vtable, 16, (void*)&hkD3DCreateDevice, &g_vhD3DCreateDevice)) {
                g_realD3DCreateDevice = (PFN_D3DCreateDevice)g_vhD3DCreateDevice.original;
                Log("D3D9: hooked IDirect3D9::CreateDevice (vtable %p, orig %p)",
                    vtable, g_realD3DCreateDevice);
            } else {
                Log("D3D9: CreateDevice vtable hook FAILED");
            }
        }
    }
    return d3d;
}

// ---------------- Settings-struct dump (aim assist offset discovery) ----------------
// The settings loaders commit into static structs. Dump them as floats so we
// can identify which offsets hold the AimAssist values, then zero them at
// runtime with a watchdog (the loaders run before the ASI attaches).
static void DumpRegionFloats(const char* tag, DWORD va, int count) {
    const float* f = (const float*)va;
    for (int i = 0; i < count; i += 8) {
        Log("AIMDUMP %s +%02X: %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g",
            tag, i * 4, f[i], f[i+1], f[i+2], f[i+3], f[i+4], f[i+5], f[i+6], f[i+7]);
    }
}

static void DumpRegionDwords(const char* tag, DWORD va, int count) {
    const DWORD* d = (const DWORD*)va;
    for (int i = 0; i < count; i += 8) {
        Log("AIMDUMP %s +%02X: %08X %08X %08X %08X %08X %08X %08X %08X",
            tag, i * 4, d[i], d[i+1], d[i+2], d[i+3], d[i+4], d[i+5], d[i+6], d[i+7]);
    }
}

static void DumpPtrRegion(const char* tag, DWORD va, int off, int count) {
    DWORD p = *(DWORD*)va;
    if (p < 0x10000 || p > 0x7F000000) { Log("AIMDUMP %s: bad ptr %08X", tag, p); return; }
    char buf[64];
    _snprintf(buf, sizeof(buf), "%s[%08X]+%02X", tag, p, off);
    DumpRegionDwords(buf, p + off, count);
}

static void DumpPtrDirect(const char* tag, DWORD p, int count) {
    if (p < 0x10000 || p > 0x7F000000) { Log("AIMDUMP %s: bad ptr %08X", tag, p); return; }
    char buf[64];
    _snprintf(buf, sizeof(buf), "%s[%08X]", tag, p);
    DumpRegionDwords(buf, p, count);
    DumpRegionFloats(buf, p, count);
}

// ---------------- Camera-behavior patcher ----------------
// Applies the verified-field edits from the config to one 56-byte block. Used by
// both the live-map watchdog and the insert hook; the insert hook is the one that
// actually wins, because instances latch their settings the moment the entry is
// created (which happens during a level load, after we attach).
static void PatchCameraBlock(BYTE* e) {
    if (g_cfg.aimZeroFields) {
        *(float*)(e + 0x00) = 0.0f;   // HorizontalAimAssistClose
        *(float*)(e + 0x04) = 0.0f;   // HorizontalAimAssistFar
        *(float*)(e + 0x08) = 0.0f;   // VerticalAimAssistClose
        *(float*)(e + 0x0C) = 0.0f;   // VerticalAimAssistFar
    }
    if (g_cfg.aimZeroMode) {
        *(DWORD*)(e + 0x24) = 0;      // AimAssist mode
    }
    if (g_cfg.aimMinYawPitch > 0.0f) {
        static const DWORD spd[] = { 0x10, 0x14, 0x18, 0x1C };
        for (int k = 0; k < 4; ++k) {
            float* f = (float*)(e + spd[k]);
            if (*f < g_cfg.aimMinYawPitch) *f = g_cfg.aimMinYawPitch;
        }
    }
}

static volatile LONG g_aimWatchEntries = -1;

static void DumpAimStructs();   // defined below with the other dump helpers
static void DumpSettingsTable(); // v37: settings store at *[0xEDC6DC]
static void DumpImportThunk();   // v38: runtime-resolve the protected seat-copy import

static void PatchCameraMap() {
    LONG n = 0;
    __try {
        const DWORD* m = (const DWORD*)0x17BCEC0;
        DWORD stride  = m[3] & 0xFFFF;
        DWORD shift   = (m[3] >> 16) & 0xFFFF;
        DWORD buckets = m[4];
        DWORD values  = m[7];
        DWORD offs    = m[8];
        if (stride != 56 || shift >= 32 || buckets == 0 || buckets > 4096) return;
        if (values < 0x10000 || values > 0x7F000000) return;
        if (offs   < 0x10000 || offs   > 0x7F000000) return;
        const DWORD* v = (const DWORD*)values;
        const DWORD* o = (const DWORD*)offs;
        for (DWORD i = 0; i < buckets; ++i) {
            if (v[i] == 0 || v[i] == 0xFFFFFFFF) continue;
            BYTE* e = (BYTE*)(o[i >> shift] + ((buckets - 1) & i) * stride);
            if ((DWORD)e < 0x10000 || (DWORD)e > 0x7F000000) continue;
            PatchCameraBlock(e);
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (n != g_aimWatchEntries) {
        Log("AimWatch: patched %ld camera entries (zeroFields=%d zeroMode=%d minYawPitch=%.3g)",
            n, g_cfg.aimZeroFields, g_cfg.aimZeroMode, g_cfg.aimMinYawPitch);
        InterlockedExchange(&g_aimWatchEntries, n);
    }
    if ((LONG)GetTickCount() < g_captureUntil) {
        static LONG lastBurst = 0;
        LONG t = (LONG)GetTickCount();
        if (t - lastBurst >= 1500) {   // keep the capture window's dumps spaced out
            lastBurst = t;
            Log("CAPTURE entries=%ld", n);
            DumpAimStructs();
        }
    }
}

static DWORD WINAPI AimWatchThread(LPVOID) {
    for (;;) {
        PatchCameraMap();
        Sleep(200);
    }
    return 0;
}

// ---------------- map-insert hook ----------------
// Inline-hook on the hash-map insert (0x64A600). It sanitizes the 56-byte source
// block BEFORE the game memcpy's it and before the constructing behavior instance
// latches its settings, then optionally logs it. This is strictly earlier than
// the watchdog, which matters because entries are created during a level load
// (map count goes 0 -> 44 after we attach) and a 200ms poll loses that race.
//
// v12 broke look/fire through this hook, but only because the field offsets were
// an off-by-four guess then: it cleared +04..+10, i.e. HorizontalAimAssistFar,
// VerticalAimAssistClose, VerticalAimAssistFar AND YawSpeed. The verified layout
// (see Config) puts the four aim floats at +00..+0C, so rotation is untouched.
//
// 0x64A600 contract (disasm, non-ASLR base 0x400000):
//   custom thiscall: edi = map descriptor (`this`)
//   [esp+4]=arg1 (key)  [esp+8]=arg2 (56-byte data block)  `ret 8`
//   copies arg2 (stride bytes) into the entry via memcpy @0x9ee832.
// Only inserts into the map at 0x17BCEC0 are observed.
static volatile LONG g_insertSanitized = 0;
static volatile LONG g_insertSeen = 0;
static volatile LONG g_insertLogged = 0;
static BYTE*  g_mapInsertAddr   = (BYTE*)0x0064A600;
static void*  g_mapInsertResume = (void*)0x0064A60B;
static BYTE   g_mapInsertOrig[16];
static bool   g_mapInsertHooked = false;

extern "C" void MapInsertHandler(DWORD descriptor, DWORD key, DWORD block) {
    LONG n = InterlockedIncrement(&g_insertSeen);
    if (block < 0x10000 || block > 0x7F000000) return;
    const DWORD* b = (const DWORD*)block;
    __try {
        // Log BEFORE patching: the source block is a reused stack buffer, so a
        // post-patch log would only show our own zeros.
        LONG burst = (LONG)GetTickCount() < g_captureUntil;
        if ((g_cfg.insertLogging && n <= 80) || burst) {
            const float* f = (const float*)block;
            Log("INSERT #%ld key=%08X blk=%08X  f00=%.6g f01=%.6g f02=%.6g f03=%.6g "
                "f04=%.6g f05=%.6g f06=%.6g f07=%.6g f08=%.6g i24=%d",
                n, key, block,
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], (int)b[9]);
        }
        if (g_cfg.aimPatchAtInsert) {
            PatchCameraBlock((BYTE*)block);
            InterlockedIncrement(&g_insertSanitized);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
}

// ASM operand: MSVC resolves this via the C++ name `MapInsertHandler` and emits
// the cdecl `_MapInsertHandler` symbol at link time.
#define _MapInsertHandler MapInsertHandler

__declspec(naked) static void hkMapInsert() {
    __asm {
        cmp edi, 0x0017BCEC0          // only the camera-behavior map
        jne doPrologue
        pushad
        mov eax, [esp+32+8]           // arg2 = 56-byte source block
        mov edx, [esp+32+4]           // arg1 = map key
        push eax                      // block
        push edx                      // key
        push edi                      // descriptor
        call _MapInsertHandler
        add esp, 12
        popad
    doPrologue:
        // stolen original prologue: 55 8B EC 83 EC 08 53 56 8B 75 08 (11 bytes)
        push ebp
        mov ebp, esp
        sub esp, 8
        push ebx
        push esi
        mov esi, [ebp+8]
        jmp dword ptr [g_mapInsertResume]
    }
}

static void InstallMapInsertHook() {
    static const BYTE expect[11] = {0x55,0x8B,0xEC,0x83,0xEC,0x08,0x53,0x56,0x8B,0x75,0x08};
    if (memcmp(g_mapInsertAddr, expect, 11) != 0) {
        Log("MapInsert: unexpected bytes at %p, hook skipped", g_mapInsertAddr);
        return;
    }
    memcpy(g_mapInsertOrig, g_mapInsertAddr, 11);
    BYTE patch[11];
    patch[0] = 0xE9;
    *(DWORD*)(patch + 1) = (DWORD)(&hkMapInsert) - ((DWORD)g_mapInsertAddr + 5);
    for (int i = 5; i < 11; ++i) patch[i] = 0x90;
    if (PatchBytes(g_mapInsertAddr, patch, 11, nullptr)) {
        g_mapInsertHooked = true;
        Log("MapInsert: hook installed at 0x64A600 -> %p (resume 0x64A60B)", &hkMapInsert);
    } else {
        Log("MapInsert: VirtualProtect failed");
    }
}

static void RevertMapInsertHook() {
    if (g_mapInsertHooked) {
        PatchBytes(g_mapInsertAddr, g_mapInsertOrig, 11, nullptr);
        g_mapInsertHooked = false;
    }
}

// ---------------- Seat/turret record recorder (0x649180) ----------------
// v20 settled that the HumanCameraBehavior hash map at 0x17BCEC0 is NOT the
// source of either remaining symptom: all 1012 sampled entries carried our
// zeros (HorizontalAimAssistClose/Far, Vertical*, at +00..+0C) and the floored
// YawSpeed/PitchSpeed=1.5, and aim assist plus mounted dampening were
// untouched.  The per-seat data lives in static containers that are filled
// through 0x649180, which no camera-map hook ever sees.
//
// 0x649180 itself is `ret 0x14`, i.e. cdecl with 5 args, and every loader in
// the game calls it with the identical 5-push sequence (verified at 0x65E5F0,
// 0x65E84E and 0x65E9F8):
//
//     lea  ecx,[esp+0x10]        arg5 = &block      <- the record data
//     mov  edx,[esp+0x10]        arg4 = block[0]    <- the key
//     push ecx                   arg3 = 0
//     push edx
//     push 0                     arg2 = loader's own [ebp+8]
//     push eax
//     push CONTAINER             arg1
//     call 0x649180
//
// Inside, ebx=arg1 is provably the container: it reads the record stride as
// `movsx eax, word ptr [ebx+0x24]` and bumps `dword ptr [ebx+0x18]`/`[ebx+0x30]`
// as counters.  arg5 is what gets copied into storage, so patching arg5 *before*
// the call is the one place a write lands without racing the game's own cache -
// the same trick the 0x64A600 hook uses for the camera map.
//
// The three seat-related containers, with their loaders and the setting names
// in the order the loader requests them (from the getter's edx name pointer):
//
//   0xDF7B88  loader 0x65E400 (first commit @0x65E5F0) - vehicle/heli camera:
//     SeatName(int) FocusOffset.xyz WorldFocusOffset.xyz CamDistToHeli YawLag
//     PitchSpeed RollAmp RollLag PitchAmp PitchLag MinPitch MaxPitch
//     PitchLimitCushion DefaultPitch PitchInputInertia PitchNoInputInertia
//   0xDF7C08  loader 0x65E400 (second commit @0x65E84E) - turret/seat control:
//     SeatName(int) YawSpeed PitchSpeed CameraOffset.xyz FocusOffset.xyz
//     TurretName(int) YawMax YawMin YawInputInertia YawNoInputInertia
//     PitchMax PitchMin PitchInputInertia PitchNoInputInertia
//     VerticalSandPaper CloseDist FarRange CloseAimAssist FarAimAssist
//     CameraBlendTime
//   0xDF7C88  loader 0x65E860 (commit @0x65E9F8) - human seat camera, stride
//     0x40=64B=16 dwords, and exactly 13 float/int fields + one vec3 = 16
//     dwords, which is the tiling proof that the record holds these fields and
//     nothing else:
//     SeatName(int) YawSpeed PitchSpeed CameraOffset MinPitch MaxPitch
//     DefaultPitch FocusOffset.xyz InputInertiaDefault
//     InputInertiaDifferentSign InputInertiaNoInput
//     CameraOffsetScaleAtMinPitch CameraOffsetScaleAtMaxPitch
//     FocusOffsetYAtMaxPitch
//
// CloseAimAssist/FarAimAssist (0xDF7C08) are the aim-assist strengths and the
// *InputInertia*/*Lag* fields are the dampening coefficients, so this is where
// the two open symptoms have to be.  What is NOT known is which dword slot each
// name occupies: the loaders scatter their stores over the frame (the compiler
// does not fill in order) and a static esp model keeps losing track at the
// vec3 getter and the callee-cleaned archive `call eax` (ret 0xC).  Per the
// standing rule for this project - never write against an inferred offset -
// this build is READ-ONLY: it dumps every seat record as raw dwords in both
// hex and float.  The name lists above plus the value magnitudes (pitch/yaw
// limits in degrees, offsets in metres, strengths in 0..1) identify the slots;
// the write pass ships only once that mapping is confirmed from the dump.
//
// v22 DUMP RESULT (24 records per container, Mercs2Fix.log 21:12): 0xDF7C08 has
// stride 96 = 24 dwords = exactly the 24 names above, so the record tiles with
// no slack.  Slot +58 is the only non-float word besides the +00 key and holds
// an FNV-style hash, which pins TurretName there; the remaining names then fill
// +04..+54 in getter order.  Under that mapping the pair at +50/+54 reads
// 0.975/0.025, 0.5/0.3, 0.7/-0.1 across records - a strong value with a near-
// zero partner per seat, which is what CloseAimAssist/FarAimAssist must look
// like.  Still unconfirmed: whether 0.0 occurs naturally in those two slots,
// which is the precondition for writing anything there.
static volatile LONG g_seatSeen = 0;
static volatile LONG g_seatDumped = 0;
static volatile LONG g_seatDup = 0;
static BYTE*  g_seatCommitAddr   = (BYTE*)0x00649180;
static void*  g_seatCommitResume = (void*)0x0064918B;
static BYTE   g_seatCommitOrig[11];
static bool   g_seatCommitHooked = false;
static bool   g_seatCommitAuto = false;   // installed by the capture window

// Per-container dump state: v21 used one global cap of 60 and spent all of it on
// 0xDF7B88 (the vehicle presets commit first), never reaching the turret and
// human-seat records that carry the aim-assist strengths.
static DWORD g_seatSeenPerCont[3] = { 0, 0, 0 };
static DWORD g_seatLastHash[3]    = { 0, 0, 0 };
static LONG  g_seatLastStride[3]  = { 0, 0, 0 };

static void ResetSeatDump() {
    for (int i = 0; i < 3; ++i) { g_seatSeenPerCont[i] = 0; g_seatLastHash[i] = 0; }
}

static void InstallSeatCommitHook();
static void RevertSeatCommitHook();

// v30: install the seat commit hook at load time when seatLogging=1, because
// the "re-commits ~30/s" assumption from v22 was wrong - the hook never fired
// during capture windows in v29/v30. Records may only commit at seat entry or
// level load, so we need the hook always armed to catch them.
static void ServiceDeferredSeatHook() {
    if (!g_cfg.seatLogging) return;
    if (!g_seatCommitHooked && !g_cfg.seatDefer) {
        InstallSeatCommitHook();
        ResetSeatDump();
    }
    // Also arm during capture windows even if defer is on
    const bool inBurst = (LONG)GetTickCount() < (LONG)g_captureUntil;
    if (inBurst && !g_seatCommitHooked) {
        InstallSeatCommitHook();
        g_seatCommitAuto = g_seatCommitHooked;
        ResetSeatDump();
    } else if (!inBurst && g_seatCommitAuto && g_cfg.seatDefer) {
        RevertSeatCommitHook();
        Log("SeatCommit: capture window closed, detour removed again");
    }
}

static bool IsSeatContainer(DWORD cont) {
    return cont == 0x00DF7B88 || cont == 0x00DF7C08 || cont == 0x00DF7C88;
}

extern "C" void SeatCommitHandler(DWORD cont, DWORD key, DWORD block) {
    LONG n = InterlockedIncrement(&g_seatSeen);
    if (!IsSeatContainer(cont)) return;
    __try {
        const WORD stride = *(const WORD*)(cont + 0x24);
        if (stride < 4 || stride > 0x400) {
            Log("SEAT: cont=%08X implausible stride=%u, not dumping", cont, stride);
            return;
        }
        if (block < 0x10000 || block > 0x7FFFFFF0) {
            Log("SEAT: cont=%08X block=%08X out of range", cont, block);
            return;
        }
        const bool burst = (LONG)GetTickCount() < (LONG)g_captureUntil;
        if (!(g_cfg.seatLogging || burst)) return;
        int slot = cont == 0x00DF7B88 ? 0 : (cont == 0x00DF7C08 ? 1 : 2);
        DWORD* seen = &g_seatSeenPerCont[slot];
        if ((LONG)stride != g_seatLastStride[slot]) {
            g_seatLastStride[slot] = (LONG)stride;
            g_seatLastHash[slot] = 0;
        }
        DWORD h = 2166136261u;
        const BYTE* pb = (const BYTE*)block;
        for (DWORD i = 0; i < stride; ++i) { h ^= pb[i]; h *= 16777619u; }
        if (h == g_seatLastHash[slot] && *seen) {
            InterlockedIncrement(&g_seatDup);
            return;                       // same record as the one just dumped
        }
        g_seatLastHash[slot] = h;
        if ((int)++(*seen) > g_cfg.seatMaxRecords && !burst) return;
        InterlockedIncrement(&g_seatDumped);
        // The key is block[0], which the loaders fill from an int/enum getter,
        // so it is either a small id or a pointer into a string table.
        Log("SEAT cont=%08X stride=%u key=%08X block=%08X count=%u  (#%u of this container, %ld commits seen overall)",
            cont, stride, key, block, *(const DWORD*)(cont + 0x18),
            *seen, n);
        const DWORD* d = (const DWORD*)block;
        for (DWORD i = 0; i + 4 <= stride; i += 4) {
            float f = *(const float*)(d + i / 4);
            Log("     + %02X  %08X  %-14.7g  %s",
                i, d[i / 4],
                (f > -1e12f && f < 1e12f && f == f) ? f : 0.0f,
                (f > -1e12f && f < 1e12f && f == f) ? "" : "(not a float)");
        }
        // v31: write 0.0 to +54 (FarAimAssist) on turret/seat records.
        // Confirmed safe: 0.0 occurs 12x at +54 in shipped data.
        if (cont == 0x00DF7C08 && stride >= 0x58 && g_cfg.seatZeroFarAim) {
            float oldVal = *(float*)(block + 0x54);
            *(float*)(block + 0x54) = 0.0f;
            Log("SEAT-WRITE: cont=%08X +54 FarAimAssist %.4f -> 0.0 (key=%08X)",
                cont, oldVal, key);
        }
        // v32: floor for +50 (CloseAimAssist). 0.0 never occurs at +50 (min=0.2),
        // so we use a configurable floor instead. Only raises, never lowers below
        // the shipped minimum unless the user explicitly sets it.
        if (cont == 0x00DF7C08 && stride >= 0x54 && g_cfg.seatCloseAimMin > 0.0f) {
            float oldVal = *(float*)(block + 0x50);
            if (oldVal > g_cfg.seatCloseAimMin) {
                *(float*)(block + 0x50) = g_cfg.seatCloseAimMin;
                Log("SEAT-WRITE: cont=%08X +50 CloseAimAssist %.4f -> %.4f (key=%08X)",
                    cont, oldVal, g_cfg.seatCloseAimMin, key);
            }
        }
        // v32: ceiling for +34 (YawNoInputInertia). Shipped values are mostly 40,
        // with 1.0 occurring 23x. Lowering reduces the "heavy" feel on turrets.
        if (cont == 0x00DF7C08 && stride >= 0x38 && g_cfg.seatYawInertiaMax > 0.0f) {
            float oldVal = *(float*)(block + 0x34);
            if (oldVal > g_cfg.seatYawInertiaMax) {
                *(float*)(block + 0x34) = g_cfg.seatYawInertiaMax;
                Log("SEAT-WRITE: cont=%08X +34 YawNoInputInertia %.4f -> %.4f (key=%08X)",
                    cont, oldVal, g_cfg.seatYawInertiaMax, key);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("SEAT: handler faulted for cont=%08X", cont);
    }
}

#define _SeatCommitHandler SeatCommitHandler

__declspec(naked) static void hkSeatCommit() {
    __asm {
        mov eax, [esp+4]               // arg1 = container
        cmp eax, 0x00DF7B88
        je  doHandler
        cmp eax, 0x00DF7C08
        je  doHandler
        cmp eax, 0x00DF7C88
        jne doPrologue
    doHandler:
        pushad
        mov eax, [esp+32+0x14]         // arg5 = &block
        mov edx, [esp+32+0x10]         // arg4 = key
        push eax                       // arg3 of our C handler
        push edx                       // arg2
        mov ecx, [esp+44]              // arg1 = container (esp now back to entry+8)
        push ecx                       // arg1
        call _SeatCommitHandler
        add esp, 12
        popad
    doPrologue:
        // stolen original prologue, 11 bytes:
        // 55 | 8B EC | 83 EC 28 | 53 | 8B 5D 08 | 56
        push ebp
        mov ebp, esp
        sub esp, 0x28
        push ebx
        mov ebx, [ebp+8]
        push esi
        jmp dword ptr [g_seatCommitResume]
    }
}

static void InstallSeatCommitHook() {
    static const BYTE expect[11] = { 0x55,0x8B,0xEC,0x83,0xEC,0x28,0x53,0x8B,0x5D,0x08,0x56 };
    if (memcmp(g_seatCommitAddr, expect, 11) != 0) {
        Log("SeatCommit: unexpected bytes at 0x649180, hook skipped");
        return;
    }
    memcpy(g_seatCommitOrig, g_seatCommitAddr, 11);
    BYTE patch[11];
    patch[0] = 0xE9;
    *(DWORD*)(patch + 1) = (DWORD)(&hkSeatCommit) - ((DWORD)g_seatCommitAddr + 5);
    for (int i = 5; i < 11; ++i) patch[i] = 0x90;
    if (PatchBytes(g_seatCommitAddr, patch, 11, nullptr)) {
        g_seatCommitHooked = true;
        Log("SeatCommit: read-only hook at 0x649180 -> %p (resume 0x64918B)", &hkSeatCommit);
    } else {
        Log("SeatCommit: VirtualProtect failed");
    }
}

static void RevertSeatCommitHook() {
    if (g_seatCommitHooked) {
        PatchBytes(g_seatCommitAddr, g_seatCommitOrig, 11, nullptr);
        g_seatCommitHooked = false;
    }
}


// ---------------- v37: settings table dump ----------------
// GetSettingFloat (VA 0x656320) walks a global table of 16-byte entries:
//   count  at *[0xEDC6E0]
//   array  at *[0xEDC6DC]
// It compares each entry's +04 dword and returns the entry's float, falling
// back to the caller's default.  This is the game's own settings store, which
// none of the four structures we already eliminated (seat containers, loader
// defaults, GetSettingFloat call sites, the camera-behavior map) belong to.
// Dump it with names so the live aim-assist / inertia settings can be located.
static bool LooksLikeName(const char* s) {
    int n = 0;
    while (n < 48) {
        unsigned char c = (unsigned char)s[n];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
        ++n;
    }
    return n >= 3;
}

static void DumpSettingsTable() {
    __try {
        const DWORD* g = (const DWORD*)0xEDC6C0;
        Log("SETT globals 0xEDC6C0: %08X %08X %08X %08X", g[0], g[1], g[2], g[3]);
        int count = *(const int*)0xEDC6E0;
        DWORD array = *(const DWORD*)0xEDC6DC;
        Log("SETT table: array=%08X count=%d", array, count);
        if (count <= 0 || count > 20000) { Log("SETT: implausible count, skipped"); return; }
        if (array < 0x10000 || array > 0x7FFFFFF0) { Log("SETT: implausible array, skipped"); return; }
        const DWORD* e = (const DWORD*)array;
        int named = 0;
        for (int i = 0; i < count; ++i) {
            const DWORD* ent = e + i * 4;
            const char* nm = 0;
            for (int k = 0; k < 4; ++k) {
                DWORD p = ent[k];
                if (p > 0x400000 && p < 0x2000000) {
                    const char* c = (const char*)p;
                    if (LooksLikeName(c)) { nm = c; break; }
                }
            }
            if (!nm) continue;
            ++named;
            if (named > 900) continue;
            float f1 = *(const float*)(ent + 1);
            float f2 = *(const float*)(ent + 2);
            float f3 = *(const float*)(ent + 3);
            Log("SETT[%4d] @%08X d0=%08X d1=%08X(%.5g) d2=%08X(%.5g) d3=%08X(%.5g) \"%s\"",
                i, (DWORD)ent, ent[0], ent[1], f1, ent[2], f2, ent[3], f3, nm);
        }
        Log("SETT: %d/%d entries had a readable name", named, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("SETT: access violation during table dump");
    }
}

// ---------------- v38: resolve the protected import thunk at runtime ----------------
// vtable[21] for every seat container (0x649040) forwards to VA 0x9EE832, which is
// `FF 25 [0x00B053B4]`. Statically that slot holds a protected value, but by the time
// a level is loaded the game has resolved it, so the real destination is readable
// memory in our own process. This reads it and dumps the code it points at: that is
// the function that receives the seat record and decides where the values are stored.
static void DumpImportThunk() {
    __try {
        const BYTE* thunk = (const BYTE*)0x009EE832;
        Log("THUNK 0x009EE832 bytes: %02X %02X %02X %02X %02X %02X",
            thunk[0], thunk[1], thunk[2], thunk[3], thunk[4], thunk[5]);
        // Slots at 0xB053B4, +4, +8 are the FF25 targets for the three consecutive thunks.
        static const DWORD slots[] = { 0x00B053B4, 0x00B053B8, 0x00B053BC };
        static const DWORD thunkVas[] = { 0x009EE832, 0x009EE838, 0x009EE83E };
        for (int i = 0; i < 3; ++i) {
            const DWORD* s = (const DWORD*)slots[i];
            DWORD tgt = *s;
            Log("THUNK[%d] 0x%08X -> [%08X] = 0x%08X", i, thunkVas[i], slots[i], tgt);
            if (tgt < 0x00400000 || tgt > 0x7FFFFFF0) { Log("  implausible, skipped"); continue; }
            const BYTE* p = (const BYTE*)tgt;
            char hex[128]; hex[0] = 0;
            for (int k = 0; k < 24; ++k) {
                char tmp[8]; wsprintfA(tmp, "%02X ", p[k]); lstrcatA(hex, tmp);
            }
            Log("  target bytes @%08X: %s", tgt, hex);
            // rep movsd = F3 A5, rep movsb = F3 A4, push ebp = 55
            int hasRepMovs = 0;
            for (int k = 0; k < 39; ++k)
                if (p[k] == 0xF3 && (p[k + 1] == 0xA5 || p[k + 1] == 0xA4)) hasRepMovs = k + 1;
            Log("  marker: prologue=%02X rep-movs@%d", p[0], hasRepMovs);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("THUNK: access violation during thunk resolution");
    }
}

static void DumpAimStructs() {    // Map descriptor used by the HumanCameraBehavior commit (0x64A600):
    //   +0C=stride(word) +0E=shift(word) +10=bucketCount +18=count
    //   +1C=values[] +20=bucketOffsets[]
    DumpRegionDwords("map17BCEC0", 0x17BCEC0, 16);
    const DWORD* m = (const DWORD*)0x17BCEC0;
    DWORD stride  = m[3] & 0xFFFF;
    DWORD shift   = (m[3] >> 16) & 0xFFFF;
    DWORD buckets = m[4];
    DWORD values  = m[7];
    DWORD offs    = m[8];
    Log("AIMDUMP map: stride=%u shift=%u buckets=%u count=%u values=%08X offs=%08X",
        stride, shift, buckets, m[6], values, offs);
    if (values > 0x10000 && values < 0x7F000000 && buckets > 0 && buckets < 4096 &&
        stride > 0 && stride < 4096 && shift < 32) {
        const DWORD* v = (const DWORD*)values;
        const DWORD* o = (offs > 0x10000 && offs < 0x7F000000) ? (const DWORD*)offs : nullptr;
        for (DWORD i = 0; i < buckets && i < 256; ++i) {
            if (v[i] == 0 || v[i] == 0xFFFFFFFF) continue;
            Log("AIMDUMP slot %u val=%08X", i, v[i]);
            if (o) {
                DWORD eoff = ((buckets - 1) & i) * stride + o[i >> shift];
                if (eoff > 0x10000 && eoff < 0x7F000000) {
                    char tag[40]; _snprintf(tag, sizeof(tag), "ent%u@%08X", i, eoff);
                    DumpRegionFloats(tag, eoff, 24);
                    // Verified layout (loader 0x65EAF0), so the fields can be
                    // named instead of guessed at.
                    const float* f = (const float*)eoff;
                    Log("CAM %08X Hac=%g Haf=%g Vac=%g Vaf=%g yaw=%g pitch=%g "
                        "cyaw=%g cpitch=%g rot=%d aim=%d fov=%g cfov=%g shake=%g disrupt=%g",
                        v[i], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                        *(const int*)(eoff + 0x20), *(const int*)(eoff + 0x24),
                        f[10], f[11], f[12], f[13]);
                }
            }
            if (v[i] > 0x10000 && v[i] < 0x7F000000) {
                char tag[40]; _snprintf(tag, sizeof(tag), "obj%u@%08X", i, v[i]);
                DumpRegionFloats(tag, v[i], 24);
            }
        }
    }
    // 0x649180 containers (CloseAimAssist/FarAimAssist, InputInertia blocks):
    // +2C/+30 hold heap pointers to their internal tables.
    DumpRegionDwords("contDF7C08", 0x0DF7C08, 32);
    DumpRegionDwords("contDF7C88", 0x0DF7C88, 32);
    DumpPtrDirect("c08tblA", *(DWORD*)(0x0DF7C08 + 0x2C), 32);
    DumpPtrDirect("c08tblB", *(DWORD*)(0x0DF7C08 + 0x30), 32);
    DumpPtrDirect("c88tblA", *(DWORD*)(0x0DF7C88 + 0x2C), 32);
    DumpPtrDirect("c88tblB", *(DWORD*)(0x0DF7C88 + 0x30), 32);
}

// ---------------- Init ----------------

// ================= v58: in-game settings menu (VK_INSERT) =================
//
// Insert opens a small panel over the game window.  Up/Down select, Left/Right adjust (PgUp/
// PgDn move ten steps), Insert closes.  Everything the user could previously only change by
// editing Mercs2Fix.ini and restarting is now a slider that takes effect while they are
// standing in the world, which is the only way a sensitivity like the turret's can actually be
// tuned rather than guessed at.
//
// Three deliberate limits, all of them consequences of how the patches are built:
//
//  * The panel is a layered, click-through, non-activating window on top of the game, not D3D
//    drawing inside the hooked device.  An in-device overlay has to survive Reset and device
//    loss and needs its own font atlas; none of that is what was asked for, and a GDI panel
//    cannot disturb the game's frame at all.  It relies on the same borderless windowing the
//    rest of the plugin already forces.
//  * Nothing here installs a code detour that was not installed at init.  Writing five bytes
//    over an instruction the game may be executing mid-read is exactly what v57's F8 note
//    refuses to do for the turret rate, so a row needing an uninstalled detour would have to
//    say "restart".  Instead [Menu] Enabled=1 ARMS the optional detours and ctor caves at
//    their inert value, because x*1.0f and X*1+(1-1) are bit-exact no-ops on a float.  That
//    costs one mulss per frame and buys live sliders.
//  * Values go back to their own [Section] Key through WritePrivateProfileStringA, which names
//    the section per call, so the INI EDIT HAZARD (a blind key rewrite across the file that
//    hit four unrelated Enabled= keys and silently turned borderless off) cannot arise here.

static const int kMenuKey      = VK_INSERT;
static const int kMenuStepKeys[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_PRIOR, VK_NEXT,
                                     VK_RETURN, VK_HOME, VK_END };
static const int kMenuNStepKeys = (int)(sizeof(kMenuStepKeys) / sizeof(kMenuStepKeys[0]));
enum { IT_PRESET = 0, IT_SHADOW, IT_RES, IT_VIEW, IT_RATE, IT_MINRATE, IT_RESP, IT_VDELAY,
       IT_VBLEND, IT_PITCH, IT_N };
static const char* kPresetName[3] = { "Custom", "Stock (minimum)", "Maximum quality" };

struct MenuItem {
    int         id;
    const char* label;
    int         kind;          // 1 = float slider, 2 = choice, 3 = int slider, 4 = display only
    float*      val;           // slider target for kind 1
    float       lo, hi, step;
    const char* sec;           // ini section to persist into (NULL = no key of its own)
    const char* key;
    const char* note;
};
static MenuItem g_menu[IT_N] = {
    { IT_PRESET, "Quality preset",        2, nullptr, 0, 0, 0,
      nullptr, nullptr,
      "Stock = every distance exactly as the game set it.  Max = shadow cast at x6 and the "
      "far clip plane at 600, which is the top of what has been driven without Z fighting." },
    { IT_SHADOW, "Shadow cast distance",  1, &g_cfg.shadowBaseScale, 1.0f, 6.0f, 0.25f,
      "Shadow", "BaseDistanceScale",
      "How far from the player things still cast a shadow.  The four caster bands move "
      "together.  Past about x6 the same atlas covers more world, so shadows get softer, not "
      "sharper - resolution is a separate problem and is NOT reachable from here." },
    { IT_RES,    "Shadow map resolution", 4, nullptr, 0, 0, 0,
      "Shadow", "MapSizeScale",
      "The atlas shadows are drawn into.  The game exposes nothing for it: the size is 15 "
      "literal immediates in the exe AND a texel constant baked into data\\shader3.bin, so both "
      "halves must move together.  With the game closed:  python tools\\shadow_res.py set 2  and "
      "[Shadow] MapSizeScale=2.  Disagree and the exe half is left alone, on purpose." },
    { IT_VIEW,   "View distance",         3, nullptr, 0.0f, 600.0f, 25.0f,
      "View", "Distance",
      "The far clip plane: 400 + 20 x this.  It is the same number Mercs2.ini [Render] "
      "ViewDistance sets, applied live.  Terrain can still fade out sooner than this, "
      "because that fade is baked into the level data, not into the camera." },
    { IT_RATE,   "Turret turn rate",      1, &g_cfg.turretTurnRate,  1.0f, 6.0f, 0.25f,
      "Turret", "TurnRateScale",
      "Ceiling on how fast the turret can swing, on top of the shipped 85.9 deg/s." },
    { IT_MINRATE,"Mounted aim speed floor",1, &g_cfg.turretMinRate,   0.0f, 6.0f, 0.25f,
      "Turret", "MinRateRad",
      "The rate every mount is lifted up to.  The ceiling is per-mount record data measured at "
      "0.35..1.5 rad/s, so the x-scale above multiplies the spread instead of closing it - this "
      "is what makes the 105mm artillery keep up.  1.50 is the light turret's own shipped rate, "
      "so the floor leaves it exactly as tuned and lifts everything slower.  0 = pure multiplier." },
    { IT_RESP,   "Turret aim response",   1, &g_cfg.turretCmdResponse, 1.0f, 10.0f, 0.5f,
      "Turret", "CommandResponse",
      "Kills the lag that made the aim stay put on a fast sweep.  The coefficients are read "
      "when the camera is BUILT, so this applies the next time you get in." },
    { IT_VDELAY, "Car camera hold time",  1, &g_cfg.vehDelayScale, 1.0f, 8.0f, 0.25f,
      "Vehicle", "ResetDelayScale",
      "How long the vehicle camera keeps pointing where you left it before it starts pulling "
      "back.  Stock 0.27 s, x8 = 2.17 s.  Applies to the NEXT vehicle you get in." },
    { IT_VBLEND, "Car camera settle time",1, &g_cfg.vehBlendScale, 1.0f, 8.0f, 0.25f,
      "Vehicle", "ResetBlendScale",
      "How long the pull-back takes once it starts - the smoothness half.  Stock 0.50 s is "
      "what makes it feel like a snap.  Same next-vehicle rule as the row above." },
    { IT_PITCH,  "Pitch lock in crouch",  4, nullptr, 0, 0, 0,
      "Camera", "FreePitchInCrouch",
      "The game drops the mouse pitch delta entirely on one camera branch that crouching and "
      "sliding take.  Removing that branch is what lets you look up and down again.  This one "
      "patches a per-frame instruction, so it is set in the ini and applied at startup only." },
};
static int  g_preset = 0;
static volatile LONG g_menuOpen = 0;
static volatile LONG g_menuDirty = 1;
static int  g_menuCur = 0;
static HWND g_hMenuWnd = nullptr;
static char g_menuNote[256] = "";

static float MenuRoundToStep(float v, float lo, float hi, float step) {
    float n = lo + step * (float)(int)((v - lo) / step + 0.5f);
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return n;
}

// ---------- the far clip plane: one int in .data, no code patch ----------
//
// The game reads Mercs2.ini [Render] ViewDistance once at startup (GetPrivateProfileIntA with
// default 100 at 0x007537B3) and stores it at [0x00DFC348].  A whole-.text scan for that
// address as a displacement finds exactly three references: that store and the two loads in
// the camera's own projection update at 0x007141EF / 0x007141F4, which computes
//     far = 400.0 + ViewDistance * 0.01 * 2000.0
// and stores it as the camera's far plane.  So this is a plain int the game re-reads every
// frame with one consumer and no other writer, which makes it the rarest kind of lever here:
// the ini key's own semantics, live.
static const DWORD VIEWDIST_RVA = 0x009FC348;   // VA 0x00DFC348
static int* g_pViewDist = nullptr;
static int  g_viewGame = 0;
static bool g_viewDead = false;

static bool ViewLocate() {
    if (g_pViewDist) return true;
    if (g_viewDead) return false;
    HMODULE base = GetModuleHandleW(nullptr);
    int* p = (int*)((BYTE*)base + VIEWDIST_RVA);
    int v = 0;
    __try { v = *p; } __except (EXCEPTION_EXECUTE_HANDLER) { g_viewDead = true; return false; }
    // The game reads Mercs2.ini into this slot during its own startup, which is after this
    // ASI's init thread runs, so zero here just means "not yet".  Anything else implausible
    // means the RVA is wrong for this exe and the slider must not write memory.
    if (v <= 0) return false;
    if (v > 4096) {
        g_viewDead = true;
        Log("View: [0x00DFC348] holds %d, not a ViewDistance - the view distance slider is OFF", v);
        return false;
    }
    g_pViewDist = p;
    g_viewGame = v;
    Log("View: [Render] ViewDistance in effect is %d, so the far clip plane is %.0f.  The "
        "slider writes this int directly; the camera re-reads it every frame.",
        v, 400.0 + 20.0 * (double)v);
    return true;
}

static void ViewApply(int v) {
    if (!ViewLocate()) return;
    __try { *g_pViewDist = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_viewDead = true; }
}

// ---------- persistence ----------
static void MenuIni(const MenuItem* it) {
    if (!g_cfg.menuSave || !it->sec) return;
    char path[MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%s\\%s", g_moduleDir, INI_FILE);
    char buf[32];
    if (it->kind == 1) _snprintf(buf, sizeof(buf), "%.2f", *it->val);
    else if (it->kind == 3) _snprintf(buf, sizeof(buf), "%d", g_cfg.viewDistance);
    else return;                      // choices and display rows have no key to write
    if (!WritePrivateProfileStringA(it->sec, it->key, buf, path))
        Log("Menu: WritePrivateProfileString([%s] %s) failed (%lu)", it->sec, it->key,
            GetLastError());
}

// ---------- applying one row ----------
static void MenuFormat(int id, char* out, size_t n);

static void MenuCommit(int id) {
    MenuItem* it = &g_menu[id];
    switch (id) {
    case IT_SHADOW:
        // Nothing to do: the cave at 0x00859322 re-reads g_cfg.shadowBaseScale every frame.
        break;
    case IT_VIEW:
        ViewApply(g_cfg.viewDistance);
        break;
    case IT_RATE:
        if (!g_turretYawOn && !g_turretPitchOn && g_cfg.turretTurnRate != 1.0f)
            Log("Menu: the turn-rate detour is not installed (the ctor sites did not hold "
                "their expected bytes), so this slider changes only what a restart would do.");
        break;
    case IT_MINRATE:
        // Same cave as the turn rate, and the same caveat: the MAXSS is applied as the game READS
        // the rate, so this is live - but the base it is compared against is constructor data, so a
        // mount the player is already sitting in keeps the rate it was built with until they
        // remount.
        if (!g_turretYawOn && !g_turretPitchOn && g_cfg.turretMinRate != 0.0f)
            Log("Menu: the turn-rate detour is not installed, so the floor slider changes only "
                "what a restart would do.");
        break;
    case IT_RESP:
        // The cave expresses X' = X*K + (1-K) as two float reads, so the bias has to move with
        // the response or the transform silently becomes a pure gain.
        g_cfg.turretCmdBias = 1.0f - g_cfg.turretCmdResponse;
        if (!g_cmdLagOn && g_cfg.turretCmdResponse != 1.0f)
            Log("Menu: the CAM-E ctor cave is not installed, so the response slider is inert "
                "until a restart.");
        break;
    case IT_VDELAY:
        if (!g_vehDelayOn && g_cfg.vehDelayScale != 1.0f)
            Log("Menu: CAM-A's delay cave is not installed, so the hold-time slider changes "
                "only what a restart would do.");
        break;
    case IT_VBLEND:
        if (!g_vehBlendOn && g_cfg.vehBlendScale != 1.0f)
            Log("Menu: CAM-A's blend cave is not installed, so the settle-time slider changes "
                "only what a restart would do.");
        break;
    default:
        break;
    }
    if (id != IT_PRESET) g_preset = 0;
    if (it->sec) MenuIni(it);
    char v[32]; MenuFormat(id, v, sizeof(v));
    _snprintf(g_menuNote, sizeof(g_menuNote) - 1, "%s = %s", it->label, v);
    Log("Menu: %s set to %s%s", it->label, v, it->sec ? " (written to ini)" : "");
    InterlockedExchange(&g_menuDirty, 1);
}

static void MenuFormat(int id, char* out, size_t n) {
    MenuItem* it = &g_menu[id];
    if (id == IT_PRESET) {
        _snprintf(out, n, "%s", kPresetName[g_preset]);
    } else if (id == IT_PITCH) {
        _snprintf(out, n, "%s", g_pitchGateOff ? "removed (ini, restart)"
                                               : "stock (ini, restart)");
    } else if (id == IT_RATE) {
        _snprintf(out, n, "x%.2f = %.0f deg/s", g_cfg.turretTurnRate,
                  85.9437f * g_cfg.turretTurnRate);
    } else if (id == IT_MINRATE) {
        // In rad/s and deg/s rather than a multiplier, because this one is an absolute floor and
        // the number the user has to compare it against is the per-mount record rate the capture
        // prints on the CAMOBJ RATE line - which is in rad/s.
        _snprintf(out, n, "%.2f rad/s = %.0f deg/s", g_cfg.turretMinRate,
                  g_cfg.turretMinRate * 57.29578f);
    } else if (id == IT_VIEW) {
        _snprintf(out, n, "%d = far %.0f", g_cfg.viewDistance,
                  400.0 + 20.0 * (double)g_cfg.viewDistance);
    } else if (id == IT_SHADOW) {
        _snprintf(out, n, "x%.2f", g_cfg.shadowBaseScale);
    } else if (id == IT_RES) {
        // Both halves on one line, because the only interesting thing about this one is whether
        // they agree: the exe immediates and the constant in data\shader3.bin.
        char f[8];
        if (g_shaderScale) _snprintf(f, sizeof(f), "x%d", g_shaderScale);
        else               memcpy(f, "?", 2);
        _snprintf(out, n, "atlas x%d / shader %s", g_atlasScaleLive, f);
    } else if (id == IT_VDELAY) {
        // The shipped figure is YawResetTime 3.25 / 12, quoted from the exe's own data, so the
        // panel shows seconds rather than a bare multiplier.
        _snprintf(out, n, "x%.2f = %.2f s", g_cfg.vehDelayScale, 0.270833 * g_cfg.vehDelayScale);
    } else if (id == IT_VBLEND) {
        _snprintf(out, n, "x%.2f = %.2f s", g_cfg.vehBlendScale, 0.5 * g_cfg.vehBlendScale);
    } else {
        _snprintf(out, n, "x%.2f", *it->val);
    }
}

static void MenuPreset(int which) {
    g_preset = which;
    if (which == 1) {
        // "Stock" has to mean the value THIS install was configured with, not a number
        // invented here, so the view distance goes back to what the game read at startup.
        g_cfg.shadowBaseScale = 1.0f;
        g_cfg.viewDistance = g_viewGame ? g_viewGame : 100;
    } else if (which == 2) {
        g_cfg.shadowBaseScale = 6.0f;
        g_cfg.viewDistance = 600;
    }
    if (which != 0) {
        ViewApply(g_cfg.viewDistance);
        MenuIni(&g_menu[IT_SHADOW]);
        MenuIni(&g_menu[IT_VIEW]);
    }
    _snprintf(g_menuNote, sizeof(g_menuNote) - 1, "Preset: %s", kPresetName[which]);
    Log("Menu: quality preset %s (shadow cast x%.2f, view distance %d)", kPresetName[which],
        g_cfg.shadowBaseScale, g_cfg.viewDistance);
    InterlockedExchange(&g_menuDirty, 1);
}

static void MenuAdjust(int id, int dir) {
    MenuItem* it = &g_menu[id];
    if (id == IT_PRESET) {
        int p = g_preset + dir;
        if (p < 0) p = 0;
        if (p > 2) p = 2;
        if (p != g_preset) MenuPreset(p);
        return;
    }
    if (id == IT_PITCH) {
        _snprintf(g_menuNote, sizeof(g_menuNote) - 1,
                  "%s is set in Mercs2Fix.ini and applied at startup.", it->label);
        InterlockedExchange(&g_menuDirty, 1);
        return;
    }
    if (id == IT_RES) {
        _snprintf(g_menuNote, sizeof(g_menuNote) - 1,
                  "Shadow resolution needs both halves: run 'python tools\\shadow_res.py set 2' "
                  "with the game CLOSED, then set [Shadow] MapSizeScale=2 and restart.  Right now "
                  "the exe is x%d and data\\shader3.bin is x%d.",
                  g_atlasScaleLive, g_shaderScale);
        InterlockedExchange(&g_menuDirty, 1);
        return;
    }
    if (id == IT_VIEW) {
        // 0 in the ini means "whatever the game was configured with", so the first nudge has
        // to start from the value the game actually read rather than from zero.
        if (g_cfg.viewDistance <= 0) g_cfg.viewDistance = g_viewGame > 0 ? g_viewGame : 100;
        int step = (int)it->step * (dir < 0 ? -1 : 1);
        if (dir <= -10 || dir >= 10) step *= 4;
        int v = g_cfg.viewDistance + step;
        if (v < 0) v = 0;
        if (v > (int)it->hi) v = (int)it->hi;
        if (v == g_cfg.viewDistance) return;
        g_cfg.viewDistance = v;
        MenuCommit(id);
        return;
    }
    float d = it->step * (float)dir;
    *it->val = MenuRoundToStep(*it->val + d, it->lo, it->hi, it->step);
    MenuCommit(id);
}

static bool MenuOpenNow() {
    return InterlockedCompareExchange(&g_menuOpen, 0, 0) != 0;
}

static bool MenuOwnsKey(int vk) {
    if (vk == kMenuKey) return true;
    for (int i = 0; i < (int)(sizeof(kMenuStepKeys) / sizeof(kMenuStepKeys[0])); ++i)
        if (kMenuStepKeys[i] == vk) return true;
    return false;
}

// ---------- the panel ----------
static void FillRect32(uint32_t* px, int W, int x, int y, int w, int h, uint32_t argb) {
    for (int j = y; j < y + h; ++j) {
        uint32_t* row = px + (size_t)j * W;
        for (int i = x; i < x + w; ++i) row[i] = argb;
    }
}

static const uint32_t kColBg     = 0xFF141820;   // opaque panel
static const uint32_t kColSel    = 0xFF2A4C78;
static const uint32_t kColHead   = 0xFF1E2632;
static const uint32_t kColText   = 0xFFE8ECF2;
static const uint32_t kColDim    = 0xFF9AA8BC;
static const uint32_t kColValue  = 0xFFFFD466;

static void MenuRender() {
    if (!g_hMenuWnd) return;
    // The note block has to hold the row note AND the status line appended under it.  At 1180 px
    // wide with a -22 Consolas the column is ~95 characters per line and ~26 px per line, and the
    // longest pair now is shadow resolution (~340 chars) plus its atlas/shader status (~200), so
    // 7 lines.  88 px clipped the third line in v59; 176 px leaves room for a longer status line
    // without the panel reaching 600 px tall.
    const int rowH = 34, headH = 40, padX = 18, noteH = 176;
    const int W = 1180;
    const int H = headH * 2 + IT_N * rowH + noteH;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    if (!mem) return;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(mem); return; }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    uint32_t* px = (uint32_t*)bits;
    memset(px, 0, (size_t)W * H * 4);

    FillRect32(px, W, 0, 0, W, H, kColBg);
    FillRect32(px, W, 0, 0, W, headH, kColHead);
    FillRect32(px, W, 0, headH + g_menuCur * rowH + headH, W, rowH, kColSel);

    HFONT font = CreateFontA(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DRAFT_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(mem, font ? font : GetStockObject(SYSTEM_FONT));
    SetBkMode(mem, TRANSPARENT);

    RECT rc = { padX, 8, W - padX, headH };
    SetTextColor(mem, kColValue);
    DrawTextA(mem, "MERC S2FIX v" MF_VERSION "   settings", -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(mem, kColDim);
    DrawTextA(mem, "Insert close+save   Up/Down pick   Left/Right change   PgUp/PgDn x10",
              -1, &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // line has to hold the row note AND the status line under it: shadow resolution's note is
    // ~340 characters and its status ~190, so 448 truncated the second one.
    char line[768], val[48];
    for (int i = 0; i < IT_N; ++i) {
        rc.left = padX; rc.top = headH * 2 + i * rowH + 4; rc.right = W - 470; rc.bottom = rc.top + rowH;
        SetTextColor(mem, i == g_menuCur ? kColText : kColDim);
        DrawTextA(mem, g_menu[i].label, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        MenuFormat(i, val, sizeof(val));
        rc.left = W - 460; rc.right = W - padX;
        SetTextColor(mem, kColValue);
        DrawTextA(mem, val, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    rc.left = padX; rc.top = headH * 2 + IT_N * rowH + 6; rc.right = W - padX;
    rc.bottom = H - 6;
    SetTextColor(mem, kColText);
    _snprintf(line, sizeof(line) - 1, "%s\n%s", g_menu[g_menuCur].note,
              g_menuNote[0] ? g_menuNote : "");
    DrawTextA(mem, line, -1, &rc, DT_LEFT | DT_WORDBREAK);

    // GDI writes RGB only and leaves the alpha byte zero on a 32-bit DIB, which UpdateLayered-
    // Window would then present as fully transparent.  The panel is opaque by design, so the
    // whole rect is forced to alpha 255 after the text goes down.
    for (int j = 0; j < H; ++j) {
        uint32_t* row = px + (size_t)j * W;
        for (int i = 0; i < W; ++i) row[i] |= 0xFF000000u;
    }

   POINT dst;
    RECT wr;
    dst.x = 40; dst.y = 40;
    if (g_hGameWnd && GetWindowRect(g_hGameWnd, &wr)) { dst.x = wr.left + 40; dst.y = wr.top + 40; }
    SIZE sz = { W, H };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 245;
    bf.AlphaFormat = AC_SRC_ALPHA;
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_hMenuWnd, screen, &dst, &sz, mem, &src, RGB(0, 0, 0), &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);

    SelectObject(mem, oldFont);
    if (font) DeleteObject(font);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    InterlockedExchange(&g_menuDirty, 0);
}

static LRESULT CALLBACK MenuWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // The panel never takes focus (WS_EX_NOACTIVATE) and never needs hit-testing
    // (WS_EX_TRANSPARENT), so there is nothing to service here.  Painting is driven by
    // g_menuDirty from MenuThread, because UpdateLayeredWindow keeps our surface - a window
    // that gets covered does not need to be redrawn.
    return DefWindowProcW(h, m, w, l);
}

// One press has to move exactly one step.  GetAsyncKeyState does not report "a press", it
// reports the instantaneous bit, and Windows key auto-repeat produces that bit by RELEASE AND
// RE-PRESS the key up to ~31 times a second.  So a rising-edge test alone counts a held arrow
// as 31 separate presses, which is the reported "1 button press and it goes up/down/left/right
// multiple times".  The fix is the standard UI behaviour, and it needs the gap explicitly:
// a key counts as still held while its last down was within kMenuGapMs (that is what sees
// through the repeat flicker), a press acts once on contact, and only after the key has really
// been held for kMenuHoldMs does it start walking, one step per kMenuRepeatMs.
struct MenuKeyState { DWORD lastSeen; int phase; DWORD tAct; int reps; };
static const DWORD kMenuGapMs    = 90;
static const DWORD kMenuHoldMs   = 450;
static const DWORD kMenuRepeatMs = 220;

static int MenuKeyEdge(MenuKeyState* st, int n) {
    DWORD now = GetTickCount();
    for (int i = 0; i < n; ++i) {
        MenuKeyState* k = &st[i];
        if (GetAsyncKeyState(kMenuStepKeys[i]) & 0x8000) k->lastSeen = now;
        if (!k->lastSeen || (LONG)(now - k->lastSeen) >= (LONG)kMenuGapMs) {
            k->phase = 0; k->reps = 0;
            continue;
        }
        if (!k->phase) { k->phase = 1; k->tAct = now; k->reps = 0; return kMenuStepKeys[i]; }
        if ((LONG)(now - k->tAct) >= (LONG)(k->reps ? kMenuRepeatMs : kMenuHoldMs)) {
            k->tAct = now; k->reps++;
            return kMenuStepKeys[i];
        }
    }
    return 0;
}

static DWORD WINAPI MenuThread(LPVOID) {
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MenuWndProc;
    wc.hInstance = g_hSelf;
    wc.lpszClassName = "Mercs2FixMenu";
    RegisterClassExA(&wc);

    int prevOpen = 0;
    MenuKeyState step[kMenuNStepKeys];
    memset(step, 0, sizeof(step));
    DWORD insSeen = 0; int insPhase = 0;
    int f8Phase = 0, f8Done = 0;
    DWORD nextView = 0;

    for (;;) {
        DWORD tick = GetTickCount();
        // Insert is watched on the async key state rather than on WM_KEYDOWN, the same way F8
        // is: the poll cannot be swallowed by however the game chooses to read its keyboard.
        // It auto-repeats like every other key, so "held" is again the gap rule below and the
        // toggle happens on contact only - a held Insert must not strobe the panel open and shut.
        if (GetAsyncKeyState(kMenuKey) & 0x8000) insSeen = tick;
        int insHeld = insSeen && (LONG)(tick - insSeen) < (LONG)kMenuGapMs;
        if (insHeld && !insPhase) {
            insPhase = 1;
            InterlockedExchange(&g_menuOpen, InterlockedCompareExchange(&g_menuOpen, 0, 0) ? 0 : 1);
        }
        if (!insHeld) insPhase = 0;

        // F8 reverts the two turret feel patches for the rest of the run so that "is this
        // better or worse than stock" is one session, not two.  One-way: a key we already
        // honoured must not be re-armed by a stray press.
        if (GetAsyncKeyState(VK_F8) & 0x8000) {
            if (!f8Phase) {
                f8Phase = 1;
                if (!f8Done) {
                    f8Done = 1;
                    KillTurretTurnRate();
                    KillTurretCommandLag();
                    Log("F8: turret turn rate and aim response back to the shipped values for "
                        "the rest of this run.");
                }
            }
        } else f8Phase = 0;

        int isOpen = InterlockedCompareExchange(&g_menuOpen, 0, 0) ? 1 : 0;
        if (isOpen && !prevOpen) {
            if (!g_hMenuWnd) {
                g_hMenuWnd = CreateWindowExA(
                    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                    WS_EX_TOOLWINDOW,
                    "Mercs2FixMenu", "", WS_POPUP, 0, 0, 100, 100,
                    nullptr, nullptr, g_hSelf, nullptr);
            }
            if (g_hMenuWnd) {
                ShowWindow(g_hMenuWnd, SW_SHOWNOACTIVATE);
                InterlockedExchange(&g_menuDirty, 1);
                Log("Menu: opened. shadow=%.2f view=%d (game %d) rate=%.2f resp=%.2f "
                    "pitchGate=%d",
                    g_cfg.shadowBaseScale, g_cfg.viewDistance, g_viewGame,
                    g_cfg.turretTurnRate, g_cfg.turretCmdResponse, g_pitchGateOff ? 1 : 0);
            }
        } else if (!isOpen && prevOpen) {
            if (g_hMenuWnd) ShowWindow(g_hMenuWnd, SW_HIDE);
            Log("Menu: closed. shadow=%.2f view=%d rate=%.2f resp=%.2f",
                g_cfg.shadowBaseScale, g_cfg.viewDistance, g_cfg.turretTurnRate,
                g_cfg.turretCmdResponse);
        }
        prevOpen = isOpen;

        if (isOpen) {
            int k = MenuKeyEdge(step, kMenuNStepKeys);
            if (k) {
                int d = 0;
                switch (k) {
                case VK_UP:   g_menuCur = (g_menuCur + IT_N - 1) % IT_N; break;
                case VK_DOWN: g_menuCur = (g_menuCur + 1) % IT_N; break;
                case VK_LEFT: case VK_PRIOR: d = -1; break;
                case VK_RIGHT: case VK_NEXT: d = +1; break;
                case VK_HOME: g_menuCur = 0; break;
                case VK_END:  g_menuCur = IT_N - 1; break;
                case VK_RETURN: d = +1; break;
                default: break;
                }
                if (k == VK_PRIOR || k == VK_NEXT) d *= 10;
                if (d) MenuAdjust(g_menuCur, d);
                else InterlockedExchange(&g_menuDirty, 1);
            }
            if (g_menuDirty) MenuRender();
        }

        // The slot only becomes meaningful once the game has read its own ini, which happens
        // after this thread starts, and the point of [View] Distance is that it should not be
        // lost again if something else writes the slot later.
        if ((LONG)(GetTickCount() - nextView) >= 0) {
            nextView = GetTickCount() + 500;
            if (!g_pViewDist) ViewLocate();
            else if (g_cfg.viewDistance > 0 && *g_pViewDist != g_cfg.viewDistance)
                ViewApply(g_cfg.viewDistance);
        }
        Sleep(20);
    }
}

static void InitAll() {
    InitializeCriticalSection(&g_logCs);
    GetModuleFileNameA(g_hSelf, g_moduleDir, sizeof(g_moduleDir));
    char* p = strrchr(g_moduleDir, '\\'); if (p) *p = 0;
    LoadConfig();

    // Must be first: it rewrites the load-time parameter reads, and the game
    // starts streaming records as soon as a level begins to load.
    GsfInit();
    if (g_cfg.setEnabled) GsaInit();
    else Log("GSA: disabled by [Settings] Enabled=0");

    Log("=== Mercs2Fix v%s initializing ===", MF_VERSION);
    Log("This build: borderless windowed at the resolution you pick in the game, and a settings");
    Log("panel on INSERT for the things worth changing while playing.  Gameplay fixes, all live:");
    Log("mounted-weapon turn rate and command response (the artillery/boat guns were aiming far");
    Log("too slowly), the crouch/slide pitch lock removed, the car camera's re-centre slowed to a");
    Log("hold and a blend, and the aim-assist snap suppressed.  Shadow map resolution and cast");
    Log("distance are the two rows that need the shader tool as well as the ini - see README.md.");
    Log("Two things to know before you raise [View] Distance: the water needs DXVK (a d3d9.dll in");
    Log("the game folder - the stock 2008 path renders Lake Maracaibo wrong on modern drivers), and");
    Log("above roughly 300 some dynamic objects (the player, mounted weapons) can vanish at certain");
    Log("camera angles.  That second one is a game bug we have not solved; the summary is in README.md.");

    HMODULE hGame = GetModuleHandleW(nullptr);
    Log("Game base: %p, PID: %u", hGame, GetCurrentProcessId());

    // On-demand logging: press the capture key to dump for a few seconds only.

    if (g_cfg.aimAssistDisable && g_cfg.aimDefaults) ApplyAimAssistPatches();
    if (g_cfg.aimAssistDisable && g_cfg.aimStub)     ApplyAimAssistStub();
    // v34: live call-site patches from static analysis
    if (g_cfg.aimAssistDisable && g_cfg.aimNopCallSite) ApplyAimCallNop();
    if (g_cfg.aimDiagHook)                              ApplyAimDiagHook();
    // v41: the two features land on single, byte-verified runtime sites found by
    // static RE; each switch is independent so a failure isolates itself.
    ApplyCameraRuntimePatches();
    g_shaderScale = ShadowProbeShader();
    ApplyShadowMapScale();
    ApplyShadowBaseScale();
    ApplyTurretTurnRate();
    ApplyTurretCommandLag();
    ApplyCamBTurnRate();
    ApplyFreePitchGate();
    ApplyVehicleRecentre();
    if (g_cfg.menuEnabled) {
        CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr);
        Log("Menu: thread started - press Insert in game to open the settings panel.");
    } else {
        Log("Menu: disabled by [Menu] Enabled=0, so the optional scale caves and detours keep "
            "their shipped off behaviour (installed only when their scale is not 1.0).");
    }
    if (g_cfg.lockOnHoldNop)   ApplyLockOnHoldDiscard();
    if (g_cfg.keyboardPrompts) ApplyKeyboardPrompts();

    if (g_cfg.borderless) {
        CreateThread(nullptr, 0, GameWindowThread, nullptr, 0, nullptr);
    }

    if (g_cfg.forceWindowedD3D || g_cfg.vsyncOff) {
        g_realDirect3DCreate9 = (PFN_Direct3DCreate9)
            HookIAT(hGame, "d3d9.dll", "Direct3DCreate9", &hkDirect3DCreate9);
        Log("D3D9: Direct3DCreate9 IAT hook -> %p", g_realDirect3DCreate9);
    }

    // The insert hook is what wins the level-load race; the watchdog only mops up
    // entries that appear without going through 0x64A600. Neither touches
    // YawSpeed/PitchSpeed unless MinYawPitch asks for a floor.
    if (g_cfg.aimAssistDisable && g_cfg.aimWatchdog) {
        CreateThread(nullptr, 0, AimWatchThread, nullptr, 0, nullptr);
        Log("AimWatch: thread started (200ms poll)");
    }
    if (g_cfg.aimAssistDisable && (g_cfg.aimPatchAtInsert || g_cfg.insertLogging)) {
        InstallMapInsertHook();
    }

    // Seat/turret records: this is where mounted-weapon dampening and the
    // aim-assist strengths live.  Still observe-only.  With DeferToCapture the
    // detour is not present at all outside a capture window, which keeps the
    // engine's shared commit path byte-identical to an unmodified game while the
    // input regression is being chased.
    if (g_cfg.seatLogging && !g_cfg.seatDefer) {
        InstallSeatCommitHook();
    } else if (g_cfg.seatLogging) {
        Log("SeatCommit: armed for the capture window only (0x649180 untouched)");
    }


    Log("=== Init complete ===");
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_hSelf = h;
        InitAll();
    } else if (reason == DLL_PROCESS_DETACH) {
        RevertAimDiagHook();
        RevertAimCallNop();
        RevertSeatCommitHook();
        RevertMapInsertHook();
        RevertAimAssistStub();
        RevertAimAssistPatches();
        RevertCameraRuntimePatches();
        GsaRevert();
        GsfRevert();
    }
    return TRUE;
}
