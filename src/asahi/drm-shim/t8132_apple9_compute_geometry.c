/* SPDX-License-Identifier: MIT */

/* Geometry cases for the native Apple9 compute Piglit runner. */

#include <EGL/egl.h>
#include <GLES3/gl31.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t8132_apple9_compute_cases.h"

#define MAX_WIDTH       14u
#define MAX_HEIGHT      22u
#define MAX_DEPTH       26u
#define MAX_WORDS       (MAX_WIDTH * MAX_HEIGHT * MAX_DEPTH)
#define PAYLOAD_BYTES   (MAX_WORDS * 4u * sizeof(uint32_t))
#define MIN_GUARD_BYTES 256u

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
      fprintf(stderr, "T8132_GLES_COMPUTE_XYZ_FAIL: %s returned GL=%#x\n",
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

static GLuint
build_program(const struct dispatch_case *test)
{
   char source[2048];
   int length = snprintf(
      source, sizeof(source),
      "#version 310 es\n"
      "layout(local_size_x=%u, local_size_y=%u, local_size_z=%u) in;\n"
      "layout(std430, binding=0) buffer Output { uvec4 v[]; } outbuf;\n"
      "void main() {\n"
      "  uvec3 g = gl_GlobalInvocationID;\n"
      "  uvec3 l = gl_LocalInvocationID;\n"
      "  uvec3 w = gl_WorkGroupID;\n"
      "  uint i = g.x + %uu * (g.y + %uu * g.z);\n"
      "  uint pg = 0xa5000000u | (g.z << 16) | (g.y << 8) | g.x;\n"
      "  uint pl = 0xb6000000u | (l.z << 16) | (l.y << 8) | l.x;\n"
      "  uint pw = 0xc7000000u | (w.z << 16) | (w.y << 8) | w.x;\n"
      "  uint pi = gl_LocalInvocationIndex |"
      " (gl_WorkGroupSize.x << 8) | (gl_WorkGroupSize.y << 16) |"
      " (gl_WorkGroupSize.z << 24);\n"
      "  outbuf.v[i] = uvec4(pg, pl, pw, pi);\n"
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
      fprintf(stderr, "%s compute compile failed: %.*s\n", test->name, size,
              log);
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
      fprintf(stderr, "%s compute link failed: %.*s\n", test->name, size, log);
      fail("compute program link");
   }
   return program;
}

static struct buffer_layout
make_layout(size_t slots, size_t alignment)
{
   size_t first = align_up(MIN_GUARD_BYTES, alignment);
   size_t stride = align_up(PAYLOAD_BYTES + 2 * MIN_GUARD_BYTES, alignment);
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
   if (slot >
       (SIZE_MAX - layout->first_payload_offset) / layout->payload_stride)
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
            size_t index = x + test->width * (y + test->height * z);
            const uint32_t local_x = x % test->local[0];
            const uint32_t local_y = y % test->local[1];
            const uint32_t local_z = z % test->local[2];
            const uint32_t values[4] = {
               0xa5000000u | (z << 16) | (y << 8) | x,
               0xb6000000u | (local_z << 16) | (local_y << 8) | local_x,
               0xc7000000u | ((z / test->local[2]) << 16) |
                  ((y / test->local[1]) << 8) | (x / test->local[0]),
               (local_x +
                test->local[0] * (local_y + test->local[1] * local_z)) |
                  (test->local[0] << 8) | (test->local[1] << 16) |
                  (test->local[2] << 24),
            };
            memcpy(expected + base + index * sizeof(values), values,
                   sizeof(values));
         }
      }
   }
}

static void
verify_complete_buffer(GLuint buffer, const uint8_t *expected, size_t size)
{
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   const uint8_t *actual =
      glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, size, GL_MAP_READ_BIT);
   if (!actual)
      fail("map guarded output");

   bool saw_nonzero = false;
   for (size_t i = 0; i < size; ++i) {
      saw_nonzero |= actual[i] != 0;
      if (actual[i] != expected[i]) {
         fprintf(stderr, "XYZ output byte %#zx=%#x expected=%#x\n", i,
                 actual[i], expected[i]);
         fail("complete guarded output mismatch");
      }
   }
   if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER))
      fail("unmap guarded output");
   if (!saw_nonzero)
      fail("all output bytes are zero");
}

static const struct dispatch_case geometry_cases[] = {
   {
      .name = "full-grid-2d-asymmetric",
      .local = {3, 5, 1},
      .groups = {4, 2, 1},
      .width = 12,
      .height = 10,
      .depth = 1,
   },
   {
      .name = "full-grid-3d-asymmetric",
      .local = {2, 2, 2},
      .groups = {7, 11, 13},
      .width = 14,
      .height = 22,
      .depth = 26,
   },
};

const char *const *
t8132_apple9_geometry_case_names(size_t *count)
{
   static const char *const names[] = {
      "full-grid-2d-asymmetric",
      "full-grid-3d-asymmetric",
   };
   *count = sizeof(names) / sizeof(names[0]);
   return names;
}

void
t8132_apple9_run_geometry_case(const char *name)
{
   const struct dispatch_case *test = NULL;
   for (unsigned i = 0; i < sizeof(geometry_cases) / sizeof(geometry_cases[0]);
        ++i) {
      if (strcmp(name, geometry_cases[i].name) == 0) {
         test = &geometry_cases[i];
         break;
      }
   }
   if (!test)
      fail("unknown geometry case");

   GLuint program = build_program(test);
   check_gl("build geometry program");

   GLint alignment_value = 0;
   glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment_value);
   if (alignment_value <= 0)
      fail("invalid SSBO offset alignment");
   size_t alignment = (size_t)alignment_value;
   if (alignment < sizeof(uint32_t))
      alignment = sizeof(uint32_t);
   struct buffer_layout layout = make_layout(1, alignment);
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

   glUseProgram(program);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, output,
                     payload_offset(&layout, 0), PAYLOAD_BYTES);
   glDispatchCompute(test->groups[0], test->groups[1], test->groups[2]);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                   GL_SHADER_STORAGE_BARRIER_BIT);
   glFinish();
   check_gl("execute geometry case");

   write_expected(expected, &layout, 0, test);
   verify_complete_buffer(output, expected, layout.buffer_bytes);

   glDeleteProgram(program);
   glDeleteBuffers(1, &output);
   free(expected);
}
