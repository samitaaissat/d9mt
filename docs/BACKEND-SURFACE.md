# BACKEND-SURFACE.md — Metal backend surface for DXVK v2.7.1 d3d9 front-end

Authoritative spec of everything the d3d9 front-end (plus vendored dxso)
calls on the backend at DXVK **v2.7.1** (clone `/tmp/dxvk-src`, checked out
at release commit `c3dd74be`; tag `v2.7.1` = the same tree). Audience: the
implementer of the Metal `DxvkContext`/`DxvkDevice`. All callsites below were
inventoried from `src/d3d9`, `src/dxso` and spot-verified against the sources.

Threading ground rule that shapes everything: **every `DxvkContext` method is
invoked from exactly one thread — the CS worker** (`DxvkCsThread` executes
chunks of type-erased lambdas with a `DxvkContext*`). The app thread only
*records* lambdas (`EmitCs`) and calls `DxvkDevice` factory/wait methods.
Therefore the context needs no internal locking, but `DxvkDevice` methods
called from inside lambdas (`createSampler`, `createCommandList`,
`getSamplerStats`) and all resource methods (`createView`, `mapPtr`,
`allocateStorage`) must be safe to call from the CS thread concurrently with
the app thread.

---

## 1. DxvkContext method table

**57 distinct methods.** Stage masks are `VkShaderStageFlags` but d3d9 only
ever passes VERTEX|FRAGMENT (textures/samplers/cbuffers), GEOMETRY
(ProcessVertices SWVP emulation only), and ALL_GRAPHICS (push data).
`bindShader<Stage>` is instantiated for VERTEX, FRAGMENT, GEOMETRY only.

Metal-mapping legend (our existing machinery in src/d3d9/d3d9.cpp):
**PASS** = pass manager (splits render passes at RT/DS/sRGB changes),
**AB** = argument-buffer u64 (`gpu_resource_id`/buffer GPU address) writes,
**WMT** = wmtcmd draw encoding, **BLIT** = blit encoder copies / scale-blit
sample pass, **PSO** = SPIRV-Cross→MSL→`newRenderPipelineState` chain,
**VIS** = visibility result buffer for occlusion.

### 1.1 Draws

| Method | Args that matter | Metal mapping |
|---|---|---|
| `draw(count, const VkDrawIndirectCommand*)` | count always 1; uses vertexCount, instanceCount, firstVertex (StartVertex); firstInstance always 0 | WMT `drawPrimitives` with instanceCount; current pass encoder |
| `drawIndexed(count, const VkDrawIndexedIndirectCommand*)` | count always 1; indexCount, instanceCount, firstIndex (StartIndex), vertexOffset (BaseVertexIndex) | WMT `drawIndexedPrimitives`; vertexOffset → baseVertex; firstIndex → indexBufferOffset |

### 1.2 Render targets / dynamic raster state

| Method | Args that matter | Metal mapping |
|---|---|---|
| `bindRenderTargets(DxvkRenderTargets&&, VkImageAspectFlags feedbackLoop)` | `{DxvkAttachment depth; color[MaxNumRenderTargets]}`, each = `{Rc<DxvkImageView>}`; slots may be null; depth read-only-ness encoded in the view; feedbackLoop = COLOR/DEPTH bits when RT/DS simultaneously sampled | PASS: end current pass, stage next MTLRenderPassDescriptor. feedbackLoop bits = our hazard case → force pass split + (depth) read-only depth attachment / texture copy |
| `setViewports(count, const DxvkViewport*)` | always count=1; `{VkViewport, VkRect2D scissor}`; y-flip + half-texel offset already applied by front-end | `setViewport`/`setScissorRect`, re-applied on pass restart |
| `setBlendMode(uint32_t attachment, const DxvkBlendMode&)` | per attachment 0..3; packed bitfield: blendEnable, colorOp/alphaOp (src/dst factor + op), writeMask; pre-`normalize()`d | PSO key: per-MRT blend + write mask (we already key PSOs on attachment formats + masks) |
| `setBlendConstants(DxvkBlendConstants)` | 4 floats (D3DRS_BLENDFACTOR) | `setBlendColor` dynamic |
| `setDepthStencilState(const DxvkDepthStencilState&)` | depthTest/depthWrite/compareOp; stencil enable; front/back `DxvkStencilOp` {fail,pass,depthFail,compareOp,compareMask,writeMask} | `MTLDepthStencilState` cache keyed on the packed struct |
| `setStencilReference(uint32_t)` | single ref front+back (& 0xff) | `setStencilReferenceValue` dynamic |
| `setRasterizerState(const DxvkRasterizerState&)` | cullMode; depthClip always true; frontFace always CLOCKWISE; polygonMode fill/line/point; flatShading | `setCullMode`/`setFrontFacingWinding(.clockwise)`/`setTriangleFillMode`; flatShading is baked into shader IO (`flatShadingInputs`) → PSO key bit |
| `setDepthBias(DxvkDepthBias)` | depthBiasConstant (pre-scaled by front-end), depthBiasSlope, clamp=0 | `setDepthBias(constant, slopeScale, clamp:FLT_MAX or 0)` dynamic |
| `setDepthBiasRepresentation(DxvkDepthBiasRepresentation)` | once at device init; depends on features we advertise (see §2 features) | Advertise float representation → front-end sets m_depthBiasScale=1.0; store + no-op |
| `setDepthBounds(DxvkDepthBounds)` | NVDB hack; defaults {0,1} | No Metal equivalent — accept and ignore (don’t advertise depthBounds feature) |
| `setMultisampleState(const DxvkMultisampleState&)` | sampleMask (16-bit), alphaToCoverage; sample count comes from RT images | PSO key: alphaToCoverage; sampleMask via `[[sample_mask]]` injection or ignore if 0xffff |
| `setLogicOpState(const DxvkLogicOpState&)` | called once at init with disabled state | Accept, assert-disabled, no-op |
| `setInputAssemblyState(const DxvkInputAssemblyState&)` | packed topology + primitiveRestart=false + patchVertexCount=0; called per prim-type change inside draw lambdas; TRIANGLE_FAN appears | Store MTLPrimitiveType for WMT draws; fan → our existing triangle-fan index emulation; restart never used |
| `setInputLayout(attrCount, const DxvkVertexInput*, bindCount, const DxvkVertexInput*)` | packed unions: attribute {location:5, binding:5, format:7(VkFormat), offset:11} / binding {binding:5, extent:12, inputRate:1, divisor:14}; (0,nullptr,0,nullptr) = no decl; null stream 16 for unmatched semantics | Build `MTLVertexDescriptor` (we already map decl→isgn); streams at Metal buffer 16+binding; divisor → stepRate, INSTANCE → stepFunction; PSO key component. USCALED/SSCALED formats need emulation (see Risks) |

