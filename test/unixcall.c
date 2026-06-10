/* Smoke test for the d9mtmetal companion unixlib: get a Metal device
 * through DXMT's winemetal, then compile MSL source at runtime through
 * d9mtmetal's newLibraryWithSource opcode.
 * d9mtmetal.dll is loaded dynamically so load failures are reportable. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../src/d9mtmetal/d9mtmetal.h"

/* minimal winemetal imports (cdecl, undecorated) */
__declspec(dllimport) uint64_t WMTCopyAllDevices(void);
__declspec(dllimport) uint64_t NSArray_object(uint64_t array, uint64_t index);
__declspec(dllimport) uint64_t NSArray_count(uint64_t array);
__declspec(dllimport) void NSObject_release(uint64_t obj);
__declspec(dllimport) uint64_t NSObject_description(uint64_t obj);
__declspec(dllimport) uint32_t NSString_getCString(uint64_t str, char *buffer,
                                                   uint64_t maxLength,
                                                   uint64_t encoding);

static const char MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut v_main(uint vid [[vertex_id]],\n"
    "                   constant float4 *verts [[buffer(0)]]) {\n"
    "  VOut o; o.pos = verts[vid]; return o;\n"
    "}\n"
    "fragment float4 f_main() { return float4(1, 0, 1, 1); }\n";

typedef int(__cdecl *PFN_D9MT_UnixCall)(unsigned int code, void *params);

int main(void) {
  /* CX winewrapper swallows console output; write results to a file */
  FILE *out = fopen("unixcall_out.txt", "w");
  if (!out)
    return 1;
#define LOG(...) do { fprintf(out, __VA_ARGS__); fputc('\n', out); fflush(out); } while (0)

  LOG("loading d9mtmetal.dll");
  HMODULE mod = LoadLibraryA("d9mtmetal.dll");
  if (!mod) {
    LOG("FAIL: LoadLibrary error %lu", GetLastError());
    return 1;
  }
  PFN_D9MT_UnixCall call =
      (PFN_D9MT_UnixCall)GetProcAddress(mod, "D9MT_UnixCall");
  if (!call) {
    LOG("FAIL: D9MT_UnixCall not exported");
    return 1;
  }
  LOG("d9mtmetal.dll loaded, D9MT_UnixCall at %p", (void *)call);

  uint64_t devices = WMTCopyAllDevices();
  if (!devices || !NSArray_count(devices)) {
    LOG("FAIL: no Metal devices");
    return 1;
  }
  uint64_t device = NSArray_object(devices, 0);
  LOG("device handle: %llx", (unsigned long long)device);

  struct d9mt_newlibrary_params p;
  memset(&p, 0, sizeof(p));
  p.device = device;
  p.source_ptr = (uint32_t)(uintptr_t)MSL;
  p.source_len = sizeof(MSL) - 1;
  p.fast_math = 1;

  int st = call(D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE, &p);
  LOG("D9MT_UnixCall status: %d", st);
  LOG("library: %llx error: %llx", (unsigned long long)p.ret_library,
      (unsigned long long)p.ret_error);

  if (p.ret_error) {
    char buf[512] = {0};
    uint64_t desc = NSObject_description(p.ret_error);
    if (desc) {
      NSString_getCString(desc, buf, sizeof(buf), 4 /* UTF8 */);
      NSObject_release(desc);
    }
    LOG("compile error: %s", buf);
  }

  if (st == 0 && p.ret_library) {
    LOG("PASS: runtime MSL compilation through d9mtmetal works");
    return 0;
  }
  LOG("FAIL");
  return 1;
}
