#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  helpers                                                            */
/* ------------------------------------------------------------------ */

static void die(const char *msg) {
  fprintf(stderr, "FATAL [gpu]: %s\n", msg);
  exit(1);
}

typedef struct {
  bool done;
  void *result;
} AsyncResult;

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView msg, void *ud1, void *ud2) {
  (void)msg;
  (void)ud2;
  AsyncResult *r = ud1;
  r->result = (status == WGPURequestAdapterStatus_Success) ? adapter : NULL;
  r->done = true;
}
static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView msg, void *ud1, void *ud2) {
  (void)msg;
  (void)ud2;
  AsyncResult *r = ud1;
  r->result = (status == WGPURequestDeviceStatus_Success) ? device : NULL;
  r->done = true;
}

/* ------------------------------------------------------------------ */
/*  surface creation (platform-specific)                               */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
#define PLATFORM_WINDOWS
#elif defined(__APPLE__)
#define PLATFORM_MACOS
#include <objc/message.h>
#include <objc/objc.h>
#elif defined(__linux__)
#define PLATFORM_LINUX
#endif

static WGPUSurface create_surface(WGPUInstance instance, SDL_Window *window) {
  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  if (!props)
    die("SDL_GetWindowProperties failed");
  WGPUSurfaceDescriptor desc = {.nextInChain = NULL, .label = {0}};

#if defined(PLATFORM_WINDOWS)
  void *hwnd =
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
  void *hinst = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);
  if (!hwnd)
    die("Could not get HWND");
  WGPUSurfaceSourceWindowsHWND src = {
      .chain = {.sType = WGPUSType_SurfaceSourceWindowsHWND},
      .hinstance = hinst,
      .hwnd = hwnd,
  };
  desc.nextInChain = (const WGPUChainedStruct *)&src;

#elif defined(PLATFORM_MACOS)
  void *ns_window =
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  if (!ns_window)
    die("Could not get NSWindow");
  id cv = ((id (*)(id, SEL))objc_msgSend)((id)ns_window,
                                          sel_registerName("contentView"));
  id layer = ((id (*)(id, SEL))objc_msgSend)(cv, sel_registerName("layer"));
  WGPUSurfaceSourceMetalLayer src = {
      .chain = {.sType = WGPUSType_SurfaceSourceMetalLayer},
      .layer = layer,
  };
  desc.nextInChain = (const WGPUChainedStruct *)&src;

#elif defined(PLATFORM_LINUX)
  /*
   * Try Wayland first (requires Dawn built with -DDAWN_USE_WAYLAND=ON),
   * then fall back to X11 (Xlib, then XCB).
   */
  void *wl_disp = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
  void *wl_surf = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
  if (wl_disp && wl_surf) {
    static WGPUSurfaceSourceWaylandSurface s;
    s = (WGPUSurfaceSourceWaylandSurface){
        .chain = {.sType = WGPUSType_SurfaceSourceWaylandSurface},
        .display = wl_disp,
        .surface = wl_surf,
    };
    desc.nextInChain = (WGPUChainedStruct *)&s;
  } else {
    void *x_disp = SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    uint64_t x_win = (uint64_t)SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (x_disp && x_win) {
      static WGPUSurfaceSourceXlibWindow s;
      s = (WGPUSurfaceSourceXlibWindow){
          .chain = {.sType = WGPUSType_SurfaceSourceXlibWindow},
          .display = x_disp,
          .window = x_win,
      };
      desc.nextInChain = (WGPUChainedStruct *)&s;
    } else {
      die("No Wayland or X11 surface found");
    }
  }
#else
  die("Unsupported platform");
#endif

  WGPUSurface surface = wgpuInstanceCreateSurface(instance, &desc);
  if (!surface)
    die("wgpuInstanceCreateSurface failed");
  return surface;
}

/* ------------------------------------------------------------------ */
/*  shader + pipeline                                                  */
/* ------------------------------------------------------------------ */

