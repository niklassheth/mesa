/* SPDX-License-Identifier: MIT */

/*
 * Exact fixed-USC ownership/lifecycle gate for the T8132 Mesa path:
 *
 *   exact copy2 A -> supported VBO triangle -> exact copy2 B
 *
 * The compute program is linked once before either dispatch.  Both launches
 * therefore use one Mesa compute state even though the intervening VBO draw
 * installs a render generation into the same fixed USC aperture.  The two
 * dispatches use different aligned ranges and data.  Full input and output
 * allocations are compared, including leading/trailing guards and the
 * interior gap.
 *
 * Render readback is deliberately absent.  The synchronous m1n1 backend
 * validates the completed 257x193 VBO attachment against its exact source
 * oracle before glFinish returns; glReadPixels can introduce an unrelated
 * conversion/decompression compute command that is outside this gate.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH           257u
#define HEIGHT          193u
#define VALUE_COUNT     64u
#define LOCAL_SIZE_X    32u
#define DISPATCH_COUNT  2u
#define RENDER_FRAMES   2u
#define PAYLOAD_BYTES   (VALUE_COUNT * sizeof(uint32_t))
#define MIN_GUARD_BYTES 256u

struct buffer_layout {
   size_t first_payload_offset;
   size_t payload_stride;
   size_t buffer_bytes;
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GLES_COMPUTE_VBO_LIFECYCLE_FAIL: %s\n", message);
   exit(1);
}

static void
check_gl(const char *operation)
{
   GLenum error = glGetError();
   if (error != GL_NO_ERROR) {
      fprintf(stderr,
              "T8132_GLES_COMPUTE_VBO_LIFECYCLE_FAIL: %s returned "
              "GL=%#x\n",
              operation, error);
      exit(1);
   }
}

static size_t
align_up(size_t value, size_t alignment)
{
   if (!alignment || value > SIZE_MAX - (alignment - 1))
      fail("invalid SSBO alignment");

   return ((value + alignment - 1) / alignment) * alignment;
}

static struct buffer_layout
make_layout(size_t alignment)
{
   size_t first = align_up(MIN_GUARD_BYTES, alignment);
   size_t stride = align_up(PAYLOAD_BYTES + 2 * MIN_GUARD_BYTES, alignment);
   if (stride < PAYLOAD_BYTES || first > SIZE_MAX - stride ||
       first + stride > SIZE_MAX - PAYLOAD_BYTES - MIN_GUARD_BYTES)
      fail("guarded SSBO layout overflow");

   return (struct buffer_layout){
      .first_payload_offset = first,
      .payload_stride = stride,
      .buffer_bytes = first + stride + PAYLOAD_BYTES + MIN_GUARD_BYTES,
   };
}

static size_t
payload_offset(const struct buffer_layout *layout, unsigned dispatch)
{
   if (!layout || dispatch >= DISPATCH_COUNT)
      fail("invalid copy2 dispatch index");
   return layout->first_payload_offset + dispatch * layout->payload_stride;
}

static uint8_t
poison_byte(unsigned resource, size_t offset)
{
   uint32_t value = (uint32_t)offset * 0x45d9f3bu;
   value ^= 0xa5c39e17u + resource * 0x31415927u;
   value ^= value >> 16;
   return (uint8_t)(value | 1u);
}

static uint32_t
input_word(unsigned dispatch, unsigned lane)
{
   uint32_t base = 0x10203040u ^ (lane * 0x01010101u);
   return dispatch ? (base ^ 0x6d2b79f5u) + lane * 0x9e3779b9u : base;
}

static void
seed_poison(uint8_t *bytes, size_t size, unsigned resource)
{
   for (size_t i = 0; i < size; ++i)
      bytes[i] = poison_byte(resource, i);
}

static void
write_word(uint8_t *bytes, size_t offset, uint32_t value)
{
   memcpy(bytes + offset, &value, sizeof(value));
}

static void
seed_inputs(uint8_t *input, const struct buffer_layout *layout)
{
   for (unsigned dispatch = 0; dispatch < DISPATCH_COUNT; ++dispatch) {
      size_t base = payload_offset(layout, dispatch);
      for (unsigned lane = 0; lane < VALUE_COUNT; ++lane) {
         write_word(input, base + lane * sizeof(uint32_t),
                    input_word(dispatch, lane));
      }
   }
}

static void
complete_expected_dispatch(uint8_t *expected, const uint8_t *input,
                           const struct buffer_layout *layout,
                           unsigned dispatch)
{
   size_t base = payload_offset(layout, dispatch);
   memcpy(expected + base, input + base, PAYLOAD_BYTES);
}

static void
report_mismatch(const char *name, const uint8_t *actual,
                const uint8_t *expected, size_t size)
{
   for (size_t i = 0; i < size; ++i) {
      if (actual[i] == expected[i])
         continue;
      fprintf(stderr,
              "T8132_GLES_COMPUTE_VBO_LIFECYCLE_FAIL: %s byte %#zx "
              "actual=%#x expected=%#x\n",
              name, i, actual[i], expected[i]);
      exit(1);
   }
}

static void
verify_buffer(GLuint buffer, const char *name, const uint8_t *expected,
              size_t size)
{
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   const uint8_t *mapped =
      glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, size, GL_MAP_READ_BIT);
   if (!mapped)
      fail("map guarded SSBO");
   report_mismatch(name, mapped, expected, size);
   if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER))
      fail("unmap guarded SSBO");
}

static GLuint
compile_shader(GLenum stage, const char *source, const char *name)
{
   GLuint shader = glCreateShader(stage);
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);

   GLint ok = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      GLsizei length = 0;
      glGetShaderInfoLog(shader, sizeof(log), &length, log);
      fprintf(stderr, "%s compile failed: %.*s\n", name, length, log);
      fail("shader compile");
   }
   return shader;
}

static GLuint
link_program(GLuint first, GLuint second, const char *name)
{
   GLuint program = glCreateProgram();
   glAttachShader(program, first);
   if (second)
      glAttachShader(program, second);
   glLinkProgram(program);

   GLint ok = GL_FALSE;
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[4096];
      GLsizei length = 0;
      glGetProgramInfoLog(program, sizeof(log), &length, log);
      fprintf(stderr, "%s link failed: %.*s\n", name, length, log);
      fail("program link");
   }
   return program;
}

static GLuint
build_copy2_program(void)
{
   static const char source[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=1) readonly buffer Input { uint v[]; } inbuf;\n"
      "layout(std430, binding=0) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  outbuf.v[i] = inbuf.v[i];\n"
      "}\n";

   GLuint shader = compile_shader(GL_COMPUTE_SHADER, source, "copy2");
   GLuint program = link_program(shader, 0, "copy2");
   glDeleteShader(shader);
   return program;
}

static GLuint
build_triangle_program(void)
{
   static const char vertex_source[] =
      "#version 300 es\n"
      "precision highp float;\n"
      "layout(location=0) in vec2 position;\n"
      "layout(location=1) in vec3 vertex_color;\n"
      "out vec3 color;\n"
      "void main() {\n"
      "  gl_Position = vec4(position, 0.0, 1.0);\n"
      "  color = vertex_color;\n"
      "}\n";
   static const char fragment_source[] =
      "#version 300 es\n"
      "precision highp float;\n"
      "in vec3 color;\n"
      "layout(location=0) out vec4 out_color;\n"
      "void main() { out_color = vec4(color, 1.0); }\n";

   GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source, "VBO vertex");
   GLuint fs =
      compile_shader(GL_FRAGMENT_SHADER, fragment_source, "VBO fragment");
   GLuint program = link_program(vs, fs, "VBO triangle");
   glDeleteShader(fs);
   glDeleteShader(vs);
   return program;
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

static void
dispatch_copy2(GLuint program, GLuint output, GLuint input,
               const struct buffer_layout *layout, unsigned dispatch)
{
   size_t offset = payload_offset(layout, dispatch);
   glUseProgram(program);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, output, offset,
                     PAYLOAD_BYTES);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, input, offset, PAYLOAD_BYTES);
   glDispatchCompute(VALUE_COUNT / LOCAL_SIZE_X, 1, 1);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                   GL_SHADER_STORAGE_BARRIER_BIT);
   glFinish();
   check_gl(dispatch ? "second exact copy2" : "first exact copy2");
}

static void
draw_vbo_triangle(GLuint program, GLuint vao)
{
   glUseProgram(program);
   glBindVertexArray(vao);
   glViewport(0, 0, WIDTH, HEIGHT);
   for (unsigned frame = 0; frame < RENDER_FRAMES; ++frame) {
      glClearColor(0.75f, 0.73f, 1.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      check_gl("supported VBO triangle");
   }
}

static int
self_test(void)
{
   const struct buffer_layout layout = make_layout(256);
   uint8_t *input = malloc(layout.buffer_bytes);
   uint8_t *output = malloc(layout.buffer_bytes);
   uint8_t *expected = malloc(layout.buffer_bytes);
   if (!input || !output || !expected)
      fail("allocate offline oracle");

   seed_poison(input, layout.buffer_bytes, 0);
   seed_poison(output, layout.buffer_bytes, 1);
   seed_inputs(input, &layout);
   memcpy(expected, output, layout.buffer_bytes);

   size_t first = payload_offset(&layout, 0);
   size_t second = payload_offset(&layout, 1);
   if (!first || (first & 255) || (second & 255) ||
       first + PAYLOAD_BYTES >= second ||
       second + PAYLOAD_BYTES >= layout.buffer_bytes)
      fail("offline guarded layout invariant");
   if (!memcmp(input + first, input + second, PAYLOAD_BYTES))
      fail("copy2 dispatch inputs are not distinct");

   complete_expected_dispatch(expected, input, &layout, 0);
   if (memcmp(expected + first, input + first, PAYLOAD_BYTES) ||
       !memcmp(expected + second, input + second, PAYLOAD_BYTES))
      fail("first copy2 CPU oracle invariant");
   complete_expected_dispatch(expected, input, &layout, 1);
   if (memcmp(expected + second, input + second, PAYLOAD_BYTES))
      fail("second copy2 CPU oracle invariant");

   free(expected);
   free(output);
   free(input);
   printf("T8132_GLES_COMPUTE_VBO_LIFECYCLE_SELF_TEST_OK "
          "dispatches=2 render_frames=2 size=257x193 guards=yes\n");
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "--self-test"))
      return self_test();
   if (argc != 1) {
      fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
      return 2;
   }

   EGLDisplay display = open_asahi_display();
   EGLint egl_major = 0, egl_minor = 0;
   if (!eglInitialize(display, &egl_major, &egl_minor) ||
       !eglBindAPI(EGL_OPENGL_ES_API))
      fail("initialize EGL");

   const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE,
      EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES3_BIT_KHR,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_NONE,
   };
   EGLConfig config;
   EGLint config_count = 0;
   if (!eglChooseConfig(display, config_attributes, &config, 1,
                        &config_count) ||
       config_count != 1)
      fail("eglChooseConfig");

   const EGLint surface_attributes[] = {
      EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE,
   };
   EGLSurface surface =
      eglCreatePbufferSurface(display, config, surface_attributes);
   const EGLint context_attributes[] = {
      EGL_CONTEXT_MAJOR_VERSION_KHR,
      3,
      EGL_CONTEXT_MINOR_VERSION_KHR,
      1,
      EGL_NONE,
   };
   EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context))
      fail("create GLES 3.1 pbuffer context");

   const char *renderer_string = (const char *)glGetString(GL_RENDERER);
   const char *version_string = (const char *)glGetString(GL_VERSION);
   if (!renderer_string || !strstr(renderer_string, "Apple M4"))
      fail("unexpected GL renderer");
   char renderer[128], version[128];
   snprintf(renderer, sizeof(renderer), "%s", renderer_string);
   snprintf(version, sizeof(version), "%s",
            version_string ? version_string : "unknown");

   GLint alignment_value = 0;
   glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment_value);
   if (alignment_value <= 0)
      fail("invalid SSBO offset alignment");
   size_t alignment = (size_t)alignment_value;
   if (alignment < sizeof(uint32_t))
      alignment = sizeof(uint32_t);
   const struct buffer_layout layout = make_layout(alignment);
   if (layout.buffer_bytes > (size_t)INTPTR_MAX)
      fail("guarded SSBO is too large");

   uint8_t *input_seed = malloc(layout.buffer_bytes);
   uint8_t *output_seed = malloc(layout.buffer_bytes);
   uint8_t *output_expected = malloc(layout.buffer_bytes);
   if (!input_seed || !output_seed || !output_expected)
      fail("allocate guarded copy2 buffers");
   seed_poison(input_seed, layout.buffer_bytes, 0);
   seed_poison(output_seed, layout.buffer_bytes, 1);
   seed_inputs(input_seed, &layout);
   memcpy(output_expected, output_seed, layout.buffer_bytes);

   /* Link compute first and retain this exact program for both dispatches. */
   GLuint compute_program = build_copy2_program();
   GLuint render_program = build_triangle_program();
   check_gl("link lifecycle programs");

   GLuint ssbos[2] = {0};
   glGenBuffers(2, ssbos);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbos[0]);
   glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes, output_seed,
                GL_DYNAMIC_COPY);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbos[1]);
   glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes, input_seed,
                GL_DYNAMIC_COPY);

   static const float vertices[] = {
      0.0f, 0.82f, 1.0f,  0.0f,   0.0f, -0.82f, -0.72f, 0.0f,
      0.0f, 1.0f,  0.82f, -0.72f, 0.0f, 1.0f,   0.0f,
   };
   GLuint vao = 0, vbo = 0;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                         (const void *)0);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                         (const void *)(2 * sizeof(float)));
   check_gl("create guarded SSBOs and VBO triangle");

   dispatch_copy2(compute_program, ssbos[0], ssbos[1], &layout, 0);
   complete_expected_dispatch(output_expected, input_seed, &layout, 0);
   verify_buffer(ssbos[1], "input after first compute", input_seed,
                 layout.buffer_bytes);
   verify_buffer(ssbos[0], "output after first compute", output_expected,
                 layout.buffer_bytes);

   draw_vbo_triangle(render_program, vao);

   dispatch_copy2(compute_program, ssbos[0], ssbos[1], &layout, 1);
   complete_expected_dispatch(output_expected, input_seed, &layout, 1);
   verify_buffer(ssbos[1], "input after render and second compute", input_seed,
                 layout.buffer_bytes);
   verify_buffer(ssbos[0], "output after render and second compute",
                 output_expected, layout.buffer_bytes);

   printf("T8132_GLES_COMPUTE_VBO_LIFECYCLE_OK "
          "compute_dispatches=2 compiled_compute_programs=1 "
          "render_frames=2 size=257x193 exact_bytes=%#zx "
          "inputs_immutable=yes guards_and_gaps=yes renderer=\"%s\" "
          "version=\"%s\" EGL=%d.%d\n",
          2 * layout.buffer_bytes, renderer, version, egl_major, egl_minor);

   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
   glUseProgram(0);
   glDeleteBuffers(1, &vbo);
   glDeleteVertexArrays(1, &vao);
   glDeleteBuffers(2, ssbos);
   glDeleteProgram(render_program);
   glDeleteProgram(compute_program);
   free(output_expected);
   free(output_seed);
   free(input_seed);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return 0;
}
