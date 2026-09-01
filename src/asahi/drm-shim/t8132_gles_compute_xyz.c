/* SPDX-License-Identifier: MIT */

/* Exact-output GLES 3.1 probe for direct Apple9 2-D/3-D dispatch. */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WIDTH 14u
#define MAX_HEIGHT 22u
#define MAX_DEPTH 26u
#define MAX_WORDS (MAX_WIDTH * MAX_HEIGHT * MAX_DEPTH)
#define PAYLOAD_BYTES (MAX_WORDS * sizeof(uint32_t))
#define MIN_GUARD_BYTES 256u
#define DEFAULT_SUBMISSIONS 4u
#define MAX_SUBMISSIONS 256u

struct buffer_layout {
   size_t first_payload_offset;
   size_t payload_stride;
   size_t buffer_bytes;
};

struct dispatch_case {
   const char *name;
   unsigned local[3];
   unsigned groups[3];
   unsigned width;
   unsigned height;
   unsigned depth;
   GLuint program;
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GLES_COMPUTE_XYZ_FAIL: %s (EGL=%#x GL=%#x)\n",
           message, eglGetError(), glGetError());
   exit(1);
}

static void
check_gl(const char *operation)
{
   GLenum error = glGetError();
   if (error != GL_NO_ERROR) {
      fprintf(stderr,
              "T8132_GLES_COMPUTE_XYZ_FAIL: %s returned GL=%#x\n",
              operation, error);
      exit(1);
   }
}

static size_t
align_up(size_t value, size_t alignment)
{
   if (!alignment || value > SIZE_MAX - (alignment - 1))
      fail("invalid alignment");
   return ((value + alignment - 1) / alignment) * alignment;
}

static unsigned
parse_count(const char *text)
{
   char *end = NULL;
   unsigned long value = strtoul(text, &end, 0);
   if (!text[0] || !end || *end || value == 0 || value > MAX_SUBMISSIONS) {
      fprintf(stderr, "invalid submission count: %s\n", text);
      exit(2);
   }
   return (unsigned)value;
}

static EGLDisplay
open_asahi_display(void)
{
   PFNEGLQUERYDEVICESEXTPROC query_devices =
      (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
   PFNEGLQUERYDEVICESTRINGEXTPROC query_string =
      (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress(
         "eglQueryDeviceStringEXT");
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
         "eglGetPlatformDisplayEXT");
   if (!query_devices || !query_string || !get_display)
      fail("EGL device enumeration unavailable");

   EGLDeviceEXT devices[16];
   EGLint count = 0;
   if (!query_devices(16, devices, &count))
      fail("eglQueryDevicesEXT");
   for (EGLint i = 0; i < count; ++i) {
      const char *render =
         query_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
      if (!render || !strstr(render, "renderD"))
         continue;
      EGLDisplay display =
         get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
      if (display != EGL_NO_DISPLAY)
         return display;
   }
   fail("Asahi DRM-shim EGL device not found");
   return EGL_NO_DISPLAY;
}

static GLuint
build_program(const struct dispatch_case *test)
{
   char source[2048];
   int length = snprintf(
      source, sizeof(source),
      "#version 310 es\n"
      "layout(local_size_x=%u, local_size_y=%u, local_size_z=%u) in;\n"
      "layout(std430, binding=0) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uvec3 g = gl_GlobalInvocationID;\n"
      "  uint i = g.x + %uu * (g.y + %uu * g.z);\n"
      "  outbuf.v[i] = 0xa5000000u | (g.z << 16) |"
      " (g.y << 8) | g.x;\n"
      "}\n",
      test->local[0], test->local[1], test->local[2], test->width,
      test->height);
   if (length < 0 || (size_t)length >= sizeof(source))
      fail("compute shader source overflow");

   GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
   const char *sources[] = {source};
   glShaderSource(shader, 1, sources, NULL);
   glCompileShader(shader);
   GLint ok = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      GLsizei size = 0;
      glGetShaderInfoLog(shader, sizeof(log), &size, log);
      fprintf(stderr, "%s compute compile failed: %.*s\n", test->name,
              size, log);
      fail("compute shader compile");
   }

   GLuint program = glCreateProgram();
   glAttachShader(program, shader);
   glLinkProgram(program);
   glDeleteShader(shader);
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[4096];
      GLsizei size = 0;
      glGetProgramInfoLog(program, sizeof(log), &size, log);
      fprintf(stderr, "%s compute link failed: %.*s\n", test->name,
              size, log);
      fail("compute program link");
   }
   return program;
}