static const char *SHADER_SRC =
    "struct VIn {\n"
    "  @location(0) pos: vec2f,\n"
    "  @location(1) col: vec4f,\n"
    "};\n"
    "struct VOut {\n"
    "  @builtin(position) pos: vec4f,\n"
    "  @location(0) col: vec4f,\n"
    "};\n"
    "@vertex fn vs(in: VIn) -> VOut {\n"
    "  var o: VOut;\n"
    "  o.pos = vec4f(in.pos * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);\n"
    "  o.col = in.col;\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(in: VOut) -> @location(0) vec4f {\n"
    "  return in.col;\n"
    "}\n";

static void create_pipeline(GpuContext *ctx) {
  /* Shader module */
  WGPUShaderSourceWGSL wgsl = {
      .chain = {.sType = WGPUSType_ShaderSourceWGSL},
      .code = {.data = SHADER_SRC, .length = strlen(SHADER_SRC)},
  };
  WGPUShaderModuleDescriptor sm_desc = {
      .nextInChain = (WGPUChainedStruct *)&wgsl,
  };
  ctx->shader = wgpuDeviceCreateShaderModule(ctx->device, &sm_desc);
  if (!ctx->shader)
    die("Shader compilation failed");

  /* Vertex layout: [vec2f pos, vec4f color] = 24 bytes */
  WGPUVertexAttribute attrs[] = {
      {.format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0},
      {.format = WGPUVertexFormat_Float32x4,
       .offset = 2 * sizeof(float),
       .shaderLocation = 1},
  };
  WGPUVertexBufferLayout vb_layout = {
      .stepMode = WGPUVertexStepMode_Vertex,
      .arrayStride = GPU_FLOATS_PER_VTX * sizeof(float),
      .attributeCount = 2,
      .attributes = attrs,
  };

  /* Color target */
  WGPUBlendState blend = {
      .color = {.srcFactor = WGPUBlendFactor_SrcAlpha,
                .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                .operation = WGPUBlendOperation_Add},
      .alpha = {.srcFactor = WGPUBlendFactor_One,
                .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha,
                .operation = WGPUBlendOperation_Add},
  };
  WGPUColorTargetState color_target = {
      .format = ctx->format,
      .blend = &blend,
      .writeMask = WGPUColorWriteMask_All,
  };
  WGPUFragmentState frag = {
      .module = ctx->shader,
      .entryPoint = {.data = "fs", .length = 2},
      .targetCount = 1,
      .targets = &color_target,
  };

  /* Pipeline */
  WGPURenderPipelineDescriptor pip = {
      .vertex =
          {
              .module = ctx->shader,
              .entryPoint = {.data = "vs", .length = 2},
              .bufferCount = 1,
              .buffers = &vb_layout,
          },
      .primitive =
          {
              .topology = WGPUPrimitiveTopology_TriangleList,
          },
      .multisample =
          {
              .count = 1,
              .mask = 0xFFFFFFFF,
          },
      .fragment = &frag,
  };
  ctx->pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &pip);
  if (!ctx->pipeline)
    die("Pipeline creation failed");

  /* Vertex buffer */
  WGPUBufferDescriptor buf = {
      .usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
      .size = sizeof(ctx->vertices),
  };
  ctx->vertex_buffer = wgpuDeviceCreateBuffer(ctx->device, &buf);
}

static void on_device_error(WGPUDevice const *device, WGPUErrorType type,
                            WGPUStringView msg, void *ud1, void *ud2) {
  (void)device;
  (void)ud1;
  (void)ud2;
  const char *kind = "Unknown";
  switch (type) {
  case WGPUErrorType_Validation:
    kind = "Validation";
    break;
  case WGPUErrorType_OutOfMemory:
    kind = "OOM";
    break;
  case WGPUErrorType_Internal:
    kind = "Internal";
    break;
  default:
    break;
  }
  fprintf(stderr, "[Dawn %s] %.*s\n", kind, (int)msg.length, msg.data);
}

/* ------------------------------------------------------------------ */
/*  public API                                                         */
/* ------------------------------------------------------------------ */