### 1.3 Shader / resource binding

Slot model: flat resource slots in “set 0” (§4.2), sampler heap “set 15
binding 0”, push-data block. In our Metal layout: set-0 AB = `[[buffer(0)]]`
(8-byte tier-2 slots), push block = `[[buffer(1)]]`, sampler heap u64 array =
`[[buffer(2)]]`, vertex streams 16+.

| Method | Args that matter | Metal mapping |
|---|---|---|
| `bindShader<Stage>(Rc<DxvkShader>&&)` | Stage ∈ {VERTEX, FRAGMENT, GEOMETRY}; nullptr = unbind; GEOMETRY only for SWVP emu (then unbound) | Dirty PSO key (VS/PS pair). GEOMETRY: translate SWVP GS → MSL compute/vertex-function trick or CPU path (see Risks) |
| `bindVertexBuffer(binding, DxvkBufferSlice&&, stride)` | binding 0..15 (+16 null stream); empty slice = unbind | AB-free: `setVertexBuffer(mtlBuffer, offset, index:16+binding)`; stride lives in vertex descriptor (PSO) |
| `bindIndexBuffer(DxvkBufferSlice&&, VkIndexType)` | UINT16/UINT32 only; empty slice + UINT32 = unbind | Stash buffer+offset+type for `drawIndexedPrimitives` |
| `bindUniformBuffer(stages, slot, DxvkBufferSlice&&)` | whole-buffer slice; once per cbuffer creation; slots per §4.2 | Record buffer for slot; AB u64 write of `gpuAddress+offset` at slot id |
| `bindUniformBufferRange(stages, slot, offset, length)` | HOT PATH: per-constant-upload rebind of suballocation within current backing | Update AB u64 = base gpuAddress + offset (cheap 8-byte write, matches our existing cbuffer ring) |
| `bindResourceImageView(stages, slot, Rc<DxvkImageView>&&)` | stages always VERTEX\|FRAGMENT; PS slots 13-28, VS 6-9; nullptr unbinds; view carries sRGB choice + swizzle | AB u64 write of `MTLTexture.gpuResourceID` at slot; swizzle baked into the cached view texture |
| `bindResourceSampler(stages, slot, Rc<DxvkSampler>&&)` | same slots as image views; sampler created inside the lambda via `device->createSampler(key)` | Sampler heap: write `MTLSamplerState.gpuResourceID` u64 into heap array at the sampler’s heap index; the heap index is what gets written into push-block dwords (§4.5) |
| `bindResourceBufferView(stages, slot, Rc<DxvkBufferView>&&)` | ProcessVertices only: GEOMETRY, slot 30, formatless STORAGE view over dst VB; nullptr after | AB u64 buffer address (only needed when SWVP path implemented) |
| `pushData(stages, offset, size, const void*)` | ALL_GRAPHICS + offsetof(D3D9RenderStateInfo field) (≤52 bytes) — shared block; GEOMETRY offset 0 size 4 = SWVP dest byteOffset (local block) | memcpy into our 96-byte render_state CPU shadow, upload region of `[[buffer(1)]]` per draw/pass |
| `setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, index, value)` | indices 0..5 (D3D9SpecializationInfo, 6 dwords) | PSO key: MTLFunctionConstantValues. CRITICAL (hard-won): ALL function constants must be supplied or PSO creation crashes silently. Set gate spec id 12 “optimized” = true (we always compile per-value) |

### 1.4 Clears / copies / blits / resolves

| Method | Args that matter | Metal mapping |
|---|---|---|
| `clearRenderTarget(view, clearAspects, VkClearValue, discardAspects)` | full-RT/DS clears; discardAspects always 0 | PASS: if view == pending attachment → loadAction Clear; else standalone clear pass (loadAction Clear + storeAction Store, empty encoder) |
| `clearImageView(view, VkOffset3D, VkExtent3D, aspect, VkClearValue)` | partial-rect clears; includes R32G32(B32A32)_UINT storage view aliasing BC blocks | Scissored clear: fullscreen-tri pass writing clear color, or compute fill for the BC-alias case |
| `copyBuffer(dst, dstOff, src, srcOff, bytes)` | staging→VB upload; ProcessVertices readback | BLIT: `copyFromBuffer:toBuffer:` |
| `copyBufferToImage(dst, subres, dstOffset, dstExtent, srcBuf, srcOff, rowAlign=0, sliceAlign=0, srcFormat)` | tight packing; srcFormat = packed D24S8/D32S8 (interleaved depth+stencil!) or UNDEFINED (= image format) | BLIT `copyFromBuffer:toTexture:`; packed DS formats need CPU/compute de-interleave into depth + stencil planes (Depth32Float_Stencil8) |
| `copyImageToBuffer(dstBuf, dstOff, rowAlign=4, sliceAlign=0, dstFormat, src, subres, srcOffset, srcExtent)` | readback (GetRenderTargetData, Lock, GetFrontBufferData); dstFormat packed-DS or UNDEFINED | BLIT `copyFromTexture:toBuffer:` honoring rowAlign 4; DS re-interleave on packed formats |
| `copyImage(dst, dstSub, dstOffset, src, srcSub, srcOffset, extent)` | same-size StretchRect fast path, ResolveZ; formats may differ but size-compatible | BLIT texture→texture when formats identical; else texture-view alias or sample pass (our StretchRect machinery) |
| `blitImageView(dstView, VkOffset3D[2], srcView, VkOffset3D[2], VkFilter)` | scaling/format-converting StretchRect; 2D_ARRAY views with packedSwizzle; can mirror (offsets) | Our fullscreen-triangle sample pass (blit_vs/ps), honoring swizzle + filter |
| `resolveImage(dst, src, VkImageResolve, format, mode, stencilMode)` | modes used: (AVERAGE,SAMPLE_ZERO), (default,SAMPLE_ZERO), (SAMPLE_ZERO,SAMPLE_ZERO), (AVERAGE,NONE); format = src format | Pass with MSAA source: storeAction MultisampleResolve, or resolve sample pass for SAMPLE_ZERO depth (`sample` at sample 0) |
| `generateMipmaps(view, VkFilter)` | autogen from level 0 | BLIT `generateMipmaps:` (our existing GPU mipgen) — linear only; point filter ignored |
| `initBuffer(buffer)` | zero-init device-local buffer at creation | BLIT `fillBuffer:range:value:0` |
| `initImage(image, VK_IMAGE_LAYOUT_UNDEFINED)` | zero/clear-init device-local image (also new backbuffers) | Clear pass per subresource, or fillBuffer on a temp + copy; cheapest: render-pass loadAction Clear black |

