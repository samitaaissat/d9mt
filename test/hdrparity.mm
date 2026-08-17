// GPU parity harness: run d9mt's ported BT.2446-A fragment and mtld3d's
// original fragment over the same input on the same device, and diff.
//
// This is the check that the port is faithful. It is deliberately NOT a
// reimplementation of the math on the CPU -- it runs both shipped shader
// sources through the real Metal compiler with the same math mode, so
// fast-math reassociation, select() semantics and matrix conventions are all
// exercised the way they will be in the driver.
//
// Host-native (arm64, no wine): the ported fragment is extracted from
// src/d3d9fe/d9mt_presenter.cpp's g_blitShaderMsl and both are compiled by the
// real Metal compiler. Run via tools/run-hdrparity.sh.
//
// Guards: docs/superpowers/specs/2026-08-17-hdr-bt2446-design.md
// Reference: mtld3d v0.6.0 unix/unix/src/metal/present.msl (zlib, (c) 2026
// Alexander Theissen) -- see THIRD_PARTY_NOTICES.md.

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <simd/simd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static NSString* readFile(const char* path) {
  NSError* err = nil;
  NSString* s = [NSString stringWithContentsOfFile:@(path)
                                          encoding:NSUTF8StringEncoding
                                             error:&err];
  if (!s) { fprintf(stderr, "cannot read %s: %s\n", path, err.description.UTF8String); exit(2); }
  return s;
}

// Shared driver: one compute pass per shader, both fed the identical linear
// input and identical derived uniforms.
static const char* kDriver = R"MSL(
kernel void run_d9mt(device const float3* in [[buffer(0)]],
                     device float3* out [[buffer(1)]],
                     constant d9mt_hdr_params& u [[buffer(2)]],
                     uint i [[thread_position_in_grid]]) {
  out[i] = d9mt_bt2446a_ictcp(in[i], u);
}

kernel void run_mtld3d(device const float3* in [[buffer(0)]],
                       device float3* out [[buffer(1)]],
                       constant HdrUniforms& u [[buffer(2)]],
                       uint i [[thread_position_in_grid]]) {
  out[i] = mtld3d_bt2446a(in[i], u);
}
)MSL";

// mtld3d's fragment body, lifted verbatim into a callable with its own
// constants. Only the wrapper differs; every expression is copied as shipped.
static const char* kMtld3d = R"MSL(
// mtld3d's 16-byte block
struct HdrUniforms {
    float l_hdr_nits; float p_hdr; float log2_p_hdr; float inv_p_minus_one;
};

constant float L_SDR_M = 100.0;
constant float P_SDR_M = 5.4395284707f;
constant float LOG_P_SDR_M = 1.6937747f;

constant float3x3 MM_BT709_TO_LMS = float3x3(
    float3(0.2958197875977f, 0.1562652587891f, 0.0351760864258f),
    float3(0.6230921020508f, 0.7272338867188f, 0.1564789062500f),
    float3(0.0810847778320f, 0.1164960937500f, 0.8083459472656f));
constant float3x3 MM_LMS_TO_BT709 = float3x3(
    float3( 6.1729507446289f, -1.3236293334961f, -0.0118084289551f),
    float3(-5.3198394775391f,  2.5602416992188f, -0.2641143798828f),
    float3( 0.1465606689453f, -0.2363464355469f,  1.2761764526367f));
constant float3x3 MM_LMS_PQ_TO_ICTCP = float3x3(
    float3(0.5f,  1.61370003f,  4.37806224f),
    float3(0.5f, -3.32339620f, -4.24553966f),
    float3(0.0f,  1.70969617f, -0.13252264f));
constant float3x3 MM_ICTCP_TO_LMS_PQ = float3x3(
    float3(1.0f, 1.0f, 1.0f),
    float3(0.00860903f, -0.00860903f,  0.56003270f),
    float3(0.11102963f, -0.11102963f, -0.32062717f));

