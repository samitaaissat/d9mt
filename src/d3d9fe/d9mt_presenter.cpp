// d9mt: Metal backend — Presenter (CAMetalLayer) + DxvkSwapchainBlitter.
//
// Present flow (BACKEND-SURFACE §5.1):
//   app thread:  presenter->acquireNextImage(sync, image)
//                  -> returns the PROXY image (B8G8R8A8 private texture,
//                     recreated on extent/sync-interval change; the layer's
//                     drawable size always equals the proxy extent)
//   CS thread:   blitter->present(cmdList, dstView(proxy), dstRect,
//                                 srcView(backbuffer), srcRect)
//                  -> fullscreen-triangle sample pass into the proxy
//                     (or blit-encoder copy when 1:1 and same format)
//                ctx->flushCommandList   (commits the frame's work)
//                device->presentImage -> presenter->presentImage(frameId)
//                  -> nextDrawable, blit proxy -> drawable, presentDrawable,
//                     commit, watcher callback -> signalFrame(frameId)
//   watcher:     signalFrame: fps limit + m_signal->signal(frameId)
//
// LIVENESS: signalFrame(frameId) fires on EVERY presentImage path — failure
// paths signal through the watcher's pure-callback entry (cmdbuf == 0),
// which runs after all previously watched command buffers retired, so the
// signal still happens "after the GPU caught up" and stays monotonic.
//
// HWND channel: the Presenter learns its window through the surface proc:
// the CreatePresenter lambda calls wsi::createSurface with our fake
// vki()->getLoaderProc(), whose vkCreateWin32SurfaceKHR smuggles the HWND
// back as the VkSurfaceKHR value (see d9mt_instance.cpp). m_surface == HWND.

#include <cstring>
#include <unordered_map>
#include <vector>

#include "d9mt_backend.h"
#include "d9mt_hud.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../../vendor/dxvk/src/dxvk/dxvk_device.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_presenter.h"
#include "../../vendor/dxvk/src/dxvk/dxvk_swapchain_blitter.h"
#include "../../vendor/dxvk/src/dxvk/hud/dxvk_hud.h"
#include "../../vendor/dxvk/src/wsi/wsi_window.h"

namespace dxvk::d9mt {

  // --------------------------------------------------------------------------
  // Multi-frame Metal frame capture. Enabled by D9MT_CAPTURE=1 (launcher pairs
  // it with MTL_CAPTURE_ENABLED=1). When the trigger file "d9mt-capture" appears
  // in the cwd (game dir): wait a short delay (so you can unpause), then capture
  // N frames to a .gputrace the user opens in Xcode.
  //   frames: D9MT_CAPTURE_FRAMES (default 12)
  //   delay : D9MT_CAPTURE_DELAY  seconds (default 5)
  // --------------------------------------------------------------------------
  static void captureTick(obj_handle_t queue) {
    static const bool enabled = [] {
      const char* v = std::getenv("D9MT_CAPTURE");
      return v && v[0] == '1';
    }();
    if (!enabled)
      return;

    static const int kFrames = [] {
      const char* v = std::getenv("D9MT_CAPTURE_FRAMES");
      int n = v ? std::atoi(v) : 0;
      return (n > 0 && n <= 600) ? n : 12;
    }();
    static const double kDelay = [] {
      const char* v = std::getenv("D9MT_CAPTURE_DELAY");
      double d = v ? std::atof(v) : -1.0;
      return (d >= 0.0 && d <= 60.0) ? d : 5.0;
    }();

    enum State { Idle, Armed, Capturing };
    static State state = Idle;
    static std::chrono::steady_clock::time_point armAt;
    static int captured = 0;

    switch (state) {
    case Idle: {
      struct stat st;
      if (::stat("d9mt-capture", &st) != 0)
        return;
      ::unlink("d9mt-capture");
      armAt = std::chrono::steady_clock::now();
      state = Armed;
      Logger::warn(str::format("d9mt: capture armed — starting in ", kDelay,
                               "s (", kFrames, " frames). Unpause now."));
      return;
    }
    case Armed: {
      double waited = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - armAt).count();
      if (waited < kDelay)
        return;
      const char* path = "d9mt-frame.gputrace";
      struct d9mt_capture_params p = {};
      p.queue = (uint64_t)queue;
      p.path_ptr = (uint64_t)(uintptr_t)path;
      p.path_len = std::strlen(path);
      p.action = 1;
      D9MT_UnixCall(D9MT_FUNC_CAPTURE, &p);
      if (p.ret_ok) {
        state = Capturing;
        captured = 0;
        Logger::warn("d9mt: capturing…");
      } else {
        state = Idle;
        Logger::warn("d9mt: frame capture failed to start (MTL_CAPTURE_ENABLED?)");
      }
      return;
    }
    case Capturing: {
      if (++captured < kFrames)
        return;
      struct d9mt_capture_params p = {};
      p.action = 0;
      D9MT_UnixCall(D9MT_FUNC_CAPTURE, &p);
      state = Idle;
      Logger::warn(str::format("d9mt: capture written to d9mt-frame.gputrace (",
                               kFrames, " frames)"));
      return;
    }
    }
  }

  // ==========================================================================
  // Presenter side state: Metal window objects + the proxy image. The
  // vendored Presenter members are Vulkan-shaped; everything Metal lives
  // here, keyed by the Presenter pointer (same pattern as cmdListState).
  //
  // Locking: state.mutex guards layer/proxy against the CS thread
  // (presentImage) racing the app thread (acquireNextImage/destroyResources).
  // The vendored preferred-extent/format/dirty members are app-thread-only.
  // ==========================================================================

  struct PresenterState {
    std::mutex   mutex;
    HWND         hwnd  = nullptr;
    obj_handle_t view  = 0;   // CreateMetalViewFromHWND; ReleaseMetalView to free
    obj_handle_t layer = 0;   // owned by the view, not separately retained
    Rc<DxvkImage> proxy;
    VkExtent2D   proxyExtent = { };
  };

  namespace {
    std::mutex s_presenterMutex;
    std::unordered_map<const void*, std::unique_ptr<PresenterState>> s_presenterStates;

    PresenterState& presenterState(const void* presenter) {
      std::lock_guard<std::mutex> lock(s_presenterMutex);
      auto& slot = s_presenterStates[presenter];
      if (!slot)
        slot = std::make_unique<PresenterState>();
      return *slot;
    }

    void erasePresenterState(const void* presenter) {
      std::lock_guard<std::mutex> lock(s_presenterMutex);
      s_presenterStates.erase(presenter);
    }
  }


  // ==========================================================================
  // blit pipeline: MSL compiled at runtime through d9mtmetal's
  // newLibraryWithSource (winemetal has no MSL-source entry point), one PSO
  // per destination pixel format. Process-global: one MTLDevice per process,
  // cache intentionally lives for the process lifetime.
  // ==========================================================================

  namespace {

    const char g_blitShaderMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct d9mt_blit_params {
  float2 uv_offset;
  float2 uv_scale;
};

struct d9mt_blit_vout {
  float4 pos [[position]];
  float2 uv;
};

struct DxvkGammaCp {
  ushort r, g, b, a;
};

vertex d9mt_blit_vout d9mt_blit_vs(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  d9mt_blit_vout o;
  o.uv  = uv;
  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  return o;
}

fragment float4 d9mt_blit_ps(d9mt_blit_vout in [[stage_in]],
                             constant d9mt_blit_params& p [[buffer(0)]],
                             texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  return src.sample(s, p.uv_offset + in.uv * p.uv_scale);
}

fragment float4 d9mt_blit_ps_point(d9mt_blit_vout in [[stage_in]],
                                   constant d9mt_blit_params& p [[buffer(0)]],
                                   texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::nearest, address::clamp_to_edge);
  return src.sample(s, p.uv_offset + in.uv * p.uv_scale);
}

float3 apply_gamma(float3 color, constant DxvkGammaCp* ramp) {
  float3 index = color * 255.0f;
  int3 i0 = clamp(int3(index), 0, 255);
  int3 i1 = min(i0 + 1, 255);
  float3 t = index - float3(i0);
  
  float3 v0 = float3(ramp[i0.x].r, ramp[i0.y].g, ramp[i0.z].b) / 65535.0f;
  float3 v1 = float3(ramp[i1.x].r, ramp[i1.y].g, ramp[i1.z].b) / 65535.0f;
  return mix(v0, v1, t);
}

