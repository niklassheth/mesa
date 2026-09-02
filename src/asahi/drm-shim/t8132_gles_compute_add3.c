/* SPDX-License-Identifier: MIT */

/* Exact-output GLES 3.1 corpus for Apple9 SSBO load graphs. */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VALUE_COUNT        64u
#define INPUT_EXTRA_VALUES (VALUE_COUNT * 3u)
#define LOCAL_SIZE_X       32u
#define PAYLOAD_BYTES   ((VALUE_COUNT + INPUT_EXTRA_VALUES) * sizeof(uint32_t))
#define MIN_GUARD_BYTES 256u
#define DEFAULT_SUBMISSIONS               2u
#define DEFAULT_DISPATCHES_PER_SUBMISSION 2u
#define MAX_SUBMISSIONS                   64u
#define MAX_DISPATCHES_PER_SUBMISSION     256u
#define MAX_TOTAL_DISPATCHES              4096u

struct buffer_layout {
   size_t first_payload_offset;
   size_t payload_stride;
   size_t buffer_bytes;
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GLES_COMPUTE_ADD3_FAIL: %s (EGL=%#x GL=%#x)\n",
           message, eglGetError(), glGetError());
   exit(1);
}

static void
check_gl(const char *operation)
{
   GLenum error = glGetError();
   if (error != GL_NO_ERROR) {
      fprintf(stderr, "T8132_GLES_COMPUTE_ADD3_FAIL: %s returned GL=%#x\n",
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
parse_count(const char *text, unsigned maximum, const char *name)
{
   char *end = NULL;
   unsigned long parsed = strtoul(text, &end, 0);
   if (!text[0] || !end || *end || parsed == 0 || parsed > maximum) {
      fprintf(stderr, "invalid %s: %s\n", name, text);
      exit(2);
   }

   return (unsigned)parsed;
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

enum workload {
   WORKLOAD_ADD3,
   WORKLOAD_SPARSE_BINDINGS,
   WORKLOAD_AOS_LOAD,
   WORKLOAD_AOS_STORE,
   WORKLOAD_TWO_LOAD_FALU,
   WORKLOAD_SIX_PENDING,
   WORKLOAD_SEVEN_PENDING,
   WORKLOAD_FANOUT,
   WORKLOAD_TWO_LOAD_AND,
   WORKLOAD_TWO_LOAD_OR,
   WORKLOAD_TWO_LOAD_XOR,
   WORKLOAD_VECTOR_SWIZZLE,
   WORKLOAD_VECTOR_FANOUT,
   WORKLOAD_VECTOR2_COPY,
   WORKLOAD_VECTOR3_COPY,
   WORKLOAD_VECTOR4_COPY,
   WORKLOAD_VECTOR4_ALU_STORE,
   WORKLOAD_MIXED,
   WORKLOAD_DEPENDENT_INDEX,
   WORKLOAD_NESTED_DEPENDENT,
   WORKLOAD_SYSTEM_LOAD_INDEX,
   WORKLOAD_SYSTEM_GRID3D,
   WORKLOAD_VECTOR_SUITE,
   WORKLOAD_BRINGUP_SUITE,
};

static const char *
workload_name(enum workload workload)
{
   switch (workload) {
   case WORKLOAD_ADD3:
      return "add3";
   case WORKLOAD_SPARSE_BINDINGS:
      return "sparse-bindings";
   case WORKLOAD_AOS_LOAD:
      return "aos-load";
   case WORKLOAD_AOS_STORE:
      return "aos-store";
   case WORKLOAD_TWO_LOAD_FALU:
      return "two-load-falu";
   case WORKLOAD_SIX_PENDING:
      return "six-pending";
   case WORKLOAD_SEVEN_PENDING:
      return "seven-pending";
   case WORKLOAD_FANOUT:
      return "fanout";
   case WORKLOAD_TWO_LOAD_AND:
      return "two-load-and";
   case WORKLOAD_TWO_LOAD_OR:
      return "two-load-or";
   case WORKLOAD_TWO_LOAD_XOR:
      return "two-load-xor";
   case WORKLOAD_VECTOR_SWIZZLE:
      return "vector-swizzle";
   case WORKLOAD_VECTOR_FANOUT:
      return "vector-fanout";
   case WORKLOAD_VECTOR2_COPY:
      return "vector2-copy";
   case WORKLOAD_VECTOR3_COPY:
      return "vector3-copy";
   case WORKLOAD_VECTOR4_COPY:
      return "vector4-copy";
   case WORKLOAD_VECTOR4_ALU_STORE:
      return "vector4-alu-store";
   case WORKLOAD_MIXED:
      return "mixed";
   case WORKLOAD_DEPENDENT_INDEX:
      return "dependent-index";
   case WORKLOAD_NESTED_DEPENDENT:
      return "nested-dependent";
   case WORKLOAD_SYSTEM_LOAD_INDEX:
      return "system-load-index";
   case WORKLOAD_SYSTEM_GRID3D:
      return "system-grid3d";
   case WORKLOAD_VECTOR_SUITE:
      return "vector-suite";
   case WORKLOAD_BRINGUP_SUITE:
      return "bringup-suite";
   }
   fail("unknown workload");
   return NULL;
}

static enum workload
parse_workload(const char *name)
{
   for (unsigned workload = WORKLOAD_ADD3; workload <= WORKLOAD_BRINGUP_SUITE;
        ++workload) {
      if (strcmp(name, workload_name((enum workload)workload)) == 0)
         return (enum workload)workload;
   }
   fprintf(stderr, "unknown workload: %s\n", name);
   exit(2);
}

static GLuint
build_program(enum workload workload)
{
   static const char add3[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { float v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { float v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { float v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  outbuf.v[i] = a.v[i] + b.v[i];\n"
      "}\n";

   static const char sparse_bindings[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=3) readonly buffer InputA { float v[]; } a;\n"
      "layout(std430, binding=7) readonly buffer InputB { float v[]; } b;\n"
      "layout(std430, binding=5) buffer Output { float v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  outbuf.v[i] = a.v[i] + b.v[i];\n"
      "}\n";

   static const char aos_load[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "struct Item { uint p0; uint value; uint p2; uint p3; };\n"
      "layout(std430, binding=0) readonly buffer InputA { Item v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { uint v[]; } outbuf;\n"
      "void main() { uint i=gl_GlobalInvocationID.x; "
      "outbuf.v[i]=a.v[i].value^0x6d5a4b39u; }\n";

   static const char aos_store[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "struct Item { uint p0; uint p1; uint value; uint p3; };\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { Item v[]; } outbuf;\n"
      "void main() { uint i=gl_GlobalInvocationID.x; "
      "outbuf.v[i].value=a.v[i]^0x31415927u; }\n";

   static const char two_load_falu[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { float v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { float v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { float v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  float x = a.v[i];\n"
      "  float y = a.v[i + 1u];\n"
      "  outbuf.v[i] = x + y;\n"
      "}\n";

   static const char six_pending[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { float v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { float v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { float v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  float p0 = a.v[i + 0u] + 1.0;\n"
      "  float p1 = a.v[i + 1u] + 2.0;\n"
      "  float p2 = a.v[i + 2u] + 3.0;\n"
      "  float p3 = a.v[i + 3u] + 4.0;\n"
      "  float p4 = a.v[i + 4u] + 5.0;\n"
      "  float p5 = a.v[i + 5u] + 6.0;\n"
      "  float q0 = p0 + p1;\n"
      "  float q1 = p2 + p3;\n"
      "  float q2 = p4 + p5;\n"
      "  outbuf.v[i] = (q0 + q1) + q2;\n"
      "}\n";

   static const char seven_pending[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { float v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { float v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { float v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  float p0 = a.v[i + 0u] + 1.0;\n"
      "  float p1 = a.v[i + 1u] + 2.0;\n"
      "  float p2 = a.v[i + 2u] + 3.0;\n"
      "  float p3 = a.v[i + 3u] + 4.0;\n"
      "  float p4 = a.v[i + 4u] + 5.0;\n"
      "  float p5 = a.v[i + 5u] + 6.0;\n"
      "  float p6 = a.v[i + 6u] + 7.0;\n"
      "  float q0 = p0 + p1;\n"
      "  float q1 = p2 + p3;\n"
      "  float q2 = p4 + p5;\n"
      "  outbuf.v[i] = ((q0 + q1) + q2) + p6;\n"
      "}\n";

   static const char fanout[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { float v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { float v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { float v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  float x = a.v[i];\n"
      "  float y = b.v[i];\n"
      "  float sum = x + y;\n"
      "  float difference = x - y;\n"
      "  outbuf.v[i] = sum * difference;\n"
      "}\n";

   static const char mixed[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  float f = uintBitsToFloat(a.v[i]) + uintBitsToFloat(b.v[i]);\n"
      "  uint logic = a.v[i + 1u] ^ b.v[i + 1u];\n"
      "  outbuf.v[i] = floatBitsToUint(f) ^ logic;\n"
      "}\n";

   static const char two_load_xor[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  outbuf.v[i] = a.v[i] ^ b.v[i];\n"
      "}\n";

   static const char two_load_and[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  outbuf.v[i] = a.v[i] & b.v[i];\n"
      "}\n";

   static const char two_load_or[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  outbuf.v[i] = a.v[i] | b.v[i];\n"
      "}\n";

   static const char vector_swizzle[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uvec4 v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  uvec4 x = a.v[i];\n"
      "  outbuf.v[i] = ((x.z ^ x.x) + (x.w & x.y)) ^ b.v[i];\n"
      "}\n";

   static const char vector_fanout[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uvec4 v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  uvec4 x = a.v[i];\n"
      "  uint first = x.x ^ x.z;\n"
      "  uint retained = x.x | x.y;\n"
      "  outbuf.v[i] = (first + retained) ^ b.v[i];\n"
      "}\n";

   static const char vector2_copy[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uvec2 v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { uvec2 v[]; } outbuf;\n"
      "void main() { uint i = gl_GlobalInvocationID.x; outbuf.v[i] = a.v[i]; }\n";

   static const char vector3_copy[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uvec3 v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { uvec3 v[]; } outbuf;\n"
      "void main() { uint i = gl_GlobalInvocationID.x; outbuf.v[i] = a.v[i]; }\n";

   static const char vector4_copy[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uvec4 v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { uvec4 v[]; } outbuf;\n"
      "void main() { uint i = gl_GlobalInvocationID.x; outbuf.v[i] = a.v[i]; }\n";

   static const char vector4_alu_store[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uvec4 v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { uvec4 v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  uvec4 x = a.v[i];\n"
      "  outbuf.v[i] = x + uvec4(1u, 2u, 3u, 4u);\n"
      "}\n";

   static const char dependent_index[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  uint j = a.v[i] & 31u;\n"
      "  outbuf.v[i] = b.v[j] ^ i;\n"
      "}\n";

   static const char nested_dependent[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) readonly buffer InputB { uint v[]; } b;\n"
      "layout(std430, binding=2) readonly buffer InputC { uint v[]; } c;\n"
      "layout(std430, binding=3) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  uint root = a.v[i];\n"
      "  uint j = (root ^ i) & 63u;\n"
      "  uint middle = b.v[j];\n"
      "  uint k = (middle ^ (i * 3u + 1u)) & 63u;\n"
      "  uint leaf = c.v[k];\n"
      "  outbuf.v[i] = (leaf ^ root) ^ middle;\n"
      "}\n";

   static const char system_grid3d[] =
      "#version 310 es\n"
      "layout(local_size_x=2, local_size_y=2, local_size_z=2) in;\n"
      "layout(std430, binding=0) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uvec3 g = gl_GlobalInvocationID;\n"
      "  uvec3 l = gl_LocalInvocationID;\n"
      "  uvec3 w = gl_WorkGroupID;\n"
      "  uint i = g.x + g.y * 4u + g.z * 16u;\n"
      "  uint packed_value = l.x | (l.y << 2u) | (l.z << 4u);\n"
      "  packed_value |= gl_LocalInvocationIndex << 6u;\n"
      "  packed_value |= w.x << 10u;\n"
      "  packed_value |= w.y << 12u;\n"
      "  packed_value |= w.z << 14u;\n"
      "  outbuf.v[i] = packed_value;\n"
      "}\n";

   static const char system_load_index[] =
      "#version 310 es\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430, binding=0) readonly buffer InputA { uint v[]; } a;\n"
      "layout(std430, binding=1) buffer Output { uint v[]; } outbuf;\n"
      "void main() {\n"
      "  uint i = gl_GlobalInvocationID.x;\n"
      "  uint local = gl_LocalInvocationIndex;\n"
      "  uint direct_value = a.v[local];\n"
      "  uint derived_value = a.v[(local + 17u) & 63u];\n"
      "  outbuf.v[i] = direct_value ^ derived_value;\n"
      "}\n";

   const char *source = add3;
   switch (workload) {
   case WORKLOAD_ADD3:
      source = add3;
      break;
   case WORKLOAD_SPARSE_BINDINGS:
      source = sparse_bindings;
      break;
   case WORKLOAD_AOS_LOAD:
      source = aos_load;
      break;
   case WORKLOAD_AOS_STORE:
      source = aos_store;
      break;
   case WORKLOAD_TWO_LOAD_FALU:
      source = two_load_falu;
      break;
   case WORKLOAD_SIX_PENDING:
      source = six_pending;
      break;
   case WORKLOAD_SEVEN_PENDING:
      source = seven_pending;
      break;
   case WORKLOAD_FANOUT:
      source = fanout;
      break;
   case WORKLOAD_TWO_LOAD_AND:
      source = two_load_and;
      break;
   case WORKLOAD_TWO_LOAD_OR:
      source = two_load_or;
      break;
   case WORKLOAD_TWO_LOAD_XOR:
      source = two_load_xor;
      break;
   case WORKLOAD_VECTOR_SWIZZLE:
      source = vector_swizzle;
      break;
   case WORKLOAD_VECTOR_FANOUT:
      source = vector_fanout;
      break;
   case WORKLOAD_VECTOR2_COPY:
      source = vector2_copy;
      break;
   case WORKLOAD_VECTOR3_COPY:
      source = vector3_copy;
      break;
   case WORKLOAD_VECTOR4_COPY:
      source = vector4_copy;
      break;
   case WORKLOAD_VECTOR4_ALU_STORE:
      source = vector4_alu_store;
      break;
   case WORKLOAD_MIXED:
      source = mixed;
      break;
   case WORKLOAD_DEPENDENT_INDEX:
      source = dependent_index;
      break;
   case WORKLOAD_NESTED_DEPENDENT:
      source = nested_dependent;
      break;
   case WORKLOAD_SYSTEM_LOAD_INDEX:
      source = system_load_index;
      break;
   case WORKLOAD_SYSTEM_GRID3D:
      source = system_grid3d;
      break;
   case WORKLOAD_VECTOR_SUITE:
   case WORKLOAD_BRINGUP_SUITE:
      fail("suite has no single shader");
      break;
   }

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
      fprintf(stderr, "%s compute compile failed: %.*s\n",
              workload_name(workload), size, log);
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
      fprintf(stderr, "%s compute link failed: %.*s\n", workload_name(workload),
              size, log);
      fail("compute program link");
   }

   return program;
}

static uint32_t
float_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

static float
bits_float(uint32_t bits)
{
   float value;
   memcpy(&value, &bits, sizeof(value));
   return value;
}

static uint32_t
input_a_bits(size_t slot, uint32_t element)
{
   uint32_t numerator = element * 4u + 2u + (uint32_t)slot * 1024u;
   return float_bits((float)numerator * (1.0f / 16.0f));
}

static uint32_t
input_b_bits(size_t slot, uint32_t element)
{
   uint32_t numerator = element * 3u + 2u + (uint32_t)slot * 2048u;
   return float_bits((float)numerator * (1.0f / 16.0f));
}

static uint32_t
input_c_bits(size_t slot, uint32_t element)
{
   uint32_t value = 0x9e3779b9u * (element + 1u);
   value ^= (uint32_t)slot * 0x85ebca6bu;
   value ^= value >> 13;
   return value;
}

static size_t
payload_offset(const struct buffer_layout *layout, size_t slot)
{
   if (slot >
       (SIZE_MAX - layout->first_payload_offset) / layout->payload_stride)
      fail("payload offset overflow");

   return layout->first_payload_offset + slot * layout->payload_stride;
}

static struct buffer_layout
make_layout(size_t slot_count, size_t alignment)
{
   const size_t first_payload_offset = align_up(MIN_GUARD_BYTES, alignment);
   const size_t payload_stride =
      align_up(PAYLOAD_BYTES + 2 * MIN_GUARD_BYTES, alignment);

   if (slot_count == 0 ||
       slot_count - 1 > (SIZE_MAX - first_payload_offset) / payload_stride)
      fail("buffer layout overflow");

   const size_t last_payload =
      first_payload_offset + (slot_count - 1) * payload_stride;
   if (last_payload > SIZE_MAX - PAYLOAD_BYTES - MIN_GUARD_BYTES)
      fail("buffer size overflow");

   return (struct buffer_layout){
      .first_payload_offset = first_payload_offset,
      .payload_stride = payload_stride,
      .buffer_bytes = last_payload + PAYLOAD_BYTES + MIN_GUARD_BYTES,
   };
}

static uint8_t
poison_byte(unsigned resource, size_t offset)
{
   uint32_t value = (uint32_t)offset * 0x45d9f3bu;
   value ^= 0xa5c39e17u + resource * 0x31415927u;
   value ^= value >> 16;
   return (uint8_t)(value | 1u);
}

static void
seed_buffer(uint8_t *buffer, size_t size, unsigned resource)
{
   for (size_t i = 0; i < size; ++i)
      buffer[i] = poison_byte(resource, i);
}

static void
write_word(uint8_t *buffer, size_t offset, uint32_t value)
{
   memcpy(buffer + offset, &value, sizeof(value));
}

static void
seed_inputs(uint8_t *input_a, uint8_t *input_b, uint8_t *input_c,
            const struct buffer_layout *layout, size_t slot_count)
{
   for (size_t slot = 0; slot < slot_count; ++slot) {
      const size_t base = payload_offset(layout, slot);
      for (uint32_t i = 0; i < VALUE_COUNT + INPUT_EXTRA_VALUES; ++i) {
         /*
          * All three values are exact binary fractions whose numerators stay
          * below 2^24 at MAX_TOTAL_DISPATCHES.  Their sums therefore have an
          * unambiguous IEEE-754 single-precision bit pattern.
          */
         write_word(input_a, base + i * sizeof(uint32_t),
                    input_a_bits(slot, i));
         write_word(input_b, base + i * sizeof(uint32_t),
                    input_b_bits(slot, i));
         write_word(input_c, base + i * sizeof(uint32_t),
                    input_c_bits(slot, i));
      }
   }
}

static void
write_expected_output(enum workload workload, uint8_t *output,
                      const struct buffer_layout *layout, size_t slot)
{
   const size_t base = payload_offset(layout, slot);
   for (uint32_t i = 0; i < VALUE_COUNT; ++i) {
      if (workload == WORKLOAD_VECTOR2_COPY ||
          workload == WORKLOAD_VECTOR3_COPY ||
          workload == WORKLOAD_VECTOR4_COPY ||
          workload == WORKLOAD_VECTOR4_ALU_STORE) {
         const unsigned components = workload == WORKLOAD_VECTOR2_COPY   ? 2
                                     : workload == WORKLOAD_VECTOR3_COPY ? 3
                                                                         : 4;
         const unsigned stride = components == 2 ? 2 : 4;
         for (unsigned c = 0; c < components; ++c) {
            uint32_t value = input_a_bits(slot, i * stride + c);
            if (workload == WORKLOAD_VECTOR4_ALU_STORE)
               value += c + 1;
            write_word(output, base + (i * stride + c) * sizeof(uint32_t),
                       value);
         }
         continue;
      }

      float a[7], b;
      for (unsigned j = 0; j < 7; ++j)
         a[j] = bits_float(input_a_bits(slot, i + j));
      b = bits_float(input_b_bits(slot, i));

      uint32_t result = 0;
      switch (workload) {
      case WORKLOAD_ADD3:
      case WORKLOAD_SPARSE_BINDINGS:
         result = float_bits(a[0] + b);
         break;
      case WORKLOAD_AOS_LOAD:
         result = input_a_bits(slot, i * 4u + 1u) ^ 0x6d5a4b39u;
         break;
      case WORKLOAD_AOS_STORE:
         result = input_a_bits(slot, i) ^ 0x31415927u;
         write_word(output, base + (i * 4u + 2u) * sizeof(uint32_t), result);
         continue;
      case WORKLOAD_TWO_LOAD_FALU:
         result = float_bits(a[0] + a[1]);
         break;
      case WORKLOAD_SIX_PENDING:
      case WORKLOAD_SEVEN_PENDING: {
         float p[7];
         unsigned count = workload == WORKLOAD_SIX_PENDING ? 6 : 7;
         for (unsigned j = 0; j < count; ++j)
            p[j] = a[j] + (float)(j + 1);
         float q0 = p[0] + p[1];
         float q1 = p[2] + p[3];
         float q2 = p[4] + p[5];
         float combined = (q0 + q1) + q2;
         if (count == 7)
            combined += p[6];
         result = float_bits(combined);
         break;
      }
      case WORKLOAD_FANOUT: {
         float sum = a[0] + b;
         float difference = a[0] - b;
         result = float_bits(sum * difference);
         break;
      }
      case WORKLOAD_TWO_LOAD_AND:
         result = input_a_bits(slot, i) & input_b_bits(slot, i);
         break;
      case WORKLOAD_TWO_LOAD_OR:
         result = input_a_bits(slot, i) | input_b_bits(slot, i);
         break;
      case WORKLOAD_TWO_LOAD_XOR:
         result = input_a_bits(slot, i) ^ input_b_bits(slot, i);
         break;
      case WORKLOAD_VECTOR_SWIZZLE: {
         uint32_t x = input_a_bits(slot, i * 4u + 0u);
         uint32_t y = input_a_bits(slot, i * 4u + 1u);
         uint32_t z = input_a_bits(slot, i * 4u + 2u);
         uint32_t w = input_a_bits(slot, i * 4u + 3u);
         result = ((z ^ x) + (w & y)) ^ input_b_bits(slot, i);
         break;
      }
      case WORKLOAD_VECTOR_FANOUT: {
         uint32_t x = input_a_bits(slot, i * 4u + 0u);
         uint32_t y = input_a_bits(slot, i * 4u + 1u);
         uint32_t z = input_a_bits(slot, i * 4u + 2u);
         uint32_t first = x ^ z;
         uint32_t retained = x | y;
         result = (first + retained) ^ input_b_bits(slot, i);
         break;
      }
      case WORKLOAD_VECTOR2_COPY:
      case WORKLOAD_VECTOR3_COPY:
      case WORKLOAD_VECTOR4_COPY:
      case WORKLOAD_VECTOR4_ALU_STORE:
         fail("unreachable vector-copy oracle");
         break;
      case WORKLOAD_MIXED: {
         uint32_t sum = float_bits(a[0] + b);
         uint32_t logic = input_a_bits(slot, i + 1) ^ input_b_bits(slot, i + 1);
         result = sum ^ logic;
         break;
      }
      case WORKLOAD_DEPENDENT_INDEX: {
         uint32_t j = input_a_bits(slot, i) & 31u;
         result = input_b_bits(slot, j) ^ i;
         break;
      }
      case WORKLOAD_NESTED_DEPENDENT: {
         uint32_t root = input_a_bits(slot, i);
         uint32_t j = (root ^ i) & 63u;
         uint32_t middle = input_b_bits(slot, j);
         uint32_t k = (middle ^ (i * 3u + 1u)) & 63u;
         uint32_t leaf = input_c_bits(slot, k);
         result = (leaf ^ root) ^ middle;
         break;
      }
      case WORKLOAD_SYSTEM_LOAD_INDEX: {
         uint32_t local = i & 31u;
         result =
            input_a_bits(slot, local) ^ input_a_bits(slot, (local + 17u) & 63u);
         break;
      }
      case WORKLOAD_SYSTEM_GRID3D: {
         uint32_t global_x = i & 3u;
         uint32_t global_y = (i >> 2u) & 3u;
         uint32_t global_z = (i >> 4u) & 3u;
         uint32_t local_x = global_x & 1u;
         uint32_t local_y = global_y & 1u;
         uint32_t local_z = global_z & 1u;
         uint32_t local_index = local_x | (local_y << 1u) | (local_z << 2u);
         result = local_x | (local_y << 2u) | (local_z << 4u) |
                  (local_index << 6u) | ((global_x >> 1u) << 10u) |
                  ((global_y >> 1u) << 12u) | ((global_z >> 1u) << 14u);
         break;
      }
      case WORKLOAD_VECTOR_SUITE:
      case WORKLOAD_BRINGUP_SUITE:
         fail("suite oracle requires a concrete workload");
         break;
      }
      write_word(output, base + i * sizeof(uint32_t), result);
   }
}

static void
report_byte_mismatch(const char *name, const uint8_t *actual,
                     const uint8_t *expected, size_t size)
{
   for (size_t i = 0; i < size; ++i) {
      if (actual[i] != expected[i]) {
         size_t word_offset = i & ~(sizeof(uint32_t) - 1);
         uint32_t actual_word = 0, expected_word = 0;
         if (word_offset + sizeof(uint32_t) <= size) {
            memcpy(&actual_word, actual + word_offset, sizeof(actual_word));
            memcpy(&expected_word, expected + word_offset,
                   sizeof(expected_word));
         }
         fprintf(stderr,
                 "%s byte %#zx=%#x expected=%#x word[%#zx]=%#x expected=%#x\n",
                 name, i, actual[i], expected[i], word_offset, actual_word,
                 expected_word);
         fail("guarded buffer mismatch");
      }
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
      fail("map guarded buffer");

   report_byte_mismatch(name, mapped, expected, size);
   if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER))
      fail("unmap guarded buffer");
}

static void
verify_completed_payloads(GLuint output, const struct buffer_layout *layout,
                          size_t completed_slots, enum workload workload)
{
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, output);
   const uint8_t *mapped = glMapBufferRange(
      GL_SHADER_STORAGE_BUFFER, 0, layout->buffer_bytes, GL_MAP_READ_BIT);
   if (!mapped)
      fail("map output payloads");

   bool saw_nonzero = false;
   for (size_t slot = 0; slot < completed_slots; ++slot) {
      size_t base = payload_offset(layout, slot);
      for (uint32_t i = 0; i < VALUE_COUNT; ++i) {
         uint32_t value;
         size_t element = workload == WORKLOAD_AOS_STORE ? i * 4u + 2u : i;
         memcpy(&value, mapped + base + element * sizeof(value), sizeof(value));
         saw_nonzero |= value != 0;
      }
   }

   if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER))
      fail("unmap output payloads");
   if (!saw_nonzero)
      fail("all completed results are zero");
}

int
main(int argc, char **argv)
{
   if (argc > 4) {
      fprintf(
         stderr,
         "usage: %s [SUBMISSIONS [DISPATCHES_PER_SUBMISSION [WORKLOAD]]]\n",
         argv[0]);
      return 2;
   }

   unsigned submissions =
      argc >= 2 ? parse_count(argv[1], MAX_SUBMISSIONS, "submission count")
                : DEFAULT_SUBMISSIONS;
   unsigned dispatches_per_submission =
      argc >= 3
         ? parse_count(argv[2], MAX_DISPATCHES_PER_SUBMISSION, "dispatch count")
         : DEFAULT_DISPATCHES_PER_SUBMISSION;
   enum workload workload = argc >= 4 ? parse_workload(argv[3]) : WORKLOAD_ADD3;
   static const enum workload vector_suite[] = {
      WORKLOAD_VECTOR2_COPY,      WORKLOAD_VECTOR3_COPY,
      WORKLOAD_VECTOR4_COPY,      WORKLOAD_VECTOR4_ALU_STORE,
      WORKLOAD_VECTOR_SWIZZLE,    WORKLOAD_VECTOR_FANOUT,
   };
   static const enum workload bringup_suite[] = {
      WORKLOAD_VECTOR2_COPY,    WORKLOAD_VECTOR3_COPY,
      WORKLOAD_VECTOR4_COPY,    WORKLOAD_VECTOR4_ALU_STORE,
      WORKLOAD_VECTOR_SWIZZLE,  WORKLOAD_VECTOR_FANOUT,
      WORKLOAD_SYSTEM_LOAD_INDEX, WORKLOAD_SYSTEM_GRID3D,
      WORKLOAD_SPARSE_BINDINGS, WORKLOAD_AOS_LOAD,
      WORKLOAD_AOS_STORE,
   };
   const enum workload *suite = NULL;
   unsigned suite_cases = 1;
   if (workload == WORKLOAD_VECTOR_SUITE) {
      suite = vector_suite;
      suite_cases = sizeof(vector_suite) / sizeof(vector_suite[0]);
   } else if (workload == WORKLOAD_BRINGUP_SUITE) {
      suite = bringup_suite;
      suite_cases = sizeof(bringup_suite) / sizeof(bringup_suite[0]);
   }

   if (submissions > MAX_TOTAL_DISPATCHES / dispatches_per_submission)
      fail("too many total dispatches");
   size_t slot_count = submissions * dispatches_per_submission;
   if (slot_count > MAX_TOTAL_DISPATCHES / suite_cases)
      fail("too many suite dispatches");
   slot_count *= suite_cases;

   EGLDisplay display = open_asahi_display();
   EGLint major = 0, minor = 0;
   if (!eglInitialize(display, &major, &minor) ||
       !eglBindAPI(EGL_OPENGL_ES_API))
      fail("initialize EGL");

   const EGLint config_attrs[] = {
      EGL_SURFACE_TYPE,       EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES3_BIT_KHR, EGL_NONE,
   };
   EGLConfig config;
   EGLint config_count = 0;
   if (!eglChooseConfig(display, config_attrs, &config, 1, &config_count) ||
       config_count != 1)
      fail("eglChooseConfig");

   const EGLint surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
   EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
   const EGLint context_attrs[] = {
      EGL_CONTEXT_MAJOR_VERSION_KHR,
      3,
      EGL_CONTEXT_MINOR_VERSION_KHR,
      1,
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

   GLint alignment_value = 0;
   glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment_value);
   if (alignment_value <= 0)
      fail("invalid SSBO offset alignment");
   size_t alignment = (size_t)alignment_value;
   if (alignment < sizeof(uint32_t))
      alignment = sizeof(uint32_t);
   struct buffer_layout layout = make_layout(slot_count, alignment);
   if (layout.buffer_bytes > (size_t)INTPTR_MAX)
      fail("guarded buffer is too large");

   uint8_t *input_a_seed = malloc(layout.buffer_bytes);
   uint8_t *input_b_seed = malloc(layout.buffer_bytes);
   uint8_t *input_c_seed = malloc(layout.buffer_bytes);
   uint8_t *output_expected = malloc(layout.buffer_bytes);
   if (!input_a_seed || !input_b_seed || !input_c_seed || !output_expected)
      fail("allocate guarded buffers");

   seed_buffer(input_a_seed, layout.buffer_bytes, 0);
   seed_buffer(input_b_seed, layout.buffer_bytes, 1);
   seed_buffer(input_c_seed, layout.buffer_bytes, 2);
   seed_buffer(output_expected, layout.buffer_bytes, 3);
   seed_inputs(input_a_seed, input_b_seed, input_c_seed, &layout, slot_count);

   GLuint buffers[4] = {0};
   glGenBuffers(4, buffers);
   const void *buffer_seeds[] = {
      input_a_seed,
      input_b_seed,
      input_c_seed,
      output_expected,
   };
   for (unsigned i = 0; i < 4; ++i) {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[i]);
      glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes,
                   buffer_seeds[i], GL_DYNAMIC_COPY);
   }
   check_gl("create guarded SSBOs");

   size_t completed_slots = 0;
   for (unsigned suite_case = 0; suite_case < suite_cases; ++suite_case) {
      const enum workload active_workload =
         suite ? suite[suite_case] : workload;
      const bool has_input_c = active_workload == WORKLOAD_NESTED_DEPENDENT;
      const bool has_one_input = active_workload == WORKLOAD_VECTOR2_COPY ||
                                 active_workload == WORKLOAD_VECTOR3_COPY ||
                                 active_workload == WORKLOAD_VECTOR4_COPY ||
                                 active_workload ==
                                    WORKLOAD_VECTOR4_ALU_STORE ||
                                 active_workload == WORKLOAD_SYSTEM_LOAD_INDEX ||
                                 active_workload == WORKLOAD_AOS_LOAD ||
                                 active_workload == WORKLOAD_AOS_STORE;
      const bool has_no_input = active_workload == WORKLOAD_SYSTEM_GRID3D;
      const unsigned input_count = has_input_c     ? 3
                                   : has_one_input ? 1
                                   : has_no_input  ? 0
                                                   : 2;
      const unsigned output_binding = input_count;
      GLuint program = build_program(active_workload);
      glUseProgram(program);
      check_gl("build and use compute program");

      for (unsigned submit = 0; submit < submissions; ++submit) {
         for (unsigned dispatch = 0; dispatch < dispatches_per_submission;
              ++dispatch) {
            size_t slot = completed_slots + dispatch;
            size_t offset = payload_offset(&layout, slot);
            if (active_workload == WORKLOAD_SPARSE_BINDINGS) {
               glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, buffers[0],
                                 offset, PAYLOAD_BYTES);
               glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 7, buffers[1],
                                 offset, PAYLOAD_BYTES);
               glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 5, buffers[3],
                                 offset, PAYLOAD_BYTES);
            } else {
               for (unsigned binding = 0; binding < input_count; ++binding) {
                  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, binding,
                                    buffers[binding], offset, PAYLOAD_BYTES);
               }
               glBindBufferRange(GL_SHADER_STORAGE_BUFFER, output_binding,
                                 buffers[3], offset, PAYLOAD_BYTES);
            }
            if (active_workload == WORKLOAD_SYSTEM_GRID3D)
               glDispatchCompute(2, 2, 2);
            else
               glDispatchCompute(VALUE_COUNT / LOCAL_SIZE_X, 1, 1);
         }
         check_gl("dispatch compute workload");

         glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                         GL_SHADER_STORAGE_BARRIER_BIT);
         glFinish();
         check_gl("finish compute submission");

         size_t first_completed = completed_slots;
         completed_slots += dispatches_per_submission;
         for (size_t slot = first_completed; slot < completed_slots; ++slot)
            write_expected_output(active_workload, output_expected, &layout,
                                  slot);

         /* Inputs are caller-owned and must remain byte-for-byte immutable. */
         verify_buffer(buffers[0], "input-a", input_a_seed,
                       layout.buffer_bytes);
         verify_buffer(buffers[1], "input-b", input_b_seed,
                       layout.buffer_bytes);
         verify_buffer(buffers[2], "input-c", input_c_seed,
                       layout.buffer_bytes);

         /*
          * Comparing the complete allocation covers every leading,
          * inter-range, trailing and not-yet-dispatched byte in addition to
          * all result words.
          */
         verify_buffer(buffers[3], "output", output_expected,
                       layout.buffer_bytes);
         verify_completed_payloads(buffers[3], &layout, completed_slots,
                                   active_workload);
      }

      glDeleteProgram(program);
   }

   printf("T8132_GLES_COMPUTE_ADD3_OK workload=%s submissions=%u "
          "dispatches_per_submission=%u total_dispatches=%zu values=%u "
          "ssbo_alignment=%zu first_offset=%#zx stride=%#zx "
          "renderer=\"%s\" version=\"%s\"\n",
          workload_name(workload), submissions, dispatches_per_submission,
          slot_count, VALUE_COUNT, alignment, layout.first_payload_offset,
          layout.payload_stride, renderer, version);

   glDeleteBuffers(4, buffers);
   free(output_expected);
   free(input_c_seed);
   free(input_b_seed);
   free(input_a_seed);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return 0;
}