bool gpu_init(GpuContext *ctx, SDL_Window *window) {
  memset(ctx, 0, sizeof(*ctx));

  WGPUInstanceFeatureName inst_features[] = {
      WGPUInstanceFeatureName_ShaderSourceSPIRV,
  };
  WGPUInstanceDescriptor inst_desc = {
      .requiredFeatureCount = 1,
      .requiredFeatures = inst_features,
  };
  ctx->instance = wgpuCreateInstance(&inst_desc);
  if (!ctx->instance) {
    fprintf(stderr, "wgpuCreateInstance failed\n");
    return false;
  }

  /* Surface */
  ctx->surface = create_surface(ctx->instance, window);

  /* Adapter — mode MUST be set; 0 is invalid in Dawn */
  AsyncResult ar = {0};
  WGPURequestAdapterOptions ao = {
      .compatibleSurface = ctx->surface,
      .powerPreference = WGPUPowerPreference_HighPerformance,
  };
  wgpuInstanceRequestAdapter(ctx->instance, &ao,
                             (WGPURequestAdapterCallbackInfo){
                                 .mode = WGPUCallbackMode_AllowProcessEvents,
                                 .callback = on_adapter,
                                 .userdata1 = &ar,
                             });
  while (!ar.done)
    wgpuInstanceProcessEvents(ctx->instance);
  ctx->adapter = ar.result;
  if (!ctx->adapter) {
    fprintf(stderr, "No adapter\n");
    return false;
  }

  /* Device — mode MUST be set; 0 is invalid in Dawn */
  ar = (AsyncResult){0};
    /* Build feature list — always request Tier1, optionally DmaBuf */
  WGPUFeatureName device_features[2];
  size_t feature_count = 0;
  device_features[feature_count++] = WGPUFeatureName_TextureFormatsTier1;

  if (wgpuAdapterHasFeature(ctx->adapter,
                            WGPUFeatureName_SharedTextureMemoryDmaBuf)) {
      device_features[feature_count++] =
          WGPUFeatureName_SharedTextureMemoryDmaBuf;
  }

  WGPUDeviceDescriptor dd = {
      .label = {.data = "dev", .length = 3},
      .requiredFeatureCount = feature_count,
      .requiredFeatures = device_features,
      .uncapturedErrorCallbackInfo =
          {
              .callback = on_device_error,
          },
  };
  wgpuAdapterRequestDevice(ctx->adapter, &dd,
                           (WGPURequestDeviceCallbackInfo){
                               .mode = WGPUCallbackMode_AllowProcessEvents,
                               .callback = on_device,
                               .userdata1 = &ar,
                           });
  while (!ar.done)
    wgpuInstanceProcessEvents(ctx->instance);
  ctx->device = ar.result;
  if (!ctx->device) {
    fprintf(stderr, "No device\n");
    return false;
  }
  ctx->queue = wgpuDeviceGetQueue(ctx->device);

  /* Surface config */
  WGPUSurfaceCapabilities caps = {0};
  wgpuSurfaceGetCapabilities(ctx->surface, ctx->adapter, &caps);
  ctx->format =
      (caps.formatCount > 0) ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;

  int pw, ph;
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
  ctx->width = (uint32_t)pw;
  ctx->height = (uint32_t)ph;

  ctx->surf_cfg = (WGPUSurfaceConfiguration){
      .device = ctx->device,
      .format = ctx->format,
      .usage = WGPUTextureUsage_RenderAttachment,
      .alphaMode = WGPUCompositeAlphaMode_Auto,
      .width = ctx->width,
      .height = ctx->height,
      .presentMode = WGPUPresentMode_Fifo,
  };
  wgpuSurfaceConfigure(ctx->surface, &ctx->surf_cfg);

  /* Pipeline + vertex buffer */
  create_pipeline(ctx);

  printf("[gpu] ready  %ux%u  fmt=%d\n", ctx->width, ctx->height, ctx->format);
  return true;
}

void gpu_resize(GpuContext *ctx, uint32_t w, uint32_t h) {
  ctx->width = w;
  ctx->height = h;
  ctx->surf_cfg.width = w;
  ctx->surf_cfg.height = h;
  wgpuSurfaceConfigure(ctx->surface, &ctx->surf_cfg);
}

bool gpu_frame_begin(GpuContext *ctx) {
  ctx->vertex_count = 0;
  ctx->clear_color[0] = 0.0f;
  ctx->clear_color[1] = 0.0f;
  ctx->clear_color[2] = 0.0f;
  ctx->clear_color[3] = 1.0f;
  return true;
}

