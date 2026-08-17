// d9mtmetal: tiny companion wine-unixlib supplying the two Metal entry
// points DXMT's winemetal lacks:
//   0: MTLDevice newLibraryWithSource   (runtime MSL compilation)
//   1: MTLDevice newRenderPipelineState WITH an MTLVertexDescriptor
//      (spirv-cross vertex shaders use [[stage_in]] and need one)
//
// obj_handle_t values interoperate with winemetal's handles: both are raw
// ObjC pointers in the same process.
//
// ABI rule: every field is fixed-width and pointers are zero-extended
// uint64_t, so the 32-bit and 64-bit param layouts are IDENTICAL and the
// wow64 call table can reuse the same entry points.

/* The HDR entry points (D9MT_FUNC_LAYER_EDR / _COLORSPACE / _EDR_METADATA) and
 * their main-queue publish design are derived from mtld3d v0.6.0
 * (unix/unix/src/metal/{macdrv,command}.rs), Copyright (c) 2026 Alexander
 * Theissen, zlib licence. ALTERED SOURCE: re-expressed as a wine-unixlib ABI
 * in C/ObjC rather than mtld3d's own unix half, with an added ABI-count
 * handshake. See THIRD_PARTY_NOTICES.md. */

#ifndef D9MTMETAL_H
#define D9MTMETAL_H

#include <stdint.h>

enum d9mt_unix_func {
  D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE = 0,
  D9MT_FUNC_NEW_RENDER_PSO = 1,
  /* key -> MTLLibrary, with the disk metallib cache + compile backends and
   * all fallback resolved entirely native-side. The PE passes a content key
   * and the source bytes; the source is only read on a cache MISS. The
   * .metallib bytes never cross this ABI — only a handle comes back up. */
  D9MT_FUNC_LIBRARY_FOR_KEY = 2,
  /* macOS 26+ Metal HUD: set a custom label's text color. On macOS 27 the HUD
   * renders custom labels in black by default (invisible on the dark overlay);
   * winemetal exposes addLabel/updateLabel but NOT the color setter, so we add
   * it here. hud + key are raw ObjC handles obtained via winemetal (same
   * process, interoperable per the ABI note above). */
  D9MT_FUNC_HUD_SET_COLOR = 3,
  /* Programmatic Metal frame capture to a .gputrace file (Xcode frame debugger).
   * Wine can't be attached by Xcode live, so we drive MTLCaptureManager
   * ourselves. Requires MTL_CAPTURE_ENABLED=1 at launch. action: 1=begin, 0=end.
   * begin captures all command buffers on `queue` until end is called. */
  D9MT_FUNC_CAPTURE = 4,
  /* Fused render-encoder transition: end the old encoder (if any) and begin
   * a render pass on the command buffer in ONE PE->unix crossing, instead of
   * the 6-7 winemetal crossings (endEncoding, release, pool alloc, encoder
   * create, retain, pool release) a pass restart costs today. Pass restarts
   * are the dominant CPU cost of render-target round-trips (WoW shadow
   * blobs / shadow maps: measured 32us avg per startRenderPass, 75% of the
   * frame in the rt benchmark). Handles interoperate with winemetal's (raw
   * ObjC pointers, same process). pass_ptr uses winemetal's WMTRenderPassInfo
   * layout (pinned DXMT v0.80); the descriptor translation is mirrored
   * native-side. end_immediately=1 encodes a load/store-action-only pass
   * (deferred clears) and returns no encoder. pass_ptr=0 = end-only. */
  D9MT_FUNC_PASS_TRANSITION = 5,
  /* CAMetalLayer EDR headroom, read on the MAIN QUEUE.
   *
   * winemetal HAS MetalLayer_getEDRValue, but its implementation walks
   * layer.delegate -> NSView -> NSWindow -> NSScreen on the CALLING thread.
   * NSView.window and NSWindow.screen are main-thread-only, and a zone
   * transition is exactly when the main thread rebuilds them. mtld3d shipped
   * that bug and crashed in AppKit ~3s into a raid (their commit 119c0db).
   * Here it would be worse than a crash: wine absorbs faults on non-wine
   * threads silently, so the present thread would just die and the render
   * would freeze with the process still alive.
   *
   * So: this call NEVER touches AppKit on the caller's thread. It returns the
   * last values published by a main-queue block and, when refresh=1, queues
   * another one (fire-and-forget, at most one outstanding). Values are seeded
   * to 1.0, which the present shader treats as "no headroom" and handles with
   * its passthrough variant. */
  D9MT_FUNC_LAYER_EDR = 6,
  /* Tag a CAMetalLayer with the DISPLAY's own colorspace, extended-linearized
   * (CGColorSpaceCreateExtendedLinearized of the screen's profile) and set
   * wantsExtendedDynamicRangeContent.
   *
   * winemetal's MetalLayer_setColorSpace only accepts four hard-coded
   * CGColorSpace names (GetColorSpaceName is a closed switch); there is no
   * route to the display's own profile. That profile is mtld3d's default
   * (`color.space = passthrough`) and therefore what the approved look was
   * tested against, so we need it for parity. Falls back reporting failure,
   * and the caller then uses winemetal's extended-linear sRGB.
   * Runs on the main queue (CAMetalLayer property writes + NSScreen access). */
  D9MT_FUNC_LAYER_COLORSPACE = 7,
  /* CAEDRMetadata on the layer (HDR10 mastering volume). Only meaningful for
   * the PQ output mode: it tells the compositor our mastering intent so its
   * PQ->display mapping is informed rather than assumed. Absent from winemetal
   * entirely. Optional - PQ works without it. */
  D9MT_FUNC_LAYER_EDR_METADATA = 8,
  D9MT_FUNC_COUNT,
};