inline float3 mpq_oetf(float3 nits) {
    constexpr float M1 = 2610.0f / 16384.0f;
    constexpr float M2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float C1 = 3424.0f / 4096.0f;
    constexpr float C2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float C3 = 2392.0f / 4096.0f * 32.0f;
    float3 y = pow(max(nits, float3(0.0f)) / 10000.0f, float3(M1));
    return pow((float3(C1) + C2 * y) / (float3(1.0f) + C3 * y), float3(M2));
}
inline float3 mpq_eotf(float3 pq) {
    constexpr float M1 = 2610.0f / 16384.0f;
    constexpr float M2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float C1 = 3424.0f / 4096.0f;
    constexpr float C2 = 2413.0f / 4096.0f * 32.0f;
    constexpr float C3 = 2392.0f / 4096.0f * 32.0f;
    float3 e = pow(max(pq, float3(0.0f)), float3(1.0f / M2));
    float3 num = max(e - float3(C1), float3(0.0f));
    float3 den = max(float3(C2) - C3 * e, float3(1e-20f));
    return 10000.0f * pow(num / den, float3(1.0f / M1));
}

float3 mtld3d_bt2446a(float3 lin, constant HdrUniforms& u) {
    float y_sdr = max(dot(lin, float3(0.2126f, 0.7152f, 0.0722f)), 1e-20f);
    float yp_sdr = pow(y_sdr, 1.0f / 2.4f);
    float yp_c = log((yp_sdr * (P_SDR_M - 1.0f)) + 1.0f) / LOG_P_SDR_M;
    float yp_0 = yp_c / 1.0770f;
    float yp_1 = (-2.7811f + sqrt(4.83307641f - 4.604f * yp_c)) / -2.302f;
    float yp_2 = (yp_c - 0.5f) / 0.5f;
    float yp_p = yp_0 <= 0.7399f ? yp_0
               : (yp_2 >= 0.9909f ? yp_2 : yp_1);
    float yp_hdr = (exp2(yp_p * u.log2_p_hdr) - 1.0f) * u.inv_p_minus_one;
    float y_hdr_linear_nits = pow(yp_hdr, 2.4f) * u.l_hdr_nits;
    float3 lms_nits = MM_BT709_TO_LMS * (lin * L_SDR_M);
    float3 lms_pq = mpq_oetf(lms_nits);
    float3 ictcp_in = MM_LMS_PQ_TO_ICTCP * lms_pq;
    float i_hdr = mpq_oetf(float3(y_hdr_linear_nits)).x;
    float i_ratio = i_hdr / max(ictcp_in.x, 1e-6f);
    float3 ictcp_out = float3(i_hdr, ictcp_in.y * i_ratio, ictcp_in.z * i_ratio);
    float3 lms_pq_out = MM_ICTCP_TO_LMS_PQ * ictcp_out;
    float3 lms_nits_out = mpq_eotf(lms_pq_out);
    float3 rgb_nits_out = MM_LMS_TO_BT709 * lms_nits_out;
    return max(rgb_nits_out, float3(0.0f));
}
)MSL";