void gpu_frame_end(GpuContext *ctx) {
  WGPUSurfaceTexture st;
  wgpuSurfaceGetCurrentTexture(ctx->surface, &st);
  if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
    return;

  WGPUTextureView view = wgpuTextureCreateView(st.texture, NULL);

  uint32_t vb_bytes = ctx->vertex_count * GPU_FLOATS_PER_VTX * sizeof(float);
  if (vb_bytes > 0)
    wgpuQueueWriteBuffer(ctx->queue, ctx->vertex_buffer, 0, ctx->vertices,
                         vb_bytes);

  WGPURenderPassColorAttachment color_att = {
      .view = view,
      .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
      .loadOp = WGPULoadOp_Clear,
      .storeOp = WGPUStoreOp_Store,
      .clearValue = (WGPUColor){ctx->clear_color[0], ctx->clear_color[1],
                                ctx->clear_color[2], ctx->clear_color[3]},
  };
  WGPURenderPassDescriptor rp = {
      .colorAttachmentCount = 1,
      .colorAttachments = &color_att,
  };
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(ctx->device, NULL);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);

  if (ctx->vertex_count > 0) {
    wgpuRenderPassEncoderSetPipeline(pass, ctx->pipeline);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, ctx->vertex_buffer, 0,
                                         vb_bytes);
    wgpuRenderPassEncoderDraw(pass, ctx->vertex_count, 1, 0, 0);
  }

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(ctx->queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  WGPUStatus status = wgpuSurfacePresent(ctx->surface);
  if (status != WGPUStatus_Success)
  {
    fprintf(stderr, "Failed to present surface for WGPU... %u\n", status);
  }
  wgpuTextureViewRelease(view);
  wgpuTextureRelease(st.texture);
}

void gpu_destroy(GpuContext *ctx) {
  wgpuBufferRelease(ctx->vertex_buffer);
  wgpuRenderPipelineRelease(ctx->pipeline);
  wgpuShaderModuleRelease(ctx->shader);
  wgpuSurfaceUnconfigure(ctx->surface);
  wgpuSurfaceRelease(ctx->surface);
  wgpuQueueRelease(ctx->queue);
  wgpuDeviceRelease(ctx->device);
  wgpuAdapterRelease(ctx->adapter);
  wgpuInstanceRelease(ctx->instance);
}

/* ---- Wasm-callable helpers ---- */

void gpu_set_clear_color(GpuContext *ctx, float r, float g, float b, float a) {
  ctx->clear_color[0] = r;
  ctx->clear_color[1] = g;
  ctx->clear_color[2] = b;
  ctx->clear_color[3] = a;
}

static void push_vertex(GpuContext *ctx, float x, float y, float r, float g,
                        float b, float a) {
  if (ctx->vertex_count >= GPU_MAX_VERTICES)
    return;
  float *p = &ctx->vertices[ctx->vertex_count * GPU_FLOATS_PER_VTX];
  p[0] = x;
  p[1] = y;
  p[2] = r;
  p[3] = g;
  p[4] = b;
  p[5] = a;
  ctx->vertex_count++;
}

void gpu_push_rect(GpuContext *ctx, float x, float y, float w, float h, float r,
                   float g, float b, float a) {
  push_vertex(ctx, x, y, r, g, b, a);
  push_vertex(ctx, x + w, y, r, g, b, a);
  push_vertex(ctx, x + w, y + h, r, g, b, a);

  push_vertex(ctx, x, y, r, g, b, a);
  push_vertex(ctx, x + w, y + h, r, g, b, a);
  push_vertex(ctx, x, y + h, r, g, b, a);
}

/* ------------------------------------------------------------------ */
/*  Offscreen texture management                                       */
/* ------------------------------------------------------------------ */