### 1.5 Layout / hazard / barrier

| Method | Args that matter | Metal mapping |
|---|---|---|
| `changeImageLayout(image, layout)` | hazard mode switch: GENERAL / ATTACHMENT_FEEDBACK_LOOP_OPTIMAL | Set per-image “hazard tracking” flag; no GPU work |
| `transformImage(image, subresRange, srcLayout, dstLayout)` | explicit transition (TransformImage helper + interop) | No-op (metadata only) |
| `ensureImageCompatibility(image, const DxvkImageUsageInfo&)` | adds usage/flags (MUTABLE_FORMAT/EXTENDED_USAGE/BLOCK_TEXEL_VIEW_COMPATIBLE), viewFormats, layout, colorSpace; return bool ignored | Create all MTLTextures permissive up front (renderTarget+shaderRead+shaderWrite+pixelFormatView where legal) → return true unconditionally |
| `emitGraphicsBarrier(srcStages, srcAccess, dstStages, dstAccess)` | only RT/DS-write → fragment-read feedback hazards between draws | PASS split (end + restart encoder) — this is exactly the GTA IV INTZ hazard fix |

### 1.6 Queries / sync / frame

| Method | Args that matter | Metal mapping |
|---|---|---|
| `beginQuery(Rc<DxvkQuery>)` | occlusion only (PRECISE) | VIS: assign visibility-buffer slot, `setVisibilityResultMode(.counting)`; must survive pass splits — allocate a new slot per encoder and sum |
| `endQuery(Rc<DxvkQuery>)` | occlusion end; available after submit | VIS: stop counting; resolve on command-buffer completion |
| `writeTimestamp(Rc<DxvkQuery>)` | TIMESTAMP / TIMESTAMPDISJOINT | `sampleTimestamps` / cmdbuf GPUEndTime, in ns consistent with advertised timestampPeriod=1.0 |
| `signalGpuEvent(Rc<DxvkEvent>)` | D3DQUERYTYPE_EVENT | Flip event in command-buffer completion handler |
| `signal(Rc<sync::Signal>, uint64_t)` | m_submissionFence (submission id) + m_stagingBufferFence (staging bytes); MUST fire at submission completion, not record time | `addCompletedHandler` → `signal->signal(value)` (sync::Fence is plain mutex+condvar, vendored) |
| `flushCommandList(const VkDebugUtilsLabelEXT* reason, DxvkSubmitStatus* status)` | reason always nullptr; status non-null only on 9on12 path | Commit current MTLCommandBuffer, start a fresh one; set `status->result` when queued; wire completion handlers (signals, queries, present) |
| `beginRecording(Rc<DxvkCommandList>)` | exactly once at device init with `device->createCommandList()` | Adopt/prime initial command-list object |
| `beginExternalRendering()` → `Rc<DxvkCommandList>` | suspends internal render pass, returns live cmd list for D3D9FormatHelper + swapchain blitter | End current encoder; return our DxvkCommandList wrapper exposing the 5 methods in §1.8 + an encoder-capable handle for our Metal blitter |
| `synchronizeWsi(PresenterSync)` | attach acquire/present sync to next submit | With our presenter, PresenterSync can be opaque/empty — drawable scheduling handled internally |
| `endFrame()` | frame-boundary bookkeeping | Rotate per-frame pools/rings; near-no-op |
| `beginLatencyTracking(tracker, frameId)` / `endLatencyTracking(tracker)` | only if tracker non-null | Stub (createLatencyTracker returns nullptr) |
| `beginDebugLabel` / `endDebugLabel` / `insertDebugLabel` | PIX events (name+color) | `pushDebugGroup`/`popDebugGroup`/`insertDebugSignpost` or no-op |
| `invalidateBuffer(buffer, Rc<DxvkResourceAllocation>&&)` | CRITICAL HOT PATH: buffer renaming (DISCARD, cbuffer/UP ring). New allocation already CPU-written by app thread before this executes | Swap the buffer’s current backing (MTLBuffer suballocation); old backing kept alive by Rc until GPU done |

### 1.7 Default-state calls at device init

`beginRecording(device->createCommandList())`, `setLogicOpState({})`,
`setDepthBiasRepresentation(...)` — must be accepted before any draw.

### 1.8 DxvkCommandList methods (via `beginExternalRendering()`)

Used by D3D9FormatHelper (YUY2/UYVY/NV12/YV12/L6V5U5/X8L8V8U8/A2W10V10U10/
W11V11U10 upload conversion) and the swapchain blitter:

| Method | Use | Metal mapping |
|---|---|---|
| `cmdBindPipeline(ExecBuffer, COMPUTE, VkPipeline)` | conversion compute pipeline | Compute encoder + our PSO handle (see §2 createBuiltInComputePipeline) |
| `bindResources(ExecBuffer, layout, n, DxvkDescriptorWrite[], pushSize, &extent)` | {STORAGE_IMAGE dst view, UNIFORM_TEXEL_BUFFER src view} + push extent | `setTexture`/`setBuffer` + `setBytes` on compute encoder |
| `cmdDispatch(ExecBuffer, (w+7)/8, (h+7)/8, 1)` | conversion dispatch | `dispatchThreadgroups` |
| `cmdPipelineBarrier(ExecBuffer, const VkDependencyInfo*)` | compute-write → consumer barrier | End compute encoder (Metal cross-encoder ordering suffices) |
| `track(resource, DxvkAccess::Read/Write)` | lifetime + hazard tracking | Retain resource on command list; mark GPU use for `waitForResource` |

Swapchain lambda additionally calls `blitter->setCursorPos(VkRect2D)` and
`blitter->present(cmdList objects, dstView, dstRect, srcView, srcRect)` — the
blitter is backend code we replace (§5.2).

---

## 2. DxvkDevice surface

Entry: `Singleton<DxvkInstance>` acquired with
`DxvkInstanceFlag::ClientApiIsD3D9` → `instance->enumAdapters(i)` →
`adapter->createDevice()` (**no arguments**, throws DxvkError on failure).

### 2.1 DxvkInstance
| Member | Contract |
|---|---|
| `DxvkInstance(DxvkInstanceFlags)` | one global object; flag only affects front-end behavior |
| `adapterCount()` / `enumAdapters(i)` | return our single Metal adapter; nullptr past end |
| `config()` | `util::Config` (vendored util) — parsed dxvk.conf/env |
| `handle()` / `getExtensionList()` | interop-only → return null/empty |