static struct buffer_layout
make_layout(size_t slots, size_t alignment)
{
   size_t first = align_up(MIN_GUARD_BYTES, alignment);
   size_t stride =
      align_up(PAYLOAD_BYTES + 2 * MIN_GUARD_BYTES, alignment);
   if (!slots || slots - 1 > (SIZE_MAX - first) / stride)
      fail("buffer layout overflow");
   size_t last = first + (slots - 1) * stride;
   if (last > SIZE_MAX - PAYLOAD_BYTES - MIN_GUARD_BYTES)
      fail("buffer size overflow");
   return (struct buffer_layout){
      .first_payload_offset = first,
      .payload_stride = stride,
      .buffer_bytes = last + PAYLOAD_BYTES + MIN_GUARD_BYTES,
   };
}

static size_t
payload_offset(const struct buffer_layout *layout, size_t slot)
{
   if (slot > (SIZE_MAX - layout->first_payload_offset) /
                 layout->payload_stride)
      fail("payload offset overflow");
   return layout->first_payload_offset + slot * layout->payload_stride;
}

static uint8_t
poison_byte(size_t offset)
{
   uint32_t value = (uint32_t)offset * 0x45d9f3bu;
   value ^= 0x6d4b27a5u;
   value ^= value >> 16;
   return (uint8_t)(value | 1u);
}

static void
seed_poison(uint8_t *bytes, size_t size)
{
   for (size_t i = 0; i < size; ++i)
      bytes[i] = poison_byte(i);
}

static void
write_expected(uint8_t *expected, const struct buffer_layout *layout,
               size_t slot, const struct dispatch_case *test)
{
   size_t base = payload_offset(layout, slot);
   for (unsigned z = 0; z < test->depth; ++z) {
      for (unsigned y = 0; y < test->height; ++y) {
         for (unsigned x = 0; x < test->width; ++x) {
            size_t index =
               x + test->width * (y + test->height * z);
            uint32_t value =
               0xa5000000u | (z << 16) | (y << 8) | x;
            memcpy(expected + base + index * sizeof(value), &value,
                   sizeof(value));
         }
      }
   }
}

static void
verify_complete_buffer(GLuint buffer, const uint8_t *expected, size_t size)
{
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   const uint8_t *actual = glMapBufferRange(
      GL_SHADER_STORAGE_BUFFER, 0, size, GL_MAP_READ_BIT);
   if (!actual)
      fail("map guarded output");

   bool saw_nonzero = false;
   for (size_t i = 0; i < size; ++i) {
      saw_nonzero |= actual[i] != 0;
      if (actual[i] != expected[i]) {
         fprintf(stderr,
                 "XYZ output byte %#zx=%#x expected=%#x\n",
                 i, actual[i], expected[i]);
         fail("complete guarded output mismatch");
      }
   }
   if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER))
      fail("unmap guarded output");
   if (!saw_nonzero)
      fail("all output bytes are zero");
}