bool gpu_create_offscreen_texture(GpuContext *ctx,
                                  uint32_t w, uint32_t h,
                                  WGPUTexture *out_tex,
                                  WGPUTextureView *out_view) {
    if (w == 0 || h == 0) return false;

    WGPUTextureDescriptor desc = {
        .label          = { .data = "offscreen", .length = 9 },
        .usage          = WGPUTextureUsage_RenderAttachment |
                          WGPUTextureUsage_TextureBinding |
                          WGPUTextureUsage_CopySrc,
        .dimension      = WGPUTextureDimension_2D,
        .size           = { .width = w, .height = h, .depthOrArrayLayers = 1 },
        .format         = ctx->format,
        .mipLevelCount  = 1,
        .sampleCount    = 1,
    };

    *out_tex = wgpuDeviceCreateTexture(ctx->device, &desc);
    if (!*out_tex) return false;

    WGPUTextureViewDescriptor vdesc = {
        .label           = { .data = "offscreen_view", .length = 14 },
        .format          = ctx->format,
        .dimension       = WGPUTextureViewDimension_2D,
        .baseMipLevel    = 0,
        .mipLevelCount   = 1,
        .baseArrayLayer  = 0,
        .arrayLayerCount = 1,
    };
    *out_view = wgpuTextureCreateView(*out_tex, &vdesc);
    if (!*out_view) {
        wgpuTextureRelease(*out_tex);
        *out_tex = NULL;
        return false;
    }
    return true;
}

bool gpu_resize_offscreen_texture(GpuContext *ctx,
                                  uint32_t w, uint32_t h,
                                  WGPUTexture *tex,
                                  WGPUTextureView *view) {
    gpu_destroy_offscreen_texture(tex, view);
    return gpu_create_offscreen_texture(ctx, w, h, tex, view);
}

void gpu_destroy_offscreen_texture(WGPUTexture *tex, WGPUTextureView *view) {
    if (view && *view) { wgpuTextureViewRelease(*view); *view = NULL; }
    if (tex  && *tex)  { wgpuTextureRelease(*tex);      *tex  = NULL; }
}

/* ------------------------------------------------------------------ */
/*  Textured-quad compositing pipeline                                 */
/* ------------------------------------------------------------------ */

static const char *COMPOSITE_SHADER_SRC =
    "struct VOut {\n"
    "  @builtin(position) pos: vec4f,\n"
    "  @location(0) uv: vec2f,\n"
    "};\n"
    "\n"
    "struct Uniforms {\n"
    "  rect: vec4f,\n"  /* x, y, w, h in NDC */
    "};\n"
    "\n"
    "/* Fullscreen quad positions (triangle strip as 2 triangles) */\n"
    "var<private> QUAD_POS: array<vec2f, 6> = array<vec2f, 6>(\n"
    "  vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0),\n"
    "  vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0),\n"
    ");\n"
    "var<private> QUAD_UV: array<vec2f, 6> = array<vec2f, 6>(\n"
    "  vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0),\n"
    "  vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(0.0, 0.0),\n"
    ");\n"
    "\n"
    "@group(0) @binding(0) var tex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var samp: sampler;\n"
    "\n"
    "@vertex fn vs(@builtin(vertex_index) vi: u32) -> VOut {\n"
    "  var o: VOut;\n"
    "  let p = QUAD_POS[vi];\n"
    "  o.uv = QUAD_UV[vi];\n"
    "  o.pos = vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "\n"
    "@fragment fn fs(in: VOut) -> @location(0) vec4f {\n"
    "  return textureSample(tex, samp, in.uv);\n"
    "}\n";