### 2.2 DxvkAdapter
| Member | Contract |
|---|---|
| `createDevice()` | build the whole Metal device |
| `deviceProperties()` | `DxvkDeviceInfo&` — see consumed fields below |
| `features()` | only `core.features.depthBounds` read adapter-side (caps) — report false |
| `memoryProperties()` | only `memoryHeapCount` + `memoryHeaps[i].size` read (GetAvailableTextureMem) — one unified heap, recommend ≥3GB (current driver reports 3GB for draw distance) |
| `getFormatFeatures(VkFormat)` | `{optimal, linear, buffer}` — only optimal\|linear read; drive from a VkFormat→MTLPixelFormat capability table |
| `matchesDriver(...)` | return false (conservative option defaults) |
| `handle()` / `vki()` | interop/Vulkan-presenter only — stub |

### 2.3 DxvkDevice — 27 methods used by d3d9

Queries:
| Method | Contract |
|---|---|
| `adapter()` / `instance()` | back-pointers |
| `handle()` / `vkd()` / `queues()` | interop + FormatHelper teardown only — stub (reimplement FormatHelper natively) |
| `config()` | `DxvkOptions&`; only `latencySleep` read |
| `debugFlags()` | only `DxvkDebugFlag::Markers` tested — return empty |
| `perfHints()` | only `preferRenderPassOps` read — recommend **true** (deferred clears suit our pass manager) |
| `features()` | fields read: `core.features.vertexPipelineStoresAndAtomics` (+`vk12.shaderInt8`) gates SWVP; `extRobustness2.robustBufferAccess2`; `extGraphicsPipelineLibrary.graphicsPipelineLibrary` (**report false** → no GPL/spec-UBO path); `extDepthBiasControl.*`; `extAttachmentFeedbackLoopLayout.attachmentFeedbackLoopLayout` (**report true** → hazard layout = feedback-loop flag, drives the hazard fix); `core.features.depthBounds` false |
| `properties()` | fields read: vendorID/deviceID/deviceName; limits.framebufferColor/DepthSampleCounts, pointSizeRange[1], timestampPeriod (=1.0 ⇒ ns), minUniformBufferOffsetAlignment / minStorageBufferOffsetAlignment (use 256/16 or our AB constraints), maxUniformBufferRange (dxso SSBO decision — keep ≥ 64KB so float constants stay UBO); vk11.deviceLUID/UUID; extRobustness2 alignments |
| `getFormatLimits(const DxvkFormatQuery&)` | input {format,type,tiling,usage,flags}; output maxExtent/maxArrayLayers/maxMipLevels/sampleCounts; nullopt = unsupported config |
| `getShaderPipelineStages()` | VERTEX\|FRAGMENT\|COMPUTE (no geom/tess) |
| `getSamplerStats()` | `{liveCount}`; polled on CS thread right after createSampler + in exhaustion loop vs `DxvkSamplerPool::MaxSamplerCount` — keep liveCount honest and small (Metal samplers effectively unlimited → return small constant) |

Factories:
| Method | Contract |
|---|---|
| `createContext()` | the Metal DxvkContext (single CS-thread consumer) |
| `createCommandList()` | command-list wrapper (MTLCommandBuffer + tracked-resource set) |
| `createBuffer(DxvkBufferCreateInfo, VkMemoryPropertyFlags)` | §3.2; must echo memFlags() back |
| `createImage(DxvkImageCreateInfo, VkMemoryPropertyFlags)` | always DEVICE_LOCAL; throws on failure |
| `createSampler(const DxvkSamplerKey&)` | **called on CS thread inside lambdas** — thread-safe, dedupe-cached MTLSamplerState; heap-index assignment for the sampler heap |
| `createGpuEvent()` / `createGpuQuery(type, flags, index)` | only (OCCLUSION, PRECISE, 0) and (TIMESTAMP, 0, 0) |
| `createLatencyTracker(presenter)` | return nullptr (all uses null-guarded) |
| `createBuiltInPipelineLayout(flags=0, COMPUTE, pushSize=sizeof(VkExtent2D), 2 bindings {STORAGE_IMAGE, UNIFORM_TEXEL_BUFFER})` / `createBuiltInComputePipeline(layout, DxvkBuiltInShaderStage{SPIR-V code,size,spec})` | FormatHelper only: 7 embedded SPIR-V compute shaders → run through SPIRV-Cross MSL once at device init (spec constant id 0 distinguishes YUY2/UYVY); return opaque PSO handle |

Shader registry:
| Method | Contract |
|---|---|
| `registerShader(Rc<DxvkShader>)` | hook for pre-compile; may no-op (we compile at PSO creation) |
| `requestCompileShader(Rc<DxvkShader>)` | called when `shader->needsLibraryCompile()` — make that return false → never called |

Submission/sync:
| Method | Contract |
|---|---|
| `presentImage(presenter, tracker, frameId, status=nullptr)` | called inside the CS present lambda after flushCommandList; enqueue present; must eventually drive `presenter->signalFrame(frameId)` (§5.1 liveness) |
| `waitForSubmission(DxvkSubmitStatus*)` | block until `status->result != VK_NOT_READY`; only 9on12 path |
| `waitForFence(sync::Fence&, uint64_t)` | block until timeline value reached (staging throttle); fence signaled via ctx->signal |
| `waitForResource(const DxvkPagedResource&, DxvkAccess)` | core Lock() sync: block until GPU finished using resource (Read ⇒ wait writers; Write ⇒ wait any). Requires per-resource use tracking via command-list `track` + completion |
| `waitForIdle()` | full drain; only in device dtor |
| `lockSubmission()` / `unlockSubmission()` | interop-only — no-op/mutex |

NOT called by d3d9 (backend-internal): `submitCommandList`, `getMemoryStats`,
`hasDedicatedTransferQueue`, `canUseGraphicsPipelineLibrary`.

### 2.4 Backend-owned helper ctors taking the device

Must exist and link: `DxvkCsThread(device, ctx)`; `DxvkStagingBuffer(device,
4MiB)` + `.alloc(size)→DxvkBufferSlice` + `.getStatistics().allocatedTotal`;
`D3D9FormatHelper(device)` (front-end TU, but depends on builtin-pipeline
device methods); `Presenter(device, Rc<sync::Signal>, PresenterDesc, surfaceProc)`;
`hud::Hud::createHud(device)` → nullptr OK; `new DxvkSwapchainBlitter(device,
hud)`. dxso reads `device->features().extRobustness2.robustBufferAccess2` and
`adapter()->deviceProperties().core.properties.limits.maxUniformBufferRange`
(dxso_options.cpp) — keep these fields real.

