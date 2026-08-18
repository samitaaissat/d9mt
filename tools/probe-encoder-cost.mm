// probe-encoder-cost.mm
//
// Host-native (macOS, no wine) probe: per-render-pass-restart CPU cost of
// [MTLCommandBuffer renderCommandEncoderWithDescriptor:] + [encoder endEncoding],
// and which knobs move it:
//   (a) MTLHazardTrackingModeUntracked on the RT/depth textures (+ MTLFence ladder)
//   (b) commandBufferWithUnretainedReferences
//   (c) both
//   (d) scaling with number of referenced resources (8 vs 256 extra sampled
//       textures declared per pass via useResource)
//
// Workload mirrors the shipped d9mt shadow path: 256 round-trips per frame =
// 512 encoder create/end pairs. Each round-trip:
//   - shadow pass: 64x64 BGRA8Unorm RT (8 RTs round-robin) + shared 64x64
//     Depth32Float_Stencil8 depth, 4 small textured draws
//   - main-pass restart: 800x600 BGRA8Unorm RT + 800x600 D32S8 depth, 1 draw
//     sampling the just-written shadow RT
// Off-screen only (no CAMetalLayer). One command buffer per frame,
// waitUntilCompleted per frame. 120 measured frames per variant, variants
// interleaved round-robin per frame so machine drift hits all equally.
//
// Build:
//   clang++ -std=c++17 -fobjc-arc -O2 -framework Metal -framework Foundation \
//       -framework QuartzCore tools/probe-encoder-cost.mm -o probe-encoder-cost
//
// Output: one machine-parseable RESULT line per variant:
//   RESULT variant=<name> res=<N> enc_ns_med=.. enc_ns_p10=.. enc_ns_p90=..
//          enc_ns_per_restart_med=.. use_ns_med=.. frame_ms_med=..
//          frame_ms_p10=.. frame_ms_p90=.. VERIFY=ok|fail
// enc_ns_* = summed CPU ns per frame spent strictly inside encoder-create and
// endEncoding. use_ns_med = summed CPU ns per frame inside the useResource
// declaration loops (reported separately; NOT included in enc_ns).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

static mach_timebase_info_data_t g_tb;
static inline uint64_t tnow() { return mach_absolute_time(); }
static inline double toNs(uint64_t ticks) {
    return (double)ticks * (double)g_tb.numer / (double)g_tb.denom;
}

static const int kShadowRTs   = 8;
static const int kRoundTrips  = 256;  // -> 512 encoders/frame
static const int kWarmupFrames  = 8;
static const int kMeasureFrames = 120;
static const int kMaxExtras   = 256;
static const int kFencePool   = 8;

struct VariantCfg {
    const char *name;
    bool untracked;   // untracked RT/depth textures + fence ladder
    bool unretained;  // commandBufferWithUnretainedReferences
    int  extraCount;  // referenced sampled textures per pass
};

// One full set of render targets + prebuilt pass descriptors, per tracking mode.
struct RTSet {
    id<MTLTexture> shadowRT[kShadowRTs];
    id<MTLTexture> shadowDepth;       // shared 64x64 D32S8
    id<MTLTexture> mainRT;            // 800x600 BGRA8, shared for readback
    id<MTLTexture> mainDepth;         // 800x600 D32S8
    MTLRenderPassDescriptor *shadowDesc[kShadowRTs];
    MTLRenderPassDescriptor *mainDescClear;
    MTLRenderPassDescriptor *mainDescLoad;
};

static id<MTLTexture> makeTex(id<MTLDevice> dev, MTLPixelFormat fmt, NSUInteger w,
                              NSUInteger h, MTLTextureUsage usage,
                              MTLStorageMode storage, bool untracked) {
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                           width:w
                                                          height:h
                                                       mipmapped:NO];
    d.usage = usage;
    d.storageMode = storage;
    d.hazardTrackingMode =
        untracked ? MTLHazardTrackingModeUntracked : MTLHazardTrackingModeTracked;
    return [dev newTextureWithDescriptor:d];
}