bool gpu_compositor_init(GpuCompositor *comp, GpuContext *ctx) {
    memset(comp, 0, sizeof(*comp));

    /* Shader */
    WGPUShaderSourceWGSL wgsl = {
        .chain = { .sType = WGPUSType_ShaderSourceWGSL },
        .code  = { .data = COMPOSITE_SHADER_SRC,
                   .length = strlen(COMPOSITE_SHADER_SRC) },
    };
    WGPUShaderModuleDescriptor sm_desc = {
        .nextInChain = (WGPUChainedStruct *)&wgsl,
    };
    comp->shader = wgpuDeviceCreateShaderModule(ctx->device, &sm_desc);
    if (!comp->shader) return false;

    /* Sampler */
    WGPUSamplerDescriptor samp_desc = {
        .magFilter   = WGPUFilterMode_Linear,
        .minFilter   = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .addressModeU = WGPUAddressMode_ClampToEdge,
        .addressModeV = WGPUAddressMode_ClampToEdge,
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .maxAnisotropy = 1,
    };
    comp->sampler = wgpuDeviceCreateSampler(ctx->device, &samp_desc);
    if (!comp->sampler) return false;

    /* Bind group layout: texture + sampler */
    WGPUBindGroupLayoutEntry bgl_entries[] = {
        {
            .binding    = 0,
            .visibility = WGPUShaderStage_Fragment,
            .texture    = {
                .sampleType    = WGPUTextureSampleType_Float,
                .viewDimension = WGPUTextureViewDimension_2D,
            },
        },
        {
            .binding    = 1,
            .visibility = WGPUShaderStage_Fragment,
            .sampler    = { .type = WGPUSamplerBindingType_Filtering },
        },
    };
    WGPUBindGroupLayoutDescriptor bgl_desc = {
        .entryCount = 2,
        .entries    = bgl_entries,
    };
    comp->bind_group_layout = wgpuDeviceCreateBindGroupLayout(ctx->device, &bgl_desc);
    if (!comp->bind_group_layout) return false;

    /* Pipeline layout */
    WGPUPipelineLayoutDescriptor pl_desc = {
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts     = &comp->bind_group_layout,
    };
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(ctx->device, &pl_desc);

    /* Color target with alpha blending */
    WGPUBlendState blend = {
        .color = { .srcFactor = WGPUBlendFactor_One,
                   .dstFactor = WGPUBlendFactor_Zero,
                   .operation = WGPUBlendOperation_Add },
        .alpha = { .srcFactor = WGPUBlendFactor_One,
                   .dstFactor = WGPUBlendFactor_Zero,
                   .operation = WGPUBlendOperation_Add },
    };
    WGPUColorTargetState color_target = {
        .format    = ctx->format,
        .blend     = &blend,
        .writeMask = WGPUColorWriteMask_All,
    };
    WGPUFragmentState frag = {
        .module      = comp->shader,
        .entryPoint  = { .data = "fs", .length = 2 },
        .targetCount = 1,
        .targets     = &color_target,
    };

    WGPURenderPipelineDescriptor pip = {
        .layout   = pl,
        .vertex   = {
            .module     = comp->shader,
            .entryPoint = { .data = "vs", .length = 2 },
        },
        .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
        .multisample = { .count = 1, .mask = 0xFFFFFFFF },
        .fragment  = &frag,
    };
    comp->pipeline = wgpuDeviceCreateRenderPipeline(ctx->device, &pip);
    wgpuPipelineLayoutRelease(pl);
    if (!comp->pipeline) return false;

    comp->initialized = true;
    printf("[gpu] Compositor pipeline ready\n");
    return true;
}

void gpu_compositor_destroy(GpuCompositor *comp) {
    if (comp->pipeline)          wgpuRenderPipelineRelease(comp->pipeline);
    if (comp->shader)            wgpuShaderModuleRelease(comp->shader);
    if (comp->sampler)           wgpuSamplerRelease(comp->sampler);
    if (comp->bind_group_layout) wgpuBindGroupLayoutRelease(comp->bind_group_layout);
    if (comp->vertex_buffer)     wgpuBufferRelease(comp->vertex_buffer);
    memset(comp, 0, sizeof(*comp));
}

void gpu_composite_quad(const GpuCompositor *comp,
                        GpuContext *ctx,
                        WGPURenderPassEncoder pass,
                        WGPUTextureView tex_view,
                        float x, float y, float w, float h,
                        float target_w, float target_h) {
    if (!comp->initialized || !tex_view) return;

    /* Create a per-draw bind group with the offscreen texture view */
    WGPUBindGroupEntry bg_entries[] = {
        { .binding = 0, .textureView = tex_view },
        { .binding = 1, .sampler     = comp->sampler },
    };
    WGPUBindGroupDescriptor bg_desc = {
        .layout     = comp->bind_group_layout,
        .entryCount = 2,
        .entries    = bg_entries,
    };
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(ctx->device, &bg_desc);
    if (!bg) return;

    /* Set viewport to the slot's rectangle */
    wgpuRenderPassEncoderSetViewport(pass, x, y, w, h, 0.0f, 1.0f);
    wgpuRenderPassEncoderSetPipeline(pass, comp->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

    /* Restore full viewport */
    wgpuRenderPassEncoderSetViewport(pass, 0, 0, target_w, target_h, 0.0f, 1.0f);

    wgpuBindGroupRelease(bg);
}