---

## 3. Resource create-info field tables

All objects intrusive-refcounted (`Rc<>`); CS lambdas capture Rc copies so
objects outlive D3D9 Release while GPU work pending.

### 3.1 DxvkImageCreateInfo — fields WRITTEN by d3d9
| Field | Values seen | Metal note |
|---|---|---|
| type | 2D / 3D | MTLTextureType2D(Array)/3D/Cube from type+flags+numLayers |
| format | mapped D3D9 format (+conversion targets) | VkFormat→MTLPixelFormat table; all depth unified on Depth32Float_Stencil8 (current design) |
| flags | 0 \| MUTABLE_FORMAT (srgb pair) \| CUBE_COMPATIBLE | MUTABLE → allocate with pixelFormatView usage |
| sampleCount | 1 or DecodeMultiSampleType | textureType2DMultisample |
| extent / numLayers / mipLevels | as requested | — |
| usage | always TRANSFER_SRC\|DST; +SAMPLED unless attachment-only; +COLOR_ATTACHMENT (RT/autogen); +DEPTH_STENCIL_ATTACHMENT; +STORAGE (conversion formats); +ATTACHMENT_FEEDBACK_LOOP (hazard textures) | map to MTLTextureUsage; recommend permissive (shaderRead+renderTarget) to make ensureImageCompatibility trivial |
| stages / access | metadata | store only |
| tiling | OPTIMAL (LINEAR fallback when getFormatLimits rejects) | ignore |
| layout | GENERAL / OptimizeLayout(usage) / SHADER_READ_ONLY (swapchain helpers) | metadata |
| shared / sharing{type,mode,handle} | backbuffers, shared handles, conversion formats | shared-handle path stubbable; `sharing.mode==None` checks must work |
| viewFormatCount=2, viewFormats | color+srgb pair, pointer into front-end mapping — **copy at create** | pre-create or lazily create sRGB view alias |

Fields READ BACK via `info()` (must round-trip verbatim): format, sampleCount,
extent, stages, access, usage, numLayers, sharing.mode, colorSpace.
`CreateResolveImage` copies `m_image->info()` wholesale and sets sampleCount=1.

### 3.2 DxvkImage methods used
`info()`, `createView(DxvkImageViewKey)` (keyed, cached, callable from CS
thread), `formatInfo()` (aspectMask, BlockCompressed flag), `mipLevelExtent(n)`,
`getMemoryInfo().size` (pool accounting), `sharedHandle()` (stub),
`handle()` (interop, stub).

### 3.3 DxvkImageViewKey (defined in dxvk_memory.h)
viewType (2D/2D_ARRAY/3D/CUBE), usage (single bit: SAMPLED / COLOR_ATTACHMENT /
DEPTH_STENCIL_ATTACHMENT / TRANSFER_SRC / TRANSFER_DST / STORAGE), format,
layout (UNDEFINED sample views; COLOR_ATTACHMENT_OPTIMAL RTVs;
DEPTH_STENCIL_ATTACHMENT_OPTIMAL / **DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL**
DSVs — the read-only-depth distinction MUST be honored for sample-while-test;
GENERAL storage), aspects, mipIndex/mipCount (u8), layerIndex/layerCount (u16),
packedSwizzle (4 bits/component; zeroed for DSVs). Metal: cache
`newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:` per key.

### 3.4 DxvkBufferCreateInfo — fields written
size, usage, stages, access, debugName. Usage combos: VB =
VERTEX(+STORAGE if SWVP)(+TRANSFER_DST); IB = INDEX(+TRANSFER_DST); staging =
TRANSFER_SRC; texture mapping buffer = TRANSFER_SRC|DST|STORAGE
(+UNIFORM_TEXEL_BUFFER for conversion formats); constant buffer =
UNIFORM|STORAGE; UP ring = VERTEX|INDEX.

### 3.5 DxvkBuffer methods used
`mapPtr(offset)` (persistent CPU pointer into **current** backing),
`allocateStorage()` → fresh `Rc<DxvkResourceAllocation>` (DISCARD renaming;
new mapPtr written by app thread BEFORE ctx->invalidateBuffer runs on CS
thread), `storage()`, `memFlags()` (initializer branches HOST_VISIBLE ⇒ CPU
memset vs ctx->initBuffer), `createView(DxvkBufferViewKey{format,usage,offset,
size})`, `info()`.

### 3.6 DxvkBufferSlice / DxvkResourceAllocation
Slice = value type {Rc<DxvkBuffer>, offset, length}; ctors (), (buf),
(buf,off,len); methods buffer(), offset(), length(), subSlice(), mapPtr().
Empty slice = valid null binding. Allocation = refcounted backing owning an
MTLBuffer suballocation + CPU pointer; only `mapPtr()` called from d3d9; old
allocations stay alive via Rc while in flight.

### 3.7 Memory-type requests (createImage/createBuffer 2nd arg)
Images: always DEVICE_LOCAL ⇒ MTLStorageModePrivate (depth textures MUST be
private — no replaceRegion). Buffers: HOST_VISIBLE|COHERENT(|CACHED)(|
DEVICE_LOCAL) combos ⇒ all MTLStorageModeShared on Apple Silicon, but
`memFlags()` must echo the requested flags (initializer branches on
HOST_VISIBLE). DEVICE_LOCAL-only buffers (BUFFER map mode real buffer) may be
Private.

### 3.8 DxvkSamplerKey
Setters used: `setFilter(min,mag,mip)`, `setAddressModes(u,v,w)` (border flag
auto), `setLegacyCubeFilter(bool)` (forces CLAMP_TO_EDGE + non-seamless),
`setDepthCompare(bool, LESS_OR_EQUAL)`, `setAniso(0..16)`,
`setLodRange(minLod=maxMipLevel, 16.0, lodBias)`; `borderColor.float32`
written directly; `u.p.hasBorder` read. Map to MTLSamplerDescriptor
(border color: Metal supports only transparent/opaque black/white — snap;
compareFunction for shadow samplers; non-seamless cube unavailable — accept).

### 3.9 DxvkImageUsageInfo (ensureImageCompatibility)
usage, flags (MUTABLE_FORMAT|EXTENDED_USAGE), stages, access, layout(GENERAL),
viewFormatCount/viewFormats (pointer to a local — copy!), colorSpace.
Permissive up-front creation makes this `return true`.

---

## 4. Shader / pipeline-state surface