static MTLRenderPassDescriptor *makePassDesc(id<MTLTexture> color,
                                             id<MTLTexture> depth,
                                             MTLLoadAction colorLoad) {
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = color;
    d.colorAttachments[0].loadAction = colorLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    d.depthAttachment.texture = depth;
    d.depthAttachment.loadAction = MTLLoadActionClear;
    d.depthAttachment.storeAction = MTLStoreActionDontCare;
    d.depthAttachment.clearDepth = 1.0;
    d.stencilAttachment.texture = depth;  // D32S8: stencil view must match
    d.stencilAttachment.loadAction = MTLLoadActionClear;
    d.stencilAttachment.storeAction = MTLStoreActionDontCare;
    return d;
}

static void buildRTSet(RTSet &s, id<MTLDevice> dev, bool untracked) {
    for (int i = 0; i < kShadowRTs; i++) {
        s.shadowRT[i] = makeTex(dev, MTLPixelFormatBGRA8Unorm, 64, 64,
                                MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                                MTLStorageModePrivate, untracked);
    }
    s.shadowDepth = makeTex(dev, MTLPixelFormatDepth32Float_Stencil8, 64, 64,
                            MTLTextureUsageRenderTarget, MTLStorageModePrivate,
                            untracked);
    s.mainRT = makeTex(dev, MTLPixelFormatBGRA8Unorm, 800, 600,
                       MTLTextureUsageRenderTarget, MTLStorageModeShared, untracked);
    s.mainDepth = makeTex(dev, MTLPixelFormatDepth32Float_Stencil8, 800, 600,
                          MTLTextureUsageRenderTarget, MTLStorageModePrivate,
                          untracked);
    for (int i = 0; i < kShadowRTs; i++)
        s.shadowDesc[i] = makePassDesc(s.shadowRT[i], s.shadowDepth, MTLLoadActionClear);
    s.mainDescClear = makePassDesc(s.mainRT, s.mainDepth, MTLLoadActionClear);
    s.mainDescLoad  = makePassDesc(s.mainRT, s.mainDepth, MTLLoadActionLoad);
}