int
main(int argc, char **argv)
{
   if (argc > 2) {
      fprintf(stderr, "usage: %s [SUBMISSIONS]\n", argv[0]);
      return 2;
   }
   unsigned submissions = argc == 2 ? parse_count(argv[1])
                                     : DEFAULT_SUBMISSIONS;

   EGLDisplay display = open_asahi_display();
   EGLint major = 0, minor = 0;
   if (!eglInitialize(display, &major, &minor) ||
       !eglBindAPI(EGL_OPENGL_ES_API))
      fail("initialize EGL");
   const EGLint config_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
      EGL_NONE,
   };
   EGLConfig config;
   EGLint config_count = 0;
   if (!eglChooseConfig(display, config_attrs, &config, 1, &config_count) ||
       config_count != 1)
      fail("eglChooseConfig");
   const EGLint surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
   EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
   const EGLint context_attrs[] = {
      EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
      EGL_CONTEXT_MINOR_VERSION_KHR, 1,
      EGL_NONE,
   };
   EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context))
      fail("create GLES 3.1 context");

   const char *renderer = (const char *)glGetString(GL_RENDERER);
   const char *version = (const char *)glGetString(GL_VERSION);
   if (!renderer || !strstr(renderer, "Apple M4"))
      fail("unexpected renderer");

   struct dispatch_case cases[] = {
      {
         .name = "2d-12x10x1-local-3x5x1",
         .local = {3, 5, 1},
         .groups = {4, 2, 1},
         .width = 12,
         .height = 10,
         .depth = 1,
      },
      {
         .name = "3d-14x22x26-local-2x2x2",
         .local = {2, 2, 2},
         .groups = {7, 11, 13},
         .width = 14,
         .height = 22,
         .depth = 26,
      },
   };
   for (unsigned i = 0; i < 2; ++i)
      cases[i].program = build_program(&cases[i]);
   check_gl("build XYZ programs");

   GLint alignment_value = 0;
   glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment_value);
   if (alignment_value <= 0)
      fail("invalid SSBO offset alignment");
   size_t alignment = (size_t)alignment_value;
   if (alignment < sizeof(uint32_t))
      alignment = sizeof(uint32_t);
   size_t slot_count = (size_t)submissions * 2;
   struct buffer_layout layout = make_layout(slot_count, alignment);
   if (layout.buffer_bytes > (size_t)INTPTR_MAX)
      fail("guarded buffer too large");

   uint8_t *expected = malloc(layout.buffer_bytes);
   if (!expected)
      fail("allocate guarded oracle");
   seed_poison(expected, layout.buffer_bytes);

   GLuint output = 0;
   glGenBuffers(1, &output);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, output);
   glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes, expected,
                GL_DYNAMIC_COPY);
   check_gl("create guarded output");

   for (unsigned submit = 0; submit < submissions; ++submit) {
      for (unsigned c = 0; c < 2; ++c) {
         size_t slot = (size_t)submit * 2 + c;
         size_t offset = payload_offset(&layout, slot);
         glUseProgram(cases[c].program);
         glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, output, offset,
                           PAYLOAD_BYTES);
         glDispatchCompute(cases[c].groups[0], cases[c].groups[1],
                           cases[c].groups[2]);
         write_expected(expected, &layout, slot, &cases[c]);
      }
      check_gl("dispatch XYZ pair");
      glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                      GL_SHADER_STORAGE_BARRIER_BIT);
      glFinish();
      check_gl("finish XYZ submission");
      verify_complete_buffer(output, expected, layout.buffer_bytes);
   }

   printf("T8132_GLES_COMPUTE_XYZ_OK submissions=%u total_dispatches=%zu "
          "cases=2d-12x10x1,3d-14x22x26 local=3x5x1,2x2x2 "
          "values=120,8008 "
          "ssbo_alignment=%zu first_offset=%#zx stride=%#zx "
          "renderer=\"%s\" version=\"%s\"\n",
          submissions, slot_count, alignment, layout.first_payload_offset,
          layout.payload_stride, renderer, version);

   for (unsigned i = 0; i < 2; ++i)
      glDeleteProgram(cases[i].program);
   glDeleteBuffers(1, &output);
   free(expected);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return 0;
}