### 4.1 DxvkShader
Sole ctor: `DxvkShader(const DxvkShaderCreateInfo&, SpirvCodeBuffer&&)`.
Three producers: dxso (SM1-3 VS/PS), fixed-function generator (VS/PS),
SWVP emulator (GS). Methods called: `setShaderKey({stage, Sha1Hash})`,
`dump(ostream)`, `debugName()`, `needsLibraryCompile()` (→ return false).
Backend consumes the SPIR-V via our SPIRV-Cross→MSL chain at PSO creation;
metadata derived from create-info, not reflection.

### 4.2 Binding model (set 0 flat slots)
`computeResourceSlotId`: stageOffset = (VSCount=6 + MaxTexturesVS=4) × stage.
| Slot | Binding |
|---|---|
| 0/1/2 | VS float/int/bool cbuffers (HWVP: single UBO `c` at 0; SWVP: 3 buffers, float/int optionally SSBO) |
| 3 | VS clip planes UBO {vec4[6]} |
| 4 | VS fixed-function UBO (D3D9FixedFunctionVS) |
| 5 | VS vertex-blend SSBO (FF, row-major mat4 runtime array) |
| 6-9 | VS images (vertex texture fetch) |
| 10 | PS cbuffer |
| 11 | PS fixed-function UBO (textureFactor) |
| 12 | PS shared UBO (bump-env) |
| 13-28 | PS images (16) |
| 30 | SWVP dest storage buffer (`getSWVPBufferSlot()`) |
| 31 | spec_state UBO (`getSpecConstantBufferSlot()`) — only on GPL path; report GPL=false and it is still *declared* by shaders but read only when spec id 12 is false |

DxvkBindingInfo fields used: set(0), binding, resourceIndex, descriptorType
(UNIFORM_BUFFER/STORAGE_BUFFER/SAMPLED_IMAGE/SAMPLER), descriptorCount(1),
viewType (SAMPLED_IMAGE; VK_IMAGE_VIEW_TYPE_MAX_ENUM when sampler type is
spec-resolved — SM<2: shader declares 2D+shadow/3D/cube variants selected by
spec constant), access, flags (UniformBuffer; **PushData for samplers**),
blockOffset (samplers: 64+2·idx). NB the SWVP GS storage buffer leaves
access=0 despite being written.

### 4.3 Sampler heap (set 15, binding 0)
`DxvkShaderCreateInfo::samplerHeap = (VK_SHADER_STAGE_ALL, 15, 0)`; SPIR-V
runtime array of OpTypeSampler indexed by u16 heap indices extracted from
push-data dwords (OpBitFieldUExtract). Our mapping (verified working): heap =
raw u64 `gpuResourceID` array at `[[buffer(2)]]`; backend writes each bound
sampler’s heap index into the push-block dword indicated by the SAMPLER
descriptor’s blockOffset.

### 4.4 Push data
Shared block: offset 0, **52 bytes** = D3D9RenderStateInfo (fogColor[3]@0,
fogScale@12, fogEnd@16, fogDensity@20, alphaRef@24, pointSize@28,
pointSizeMin@32, pointSizeMax@36, pointScaleA@40, pointScaleB@44,
pointScaleC@48), updated piecemeal via `pushData(ALL_GRAPHICS, fieldOffset,
len, data)`. Local per-stage blocks start at byte 64 (MaxSharedPushDataSize):
sampler heap-index dwords, samplerDwordCount=(samplerCount+1)/2 (dxso VS=5→3
dwords, dxso PS=16→8, FF PS=8→4, FF VS=0); resourceMask marks
backend-rewritten dwords. SWVP GS local block = (64, 4, 4, 0) holding dest
byteOffset. Limits: MaxTotalPushDataSize=256, MaxPerStagePushDataSize=32.
Our `[[buffer(1)]]` render_state block (96B) already implements this layout.

### 4.5 Spec constants
`D3D9SpecializationInfo`: 6 dwords (ids 0..5): dw0 SamplerType; dw1
SamplerDepthMode+AlphaCompareOp+PointMode+VertexFogMode+PixelFogMode+
FogEnabled; dw2 SamplerNull+ProjectionType+AlphaPrecisionBits; dw3 VS/PS
bools; dw4 Fetch4; dw5 DrefClamp+ClipPlaneCount. Gate bool at spec id 12
(MaxNumSpecConstants): true ⇒ use spec dwords, false ⇒ read spec_state UBO
slot 31. **Metal contract: supply ALL function constants (0..5 and 12) at
every PSO creation; set id 12 = true.** First 20 bytes (MaxUBODwords=5)
mirrored to m_specBuffer only when GPL — disabled for us.

### 4.6 Render-state value structs (see §1.2 table for semantics)
DxvkInputAssemblyState (topology:4, restart:1, patch:6 packed u16);
DxvkRasterizerState; DxvkMultisampleState; DxvkBlendMode ×4 + constants;
DxvkDepthStencilState/DxvkStencilOp; DxvkDepthBias(+Representation);
DxvkDepthBounds; DxvkVertexInput packed unions (attr: location:5/binding:5/
format:7/offset:11 — format compressed to 7 bits, offset ≤2047; binding:
binding:5/extent:12/inputRate:1/divisor:14). `DecodeDecltype` emits
USCALED/SSCALED (UBYTE4, SHORT2/4, UDEC3) — no Metal equivalent (see Risks).

### 4.7 Metal PSO key (what must key a pipeline)
VS+PS DxvkShader pair, vertex input layout, attachment formats (color×4 +
depth) + sample count, per-MRT blend+writeMask, alphaToCoverage, topology
class, 6 spec dwords, flat-shading bit. Dynamic (no rebuild): viewport/
scissor, depth-stencil state object, stencil ref, blend color, depth bias,
buffer/texture/sampler bindings, push data.

### 4.8 D3D9FormatHelper compute path
7 SPIR-V blobs (yuy2_uyvy[spec id0 =0/1], l6v5u5, x8l8v8u8, a2w10v10u10,
w11v11u10, nv12, yv12) compiled via createBuiltInComputePipeline, dispatched
through raw DxvkCommandList calls (§1.8). Translate blobs once via
SPIRV-Cross MSL at device init, or stub ConvertFormat (loses YUV video
surfaces only).

---

## 5. Swapchain / query / CS-thread contract