struct d9mt_pass_transition_params {
  uint64_t cmdbuf;          /* in: MTLCommandBuffer handle (needed unless end-only) */
  uint64_t old_encoder;     /* in: encoder to endEncoding+release, or 0            */
  uint64_t pass_ptr;        /* in: const struct WMTRenderPassInfo*, or 0=end-only  */
  uint32_t end_immediately; /* in: 1 = end the new pass right away (clear pass)    */
  uint32_t padding;
  uint64_t ret_encoder;     /* out: retained MTLRenderCommandEncoder or 0          */
};

struct d9mt_capture_params {
  uint64_t queue;     /* in: MTLCommandQueue handle (begin only)           */
  uint64_t path_ptr;  /* in: UTF-8 output .gputrace path (begin only)      */
  uint64_t path_len;  /* in */
  uint32_t action;    /* in: 1=begin, 0=end                                */
  uint32_t ret_ok;    /* out: 1 if the action succeeded                    */
};

/* colorspace_kind for d9mt_layer_colorspace_params */
enum d9mt_layer_colorspace {
  /* CGColorSpaceCreateExtendedLinearized(screen.colorSpace.CGColorSpace) —
   * the display's own primaries, linearized. mtld3d's `passthrough`. */
  D9MT_COLORSPACE_DISPLAY_EXTENDED_LINEAR = 0,
};

/* refresh mode for d9mt_layer_edr_params */
enum d9mt_layer_edr_refresh {
  /* Answer from the published snapshot and do nothing else. */
  D9MT_EDR_READ_ONLY = 0,
  /* Answer from the snapshot, then queue an async main-queue re-read for next
   * time. The ONLY mode the per-present path may use. */
  D9MT_EDR_REFRESH_ASYNC = 1,
  /* Walk NOW on the main queue and publish before returning, so the caller
   * sees a real value rather than the seed.
   *
   * Required by the one-shot capability gate: the gate latches on its first
   * answer, and an async refresh cannot possibly have landed by then, so the
   * gate would read the 1.0 seed, conclude "no EDR headroom" and pin HDR off
   * for the process -- on every machine, however capable the display.
   *
   * Safe only at configure time, and only because the caller's own call stack
   * already hops to the main queue moments later (MetalLayer_setProps,
   * D9MT_FUNC_LAYER_COLORSPACE), so this introduces no new blocking class.
   * NEVER use this per frame. */
  D9MT_EDR_REFRESH_SYNC = 2,
};