fragment float4 d9mt_blit_ps_gamma(d9mt_blit_vout in [[stage_in]],
                                   constant d9mt_blit_params& p [[buffer(0)]],
                                   texture2d<float> src [[texture(0)]],
                                   constant DxvkGammaCp* ramp [[buffer(1)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 color = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  color.rgb = apply_gamma(color.rgb, ramp);
  return color;
}

fragment float4 d9mt_blit_ps_point_gamma(d9mt_blit_vout in [[stage_in]],
                                         constant d9mt_blit_params& p [[buffer(0)]],
                                         texture2d<float> src [[texture(0)]],
                                         constant DxvkGammaCp* ramp [[buffer(1)]]) {
  constexpr sampler s(filter::nearest, address::clamp_to_edge);
  float4 color = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  color.rgb = apply_gamma(color.rgb, ramp);
  return color;
}

// ===========================================================================
// HDR present: ITU-R BT.2446 Method A inverse tone mapping, operated in
// ICtCp (BT.2100).
//
// Derived from mtld3d v0.6.0 (unix/unix/src/metal/present.msl),
// Copyright (c) 2026 Alexander Theissen, zlib licence. ALTERED SOURCE:
// re-targeted onto d9mt's blitter vertex stage, buffer bindings and
// source-rect UV mapping; the PQ output tail is an addition; mtld3d's SDR copy
// entry point and its Rust host side were not ported.
// See THIRD_PARTY_NOTICES.md.
//
// PROVENANCE, stated precisely, because the upstream comment is inaccurate.
// mtld3d's own header says this is ported from "the ICtCp branch of Bt2446A"
// in Lilium's ReShade HDR shaders and that the matrices come from Lilium's
// colour_space.fxh. Checked against that upstream, both claims are wrong:
//   * Lilium's Bt2446A has only LUMINANCE and YCBCR_LIKE modes -- there is no
//     ICtCp branch. Its one ICtCp inverse tone mapper is a different algorithm
//     (Itmos::Dice) with a shoulder early-out and a different chroma
//     correction (min(min(I1/I2, I2/I1)*1.1, 1)) than the i_ratio scale here.
//   * The matrix values differ from Lilium's at the 4th-5th significant digit
//     (BT.709->LMS 0.2958197875977 vs 0.295764088; ICtCp->LMS-PQ 0.00860903 vs
//     0.00860647484), i.e. independently derived. The one exact match is the
//     forward LMS-PQ->ICtCp table, which is the verbatim BT.2100 published
//     matrix that Lilium also copied from the standard.
// What does coincide with Lilium is the quadratic-inversion line and the
// three-way branch order. Those constants are mechanically forced by inverting
// the piecewise published in ITU-R BT.2446-A section 6.1 (see the derivation
// at the call site), and the branch order is the defensive one: the published
// segment bounds do not tile exactly in float, so testing the outer segments
// first and letting the middle one be the fallback is what avoids an
// unmatched input.
//
// Lilium's ReShade HDR shaders are GPL-3.0, and mtld3d ships no Lilium notice.
// THIRD_PARTY_NOTICES.md records the analysis; the licensing question is NOT
// settled by this comment and gates any public push of this branch.
//
// The BT.709<->LMS and LMS-PQ<->ICtCp matrices and the PQ OETF/EOTF constants
// are ITU-R BT.2100 / SMPTE ST.2084 standard values.
//
// Design: docs/superpowers/specs/2026-08-17-hdr-bt2446-design.md
// ===========================================================================

// Byte-identical to mtld3d's HdrUniforms. `p_hdr` is never read by the
// shader (log2_p_hdr and inv_p_minus_one are its CPU-hoisted forms) but is
// kept so the block matches the reference layout exactly.
struct d9mt_hdr_params {
  float l_hdr_nits;      // live EDR headroom * D9MT_L_SDR
  float p_hdr;           // 1 + 32*pow(l_hdr_nits/10000, 1/2.4)  [unread]
  float log2_p_hdr;      // log2(p_hdr)    -- replaces a pow()
  float inv_p_minus_one; // 1/(p_hdr - 1)  -- replaces a divide
  float ui_nits;         // UI-tag branch: srgb_eotf(rgb) * ui_nits
};

// Reference / diffuse white in nits. Pinned to 100 (NOT the scRGB-conventional
// 80): Apple's compositor anchors 1.0-in-the-drawable to 100 nits and reports
// EDR headroom as a multiplier of that same 100-nit reference. Any other value
// puts the BT.2446-A normalisation out of phase with the compositor.
constant float D9MT_L_SDR = 100.0f;

// BT.2446 pSDR at the SDR reference. mtld3d's literals, kept verbatim for
// look-parity with the build the user tested and approved -- and kept as
// COMPILE-TIME CONSTANTS rather than uniforms for the same reason: as
// constants the compiler folds the reciprocal and the (P_SDR-1) product,
// and under fast-math a runtime-uniform form lands on measurably different
// last bits (a GPU A/B against the reference shader showed ~3e-4 relative
// drift on grey, amplified by the ICtCp round-trip). Parity is the point.
//
// Note they are NOT self-consistent: 1 + 32*pow(100/10000, 1/2.4) evaluates
// to 5.6969576564, and ln(5.4395284707) is 1.6936924, not 1.6937747. The
// shipped pair corresponds to ~87.35 nits of paper white and runs the curve
// 2-5% darker through the midtones. Do not "correct" them: they ARE the
// approved look.
constant float D9MT_P_SDR     = 5.4395284707f;
constant float D9MT_LOG_P_SDR = 1.6937747f;   // natural log, NOT log2

// sRGB EOTF, IEC 61966-2-1 piecewise. NOT the pow(x, 2.2) shortcut: at EDR
// brightness its ~2% midtone error is visible.
inline float3 d9mt_srgb_eotf(float3 c) {
  return select(pow((c + 0.055f) / 1.055f, float3(2.4f)),
                c / 12.92f,
                c <= float3(0.04045f));
}

// BT.709 linear-light -> LMS (BT.709 -> BT.2020 -> LMS, folded).
// Column-major, per MSL's float3x3(col0, col1, col2).
constant float3x3 D9MT_M_BT709_TO_LMS = float3x3(
    float3(0.2958197875977f, 0.1562652587891f, 0.0351760864258f),
    float3(0.6230921020508f, 0.7272338867188f, 0.1564789062500f),
    float3(0.0810847778320f, 0.1164960937500f, 0.8083459472656f));

constant float3x3 D9MT_M_LMS_TO_BT709 = float3x3(
    float3( 6.1729507446289f, -1.3236293334961f, -0.0118084289551f),
    float3(-5.3198394775391f,  2.5602416992188f, -0.2641143798828f),
    float3( 0.1465606689453f, -0.2363464355469f,  1.2761764526367f));

// LMS-PQ <-> ICtCp, BT.2100-2 section 3.4. MATCHED /4096 PAIR. A /8192-form
// forward (half these Ct/Cp values) against this /4096 inverse silently drops
// chromatic deltas by ~50% — invisible on grey (Ct=Cp=0), catastrophic on
// saturated content. This was a shipped bug in mtld3d; do not mix forms.
// test/hdrcurve.cpp asserts the round-trip specifically to catch it.
constant float3x3 D9MT_M_LMS_PQ_TO_ICTCP = float3x3(
    float3(0.5f,  1.61370003f,  4.37806224f),
    float3(0.5f, -3.32339620f, -4.24553966f),
    float3(0.0f,  1.70969617f, -0.13252264f));

constant float3x3 D9MT_M_ICTCP_TO_LMS_PQ = float3x3(
    float3(1.0f, 1.0f, 1.0f),
    float3(0.00860903f, -0.00860903f,  0.56003270f),
    float3(0.11102963f, -0.11102963f, -0.32062717f));

// ST.2084 inverse EOTF: linear nits (0..10000) -> PQ-encoded [0..1].
inline float3 d9mt_pq_oetf(float3 nits) {
  constexpr float M1 = 2610.0f / 16384.0f;
  constexpr float M2 = 2523.0f / 4096.0f * 128.0f;
  constexpr float C1 = 3424.0f / 4096.0f;
  constexpr float C2 = 2413.0f / 4096.0f * 32.0f;
  constexpr float C3 = 2392.0f / 4096.0f * 32.0f;
  float3 y = pow(max(nits, float3(0.0f)) / 10000.0f, float3(M1));
  return pow((float3(C1) + C2 * y) / (float3(1.0f) + C3 * y), float3(M2));
}

// ST.2084 forward EOTF: PQ-encoded [0..1] -> linear nits.
inline float3 d9mt_pq_eotf(float3 pq) {
  constexpr float M1 = 2610.0f / 16384.0f;
  constexpr float M2 = 2523.0f / 4096.0f * 128.0f;
  constexpr float C1 = 3424.0f / 4096.0f;
  constexpr float C2 = 2413.0f / 4096.0f * 32.0f;
  constexpr float C3 = 2392.0f / 4096.0f * 32.0f;
  float3 e   = pow(max(pq, float3(0.0f)), float3(1.0f / M2));
  float3 num = max(e - float3(C1), float3(0.0f));
  float3 den = max(float3(C2) - C3 * e, float3(1e-20f));
  return 10000.0f * pow(num / den, float3(1.0f / M1));
}

// BT.2446-A inverse curve in ICtCp.
// Input : linear BT.709, 1.0 = paper white.
// Output: linear BT.709 in ABSOLUTE NITS (shared by the scRGB and PQ tails).
inline float3 d9mt_bt2446a_ictcp(float3 lin, constant d9mt_hdr_params& u) {
  // --- luminance curve ---
  // Forward: encode SDR luminance through the BT.2446-A log curve at the SDR
  // reference (P_SDR), invert the piecewise, then DEcode through the same log
  // curve at the HDR target (p_hdr, from live headroom). Both halves are
  // required; collapsing them is not the same function.
  float y_sdr  = max(dot(lin, float3(0.2126f, 0.7152f, 0.0722f)), 1e-20f);
  float yp_sdr = pow(y_sdr, 1.0f / 2.4f);
  float yp_c   = log((yp_sdr * (D9MT_P_SDR - 1.0f)) + 1.0f) / D9MT_LOG_P_SDR;

  // Three-segment inversion of BT.2446-A's forward piecewise:
  //   Y'c = 1.0770 Y'p                            for Y'p <= 0.7399
  //   Y'c = -1.1510 Y'p^2 + 2.7811 Y'p - 0.6302   for 0.7399 < Y'p < 0.9909
  //   Y'c = 0.5 Y'p + 0.5                         for Y'p >= 0.9909
  // The literal 4.83307641 - 4.604*Y'c is the discriminant
  // 2.7811^2 - 4*1.1510*(0.6302 + Y'c); the minus root is correct because the
  // parabola's vertex (Y'p = 1.2081) lies right of the segment's domain.
  float yp_0 = yp_c / 1.0770f;
  float yp_1 = (-2.7811f + sqrt(4.83307641f - 4.604f * yp_c)) / -2.302f;
  float yp_2 = (yp_c - 0.5f) / 0.5f;
  float yp_p = yp_0 <= 0.7399f ? yp_0 : (yp_2 >= 0.9909f ? yp_2 : yp_1);

  // Inverse of the log encoding at the HDR target:
  //   yp_hdr = (p_hdr^yp_p - 1) / (p_hdr - 1)
  // written with the CPU-hoisted log2/reciprocal so the GPU does one exp2.
  float yp_hdr = (exp2(yp_p * u.log2_p_hdr) - 1.0f) * u.inv_p_minus_one;
  float y_hdr  = pow(yp_hdr, 2.4f) * u.l_hdr_nits;

  // --- chroma, carried through ICtCp ---
  // Multiply by L_SDR to enter the absolute-nits domain PQ expects.
  float3 lms_nits = D9MT_M_BT709_TO_LMS * (lin * D9MT_L_SDR);
  float3 lms_pq   = d9mt_pq_oetf(lms_nits);
  float3 ictcp_in = D9MT_M_LMS_PQ_TO_ICTCP * lms_pq;

  float  i_hdr   = d9mt_pq_oetf(float3(y_hdr)).x;
  // NOTE: mtld3d's Rust doc comments claim chroma passes through untouched;
  // the shipped MSL scales Ct/Cp by this ratio. The MSL is the tested
  // artifact, so the multiply is what we port. Without it, saturated content
  // (fire, spell effects) bleaches as luminance is lifted.
  float  i_ratio = i_hdr / max(ictcp_in.x, 1e-6f);
  float3 ictcp_out = float3(i_hdr, ictcp_in.y * i_ratio, ictcp_in.z * i_ratio);

  float3 lms_pq_out   = D9MT_M_ICTCP_TO_LMS_PQ * ictcp_out;
  float3 lms_nits_out = d9mt_pq_eotf(lms_pq_out);
  // The only gamut handling in the pass: a hard per-channel clamp to zero.
  // The ICtCp round-trip plus float-truncated matrices push saturated
  // primaries slightly negative (worst case ~-10% of peak on pure green);
  // clipping is a mild hue shift, not a visible artifact.
  return max(D9MT_M_LMS_TO_BT709 * lms_nits_out, float3(0.0f));
}

// Selected when the layer is HDR but live headroom is <= 1.0 (macOS has not
// promoted the screen yet, or the window sits on an SDR display). BT.2446-A is
// NOT identity at L_hdr == L_sdr — it under-corrects by ~28% at mid grey — so
// this is a separate pipeline, not BT.2446 with a no-op target.
fragment float4 d9mt_blit_ps_hdr_passthrough(
    d9mt_blit_vout in [[stage_in]],
    constant d9mt_blit_params& p [[buffer(0)]],
    texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 c = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  return float4(d9mt_srgb_eotf(c.rgb), c.a);
}

// Extended-linear output: linear BT.709 primaries, 1.0 = D9MT_L_SDR nits.
// The layer's extended-linear colorspace tag performs the gamut handling and
// display encoding. This is the mtld3d-parity path.
fragment float4 d9mt_blit_ps_hdr_bt2446(
    d9mt_blit_vout in [[stage_in]],
    constant d9mt_blit_params& p [[buffer(0)]],
    texture2d<float> src [[texture(0)]],
    constant d9mt_hdr_params& u [[buffer(2)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 c = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  float3 nits = d9mt_bt2446a_ictcp(d9mt_srgb_eotf(c.rgb), u);
  return float4(nits / D9MT_L_SDR, c.a);
}

// PQ / HDR10 output (NOT mtld3d parity — mtld3d never uses PQ). BT.709 nits ->
// BT.2020 primaries -> ST.2084 encode, for a layer tagged
// kCGColorSpaceITUR_2100_PQ. macOS then applies its own PQ->display mapping,
// which will NOT match the extended-linear look above.
constant float3x3 D9MT_M_BT709_TO_BT2020 = float3x3(
    float3(0.6274039f, 0.0690973f, 0.0163914f),
    float3(0.3292830f, 0.9195404f, 0.0880133f),
    float3(0.0433131f, 0.0113623f, 0.8955953f));

// PQ counterpart of the passthrough variant. Selected when the layer is tagged
// PQ but live headroom is <= 1.0. Without it that case would emit
// extended-linear values into a PQ-tagged layer, i.e. the wrong transfer
// function entirely, which is far more wrong than a dark image.
fragment float4 d9mt_blit_ps_hdr_passthrough_pq(
    d9mt_blit_vout in [[stage_in]],
    constant d9mt_blit_params& p [[buffer(0)]],
    texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 c = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  float3 nits2020 = D9MT_M_BT709_TO_BT2020 * (d9mt_srgb_eotf(c.rgb) * D9MT_L_SDR);
  return float4(d9mt_pq_oetf(clamp(nits2020, 0.0f, 10000.0f)), c.a);
}

fragment float4 d9mt_blit_ps_hdr_bt2446_pq(
    d9mt_blit_vout in [[stage_in]],
    constant d9mt_blit_params& p [[buffer(0)]],
    texture2d<float> src [[texture(0)]],
    constant d9mt_hdr_params& u [[buffer(2)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 c = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  float3 nits2020 = D9MT_M_BT709_TO_BT2020
                  * d9mt_bt2446a_ictcp(d9mt_srgb_eotf(c.rgb), u);
  return float4(d9mt_pq_oetf(clamp(nits2020, 0.0f, 10000.0f)), c.a);
}

// UI-aware variants. src is the RAW backbuffer texture (not the RGB1 sample
// view): .a carries the per-pixel UI coverage the draw path accumulated into
// the X8 format's dead alpha channel. World pixels (tag 0) get the full
// BT.2446-A curve; UI pixels (tag 1) get an SDR-faithful branch anchored at
// ui_nits, so text keeps its authored antialiasing ramp instead of having
// its white cores launched to the display peak (the "HDR text is blurry /
// unantialiased" report — the core:edge luminance ratio triples when white
// goes to peak while the AA midtones stay near SDR). The mix happens in
// display-referred nits: both branches are exact at tag 0 and 1, and edge
// pixels interpolate between the two exposures by coverage.
fragment float4 d9mt_blit_ps_hdr_bt2446_ui(
    d9mt_blit_vout in [[stage_in]],
    constant d9mt_blit_params& p [[buffer(0)]],
    texture2d<float> src [[texture(0)]],
    constant d9mt_hdr_params& u [[buffer(2)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 c = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  float3 lin = d9mt_srgb_eotf(c.rgb);
  float3 world_nits = d9mt_bt2446a_ictcp(lin, u);
  float3 ui_nits = lin * u.ui_nits;
  float3 nits = mix(world_nits, ui_nits, saturate(c.a));
  return float4(nits / D9MT_L_SDR, 1.0f);
}

fragment float4 d9mt_blit_ps_hdr_bt2446_ui_pq(
    d9mt_blit_vout in [[stage_in]],
    constant d9mt_blit_params& p [[buffer(0)]],
    texture2d<float> src [[texture(0)]],
    constant d9mt_hdr_params& u [[buffer(2)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 c = src.sample(s, p.uv_offset + in.uv * p.uv_scale);
  float3 lin = d9mt_srgb_eotf(c.rgb);
  float3 nits = mix(d9mt_bt2446a_ictcp(lin, u), lin * u.ui_nits, saturate(c.a));
  float3 nits2020 = D9MT_M_BT709_TO_BT2020 * nits;
  return float4(d9mt_pq_oetf(clamp(nits2020, 0.0f, 10000.0f)), 1.0f);
}
)";

    std::mutex   s_blitMutex;
    bool         s_blitInitFailed = false;
    obj_handle_t s_blitLibrary = 0;
    obj_handle_t s_blitVs = 0;
    obj_handle_t s_blitPs = 0;
    obj_handle_t s_blitPsPoint = 0;
    obj_handle_t s_blitPsGamma = 0;
    obj_handle_t s_blitPsPointGamma = 0;
    obj_handle_t s_blitPsHdrPassthrough = 0;
    obj_handle_t s_blitPsHdrPassthroughPq = 0;
    obj_handle_t s_blitPsHdrBt2446 = 0;
    obj_handle_t s_blitPsHdrBt2446Pq = 0;
    obj_handle_t s_blitPsHdrBt2446Ui = 0;
    obj_handle_t s_blitPsHdrBt2446UiPq = 0;
    std::vector<std::pair<uint32_t, obj_handle_t>> s_blitPsoCache;

    // d9mtmetal ABI handshake result, smuggled out of the newLibrary call.
    // 0 until the blit library has been built once; see the comment on
    // d9mt_newlibrary_params::abi_func_count. Anything using a call code above
    // D9MT_FUNC_PASS_TRANSITION MUST gate on this -- wine's unix dispatch has
    // no bounds check, so calling into an older .so executes whatever bytes
    // follow its table.
    std::atomic<uint32_t> s_d9mtmetalFuncCount = { 0u };

    bool ensureBlitFunctionsLocked() {
      if (s_blitVs && s_blitPs && s_blitPsPoint && s_blitPsGamma && s_blitPsPointGamma
       && s_blitPsHdrPassthrough && s_blitPsHdrPassthroughPq
       && s_blitPsHdrBt2446 && s_blitPsHdrBt2446Pq
       && s_blitPsHdrBt2446Ui && s_blitPsHdrBt2446UiPq)
        return true;
      if (s_blitInitFailed)
        return false;

      obj_handle_t device = mtlDevice();
      if (!device) {
        s_blitInitFailed = true;
        return false;
      }

      d9mt_newlibrary_params lp = { };
      lp.device     = device;
      lp.source_ptr = uint64_t(uintptr_t(g_blitShaderMsl));
      lp.source_len = sizeof(g_blitShaderMsl) - 1u;
      lp.fast_math  = 1u;

      int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);

      // ABI handshake: a d9mtmetal.so built with the HDR entry points writes
      // its D9MT_FUNC_COUNT here; an older one leaves the zero-initialised
      // field alone. Recorded even on the failure path -- the .so answered.
      s_d9mtmetalFuncCount.store(lp.abi_func_count, std::memory_order_relaxed);

      if (st != 0 || !lp.ret_library) {
        Logger::err("d9mt: blitter: newLibraryWithSource failed");
        logf("d9mt: blitter: newLibraryWithSource status %d", st);
        if (lp.ret_error)
          logNSError("d9mt: blitter MSL compile", lp.ret_error);
        s_blitInitFailed = true;
        return false;
      }
      if (lp.ret_error)
        NSObject_release(lp.ret_error); // compile warnings only

      s_blitLibrary = lp.ret_library;
      s_blitVs = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_vs");
      s_blitPs = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps");
      s_blitPsPoint = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_point");
      s_blitPsGamma = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_gamma");
      s_blitPsPointGamma = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_point_gamma");
      s_blitPsHdrPassthrough = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_hdr_passthrough");
      s_blitPsHdrPassthroughPq = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_hdr_passthrough_pq");
      s_blitPsHdrBt2446 = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_hdr_bt2446");
      s_blitPsHdrBt2446Pq = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_hdr_bt2446_pq");
      s_blitPsHdrBt2446Ui = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_hdr_bt2446_ui");
      s_blitPsHdrBt2446UiPq = MTLLibrary_newFunction(s_blitLibrary, "d9mt_blit_ps_hdr_bt2446_ui_pq");

      if (!s_blitVs || !s_blitPs || !s_blitPsPoint || !s_blitPsGamma || !s_blitPsPointGamma
       || !s_blitPsHdrPassthrough || !s_blitPsHdrPassthroughPq
       || !s_blitPsHdrBt2446 || !s_blitPsHdrBt2446Pq
       || !s_blitPsHdrBt2446Ui || !s_blitPsHdrBt2446UiPq) {
        Logger::err("d9mt: blitter: blit functions missing from compiled library");
        s_blitInitFailed = true;
        return false;
      }
      return true;
    }

  } // anonymous namespace

  // non-static: also used by DxvkContext::blitImageView / copyImage /
  // resolveImage (declared in d9mt_backend.h)
  obj_handle_t getBlitPso(WMTPixelFormat dstFormat, bool pointFilter, bool useGamma,
                          HdrMode hdrMode) {
    std::lock_guard<std::mutex> lock(s_blitMutex);

    // WMTPixelFormat's largest value is 555, i.e. TEN bits (0-9) -- NOT the
    // 8 an earlier note claimed, so bits 8-9 are taken and a field placed
    // there would collide with the format. hdrMode needs three bits (5 modes)
    // and must clear bits 30/31, so it sits at 24-26. With hdrMode == None the
    // shift contributes 0 and the key is bit-identical to the pre-HDR
    // expression, which is what keeps the SDR PSO cache unchanged.
    uint32_t key = uint32_t(dstFormat)
                 | (uint32_t(hdrMode) << 24)
                 | (pointFilter ? 0x80000000u : 0u)
                 | (useGamma ? 0x40000000u : 0u);

    for (const auto& e : s_blitPsoCache) {
      if (e.first == key)
        return e.second;
    }

    if (!ensureBlitFunctionsLocked())
      return 0;

    WMTRenderPipelineInfo info = { };
    info.colors[0].pixel_format = dstFormat;
    info.colors[0].write_mask = WMTColorWriteMaskAll;
    info.colors[0].blending_enabled = false;
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    info.depth_pixel_format = WMTPixelFormatInvalid;
    info.stencil_pixel_format = WMTPixelFormatInvalid;
    info.vertex_function = s_blitVs;
    // HDR variants are present-only and always linearly filtered, so they do
    // not multiply with pointFilter/useGamma (gamma is mutually exclusive with
    // HDR -- see DxvkSwapchainBlitter::present).
    switch (hdrMode) {
      case HdrMode::Passthrough:   info.fragment_function = s_blitPsHdrPassthrough;   break;
      case HdrMode::PassthroughPq: info.fragment_function = s_blitPsHdrPassthroughPq; break;
      case HdrMode::Bt2446:        info.fragment_function = s_blitPsHdrBt2446;        break;
      case HdrMode::Bt2446Pq:      info.fragment_function = s_blitPsHdrBt2446Pq;      break;
      case HdrMode::Bt2446Ui:      info.fragment_function = s_blitPsHdrBt2446Ui;      break;
      case HdrMode::Bt2446UiPq:    info.fragment_function = s_blitPsHdrBt2446UiPq;    break;
      case HdrMode::None:
        info.fragment_function = useGamma ? (pointFilter ? s_blitPsPointGamma : s_blitPsGamma)
                                          : (pointFilter ? s_blitPsPoint : s_blitPs);
        break;
    }
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    info.max_tessellation_factor = 16; // Metal default; 0 trips validation

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newRenderPipelineState(mtlDevice(), &info, &err);
    if (!pso) {
      Logger::err("d9mt: blitter: newRenderPipelineState failed");
      logNSError("d9mt: blitter PSO", err);
      return 0;
    }

    s_blitPsoCache.push_back({ key, pso });
    return pso;
  }


  // Looks up the WMT pixel format backing a Vulkan view format.
  WMTPixelFormat wmtFormatFor(VkFormat format) {
    const FormatCaps* caps = lookupFormatCaps(format);
    return caps ? caps->wmtFormat : WMTPixelFormatInvalid;
  }


  // ==========================================================================
  // HDR present state.
  //
  // Ported from mtld3d v0.6.0 (unix/unix/src/metal/{present,command,macdrv}.rs),
  // Copyright (c) 2026 Alexander Theissen, zlib licence. ALTERED SOURCE: the
  // capability gate, the 32-present refresh cadence and the uniform derivation
  // are re-expressed in C++ against winemetal/d9mtmetal instead of mtld3d's
  // own unix half. See THIRD_PARTY_NOTICES.md.
  //
  // Design: docs/superpowers/specs/2026-08-17-hdr-bt2446-design.md
  // ==========================================================================

  namespace {

    enum class HdrEnv : uint32_t { Off = 0, On = 1, Auto = 2, Force = 3 };
    enum class HdrSpace : uint32_t { Display = 0, ScRgb = 1, Pq = 2 };

    std::atomic<bool>     s_hdrActive     = { false };
    std::atomic<float>    s_hdrPeak       = { 1.0f };
    std::atomic<float>    s_hdrLoggedPeak = { 1.0f }; // drift-log baseline
    std::atomic<bool>     s_hdrLatched    = { false }; // gate evaluated
    std::atomic<uint32_t> s_hdrPresents   = { 0u };    // refresh cadence counter
    HdrSpace              s_hdrSpace      = HdrSpace::Display;

    // Case-insensitive, whitespace-trimmed env read. Without this a stray
    // "TRUE" or a trailing space silently means "off", which is the worst
    // possible failure mode for a feature whose absence looks like success.
    std::string envLower(const char* name) {
      const char* v = std::getenv(name);
      if (!v)
        return std::string();
      std::string s(v);
      size_t b = s.find_first_not_of(" \t\r\n");
      size_t e = s.find_last_not_of(" \t\r\n");
      s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    HdrEnv hdrEnvMode() {
      static const HdrEnv mode = [] {
        std::string v = envLower("D9MT_HDR");
        // Default OFF for this drop: the user A/Bs by relaunching, and a
        // regression cannot cost them a raid night. Flip to Auto once in-game
        // parity against mtld3d is signed off.
        if (v.empty())
          return HdrEnv::Off;
        if (v == "1" || v == "on" || v == "true" || v == "yes")  return HdrEnv::On;
        if (v == "auto")                                        return HdrEnv::Auto;
        // force: latch the gate active without EDR headroom — validation /
        // debugging only (on an SDR panel the compositor clips everything
        // above 1.0; pair with D9MT_HDR_PEAK to pin the curve).
        if (v == "force")                                       return HdrEnv::Force;
        if (v == "0" || v == "off" || v == "false" || v == "no") return HdrEnv::Off;
        Logger::warn(str::format("d9mt: hdr: D9MT_HDR='", v,
          "' not recognised (want 0/1/auto/force) - treating as off"));
        return HdrEnv::Off;
      }();
      return mode;
    }

    HdrSpace hdrEnvSpace() {
      static const HdrSpace space = [] {
        // Display = the panel's own profile, extended-linearized. mtld3d's
        // `color.space = passthrough` default, i.e. what the approved look was
        // tested against.
        std::string v = envLower("D9MT_HDR_COLORSPACE");
        if (v.empty() || v == "display") return HdrSpace::Display;
        if (v == "scrgb")                return HdrSpace::ScRgb;
        if (v == "pq")                   return HdrSpace::Pq;
        Logger::warn(str::format("d9mt: hdr: D9MT_HDR_COLORSPACE='", v,
          "' not recognised (want display/scrgb/pq) - using display"));
        return HdrSpace::Display;
      }();
      return space;
    }

    // Debug override: pin the curve's peak instead of following the display.
    // Documented and accepted in NITS (the same unit the log lines print), and
    // converted to the headroom multiplier the uniforms want. 0 = follow the
    // display. An out-of-range value warns rather than being dropped in
    // silence.
    float hdrEnvPeakOverride() {
      static const float peak = [] {
        std::string v = envLower("D9MT_HDR_PEAK");
        if (v.empty())
          return 0.0f;
        double nits = std::atof(v.c_str());
        if (nits >= 100.0 && nits <= 10000.0) {
          Logger::info(str::format("d9mt: hdr: D9MT_HDR_PEAK=", nits,
            " nits pinned (headroom ", nits / 100.0, "x)"));
          return float(nits / 100.0);
        }
        Logger::warn(str::format("d9mt: hdr: D9MT_HDR_PEAK='", v,
          "' out of range (want 100..10000 nits) - following the display"));
        return 0.0f;
      }();
      return peak;
    }

    // D9MT_HDR_UI: mix the UI's dead-alpha coverage tag into present so text
    // and interface stay SDR-anchored while the world gets the full curve.
    // Default ON — the flat curve launches white UI text to the display peak
    // while its antialiasing midtones stay near SDR, which reads as blurry,
    // unantialiased text (the field report this ships for). 0 restores strict
    // mtld3d parity.
    bool hdrUiEnvEnabled() {
      static const bool enabled = [] {
        std::string v = envLower("D9MT_HDR_UI");
        if (v.empty())
          return true;
        return !(v == "0" || v == "off" || v == "false" || v == "no");
      }();
      return enabled;
    }

    // D9MT_HDR_UI_NITS: the UI branch's white anchor. 200 keeps interface
    // white at 2x SDR reference — bright enough to not look grey next to EDR
    // world highlights, dim enough to keep the AA ramp legible.
    float hdrUiEnvNits() {
      static const float nits = [] {
        std::string v = envLower("D9MT_HDR_UI_NITS");
        if (v.empty())
          return 200.0f;
        double n = std::atof(v.c_str());
        if (n >= 80.0 && n <= 1000.0)
          return float(n);
        Logger::warn(str::format("d9mt: hdr: D9MT_HDR_UI_NITS='", v,
          "' out of range (want 80..1000 nits) - using 200"));
        return 200.0f;
      }();
      return nits;
    }

    // True once a d9mtmetal.so new enough to carry the HDR entry points has
    // answered the ABI handshake. See d9mt_newlibrary_params::abi_func_count:
    // wine's unix dispatch has NO bounds check, so this is the only safe way
    // to ask "does the other half have function N".
    bool d9mtmetalHas(uint32_t func) {
      if (s_d9mtmetalFuncCount.load(std::memory_order_relaxed) > func)
        return true;
      // The handshake rides on the blit-library build; force it if the
      // blitter has not been touched yet (the gate runs before the first blit).
      std::lock_guard<std::mutex> lock(s_blitMutex);
      ensureBlitFunctionsLocked();
      return s_d9mtmetalFuncCount.load(std::memory_order_relaxed) > func;
    }

    // Tag the layer and switch on extendedDynamicRange.
    //
    // Display: the panel's own profile, extended-linearized (mtld3d's
    //   `passthrough` default, and what the approved look was tested with).
    //   Needs d9mtmetal -- winemetal's setColorSpace is a closed switch over
    //   four fixed CGColorSpace names with no route to the display's own.
    // ScRgb:   kCGColorSpaceExtendedLinearSRGB via winemetal. Always available.
    // Pq:      kCGColorSpaceITUR_2100_PQ via winemetal. Already implemented in
    //   the pinned v0.80 -- PQ output needs no unixlib work, only the optional
    //   mastering metadata does.
    void applyHdrColorSpaceLocked(obj_handle_t layer) {
      if (s_hdrSpace == HdrSpace::Display
       && d9mtmetalHas(D9MT_FUNC_LAYER_COLORSPACE)) {
        d9mt_layer_colorspace_params p = { };
        p.layer           = uint64_t(layer);
        p.colorspace_kind = D9MT_COLORSPACE_DISPLAY_EXTENDED_LINEAR;
        p.wants_edr       = 1u;
        if (D9MT_UnixCall(D9MT_FUNC_LAYER_COLORSPACE, &p) == 0 && p.ret_ok)
          return;
        Logger::warn("d9mt: hdr: display-native colorspace unavailable — "
                     "falling back to extended-linear sRGB");
      }

      WMTColorSpace cs = s_hdrSpace == HdrSpace::Pq ? WMTColorSpaceHDR_PQ
                                                    : WMTColorSpaceHDR_scRGB;
      if (!CGColorSpace_checkColorSpaceSupported(cs)) {
        Logger::warn("d9mt: hdr: colorspace unsupported by CoreGraphics — "
                     "layer left untagged");
        return;
      }
      // NB: HDR_scRGB, not SRGBLinear. Both map to kCGColorSpaceExtendedLinear
      // SRGB, but only the HDR_ enum sets wantsExtendedDynamicRangeContent
      // (WMT_COLORSPACE_IS_HDR tests bit 2) — picking the wrong one gives a
      // correct-looking tag with the headroom permanently pinned at 1.0.
      if (!MetalLayer_setColorSpace(layer, cs))
        Logger::warn("d9mt: hdr: setColorSpace failed");
    }

    // Ask d9mtmetal for the layer's EDR headroom. NEVER calls winemetal's
    // MetalLayer_getEDRValue: that walks layer.delegate -> NSView -> NSWindow
    // -> NSScreen on the calling thread, and those are main-thread-only.
    // refresh=true also queues a main-queue re-read for next time.
    bool queryLayerEdr(obj_handle_t layer, uint32_t mode,
                       float* outCur, float* outPotential, bool* outPublished = nullptr) {
      if (!layer || !d9mtmetalHas(D9MT_FUNC_LAYER_EDR))
        return false;

      d9mt_layer_edr_params p = { };
      p.layer   = uint64_t(layer);
      p.refresh = mode;
      if (D9MT_UnixCall(D9MT_FUNC_LAYER_EDR, &p) != 0)
        return false;

      if (outCur)       *outCur = p.ret_max_edr;
      if (outPotential) *outPotential = p.ret_max_potential;
      if (outPublished) *outPublished = p.ret_published != 0u;
      return true;
    }

  } // anonymous namespace


  // Called once per present. Cheap: two relaxed atomic loads plus, every 32nd
  // present, one unixcall that only reads published statics and queues a
  // main-queue block. mtld3d uses the same 32-present cadence.
  void refreshHdrHeadroom(obj_handle_t layer) {
    if (!s_hdrActive.load(std::memory_order_relaxed)) {
      // SDR-latched retry. The gate can legitimately read a PUBLISHED
      // potential of 1.0 and latch HDR off when the window has not been
      // assigned its real screen yet (fresh explorer start, or the window
      // later moves to an EDR display) — the published guard cannot tell
      // "no headroom" from "wrong screen". So while the user asked for HDR,
      // keep probing on the same 32-present cadence as the active path and
      // unlatch the gate the moment the panel shows potential; the next
      // acquire re-runs the gate against the now-correct screen. Default-off
      // users pay nothing (env check first).
      if (hdrEnvMode() == HdrEnv::Off
       || !s_hdrLatched.load(std::memory_order_relaxed))
        return;
      uint32_t n = s_hdrPresents.fetch_add(1u, std::memory_order_relaxed);
      if ((n % 32u) != 0u)
        return;
      float cur = 1.0f, potential = 1.0f;
      bool published = false;
      if (!queryLayerEdr(layer, D9MT_EDR_REFRESH_ASYNC, &cur, &potential, &published))
        return;
      if (published && potential > 1.0f) {
        Logger::info(str::format("d9mt: hdr: potential headroom ", potential,
          "x appeared after an SDR latch - re-running the gate"));
        s_hdrLatched.store(false, std::memory_order_relaxed);
      }
      return;
    }

    // Only cross into the unixlib on a due present. The call itself is cheap
    // (it reads published statics) but this driver is CPU-bound and a PE->unix
    // crossing under Rosetta is ~1.5us, so paying it 32x more often than the
    // design says is a real, if small, waste.
    uint32_t n = s_hdrPresents.fetch_add(1u, std::memory_order_relaxed);
    if ((n % 32u) != 0u)   // n starts at 0, so the first present is due
      return;

    float cur = 1.0f;
    if (!queryLayerEdr(layer, D9MT_EDR_REFRESH_ASYNC, &cur, nullptr))
      return;

    s_hdrPeak.store(cur, std::memory_order_relaxed);

    // Log only past 5% drift from the last LOGGED value, not the last seen
    // one: comparing against the previous sample lets a slow ramp creep
    // arbitrarily far without ever tripping the threshold.
    float logged = s_hdrLoggedPeak.load(std::memory_order_relaxed);
    if (std::fabs(cur - logged) > 0.05f * std::max(logged, 1.0f)) {
      s_hdrLoggedPeak.store(cur, std::memory_order_relaxed);
      Logger::info(str::format("d9mt: hdr: headroom ", logged, "x -> ", cur,
        "x (L_hdr=", cur * 100.0f, " nits)"));
    }
  }


  // Gate on maximum_POTENTIAL headroom: it is the static panel ceiling and is
  // readable before anything is configured. The DYNAMIC value reads 1.0 until
  // macOS actually promotes the screen, so gating on that would mean HDR never
  // engages at all. Latched once and never re-evaluated, Reset included.
  bool hdrEvaluateGate(obj_handle_t layer) {
    // Do NOT latch without a layer: the latch is permanent, so an early call
    // with no layer yet would pin HDR off for the whole process.
    if (!layer)
      return s_hdrActive.load(std::memory_order_relaxed);

    if (hdrEnvMode() == HdrEnv::Off) {
      // Resolve + log once even when disabled, so a typo in D9MT_HDR is visible
      // rather than silently meaning "off".
      if (!s_hdrLatched.exchange(true))
        Logger::info("d9mt: hdr: disabled (D9MT_HDR)");
      return false;
    }

    // Read SYNCHRONOUSLY. This is the one-shot gate: it latches on this single
    // answer, so an async refresh cannot have landed yet -- we would read the
    // 1.0 seed, conclude "no headroom" and pin HDR off on EVERY machine no
    // matter how capable the panel.
    float cur = 1.0f, potential = 1.0f;
    bool published = false;
    bool haveEdr = queryLayerEdr(layer, D9MT_EDR_REFRESH_SYNC,
                                 &cur, &potential, &published);

    // Never latch off an unpublished snapshot: potential == 1.0 would then mean
    // "nobody has looked yet", not "this display has no headroom". Leave the
    // latch clear so the next acquire retries.
    if (haveEdr && !published) {
      Logger::warn("d9mt: hdr: EDR headroom not published yet - deferring the gate");
      return false;
    }

    if (!s_hdrLatched.exchange(true)) {
      // Set before s_hdrActive is published: readers only consult s_hdrSpace
      // once s_hdrActive is true, so this ordering is safe by construction.
      s_hdrSpace = hdrEnvSpace();

      bool active = haveEdr && std::isfinite(potential) && potential > 1.0f;

      if (hdrEnvMode() == HdrEnv::Force && haveEdr && !active) {
        Logger::warn("d9mt: hdr: FORCED active without EDR headroom "
                     "(D9MT_HDR=force) - expect clipped highlights on SDR panels");
        active = true;
      }

      if (!haveEdr) {
        Logger::warn("d9mt: hdr: requested, but this d9mtmetal has no main-queue "
                     "EDR entry point (stale unixlib) - running SDR");
      } else if (!active) {
        Logger::info(str::format("d9mt: hdr: no EDR headroom on this display "
          "(potential=", potential, "x) - running SDR"));
      } else if (active) {
        Logger::info(str::format("d9mt: hdr: active, potential headroom ", potential,
          "x, colorspace=", s_hdrSpace == HdrSpace::Display ? "display-extended-linear"
                          : s_hdrSpace == HdrSpace::Pq      ? "pq"
                                                            : "scrgb"));
      }
      // Seed the live peak from the same synchronous read so the first present
      // already picks the right variant instead of spending frames on
      // passthrough.
      s_hdrPeak.store(cur, std::memory_order_relaxed);
      s_hdrLoggedPeak.store(cur, std::memory_order_relaxed);
      s_hdrActive.store(active, std::memory_order_relaxed);
    }
    return s_hdrActive.load(std::memory_order_relaxed);
  }

  void hdrApplyColorSpace(obj_handle_t layer) { applyHdrColorSpaceLocked(layer); }

  bool hdrLayerActive() { return s_hdrActive.load(std::memory_order_relaxed); }

  // True once the gate has reached a verdict. While false, recreateSwapChain
  // must not take its unchanged-swapchain early-return, or a deferred gate
  // would never get a second attempt.
  bool hdrGateLatched() { return s_hdrLatched.load(std::memory_order_relaxed); }

  bool hdrWantsPq() { return s_hdrSpace == HdrSpace::Pq; }

  float hdrPeak() {
    float over = hdrEnvPeakOverride();
    return over > 0.0f ? over : s_hdrPeak.load(std::memory_order_relaxed);
  }

  HdrParams hdrParamsForPeak(float peak) {
    HdrParams u = { };
    u.lHdrNits     = peak * 100.0f;
    u.pHdr         = 1.0f + 32.0f * std::pow(u.lHdrNits / 10000.0f, 1.0f / 2.4f);
    u.log2PHdr     = std::log2(u.pHdr);
    u.invPMinusOne = 1.0f / (u.pHdr - 1.0f);
    u.uiNits       = hdrUiEnvNits();
    return u;
  }

  bool hdrUiTagActive() {
    return hdrUiEnvEnabled() && hdrLayerActive();
  }

  float hdrUiNits() { return hdrUiEnvNits(); }

  namespace {
    // Ring of backbuffer image handles present has tone-mapped in UI mode.
    // Lock-free: written only from the CS thread (present), read from the
    // CS thread (draw path) — the atomics are for hygiene, not contention.
    constexpr size_t UiTagRingSize = 4;
    std::atomic<uint64_t> s_uiTagImages[UiTagRingSize] = { };
    std::atomic<uint32_t> s_uiTagNext = { 0u };
  }

  void noteUiTagBackbuffer(obj_handle_t image) {
    for (size_t i = 0; i < UiTagRingSize; i++) {
      if (s_uiTagImages[i].load(std::memory_order_relaxed) == image)
        return;
    }
    uint32_t slot = s_uiTagNext.fetch_add(1u, std::memory_order_relaxed) % UiTagRingSize;
    s_uiTagImages[slot].store(image, std::memory_order_relaxed);
  }

  bool isUiTagBackbuffer(obj_handle_t image) {
    for (size_t i = 0; i < UiTagRingSize; i++) {
      if (s_uiTagImages[i].load(std::memory_order_relaxed) == image)
        return true;
    }
    return false;
  }


  // ==========================================================================
  // depth(+stencil) SAMPLE_ZERO resolve pipeline (DxvkContext::resolveImage):
  // fullscreen triangle reading sample 0 of the MSAA source and exporting
  // [[depth(any)]] (+ [[stencil]], shader stencil export — supported on all
  // Apple-silicon Metal GPUs) into the 1x destination's depth/stencil
  // attachments. Kept in its OWN MTLLibrary so a hypothetical stencil-export
  // compile failure cannot take down the color blit pipeline above.
  // ==========================================================================

  namespace {

    const char g_depthResolveMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

vertex float4 d9mt_resolve_vs(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

struct d9mt_resolve_d_out {
  float depth [[depth(any)]];
};

fragment d9mt_resolve_d_out d9mt_resolve_d_ps(
    float4 pos [[position]],
    depth2d_ms<float> src [[texture(0)]]) {
  d9mt_resolve_d_out o;
  o.depth = src.read(uint2(pos.xy), 0);
  return o;
}

struct d9mt_resolve_ds_out {
  float depth [[depth(any)]];
  uint stencil [[stencil]];
};

fragment d9mt_resolve_ds_out d9mt_resolve_ds_ps(
    float4 pos [[position]],
    depth2d_ms<float> src [[texture(0)]],
    texture2d_ms<uint> stc [[texture(1)]]) {
  d9mt_resolve_ds_out o;
  o.depth   = src.read(uint2(pos.xy), 0);
  o.stencil = stc.read(uint2(pos.xy), 0).x;
  return o;
}
)";

    bool         s_resolveInitFailed = false;
    obj_handle_t s_resolveLibrary = 0;
    obj_handle_t s_resolveVs = 0;
    obj_handle_t s_resolveDPs = 0;
    obj_handle_t s_resolveDsPs = 0;
    obj_handle_t s_resolveDPso = 0;
    obj_handle_t s_resolveDsPso = 0;

    bool ensureResolveFunctionsLocked() {
      if (s_resolveVs)
        return true;
      if (s_resolveInitFailed)
        return false;

      obj_handle_t device = mtlDevice();
      if (!device) {
        s_resolveInitFailed = true;
        return false;
      }

      d9mt_newlibrary_params lp = { };
      lp.device     = device;
      lp.source_ptr = uint64_t(uintptr_t(g_depthResolveMsl));
      lp.source_len = sizeof(g_depthResolveMsl) - 1u;
      lp.fast_math  = 1u;

      int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
      if (st != 0 || !lp.ret_library) {
        Logger::err("d9mt: depth resolve: newLibraryWithSource failed");
        logf("d9mt: depth resolve: newLibraryWithSource status %d", st);
        if (lp.ret_error)
          logNSError("d9mt: depth resolve MSL compile", lp.ret_error);
        s_resolveInitFailed = true;
        return false;
      }
      if (lp.ret_error)
        NSObject_release(lp.ret_error); // compile warnings only

      s_resolveLibrary = lp.ret_library;
      s_resolveVs   = MTLLibrary_newFunction(s_resolveLibrary, "d9mt_resolve_vs");
      s_resolveDPs  = MTLLibrary_newFunction(s_resolveLibrary, "d9mt_resolve_d_ps");
      s_resolveDsPs = MTLLibrary_newFunction(s_resolveLibrary, "d9mt_resolve_ds_ps");

      if (!s_resolveVs || !s_resolveDPs || !s_resolveDsPs) {
        Logger::err("d9mt: depth resolve: functions missing from compiled library");
        s_resolveInitFailed = true;
        return false;
      }
      return true;
    }

  } // anonymous namespace

  // non-static: used by DxvkContext::resolveImage (declared in d9mt_backend.h).
  // All backend depth formats unify on Depth32Float_Stencil8, so there is
  // exactly one PSO per (with/without stencil export) variant.
  obj_handle_t getDepthResolvePso(bool withStencil) {
    std::lock_guard<std::mutex> lock(s_blitMutex);

    obj_handle_t& cached = withStencil ? s_resolveDsPso : s_resolveDPso;
    if (cached)
      return cached;

    if (!ensureResolveFunctionsLocked())
      return 0;

    WMTRenderPipelineInfo info = { };
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    info.depth_pixel_format   = WMTPixelFormatDepth32Float_Stencil8;
    info.stencil_pixel_format = WMTPixelFormatDepth32Float_Stencil8;
    info.vertex_function = s_resolveVs;
    info.fragment_function = withStencil ? s_resolveDsPs : s_resolveDPs;
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    info.max_tessellation_factor = 16; // Metal default; 0 trips validation

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newRenderPipelineState(mtlDevice(), &info, &err);
    if (!pso) {
      Logger::err("d9mt: depth resolve: newRenderPipelineState failed");
      logNSError("d9mt: depth resolve PSO", err);
      return 0;
    }

    cached = pso;
    return pso;
  }


  // ==========================================================================
  // partial depth/stencil clear pipeline (DxvkContext::clearImageView):
  // fullscreen triangle at z = 0 with a void fragment function. The clear
  // depth value is encoded as viewport znear == zfar (depth written =
  // znear + z_ndc * (zfar - znear) = znear), the stencil clear value as the
  // DSSO stencil reference with op Replace; the scissor rect restricts the
  // clear to the requested region. Own MTLLibrary: a compile failure here
  // must not take down the blit / resolve pipelines above.
  // ==========================================================================

  namespace {

    const char g_dsClearMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

vertex float4 d9mt_dsclear_vs(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

fragment void d9mt_dsclear_fs() {}
)";

    bool         s_dsClearInitFailed = false;
    obj_handle_t s_dsClearLibrary = 0;
    obj_handle_t s_dsClearVs = 0;
    obj_handle_t s_dsClearFs = 0;
    std::vector<std::pair<uint32_t, obj_handle_t>> s_dsClearPsoCache;

    bool ensureDsClearFunctionsLocked() {
      if (s_dsClearVs)
        return true;
      if (s_dsClearInitFailed)
        return false;

      obj_handle_t device = mtlDevice();
      if (!device) {
        s_dsClearInitFailed = true;
        return false;
      }

      d9mt_newlibrary_params lp = { };
      lp.device     = device;
      lp.source_ptr = uint64_t(uintptr_t(g_dsClearMsl));
      lp.source_len = sizeof(g_dsClearMsl) - 1u;
      lp.fast_math  = 1u;

      int st = D9MT_UnixCall(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &lp);
      if (st != 0 || !lp.ret_library) {
        Logger::err("d9mt: ds clear: newLibraryWithSource failed");
        logf("d9mt: ds clear: newLibraryWithSource status %d", st);
        if (lp.ret_error)
          logNSError("d9mt: ds clear MSL compile", lp.ret_error);
        s_dsClearInitFailed = true;
        return false;
      }
      if (lp.ret_error)
        NSObject_release(lp.ret_error); // compile warnings only

      s_dsClearLibrary = lp.ret_library;
      s_dsClearVs = MTLLibrary_newFunction(s_dsClearLibrary, "d9mt_dsclear_vs");
      s_dsClearFs = MTLLibrary_newFunction(s_dsClearLibrary, "d9mt_dsclear_fs");

      if (!s_dsClearVs || !s_dsClearFs) {
        Logger::err("d9mt: ds clear: functions missing from compiled library");
        s_dsClearInitFailed = true;
        return false;
      }
      return true;
    }

  } // anonymous namespace

  // non-static: used by DxvkContext::clearImageView (declared in
  // d9mt_backend.h). All backend depth formats unify on
  // Depth32Float_Stencil8, so the PSO is keyed by sample count alone.
  obj_handle_t getDepthStencilClearPso(uint32_t sampleCount) {
    std::lock_guard<std::mutex> lock(s_blitMutex);

    for (const auto& e : s_dsClearPsoCache) {
      if (e.first == sampleCount)
        return e.second;
    }

    if (!ensureDsClearFunctionsLocked())
      return 0;

    WMTRenderPipelineInfo info = { };
    info.rasterization_enabled = true;
    info.raster_sample_count = uint8_t(sampleCount ? sampleCount : 1u);
    info.depth_pixel_format   = WMTPixelFormatDepth32Float_Stencil8;
    info.stencil_pixel_format = WMTPixelFormatDepth32Float_Stencil8;
    info.vertex_function = s_dsClearVs;
    info.fragment_function = s_dsClearFs;
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    info.max_tessellation_factor = 16; // Metal default; 0 trips validation

    obj_handle_t err = 0;
    obj_handle_t pso = MTLDevice_newRenderPipelineState(mtlDevice(), &info, &err);
    if (!pso) {
      Logger::err("d9mt: ds clear: newRenderPipelineState failed");
      logNSError("d9mt: ds clear PSO", err);
      return 0;
    }

    s_dsClearPsoCache.push_back({ sampleCount, pso });
    return pso;
  }


  // ==========================================================================
  // Blitter side state (gamma/cursor metadata + log-once flags). The
  // vendored members are Vulkan-resource-shaped; we only need a few bools.
  // Guarded by the blitter's own vendored m_mutex (per BACKEND-SURFACE §5.2).
  // ==========================================================================

  struct BlitterState {
    bool cursorTextureSet = false;
    bool warnedGamma  = false;
    bool warnedCursor = false;
    bool warnedMsaa   = false;
    bool warnedHdrGamma    = false;
    bool warnedHdrSrgbView = false;
    std::vector<DxvkGammaCp> gammaRamp;
    // Cached 1-sample target for resolving a multisampled present source (a game
    // that requests an MSAA swapchain). Created lazily, reused every frame,
    // recreated on a size/format change. The downstream sample path scales it to dst.
    obj_handle_t msaaResolveTex = 0;
    uint32_t msaaResolveW = 0, msaaResolveH = 0, msaaResolveFmt = 0;
  };

  namespace {
    std::mutex s_blitterMutex;
    std::unordered_map<const void*, std::unique_ptr<BlitterState>> s_blitterStates;

    BlitterState& blitterState(const void* blitter) {
      std::lock_guard<std::mutex> lock(s_blitterMutex);
      auto& slot = s_blitterStates[blitter];
      if (!slot)
        slot = std::make_unique<BlitterState>();
      return *slot;
    }

    void eraseBlitterState(const void* blitter) {
      std::lock_guard<std::mutex> lock(s_blitterMutex);
      s_blitterStates.erase(blitter);
    }
  }

} // namespace dxvk::d9mt


namespace dxvk {

  // ==========================================================================
  // Presenter
  // ==========================================================================

  Presenter::Presenter(
    const Rc<DxvkDevice>&   device,
    const Rc<sync::Signal>& signal,
    const PresenterDesc&    desc,
          PresenterSurfaceProc&& proc)
  : m_device(device), m_signal(signal), m_surfaceProc(std::move(proc)) {
    auto& state = d9mt::presenterState(this);

    if (!desc.deferSurfaceCreation) {
      std::lock_guard<std::mutex> lock(state.mutex);
      VkResult vr = createSurface();
      if (vr != VK_SUCCESS)
        Logger::err(str::format("d9mt: Presenter: deferred surface creation after error ", vr));
    }

    d9mt::logf("Presenter: created (deferSurfaceCreation=%d)",
      desc.deferSurfaceCreation ? 1 : 0);
  }


  Presenter::~Presenter() {
    destroyResources();
    d9mt::erasePresenterState(this);
    d9mt::logf("Presenter: destroyed");
  }


  VkResult Presenter::acquireNextImage(
          PresenterSync&  sync,
          Rc<DxvkImage>&  image) {
    sync = PresenterSync();

    auto& state = d9mt::presenterState(this);
    std::lock_guard<std::mutex> lock(state.mutex);

    // Recreate the surface if it was invalidated (multi-surface hack)
    if (m_dirtySurface) {
      destroySwapchain();
      destroySurface();
      m_dirtySurface = false;
    }

    if (!state.layer) {
      VkResult vr = createSurface();
      if (vr != VK_SUCCESS)
        return vr;
    }

    VkResult vr = recreateSwapChain();
    if (vr != VK_SUCCESS)
      return vr;

    image = state.proxy;
    m_acquireStatus = VK_SUCCESS;
    return VK_SUCCESS;
  }


  VkResult Presenter::presentImage(
          uint64_t                frameId,
    const Rc<DxvkLatencyTracker>& tracker) {
    auto& state = d9mt::presenterState(this);

    VkResult status = VK_SUCCESS;
    bool signalQueued = false;

    obj_handle_t pool = NSAutoreleasePool_alloc_init();
    {
      std::lock_guard<std::mutex> lock(state.mutex);

      obj_handle_t queue = d9mt::mtlCommandQueue();

      d9mt::refreshHdrHeadroom(state.layer);

      if (!state.layer || state.proxy == nullptr || !queue) {
        status = VK_ERROR_OUT_OF_DATE_KHR;
      } else {
        obj_handle_t drawable = MetalLayer_nextDrawable(state.layer);

        if (!drawable) {
          d9mt::logf("Presenter: nextDrawable returned null");
          status = VK_ERROR_OUT_OF_DATE_KHR;
        } else {
          obj_handle_t drawTex = MetalDrawable_texture(drawable);
          obj_handle_t cmdbuf  = MTLCommandQueue_commandBuffer(queue);

          if (!drawTex || !cmdbuf) {
            d9mt::logf("Presenter: drawable texture / command buffer unavailable");
            status = VK_ERROR_DEVICE_LOST;
          } else {
            // proxy -> drawable; the layer's drawable size always matches
            // the proxy extent (both set under this lock in recreateSwapChain)
            wmtcmd_blit_copy_from_texture_to_texture cp = { };
            cp.type = WMTBlitCommandCopyFromTextureToTexture;
            cp.src = obj_handle_t(state.proxy->handle());
            cp.src_size = { state.proxyExtent.width, state.proxyExtent.height, 1u };
            cp.dst = drawTex;

            obj_handle_t benc = MTLCommandBuffer_blitCommandEncoder(cmdbuf);
            if (benc) {
              MTLBlitCommandEncoder_encodeCommands(benc,
                reinterpret_cast<const wmtcmd_base*>(&cp));
              MTLCommandEncoder_endEncoding(benc);
            }

            MTLCommandBuffer_presentDrawable(cmdbuf, drawable);
            MTLCommandBuffer_commit(cmdbuf);

            // Custom HUD: once per displayed frame, push our D3D9-internal
            // counters onto Apple's Metal Performance HUD. No-op unless -DD9MT_HUD.
            D9MT_HUD_FRAME();

            // One-shot frame capture (no-op unless D9MT_CAPTURE=1 + trigger file).
            d9mt::captureTick(queue);

            // Frame signal fires when the present command buffer retires.
            // Keep presenter + proxy alive until then.
            Rc<Presenter>  self  = this;
            Rc<DxvkImage>  proxy = state.proxy;

            d9mt::watchCommandBuffer(cmdbuf, [self, proxy, frameId, tracker] {
              self->signalFrame(frameId, tracker);
            });

            signalQueued = true;
          }
        }
      }
    }
    NSObject_release(pool);

    if (!signalQueued) {
      // LIVENESS: failed presents still signal, ordered behind all
      // previously submitted GPU work (watcher pure-callback path)
      Rc<Presenter> self = this;
      d9mt::watchCommandBuffer(0, [self, frameId, tracker] {
        self->signalFrame(frameId, tracker);
      });
    }

    return status;
  }


  void Presenter::signalFrame(
          uint64_t                frameId,
    const Rc<DxvkLatencyTracker>& tracker) {
    // Runs on the watcher thread, after the present command buffer (and all
    // prior GPU work) retired — satisfies the "GPU work has completed"
    // precondition. Presents retire in submission order, so frameId stays
    // monotonic per presenter.
    m_fpsLimiter.delay();

    if (tracker != nullptr)
      tracker->notifyGpuPresentEnd(frameId);

    m_signal->signal(frameId);
    m_lastSignaled = frameId;
  }


  void Presenter::setSyncInterval(uint32_t syncInterval) {
    // CAMetalLayer only knows vsync on/off; clamp like upstream FIFO/IMMEDIATE
    if (syncInterval > 1u)
      syncInterval = 1u;

    if (syncInterval != m_preferredSyncInterval) {
      m_preferredSyncInterval = syncInterval;
      m_dirtySwapchain = true;
    }
  }


  void Presenter::setFrameRateLimit(double frameRate, uint32_t maxLatency) {
    m_fpsLimiter.setTargetFrameRate(frameRate, maxLatency);
  }


  void Presenter::setSurfaceFormat(VkSurfaceFormatKHR format) {
    // The proxy is always B8G8R8A8_UNORM (BGRA8 layer); other formats are
    // converted by the blitter's sample pass. Warn once about precision
    // loss for wide formats.
    if (format.format != m_preferredFormat.format) {
      if (format.format != VK_FORMAT_UNDEFINED
       && format.format != VK_FORMAT_B8G8R8A8_UNORM
       && format.format != VK_FORMAT_R8G8B8A8_UNORM
       && format.format != VK_FORMAT_B8G8R8A8_SRGB
       && format.format != VK_FORMAT_R8G8B8A8_SRGB) {
        Logger::warn(str::format("d9mt: Presenter: surface format ", format.format,
          " presented through a B8G8R8A8 proxy (precision loss possible)"));
      }
    }

    m_preferredFormat = format;
  }


  void Presenter::setSurfaceExtent(VkExtent2D extent) {
    if (extent.width  != m_preferredExtent.width
     || extent.height != m_preferredExtent.height) {
      m_preferredExtent = extent;
      m_dirtySwapchain = true;
    }
  }


  void Presenter::setHdrMetadata(VkHdrMetadataEXT hdrMetadata) {
    // HDR color spaces are not supported (supportsColorSpace), so the
    // metadata can never become active; store it for completeness
    m_hdrMetadata = hdrMetadata;
    m_hdrMetadataDirty = false;
  }


  bool Presenter::supportsColorSpace(VkColorSpaceKHR colorspace) {
    if (colorspace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return true;
    // Only advertise the HDR space once the gate has actually latched it on;
    // otherwise an app querying early would be told yes on an SDR panel.
    return colorspace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
        && d9mt::hdrLayerActive();
  }


  void Presenter::invalidateSurface() {
    // applied on the next acquireNextImage (app thread, like this call)
    m_dirtySurface = true;
  }


  void Presenter::destroyResources() {
    // block until pending swapchain operations (presents, proxy blits)
    // have completed, then tear down the Metal window objects
    d9mt::watcherWaitIdle();

    auto& state = d9mt::presenterState(this);
    std::lock_guard<std::mutex> lock(state.mutex);

    destroySwapchain();
    destroySurface();
  }


  // --------------------------------------------------------------------------
  // private helpers — all assume the side-state mutex is held by the caller
  // --------------------------------------------------------------------------

  VkResult Presenter::createSurface() {
    auto& state = d9mt::presenterState(this);

    if (state.layer)
      return VK_SUCCESS;

    if (!m_surface) {
      // The proc routes through wsi::createSurface over our fake instance
      // dispatch; the resulting VkSurfaceKHR value IS the HWND.
      VkResult vr = m_surfaceProc(&m_surface);

      if (vr != VK_SUCCESS || !m_surface) {
        Logger::err(str::format("d9mt: Presenter: surface proc failed: ", vr));
        m_surface = VK_NULL_HANDLE;
        return vr != VK_SUCCESS ? vr : VK_ERROR_SURFACE_LOST_KHR;
      }
    }

    state.hwnd = reinterpret_cast<HWND>(uintptr_t(m_surface));

    obj_handle_t device = d9mt::mtlDevice();
    if (!device)
      return VK_ERROR_SURFACE_LOST_KHR;

    obj_handle_t layer = 0;
    obj_handle_t view = CreateMetalViewFromHWND(
      intptr_t(state.hwnd), device, &layer);

    if (!view || !layer) {
      Logger::err("d9mt: Presenter: CreateMetalViewFromHWND failed");
      d9mt::logf("Presenter: CreateMetalViewFromHWND failed (hwnd=%p)",
        reinterpret_cast<void*>(state.hwnd));
      if (view)
        ReleaseMetalView(view);
      return VK_ERROR_SURFACE_LOST_KHR;
    }

    state.view  = view;
    state.layer = layer;

    // swapchain (proxy + layer drawable size) is created on first acquire
    m_dirtySwapchain = true;

    d9mt::logf("Presenter: surface created (hwnd=%p layer=%llx)",
      reinterpret_cast<void*>(state.hwnd), (unsigned long long)layer);
    return VK_SUCCESS;
  }


  void Presenter::destroySurface() {
    auto& state = d9mt::presenterState(this);

    if (state.view)
      ReleaseMetalView(state.view);

    state.view  = 0;
    state.layer = 0;
    state.hwnd  = nullptr;

    m_surface = VK_NULL_HANDLE;
  }


  void Presenter::destroySwapchain() {
    auto& state = d9mt::presenterState(this);

    state.proxy = nullptr;
    state.proxyExtent = { };
  }


  VkResult Presenter::recreateSwapChain() {
    auto& state = d9mt::presenterState(this);

    VkExtent2D extent = m_preferredExtent;
    if (!extent.width || !extent.height)
      wsi::getWindowSize(state.hwnd, &extent.width, &extent.height);

    if (!extent.width || !extent.height)
      return VK_NOT_READY;

    // One-shot HDR capability gate; see d9mt::hdrEvaluateGate. Evaluated BEFORE
    // the unchanged-swapchain early-return below, because the gate can decline
    // to latch (headroom not published yet) and must then get another attempt.
    // If it were behind the early-return, the first call would create the proxy
    // and every later call would bail out here, leaving HDR stuck off until the
    // window happened to resize.
    const bool hdr = d9mt::hdrEvaluateGate(state.layer);

    if (state.proxy != nullptr
     && state.proxyExtent.width  == extent.width
     && state.proxyExtent.height == extent.height
     && !m_dirtySwapchain
     && d9mt::hdrGateLatched())
      return VK_SUCCESS;

    // resize the layer's drawables; framebuffer_only=false is REQUIRED
    // (the drawable is a blit destination, not a render target)
    WMTLayerProps props = { };
    MetalLayer_getProps(state.layer, &props);
    props.device = d9mt::mtlDevice();
    props.contents_scale = 1.0;
    props.drawable_width  = extent.width;
    props.drawable_height = extent.height;
    props.opaque = true;
    props.display_sync_enabled = m_preferredSyncInterval != 0u;
    props.framebuffer_only = false;
    // COUPLED with the proxy format below: presentImage copies proxy ->
    // drawable through the BLIT encoder, which requires identical formats.
    // Changing one without the other is a Metal validation abort.
    props.pixel_format = hdr ? WMTPixelFormatRGBA16Float : WMTPixelFormatBGRA8Unorm;
    MetalLayer_setProps(state.layer, &props);

    // Must follow setProps: the tag applies to the layer we just reformatted.
    if (hdr)
      d9mt::hdrApplyColorSpace(state.layer);

    if (state.proxy == nullptr
     || state.proxyExtent.width  != extent.width
     || state.proxyExtent.height != extent.height) {
      DxvkImageCreateInfo info;
      info.type        = VK_IMAGE_TYPE_2D;
      // COUPLED with props.pixel_format above — same-format blit requirement.
      info.format      = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT
                             : VK_FORMAT_B8G8R8A8_UNORM;
      info.flags       = 0u;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.extent      = { extent.width, extent.height, 1u };
      info.numLayers   = 1u;
      info.mipLevels   = 1u;
      info.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      info.stages      = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                       | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                       | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access      = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                       | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                       | VK_ACCESS_SHADER_READ_BIT
                       | VK_ACCESS_TRANSFER_READ_BIT
                       | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.tiling      = VK_IMAGE_TILING_OPTIMAL;
      info.layout      = VK_IMAGE_LAYOUT_GENERAL;
      info.colorSpace  = hdr ? VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
                             : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      info.debugName   = "d9mt presenter proxy";

      try {
        state.proxy = m_device->createImage(info,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      } catch (const DxvkError& e) {
        Logger::err(str::format("d9mt: Presenter: proxy creation failed: ", e.message()));
        state.proxy = nullptr;
        return VK_NOT_READY;
      }

      state.proxyExtent = extent;

      // Zero-init the fresh proxy so partial first presents (letterboxed
      // dstRect) read defined black borders. Watched (empty callback) so
      // destroyResources' watcherWaitIdle accounts for it.
      obj_handle_t queue = d9mt::mtlCommandQueue();
      obj_handle_t pool  = NSAutoreleasePool_alloc_init();
      obj_handle_t cmdbuf = queue ? MTLCommandQueue_commandBuffer(queue) : 0;

      if (cmdbuf) {
        WMTRenderPassInfo pass = { };
        pass.render_target_width  = extent.width;
        pass.render_target_height = extent.height;
        pass.colors[0].texture = obj_handle_t(state.proxy->handle());
        pass.colors[0].load_action = WMTLoadActionClear;
        pass.colors[0].store_action = WMTStoreActionStore;
        pass.colors[0].clear_color = { 0.0, 0.0, 0.0, 1.0 };

        obj_handle_t enc = MTLCommandBuffer_renderCommandEncoder(cmdbuf, &pass);
        if (enc)
          MTLCommandEncoder_endEncoding(enc);

        MTLCommandBuffer_commit(cmdbuf);
        d9mt::watchCommandBuffer(cmdbuf, [] { });
      }
      NSObject_release(pool);

      d9mt::logf("Presenter: proxy %ux%u created (vsync=%d)",
        extent.width, extent.height, m_preferredSyncInterval != 0u ? 1 : 0);
    }

    m_dirtySwapchain = false;
    return VK_SUCCESS;
  }


  // ==========================================================================
  // DxvkSwapchainBlitter
  // ==========================================================================

  DxvkSwapchainBlitter::DxvkSwapchainBlitter(
    const Rc<DxvkDevice>& device,
    const Rc<hud::Hud>&   hud)
  : m_device(device), m_hud(hud) {
    d9mt::blitterState(this);
  }


  DxvkSwapchainBlitter::~DxvkSwapchainBlitter() {
    // command lists track every image the blitter touched; the global blit
    // PSO cache outlives all blitters by design — nothing to release here
    d9mt::eraseBlitterState(this);
  }


  void DxvkSwapchainBlitter::present(
    const Rc<DxvkCommandList>&ctx,
    const Rc<DxvkImageView>&  dstView,
          VkRect2D            dstRect,
    const Rc<DxvkImageView>&  srcView,
          VkRect2D            srcRect) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    auto& bs = d9mt::blitterState(this);

    if (srcView == nullptr || dstView == nullptr) {
      Logger::err("d9mt: blitter: null view");
      return;
    }

    obj_handle_t srcHandle = obj_handle_t(srcView->handle());
    obj_handle_t dstHandle = obj_handle_t(dstView->handle());

    if (!srcHandle || !dstHandle) {
      Logger::err("d9mt: blitter: view has no Metal texture");
      return;
    }

    bool useGamma = (m_gammaCpCount != 0 && !bs.gammaRamp.empty());

    // HDR present variant. Keyed off the DESTINATION format so this can only
    // ever engage on the fp16 proxy the HDR gate created — a stray blit into
    // anything else keeps HdrMode::None.
    //
    // hdrLayerActive() is tested FIRST and short-circuits: it is one relaxed
    // atomic load, whereas wmtFormatFor is a linear scan of the ~55-entry
    // format table. With HDR off this whole block therefore costs one atomic
    // load, which is what "the SDR path is unchanged" has to mean.
    d9mt::HdrMode hdrMode = d9mt::HdrMode::None;
    float hdrPeakValue = 1.0f;

    if (d9mt::hdrLayerActive()
     && d9mt::wmtFormatFor(dstView->info().format) == WMTPixelFormatRGBA16Float) {
      hdrPeakValue = d9mt::hdrPeak();
      // BT.2446-A is NOT identity at L_hdr == L_sdr (it under-corrects by
      // ~28% at mid grey), so below/at unity headroom we must run a plain
      // sRGB decode instead of the curve with a no-op target. macOS reports
      // 1.0 until it actually promotes the screen, so this is the state every
      // session starts in — without the short-circuit the first frames are
      // visibly crushed.
      // The encode must always match the layer's tag, curve or no curve.
      const bool pq = d9mt::hdrWantsPq();
      hdrMode = (hdrPeakValue > 1.0f)
              ? (pq ? d9mt::HdrMode::Bt2446Pq      : d9mt::HdrMode::Bt2446)
              : (pq ? d9mt::HdrMode::PassthroughPq : d9mt::HdrMode::Passthrough);

      // UI-aware curve: engages only when the source is a 1-sample image
      // whose view carries the RGB1 swizzle of an alpha-dead X8 format —
      // exactly the case where the draw path may have written its UI
      // coverage tag into the spare channel. The shader then needs the RAW
      // image (the swizzled sample view pins .a to 1), so rebind the source
      // to the image itself; .rgb reads identically through either. MSAA
      // sources are excluded — the resolve pass below goes through the
      // swizzled view and would average an untagged alpha.
      if (hdrPeakValue > 1.0f && d9mt::hdrUiTagActive()
       && srcView->image()->info().sampleCount == VK_SAMPLE_COUNT_1_BIT) {
        VkComponentMapping m = srcView->info().unpackSwizzle();
        if (m.a == VK_COMPONENT_SWIZZLE_ONE
         && m.r == VK_COMPONENT_SWIZZLE_R
         && m.g == VK_COMPONENT_SWIZZLE_G
         && m.b == VK_COMPONENT_SWIZZLE_B) {
          hdrMode = pq ? d9mt::HdrMode::Bt2446UiPq : d9mt::HdrMode::Bt2446Ui;
          srcHandle = obj_handle_t(srcView->image()->handle());
          // Teach the draw path which image carries the tag — the attachment
          // views it sees are swizzle-less, so it cannot detect RGB1 itself.
          d9mt::noteUiTagBackbuffer(srcHandle);
        }
      }

      // Gamma and HDR are mutually exclusive, HDR wins (DXMT takes the same
      // posture). Applying the ramp first would feed a display-referred LUT
      // into an inverse tone map as if it were scene luminance; applying it
      // after is worse still, because the LUT's clamp(int3(index),0,255) hard
      // clips every EDR highlight back to 1.0 and undoes the whole point of
      // the fp16 layer. Dead code in practice: DXVK only forwards ramps when
      // !Windowed, and the launcher pins gxWindow=1.
      if (useGamma && !bs.warnedHdrGamma) {
        bs.warnedHdrGamma = true;
        Logger::warn("d9mt: hdr: gamma ramp ignored while HDR present is active");
      }
      useGamma = false;

      // One-shot: the fragments assume an sRGB-ENCODED source, because DXVK
      // hands present the non-sRGB view (GetSampleView(false)). If that ever
      // changes, Metal would decode too and we would double-decode.
      if (!bs.warnedHdrSrgbView
       && srcView->info().format != srcView->image()->info().format) {
        bs.warnedHdrSrgbView = true;
        Logger::warn("d9mt: hdr: present source view format differs from the image "
                     "format — verify it is not an _SRGB view (double decode)");
      }
    }

    // unimplemented composition features — fail loud, keep presenting
    if (bs.cursorTextureSet && m_cursorRect.extent.width && !bs.warnedCursor) {
      bs.warnedCursor = true;
      Logger::err("d9mt: blitter: software cursor composition not implemented — ignored");
    }

    VkExtent3D dstExtent = dstView->mipLevelExtent(0);
    VkExtent3D srcExtent = srcView->mipLevelExtent(0);

    // Multisampled present source (a game that requests an MSAA swapchain).
    // Neither the blit fast-path nor the fullscreen-sample path can read an MSAA
    // texture, so FIRST resolve it to a cached 1-sample texture (idiomatic Metal:
    // a no-draw render pass that LOADs the MSAA attachment and resolves it at store
    // time — ported from our DXMT resolve-aware StretchRect). Then fall through with
    // srcHandle pointing at the resolved 1-sample texture, so the existing downstream
    // path handles any scale (a drawable smaller than the backbuffer), format
    // conversion, swizzle, and gamma for free.
    if (srcView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      WMTPixelFormat sF = d9mt::wmtFormatFor(srcView->info().format);
      if (sF == WMTPixelFormatInvalid || srcView->info().packedSwizzle) {
        if (!bs.warnedMsaa) {
          bs.warnedMsaa = true;
          Logger::err("d9mt: blitter: MSAA present source has no plain Metal format — skipping");
        }
        return;
      }
      // (Re)create the cached resolve target on first use or a size/format change.
      if (!bs.msaaResolveTex || bs.msaaResolveW != srcExtent.width
       || bs.msaaResolveH != srcExtent.height || bs.msaaResolveFmt != uint32_t(sF)) {
        WMTTextureInfo ti = { };
        ti.pixel_format       = sF;
        ti.width              = srcExtent.width;
        ti.height             = srcExtent.height;
        ti.depth              = 1;
        ti.array_length       = 1;
        ti.type               = WMTTextureType2D;
        ti.mipmap_level_count = 1;
        ti.sample_count       = 1;
        ti.usage              = WMTTextureUsage(WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget);
        ti.options            = WMTResourceStorageModePrivate;
        obj_handle_t tex = MTLDevice_newTexture(d9mt::mtlDevice(), &ti);
        if (!tex) {
          if (!bs.warnedMsaa) {
            bs.warnedMsaa = true;
            Logger::err("d9mt: blitter: failed to allocate MSAA resolve texture — skipping");
          }
          return;
        }
        bs.msaaResolveTex = tex; bs.msaaResolveW = srcExtent.width;
        bs.msaaResolveH = srcExtent.height; bs.msaaResolveFmt = uint32_t(sF);
      }
      ctx->track(srcView->image(), DxvkAccess::Read);
      WMTRenderPassInfo rp = { };
      rp.render_target_width  = srcExtent.width;
      rp.render_target_height = srcExtent.height;
      rp.default_raster_sample_count = srcView->image()->info().sampleCount;
      rp.colors[0].texture         = srcHandle;
      rp.colors[0].load_action     = WMTLoadActionLoad;       // keep the rendered MSAA scene
      rp.colors[0].store_action    = WMTStoreActionMultisampleResolve;
      rp.colors[0].resolve_texture = bs.msaaResolveTex;       // 1-sample resolve target
      obj_handle_t renc = d9mt::cmdListBeginRenderPass(ctx.ptr(), rp);
      if (renc)
        d9mt::cmdListEndEncoder(ctx.ptr());                   // no draws — resolve happens at store
      // Continue as if the (now 1-sample) resolved texture were the present source.
      srcHandle = bs.msaaResolveTex;
    }

    bool fullDst = dstRect.offset.x == 0 && dstRect.offset.y == 0
                && dstRect.extent.width  == dstExtent.width
                && dstRect.extent.height == dstExtent.height;

    WMTPixelFormat srcFormat = d9mt::wmtFormatFor(srcView->info().format);
    WMTPixelFormat dstFormat = d9mt::wmtFormatFor(dstView->info().format);

    // lifetime + waitForResource tracking (BACKEND-SURFACE §1.8)
    ctx->track(srcView->image(), DxvkAccess::Read);
    ctx->track(dstView->image(), DxvkAccess::Write);

    // fast path: 1:1 same-format copy through the blit encoder. Swizzled
    // views can't be blitted on Metal (the draw path samples them instead).
    if (srcFormat != WMTPixelFormatInvalid
     && srcFormat == dstFormat
     && srcRect.extent.width  == dstRect.extent.width
     && srcRect.extent.height == dstRect.extent.height
     && !srcView->info().packedSwizzle
     && !dstView->info().packedSwizzle
     && !useGamma
     && hdrMode == d9mt::HdrMode::None) {
      wmtcmd_blit_copy_from_texture_to_texture cp = { };
      cp.type = WMTBlitCommandCopyFromTextureToTexture;
      cp.src = srcHandle;
      cp.src_origin = { uint32_t(srcRect.offset.x), uint32_t(srcRect.offset.y), 0u };
      cp.src_size = { dstRect.extent.width, dstRect.extent.height, 1u };
      cp.dst = dstHandle;
      cp.dst_origin = { uint32_t(dstRect.offset.x), uint32_t(dstRect.offset.y), 0u };

      obj_handle_t enc = d9mt::cmdListGetBlitEncoder(ctx.ptr());
      if (enc)
        MTLBlitCommandEncoder_encodeCommands(enc,
          reinterpret_cast<const wmtcmd_base*>(&cp));
      return;
    }

    // general path: fullscreen-triangle sample pass (scaling + format
    // conversion + view swizzles, linear filtering)
    if (dstFormat == WMTPixelFormatInvalid) {
      Logger::err(str::format("d9mt: blitter: unsupported destination format ",
        dstView->info().format));
      return;
    }

    obj_handle_t pso = d9mt::getBlitPso(dstFormat, false, useGamma, hdrMode);
    if (!pso)
      return;

    WMTRenderPassInfo pass = { };
    pass.render_target_width  = dstExtent.width;
    pass.render_target_height = dstExtent.height;
    pass.colors[0].texture = dstHandle;
    pass.colors[0].store_action = WMTStoreActionStore;
    if (fullDst && hdrMode == d9mt::HdrMode::None) {
      pass.colors[0].load_action = WMTLoadActionDontCare;
    } else if (fullDst) {
      // Undefined fp16 memory reads back as magenta noise, and the fullscreen
      // triangle only *usually* covers every sample. A tile-GPU fast clear is
      // near-free and removes the whole failure class (mtld3d shipped this bug
      // three separate times). SDR keeps DontCare so that path is unchanged.
      pass.colors[0].load_action = WMTLoadActionClear;
      pass.colors[0].clear_color = { 0.0, 0.0, 0.0, 1.0 };
    } else {
      // letterbox: clear the uncovered border to opaque black
      pass.colors[0].load_action = WMTLoadActionClear;
      pass.colors[0].clear_color = { 0.0, 0.0, 0.0, 1.0 };
    }

    obj_handle_t enc = d9mt::cmdListBeginRenderPass(ctx.ptr(), pass);
    if (!enc)
      return;

    d9mt::BlitParams params = { };
    params.uvOffset[0] = float(srcRect.offset.x) / float(srcExtent.width);
    params.uvOffset[1] = float(srcRect.offset.y) / float(srcExtent.height);
    params.uvScale[0]  = float(srcRect.extent.width)  / float(srcExtent.width);
    params.uvScale[1]  = float(srcRect.extent.height) / float(srcExtent.height);

    wmtcmd_render_setpso setPso = { };
    wmtcmd_render_setviewport setVp = { };
    wmtcmd_render_setscissorrect setSc = { };
    wmtcmd_render_useresource use = { };
    wmtcmd_render_settexture setTex = { };
    wmtcmd_render_setbytes setBytes = { };
    wmtcmd_render_setbytes setGammaBytes = { };
    wmtcmd_render_setbytes setHdrBytes = { };
    wmtcmd_render_draw draw = { };

    // buffer(0) is BlitParams, buffer(1) is the gamma ramp, so HDR takes
    // buffer(2). Gamma and HDR are mutually exclusive, so the chain never
    // carries both.
    const bool needsHdrBytes = hdrMode == d9mt::HdrMode::Bt2446
                            || hdrMode == d9mt::HdrMode::Bt2446Pq
                            || hdrMode == d9mt::HdrMode::Bt2446Ui
                            || hdrMode == d9mt::HdrMode::Bt2446UiPq;
    d9mt::HdrParams hdrParams = { };
    if (needsHdrBytes)
      hdrParams = d9mt::hdrParamsForPeak(hdrPeakValue);

    setPso.type = WMTRenderCommandSetPSO;
    setPso.next.set(&setVp);
    setPso.pso = pso;

    setVp.type = WMTRenderCommandSetViewport;
    setVp.next.set(&setSc);
    setVp.viewport = { double(dstRect.offset.x), double(dstRect.offset.y),
                       double(dstRect.extent.width), double(dstRect.extent.height),
                       0.0, 1.0 };

    setSc.type = WMTRenderCommandSetScissorRect;
    setSc.next.set(&use);
    setSc.scissor_rect = {
      uint64_t(std::max(dstRect.offset.x, 0)),
      uint64_t(std::max(dstRect.offset.y, 0)),
      std::min(uint64_t(dstRect.extent.width),  uint64_t(dstExtent.width)),
      std::min(uint64_t(dstRect.extent.height), uint64_t(dstExtent.height)) };

    use.type = WMTRenderCommandUseResource;
    use.next.set(&setTex);
    use.resource = srcHandle;
    use.usage = WMTResourceUsageRead;
    use.stages = WMTRenderStages(WMTRenderStageFragment);

    setTex.type = WMTRenderCommandSetFragmentTexture;
    setTex.next.set(&setBytes);
    setTex.texture = srcHandle;
    setTex.index = 0;

    setBytes.type = WMTRenderCommandSetFragmentBytes;
    // The passthrough variant takes no uniform block (it is a bare sRGB
    // decode), so only the curve variants get one -- needsHdrBytes above.
    if (useGamma) {
      setBytes.next.set(&setGammaBytes);
      setGammaBytes.type = WMTRenderCommandSetFragmentBytes;
      setGammaBytes.next.set(&draw);
      setGammaBytes.bytes.set(bs.gammaRamp.data());
      setGammaBytes.length = bs.gammaRamp.size() * sizeof(DxvkGammaCp);
      setGammaBytes.index = 1;
    } else if (needsHdrBytes) {
      setBytes.next.set(&setHdrBytes);
      setHdrBytes.type = WMTRenderCommandSetFragmentBytes;
      setHdrBytes.next.set(&draw);
      setHdrBytes.bytes.set(&hdrParams);
      setHdrBytes.length = sizeof(hdrParams);
      setHdrBytes.index = 2;
    } else {
      setBytes.next.set(&draw);
    }
    setBytes.bytes.set(&params);
    setBytes.length = sizeof(params);
    setBytes.index = 0;

    draw.type = WMTRenderCommandDraw;
    draw.primitive_type = WMTPrimitiveTypeTriangle;
    draw.vertex_start = 0;
    draw.vertex_count = 3;
    draw.instance_count = 1;
    draw.base_instance = 0;

    MTLRenderCommandEncoder_encodeCommands(enc,
      reinterpret_cast<const wmtcmd_base*>(&setPso));

    // close the pass; the front-end flushes the command list right after
    d9mt::cmdListEndEncoder(ctx.ptr());
  }


  void DxvkSwapchainBlitter::setGammaRamp(
          uint32_t            cpCount,
    const DxvkGammaCp*        cpData) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    // identity ramps (windowed path, default ramps) are equivalent to
    // "disabled"; only true LUTs are unimplemented (warned at present time)
    bool identity = true;

    if (cpCount && cpData) {
      for (uint32_t i = 0; i < cpCount && identity; i++) {
        uint32_t expected = (uint64_t(i) * 65535u) / std::max(cpCount - 1u, 1u);

        auto close = [expected] (uint16_t v) {
          int32_t d = int32_t(v) - int32_t(expected);
          return d >= -256 && d <= 256;
        };

        identity = close(cpData[i].r) && close(cpData[i].g) && close(cpData[i].b);
      }
    } else {
      cpCount = 0;
    }

    auto& bs = d9mt::blitterState(this);
    m_gammaCpCount = identity ? 0u : cpCount;

    if (m_gammaCpCount) {
      bs.gammaRamp.assign(cpData, cpData + cpCount);
      bs.warnedGamma = false; // re-warn on new ramps
    } else {
      bs.gammaRamp.clear();
    }
  }


  void DxvkSwapchainBlitter::setCursorTexture(
          VkExtent2D          extent,
          VkFormat            format,
    const void*               data) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    auto& bs = d9mt::blitterState(this);

    bs.cursorTextureSet = extent.width && extent.height && data;
    if (bs.cursorTextureSet)
      bs.warnedCursor = false;
  }


  void DxvkSwapchainBlitter::setCursorPos(
          VkRect2D            rect) {
    // called on the CS thread (BACKEND-SURFACE §5.2) — hence the mutex
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_cursorRect = rect;
  }


  // ==========================================================================
  // hud — disabled on this backend; a null Hud makes every HudItem/
  // HudRenderer path unreachable (upstream returns nullptr when no HUD
  // elements are enabled, and all front-end uses are null-guarded)
  // ==========================================================================

  namespace hud {

    Rc<Hud> Hud::createHud(const Rc<DxvkDevice>& device) {
      return nullptr;
    }

  }

}