### 5.1 Present flow + liveness (DEADLOCK-CRITICAL)
Per `D3D9SwapChainEx::PresentImage`: EndFrame+Flush → **app thread**
`presenter->acquireNextImage(sync, backBufferImage)` → one EmitCs lambda:
`ensureImageCompatibility(colorSpace)` → `beginExternalRendering()` →
`blitter->present(...)` → `synchronizeWsi(sync)` → `flushCommandList(nullptr,
nullptr)` → `device->presentImage(presenter, tracker, frameId, nullptr)` →
FlushCsChunk → SyncFrameLatency waits `sync::Fence frameLatencySignal` to
`frameId - actualLatency` → backbuffer rotation (D3D9Surface::Swap).

**Liveness contract:** the backend MUST `presenter->signalFrame(frameId)` →
`m_signal->signal(frameId)` on the ctor Signal after each presented frame
(DXVK does it on the submission thread, post fps-limit) — otherwise
SyncFrameLatency deadlocks after maxLatency (≤ min(3, cap, backbuffers+1))
frames. Likewise `ctx->signal(m_submissionFence, id)` recorded by
ExecuteFlush must fire on submission completion or GpuFlushTracker /
WaitStagingBuffer stall forever.

### 5.2 Presenter replacement (CAMetalLayer)
Replace `Presenter` wholesale. Surface the front-end touches:
`acquireNextImage(PresenterSync&, Rc<DxvkImage>&)` (returned image must
support `info()`/`createView`; this is the presenter’s OWN image — d3d9
backbuffers are ordinary D3D9Surface RTs blitted into it; keep our proxy
texture design: acquire returns the proxy, Present blits proxy→drawable,
layer framebuffer_only=false); `setSyncInterval`, `setSurfaceExtent`,
`setSurfaceFormat`, `setFrameRateLimit`, `setHdrMetadata`,
`supportsColorSpace` (SRGB_NONLINEAR true, else false), `invalidateSurface`,
`destroyResources` (blocking). PresenterSync can be redefined as an empty
struct — only our presenter+context read it. Backbuffer formats requested:
B8G8R8A8_UNORM, R8G8B8A8_UNORM, A2R10G10B10/A2B10G10R10_PACK32,
B5G5R5A1_PACK16, B5G6R5_PACK16, R16G16B16A16_SFLOAT.

`DxvkSwapchainBlitter(device, hud)`: reimplement as Metal blit/sample pass.
Methods: `present(cmdListObjects, dstView, dstRect, srcView, srcRect)`
(scaling, MSAA src, gamma LUT, cursor composite), `setGammaRamp(cpCount,
DxvkGammaCp[256]{u16 rgba})` (0 disables — windowed path always disables),
`setCursorTexture(VkExtent2D, B8G8R8A8_SRGB, data)`, `setCursorPos(VkRect2D)`
— setCursorPos called on **CS thread**, setGammaRamp/setCursorTexture on app
thread ⇒ internal mutex.

### 5.3 Queries
Created: OCCLUSION (precise), TIMESTAMP, TIMESTAMPDISJOINT (2 timestamps,
disjoint = t0 > t1), TIMESTAMPFREQ (CPU-side: 1e9/timestampPeriod), EVENT,
VCACHE (faked front-end). Polled non-blocking, never waited:
`DxvkQuery::getData(DxvkQueryData&)` → {Invalid, Pending, Available, Failed};
reads `occlusion.samplesPassed` (u64) / `timestamp.time`. `DxvkEvent::test()`
→ {Invalid, Pending, Signaled}. **Status must stay Pending until the
flushCommandList submission containing end/signal retires** — front-end spins
GetData+ConsiderFlush; if flush doesn’t really submit, games hang. Occlusion
spans pass splits: VIS buffer slot per encoder, sum at completion.

### 5.4 CS thread machinery (vendor verbatim)
`dxvk_cs.{h,cpp}` is self-contained: `DxvkCsThread(device, ctx)`,
`dispatchChunk` (Ordered) → seq, `injectChunk(Ordered|HighPriority, chunk,
sync)` (HighPriority drained first: image init, cursor, latency teardown),
`synchronize(seq|All)`, `lastSequenceNumber()`. Only backend deps:
`DxvkDevice::addStatCtr` / `DxvkContext::addStatCtr` (no-op OK), Rc-compat,
util thread. Sequence numbers = CS-execution progress, NOT GPU completion
(GPU tracked via sync::Signal + resource use tracking). Bring-up can run
chunks synchronously (dispatch = execute inline) per PLAN.md.

### 5.5 Flush path
`ExecuteFlush`: EmitCs{signal(submissionFence, ++id); signal(stagingFence,
bytes); flushCommandList(nullptr, statusOrNull)}; GpuFlushTracker
(util_flush, vendored) decides implicit flush cadence. `DxvkSubmitStatus` =
{atomic<VkResult>}; `waitForSubmission` trivial for us.

---

## 6. Build closure (mingw i686, -std=c++17)

### 6.1 Headers by directory (212-file closure)
| Dir | Count | Role |
|---|---|---|
| src/d3d9 | 44 h (+36 cpp) | front-end — vendor whole dir (already 44 headers vendored) |
| src/dxvk | 64 h + hud/dxvk_hud{,_font,_item,_renderer}.h | THE interface surface — headers kept, .cpp replaced by Metal impls (dxvk_{context,device,instance,adapter,image,buffer,memory,sampler,shader,cmdlist,cs,staging,presenter,swapchain_blitter,gpu_query,gpu_event,latency,limits,constant_state,pipelayout,format,...}.h) |
| src/dxso | 14 h | vendored, compiled, working — do not break |
| src/dxbc | 2 h (dxbc_tag.h, dxbc_include.h) | header-only via dxso_reader.h; NO dxbc TUs |
| src/spirv | 5 h | code buffer/module |
| src/util | 41 h | thread, bit, rc, sync, com, config, log, sha1, math... |
| src/vulkan | 3 h (vulkan_loader/names/util.h) | type definitions only on Metal path |
| src/wsi | 3 h (wsi_edid/monitor/window.h) | win32 wsi kept |
| external | Vulkan-Headers + SPIRV-Headers | already vendored: `vendor/dxvk/include/{vulkan,spirv}/include` |

### 6.2 TUs to compile
- **d3d9**: all 36 .cpp (incl. d3d9_format_helpers.cpp — needs generated
  headers, see 6.3; d3d9_hud.cpp needs hud headers or drop it + the
  addItem calls at d3d9_swapchain.cpp:1082-1092).