struct d9mt_layer_edr_params {
  uint64_t layer;             /* in:  CAMetalLayer handle                        */
  uint32_t refresh;           /* in:  enum d9mt_layer_edr_refresh                */
  uint32_t ret_published;     /* out: 1 if a main-queue walk has ever completed  */
  float    ret_max_edr;       /* out: live headroom multiplier, or 1.0           */
  float    ret_max_potential; /* out: static panel ceiling, or 1.0               */
};

struct d9mt_layer_colorspace_params {
  uint64_t layer;           /* in:  CAMetalLayer handle                    */
  uint32_t colorspace_kind; /* in:  enum d9mt_layer_colorspace             */
  uint32_t wants_edr;       /* in:  wantsExtendedDynamicRangeContent value */
  uint32_t ret_ok;          /* out: 1 if the tag was applied               */
  uint32_t padding;
};

/* Values are SMPTE ST.2086 style: primaries/white point in 0.00002 units,
 * luminance in 0.0001 cd/m2 for min and 1 cd/m2 for max, matching
 * CAEDRMetadata.hdr10ContentProperties semantics after conversion. */
struct d9mt_layer_edr_metadata_params {
  uint64_t layer;                     /* in: CAMetalLayer handle              */
  float    min_luminance_nits;        /* in: mastering display min            */
  float    max_luminance_nits;        /* in: mastering display max            */
  float    max_content_light_level;   /* in: MaxCLL,  0 = omit                */
  float    max_frame_average_light;   /* in: MaxFALL, 0 = omit                */
  uint32_t clear;                     /* in: 1 = drop the metadata instead    */
  uint32_t ret_ok;                    /* out: 1 if applied                    */
};

struct d9mt_hud_color_params {
  uint64_t hud;          /* in: _CADeveloperHUDProperties instance handle */
  uint64_t key;          /* in: NSString label key handle                 */
  uint32_t name_color;   /* in: 0xAARRGGBB (or 0xFFFFFFFF for white)      */
  uint32_t value_color;  /* in */
};

/* source_kind values (InputKind). Today every backend is fed MSL_TEXT;
 * SPIRV/DXBC are reserved for future earlier-input backends (MSC/airconv). */
enum d9mt_source_kind {
  D9MT_SOURCE_MSL_TEXT = 0,
  D9MT_SOURCE_SPIRV = 1,
  D9MT_SOURCE_DXBC = 2,
};

/* target_flags bits: codegen/compile options that affect the artifact bytes.
 * (msl language version is implicit 3.0 today; folded into the key PE-side.) */
enum d9mt_target_flags {
  D9MT_TARGET_FAST_MATH = 1u << 0,
};

/* ret_status values (observable, never branched on for control flow). */
enum d9mt_library_status {
  D9MT_LIBRARY_HIT = 0,       /* served from the disk cache               */
  D9MT_LIBRARY_COMPILED = 1,  /* compiled by a backend, then cached       */
  D9MT_LIBRARY_FELL_BACK = 2, /* degraded to live newLibraryWithSource    */
  D9MT_LIBRARY_FAILED = 3,    /* no library produced                      */
};

struct d9mt_library_params {
  uint64_t device;       /* in:  obj_handle_t MTLDevice                       */
  uint64_t key_ptr;      /* in:  const void* ShaderKey digest bytes           */
  uint64_t key_len;      /* in */
  uint64_t source_ptr;   /* in:  source blob (only read on a cache MISS)      */
  uint64_t source_len;   /* in */
  uint32_t source_kind;  /* in:  enum d9mt_source_kind                        */
  uint32_t target_flags; /* in:  enum d9mt_target_flags bitmask              */
  uint64_t ret_library;  /* out: retained MTLLibrary or 0                     */
  uint64_t ret_status;   /* out: enum d9mt_library_status                     */
  uint64_t ret_error;    /* out: retained NSError or 0 (caller releases)      */
};