int main(int argc, const char** argv) {
  @autoreleasepool {
    if (argc < 2) { fprintf(stderr, "usage: parity <d9mt-shader.metal>\n"); return 2; }

    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }

    NSString* src = [NSString stringWithFormat:@"%@\n%s\n%s\n",
                     readFile(argv[1]), kMtld3d, kDriver];

    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion3_0;
    opts.mathMode = MTLMathModeFast;   // same mode the driver uses
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&err];
    if (!lib) { fprintf(stderr, "compile failed:\n%s\n", err.description.UTF8String); return 2; }

    // --- inputs: neutral ramp + saturated primaries + mixed colours ---
    std::vector<simd_float3> input;
    auto srgb_eotf = [](float c) {
      return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    for (int i = 0; i <= 256; i++) {
      float e = float(i) / 256.0f, l = srgb_eotf(e);
      input.push_back(simd_make_float3(l, l, l));
    }
    const float sats[][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,0},{0,1,1},{1,0,1},
                             {0.9f,0.3f,0.1f},{0.2f,0.6f,0.95f},{0.5f,0.05f,0.7f}};
    for (auto& s : sats)
      input.push_back(simd_make_float3(srgb_eotf(s[0]), srgb_eotf(s[1]), srgb_eotf(s[2])));

    const size_t N = input.size();

    // --- uniforms, both derived from the same peak ---
    struct D9 { float l, p_hdr, log2p, invp1; };
    struct MT { float l, p_hdr, log2p, invp1; };

    int failures = 0;
    for (float peak : {4.2686f, 1.6f, 2.0f, 8.0f, 1.0001f}) {
      float lHdr = peak * 100.0f;
      float pHdr = 1.0f + 32.0f * std::pow(lHdr / 10000.0f, 1.0f / 2.4f);
      D9 ud = { lHdr, pHdr, std::log2(pHdr), 1.0f / (pHdr - 1.0f) };
      MT um = { lHdr, pHdr, std::log2(pHdr), 1.0f / (pHdr - 1.0f) };

      id<MTLBuffer> bin  = [dev newBufferWithBytes:input.data()
                                            length:N * sizeof(simd_float3)
                                           options:MTLResourceStorageModeShared];
      id<MTLBuffer> outA = [dev newBufferWithLength:N * sizeof(simd_float3)
                                            options:MTLResourceStorageModeShared];
      id<MTLBuffer> outB = [dev newBufferWithLength:N * sizeof(simd_float3)
                                            options:MTLResourceStorageModeShared];

      id<MTLCommandQueue> q = [dev newCommandQueue];
      auto dispatch = [&](const char* fn, id<MTLBuffer> out, void* u, size_t ul) {
        id<MTLFunction> f = [lib newFunctionWithName:@(fn)];
        NSError* e2 = nil;
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:f error:&e2];
        if (!pso) { fprintf(stderr, "pso %s: %s\n", fn, e2.description.UTF8String); exit(2); }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:bin offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBytes:u length:ul atIndex:2];
        [enc dispatchThreads:MTLSizeMake(N, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      };
      dispatch("run_d9mt",   outA, &ud, sizeof(ud));
      dispatch("run_mtld3d", outB, &um, sizeof(um));

      const simd_float3* A = (const simd_float3*)outA.contents;
      const simd_float3* B = (const simd_float3*)outB.contents;

      double worstAbs = 0, worstRel = 0; size_t worstIdx = 0;
      for (size_t i = 0; i < N; i++) {
        for (int c = 0; c < 3; c++) {
          double a = A[i][c], b = B[i][c];
          double abs_ = std::fabs(a - b);
          double rel  = abs_ / std::max(1e-4, std::fabs(b));
          if (rel > worstRel) { worstRel = rel; worstAbs = abs_; worstIdx = i; }
        }
      }
      bool ok = worstRel < 1e-5;
      printf("peak %-8.4f  L_hdr %-8.2f  worst rel %.3e (abs %.3e) at idx %zu   %s\n",
             peak, lHdr, worstRel, worstAbs, worstIdx, ok ? "MATCH" : "*** MISMATCH ***");
      if (!ok) {
        failures++;
        printf("      d9mt   = (%g, %g, %g)\n", A[worstIdx][0], A[worstIdx][1], A[worstIdx][2]);
        printf("      mtld3d = (%g, %g, %g)\n", B[worstIdx][0], B[worstIdx][1], B[worstIdx][2]);
      }
      if (peak == 4.2686f) {
        printf("      transfer (sRGB -> nits, d9mt): ");
        for (float e : {0.05f, 0.20f, 0.50f, 0.75f, 0.90f, 1.00f}) {
          size_t i = size_t(std::lround(e * 256.0f));
          printf("%.2f->%.1f  ", e, A[i][1]);
        }
        printf("\n");
      }
    }
    printf("\n%s\n", failures ? "PARITY FAILED" : "PARITY OK: ported fragment is numerically identical to mtld3d");
    return failures ? 1 : 0;
  }
}