- **dxso**: all 13 .cpp (already compiled in d9mt).
- **spirv**: spirv_code_buffer.cpp, spirv_compression.cpp, spirv_module.cpp.
- **util (18)**: thread.cpp, util_env.cpp, util_string.cpp, log/log.cpp
  (+log_debug.cpp), config/config.cpp, sha1/sha1.c (**plain C — gcc not
  g++**) + sha1/sha1_util.cpp, sync/sync_recursive.cpp, com/com_guid.cpp,
  com/com_private_data.cpp, util_flush.cpp, util_gdi.cpp, util_luid.cpp,
  util_matrix.cpp, util_shared_res.cpp; util_fps_limiter.cpp + util_sleep.cpp
  only if Metal presenter reuses FpsLimiter. OMIT com_destruction_notifier.cpp.
- **wsi (4)**: wsi_platform.cpp, win32/wsi_platform_win32.cpp,
  win32/wsi_monitor_win32.cpp, win32/wsi_window_win32.cpp (its `createSurface`
  becomes unreachable with our presenter); wsi_edid.cpp → **stub
  parseColorimetryInfo → std::nullopt** (avoids libdisplay-info).
- **vulkan**: none needed for pure-Metal backend (vulkan_names.cpp only if
  any retained code streams Vk enums).
- **Metal backend**: our implementations of every src/dxvk class in §1-§5.

### 6.3 Generated headers
- `version.h` (`#define DXVK_VERSION "v2.7.1"`) + `buildenv.h` — only needed
  by dxvk_instance.cpp/hud_item.cpp; create trivially if those TUs kept.
- **7 glslang SPIR-V array headers** `d3d9_convert_{yuy2_uyvy,l6v5u5,
  x8l8v8u8,a2w10v10u10,w11v11u10,nv12,yv12}.h` for d3d9_format_helpers.cpp
  (`glslang --quiet --target-env vulkan1.3 --vn <name>` over
  src/d3d9/shaders/*.comp) — the ONLY generated dep inside a d3d9 TU.
  Pre-generate once and check in, or stub D3D9FormatHelper::ConvertFormat.

### 6.4 Defines / flags / libs
Required: `-DNOMINMAX -D_WIN32_WINNT=0xa00 -DDXVK_WSI_WIN32`.
Auto (no -D): VK_USE_PLATFORM_WIN32_KHR (vulkan_loader.h self-defines),
D3D9_ALLOW_UNMAPPING (auto on _WIN32 && !_WIN64), DXVK_ARCH_X86.
Compiler: `-std=c++17 -msse -msse2 -msse3 -mfpmath=sse
-mpreferred-stack-boundary=2` (32-bit stack alignment — important)
`-Wno-missing-field-initializers -Wno-unused-parameter`.
Linker: `-static -static-libgcc -static-libstdc++
-Wl,--file-alignment=4096,--enable-stdcall-fixup,--kill-at`
(+ keep our wine-builtin constraints: FileAlignment==SectionAlignment 0x1000,
17-byte sig). Libs: `-lsetupapi` (SetupDi in wsi_monitor), `-luser32
-lgdi32`. System headers needed from mingw-w64: d3d9.h, d3d11_1.h, d3d11_4.h,
d3d12.h (recent mingw-w64). PE-ld lesson: symbols resolve BEFORE
--gc-sections — stub every backend symbol from day one
(src/d3d9/d9mt_dxvk_stubs.cpp approach).

---

## 7. Risk / unknowns

1. **Triangle fans + USCALED/SSCALED vertex formats.** DecodeInputAssemblyState
   emits TRIANGLE_FAN; DecodeDecltype emits R8G8B8A8_USCALED, R16-SSCALED,
   A2B10G10R10_USCALED — none exist in Metal. Fan emulation exists in the
   current driver; SCALED needs fetch-shader emulation or vertex-descriptor
   tricks (read as UINT/SINT + convert in shader — but the shader is generated
   from SPIR-V expecting float input). Highest-confidence correctness risk.
2. **SWVP geometry-shader emulator.** ProcessVertices binds a GEOMETRY-stage
   DxvkShader that writes a storage buffer and emits nothing. Metal has no GS.
   Options: translate as compute (input assembly done manually), or fail
   ProcessVertices initially (GTA IV doesn’t use it; some D3D9 titles do).
   Also gates: report vertexPipelineStoresAndAtomics=false to disable SWVP
   caps entirely at first.
3. **Packed depth-stencil copies.** copyBufferToImage/copyImageToBuffer with
   srcFormat/dstFormat = D24S8/D32S8 interleaved require de/re-interleave
   passes against our unified Depth32Float_Stencil8; getting Lock() of depth
   surfaces (INTZ readback) right is fiddly.
4. **Occlusion across pass splits.** Visibility result offsets are per-encoder;
   our pass manager splits aggressively (and emitGraphicsBarrier adds more
   splits). Need per-query slot lists + summation at completion, and results
   must stay Pending until the right command buffer retires (front-end spins).
5. **invalidateBuffer/allocateStorage allocator throughput.** DISCARD renaming
   happens per-draw in hot paths; needs a real suballocator with recycling
   (Rc-held old backings), not per-call MTLBuffer creation. Same for
   DxvkStagingBuffer reimplementation + fence-based throttling.
6. **Liveness signals.** Three independent deadlock sources if completion
   handlers are wrong: presenter frame signal (SyncFrameLatency), submission
   fence (GpuFlushTracker), staging fence (WaitStagingBuffer). All must fire
   even on empty/failed submissions.
7. **ensureImageCompatibility “recreate with usage”.** We plan permissive
   up-front creation, but MUTABLE_FORMAT/BLOCK_TEXEL_VIEW (BC-alias clears via
   R32G32_UINT views) needs `textureView` format aliasing across compression
   classes — Metal forbids BC↔non-BC views; clearImageView on BC images may
   need a compute writeback path instead.
8. **Sampler border colors + non-seamless cubes.** DxvkSamplerKey carries
   arbitrary float border colors and legacy (non-seamless) cube filtering;
   Metal supports neither (fixed border palette, always-seamless cubes).
   Known D3D9 visual-deviation class, likely acceptable.
9. **Spec-constant PSO explosion.** 6 spec dwords (sampler types, fog,
   alpha-test...) key pipelines; with gate id 12=true every dword change can
   force a new PSO. The ~1500-shader/2-min boot already hurts — needs the
   PSO disk cache from the perf backlog, or implement the spec-UBO path
   (declare GPL=true semantics without GPL) to fast-link.
10. **Inventory residual risk.** Six-agent inventory spot-checked at ~12
    points against v2.7.1 sources (all passed), but line-level callsites and
    rarely-hit paths (9on12, shared handles, GDI present fallback, interop)
    were not exhaustively re-verified; treat their stubs as
    fail-loud (`Logger::err` + graceful return), not silent.