struct d9mt_newlibrary_params {
  uint64_t device;     /* in:  obj_handle_t MTLDevice */
  uint64_t source_ptr; /* in:  const char* UTF-8 MSL  */
  uint64_t source_len; /* in */
  uint32_t fast_math;  /* in:  bool */
  /* out: ABI HANDSHAKE. The .so writes D9MT_FUNC_COUNT here, unconditionally
   * and before any early return. This was `padding` and PE callers already
   * zero-initialise the struct, so an OLDER .so leaves it 0.
   *
   * Why it has to be smuggled through an entry point that already exists:
   * __wine_unix_call indexes the table with NO bounds check, so probing a new
   * function code against an old .so does not fail — it jumps through whatever
   * bytes follow the table. "Call it and check the status" IS the crash. Every
   * call code added after 5 must therefore be gated on
   *   abi_func_count > <that code>
   * and a mismatched pair degrades to the old feature set instead of
   * executing garbage. (The payload ships d3d9.dll + d9mtmetal.{dll,so}
   * atomically under one sha256, so skew should not happen; this is the belt
   * to that braces.) */
  uint32_t abi_func_count;
  uint64_t ret_library; /* out: retained MTLLibrary or 0 */
  uint64_t ret_error;   /* out: retained NSError or 0 (caller releases) */
};

/* matches MTLVertexFormat raw values */
struct d9mt_vertex_attribute {
  uint32_t format;       /* MTLVertexFormat */
  uint32_t offset;
  uint32_t buffer_index;
  uint32_t location;     /* shader [[attribute(location)]] slot */
};

struct d9mt_vertex_layout {
  uint32_t buffer_index;
  uint32_t stride;
  uint32_t step_function; /* MTLVertexStepFunction: 1=per-vertex 2=per-instance */
  uint32_t step_rate;
};

struct d9mt_color_attachment {
  uint32_t pixel_format; /* MTLPixelFormat */
  uint32_t blending_enabled;
  uint32_t rgb_blend_op;       /* MTLBlendOperation */
  uint32_t alpha_blend_op;
  uint32_t src_rgb_blend_factor; /* MTLBlendFactor */
  uint32_t dst_rgb_blend_factor;
  uint32_t src_alpha_blend_factor;
  uint32_t dst_alpha_blend_factor;
  uint32_t write_mask; /* MTLColorWriteMask */
  uint32_t padding;
};

struct d9mt_pso_info {
  uint64_t vertex_function;   /* in: obj_handle_t MTLFunction */
  uint64_t fragment_function; /* in: obj_handle_t MTLFunction or 0 */
  struct d9mt_color_attachment colors[8];
  uint32_t depth_pixel_format;   /* MTLPixelFormat, 0 = none */
  uint32_t stencil_pixel_format;
  uint32_t raster_sample_count;
  uint32_t alpha_to_coverage;
  uint32_t num_attributes;
  uint32_t num_layouts;
  struct d9mt_vertex_attribute attributes[18];
  struct d9mt_vertex_layout layouts[16];
};

struct d9mt_newpso_params {
  uint64_t device;   /* in: obj_handle_t MTLDevice */
  uint64_t info_ptr; /* in: struct d9mt_pso_info* */
  uint64_t ret_pso;  /* out: retained MTLRenderPipelineState or 0 */
  uint64_t ret_error; /* out: retained NSError or 0 */
};

#ifdef _WIN32
/* PE-side entry point exported by d9mtmetal.dll */
#ifdef D9MTMETAL_EXPORTS
#define D9MT_API __declspec(dllexport)
#else
#define D9MT_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
D9MT_API int __cdecl D9MT_UnixCall(unsigned int code, void *params);
#ifdef __cplusplus
}
#endif
#endif

#endif