int main() {
    mach_timebase_info(&g_tb);
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            fprintf(stderr, "FAIL: no Metal device\n");
            return 1;
        }
        fprintf(stderr, "# device: %s\n", dev.name.UTF8String);
        id<MTLCommandQueue> queue = [dev newCommandQueue];

        // --- trivial textured-quad PSO, MSL compiled at runtime -------------
        NSString *src = @"#include <metal_stdlib>\n"
                         "using namespace metal;\n"
                         "struct VOut { float4 pos [[position]]; float2 uv; };\n"
                         "vertex VOut vmain(uint vid [[vertex_id]]) {\n"
                         "  float2 p = float2((vid & 1) ? 1.0 : -1.0,\n"
                         "                    (vid & 2) ? 1.0 : -1.0);\n"
                         "  VOut o; o.pos = float4(p, 0.5, 1.0);\n"
                         "  o.uv = p * 0.5 + 0.5; return o;\n"
                         "}\n"
                         "fragment float4 fmain(VOut in [[stage_in]],\n"
                         "        texture2d<float> t [[texture(0)]]) {\n"
                         "  constexpr sampler s(filter::linear);\n"
                         "  return t.sample(s, in.uv);\n"
                         "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "FAIL: MSL compile: %s\n",
                    err.localizedDescription.UTF8String);
            return 1;
        }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd
                                                                             error:&err];
        if (!pso) {
            fprintf(stderr, "FAIL: PSO: %s\n", err.localizedDescription.UTF8String);
            return 1;
        }

        // --- resource sets ---------------------------------------------------
        // Two full RT sets: tracked and untracked. Extra sampled textures stay
        // tracked in every variant (they model game textures); the knob under
        // test is untracked on the RT/depth textures only.
        RTSet tracked, untracked;
        buildRTSet(tracked, dev, false);
        buildRTSet(untracked, dev, true);

        std::vector<id<MTLTexture>> extras;  // ARC strong refs, alive for the
        extras.reserve(kMaxExtras);          // whole run (covers unretained CBs)
        {
            uint32_t px[16];
            for (int i = 0; i < 16; i++) px[i] = 0xFF2080FFu;  // BGRA orange-ish
            for (int i = 0; i < kMaxExtras; i++) {
                id<MTLTexture> t = makeTex(dev, MTLPixelFormatBGRA8Unorm, 4, 4,
                                           MTLTextureUsageShaderRead,
                                           MTLStorageModeShared, false);
                [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
                     mipmapLevel:0
                       withBytes:px
                     bytesPerRow:16];
                extras.push_back(t);
            }
        }

        id<MTLFence> fences[kFencePool];  // strong refs for CB lifetime
        for (int i = 0; i < kFencePool; i++) fences[i] = [dev newFence];

        VariantCfg variants[] = {
            {"baseline",             false, false, 8},
            {"untracked",            true,  false, 8},
            {"unretained",           false, true,  8},
            {"untracked+unretained", true,  true,  8},
            {"baseline",             false, false, 256},
            {"untracked",            true,  false, 256},
            {"unretained",           false, true,  256},
            {"untracked+unretained", true,  true,  256},
        };
        const int nVariants = (int)(sizeof(variants) / sizeof(variants[0]));

        std::vector<double> encNs[nVariants], useNs[nVariants], frameNs[nVariants];
        int verifyFail[nVariants];
        for (int v = 0; v < nVariants; v++) {
            encNs[v].reserve(kMeasureFrames);
            useNs[v].reserve(kMeasureFrames);
            frameNs[v].reserve(kMeasureFrames);
            verifyFail[v] = 0;
        }

        // --- one frame of the 512-restart workload ---------------------------
        auto runFrame = [&](const VariantCfg &v, RTSet &rs, uint64_t &encTicks,
                            uint64_t &useTicks, bool &verifyOk) {
            @autoreleasepool {
                id<MTLCommandBuffer> cb =
                    v.unretained ? [queue commandBufferWithUnretainedReferences]
                                 : [queue commandBuffer];
                encTicks = 0;
                useTicks = 0;
                int fenceIdx = 0;  // index of the NEXT fence to update
                for (int r = 0; r < kRoundTrips; r++) {
                    int s = r & (kShadowRTs - 1);

                    // ---- shadow pass: 64x64, 4 textured draws ----
                    MTLRenderPassDescriptor *sd = rs.shadowDesc[s];
                    uint64_t t0 = tnow();
                    id<MTLRenderCommandEncoder> enc =
                        [cb renderCommandEncoderWithDescriptor:sd];
                    encTicks += tnow() - t0;
                    if (v.untracked && fenceIdx > 0)
                        [enc waitForFence:fences[(fenceIdx - 1) % kFencePool]
                             beforeStages:MTLRenderStageVertex];
                    [enc setRenderPipelineState:pso];
                    t0 = tnow();
                    for (int i = 0; i < v.extraCount; i++)
                        [enc useResource:extras[(size_t)i]
                                   usage:MTLResourceUsageRead
                                  stages:MTLRenderStageFragment];
                    useTicks += tnow() - t0;
                    for (int d = 0; d < 4; d++) {
                        [enc setFragmentTexture:extras[(size_t)((r * 4 + d) % v.extraCount)]
                                        atIndex:0];
                        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                                vertexStart:0
                                vertexCount:4];
                    }
                    if (v.untracked)
                        [enc updateFence:fences[fenceIdx % kFencePool]
                             afterStages:MTLRenderStageFragment];
                    t0 = tnow();
                    [enc endEncoding];
                    encTicks += tnow() - t0;
                    if (v.untracked) fenceIdx++;

                    // ---- main-pass restart: 800x600, 1 draw sampling the RT ----
                    MTLRenderPassDescriptor *md =
                        (r == 0) ? rs.mainDescClear : rs.mainDescLoad;
                    t0 = tnow();
                    enc = [cb renderCommandEncoderWithDescriptor:md];
                    encTicks += tnow() - t0;
                    if (v.untracked)
                        [enc waitForFence:fences[(fenceIdx - 1) % kFencePool]
                             beforeStages:MTLRenderStageVertex];
                    [enc setRenderPipelineState:pso];
                    t0 = tnow();
                    for (int i = 0; i < v.extraCount; i++)
                        [enc useResource:extras[(size_t)i]
                                   usage:MTLResourceUsageRead
                                  stages:MTLRenderStageFragment];
                    useTicks += tnow() - t0;
                    [enc setFragmentTexture:rs.shadowRT[s] atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:0
                            vertexCount:4];
                    if (v.untracked)
                        [enc updateFence:fences[fenceIdx % kFencePool]
                             afterStages:MTLRenderStageFragment];
                    t0 = tnow();
                    [enc endEncoding];
                    encTicks += tnow() - t0;
                    if (v.untracked) fenceIdx++;
                }
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error)
                    fprintf(stderr, "# CB error (%s res=%d): %s\n", v.name,
                            v.extraCount, cb.error.localizedDescription.UTF8String);
                // Verify: one pixel of the main RT must be non-black (it sampled
                // the just-written shadow RT, which sampled an orange texture).
                uint32_t px = 0;
                [rs.mainRT getBytes:&px
                        bytesPerRow:4
                         fromRegion:MTLRegionMake2D(400, 300, 1, 1)
                        mipmapLevel:0];
                verifyOk = (px & 0x00FFFFFFu) != 0;
            }
        };

        // --- interleaved measurement loop: A B C D E F G H, repeated ---------
        for (int round = 0; round < kWarmupFrames + kMeasureFrames; round++) {
            for (int v = 0; v < nVariants; v++) {
                RTSet &rs = variants[v].untracked ? untracked : tracked;
                uint64_t et = 0, ut = 0;
                uint64_t f0 = tnow();
                bool ok = true;
                runFrame(variants[v], rs, et, ut, ok);
                uint64_t f1 = tnow();
                if (round >= kWarmupFrames) {
                    encNs[v].push_back(toNs(et));
                    useNs[v].push_back(toNs(ut));
                    frameNs[v].push_back(toNs(f1 - f0));
                    if (!ok) verifyFail[v]++;
                }
            }
        }

        // --- report -----------------------------------------------------------
        auto pct = [](std::vector<double> &xs, double p) {
            std::sort(xs.begin(), xs.end());
            size_t i = (size_t)(p * (double)(xs.size() - 1) + 0.5);
            return xs[i];
        };
        for (int v = 0; v < nVariants; v++) {
            double em = pct(encNs[v], 0.5), e10 = pct(encNs[v], 0.1),
                   e90 = pct(encNs[v], 0.9);
            double um = pct(useNs[v], 0.5);
            double fm = pct(frameNs[v], 0.5), f10 = pct(frameNs[v], 0.1),
                   f90 = pct(frameNs[v], 0.9);
            printf("RESULT variant=%s res=%d enc_ns_med=%.0f enc_ns_p10=%.0f "
                   "enc_ns_p90=%.0f enc_ns_per_restart_med=%.1f use_ns_med=%.0f "
                   "frame_ms_med=%.3f frame_ms_p10=%.3f frame_ms_p90=%.3f "
                   "VERIFY=%s\n",
                   variants[v].name, variants[v].extraCount, em, e10, e90,
                   em / (2.0 * kRoundTrips), um, fm / 1e6, f10 / 1e6, f90 / 1e6,
                   verifyFail[v] == 0 ? "ok" : "fail");
        }
        fflush(stdout);
    }
    return 0;
}
