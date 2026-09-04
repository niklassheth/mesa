/* SPDX-License-Identifier: MIT */

/* Generated GLSL math, checked against independent host libm results. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl31.h>

#include "t8132_apple9_compute_cases.h"

static const char *const names[] = {
   "sfu-rsqrt",
   "sfu-sqrt",
   "sfu-exp2",
   "sfu-log2",
   "sfu-floor",
   "sfu-ceil",
   "sfu-trunc",
   "sfu-round-even",
   "math-sin",
   "math-cos",
   "sfu-rsqrt-materialized",
   "sfu-sqrt-materialized",
   "sfu-exp2-materialized",
   "sfu-log2-materialized",
   "sfu-floor-materialized",
   "sfu-ceil-materialized",
   "sfu-trunc-materialized",
   "sfu-round-even-materialized",
   "math-sin-boundaries",
   "math-cos-boundaries",
   "math-sin-reduced",
   "math-cos-reduced",
};

const char *const *
t8132_apple9_math_case_names(size_t *count)
{
   *count = sizeof(names) / sizeof(names[0]);
   return names;
}

static uint32_t
bits(float x)
{
   uint32_t u;
   memcpy(&u, &x, sizeof(u));
   return u;
}

static float
from_bits(uint32_t u)
{
   float x;
   memcpy(&x, &u, sizeof(x));
   return x;
}

static float
reference(unsigned op, float x)
{
   /* The tested FP32 arithmetic mode treats denormal operands as signed zero. */
   if (op < 8 && fpclassify(x) == FP_SUBNORMAL)
      x = copysignf(0, x);
   double y;
   switch (op) {
   case 0:
      y = 1.0 / sqrt((double)x);
      break;
   case 1:
      y = sqrt((double)x);
      break;
   case 2:
      y = exp2((double)x);
      break;
   case 3:
      y = log2((double)x);
      break;
   case 4:
      y = floor((double)x);
      break;
   case 5:
      y = ceil((double)x);
      break;
   case 6:
      y = trunc((double)x);
      break;
   case 7:
      y = nearbyint((double)x);
      break;
   case 8:
      y = sin((double)x);
      break;
   case 9:
      y = cos((double)x);
      break;
   default:
      abort();
   }
   float result = y;
   return op < 8 && fpclassify(result) == FP_SUBNORMAL ? copysignf(0, result)
                                                       : result;
}

static bool
matches(float got, float expected, unsigned ulps)
{
   if (isnan(expected))
      return isnan(got);
   if (isinf(expected) || expected == 0)
      return bits(got) == bits(expected);
   if (!isfinite(got) || signbit(got) != signbit(expected))
      return false;
   uint32_t a = bits(got), b = bits(expected);
   return (a > b ? a - b : b - a) <= ulps;
}

int
t8132_apple9_run_math_case(const char *name)
{
   bool materialized = false, boundaries = false, reduced = false;
   char base_name[64];
   const char *suffix = strstr(name, "-materialized");
   if (suffix && suffix[13] == '\0') {
      snprintf(base_name, sizeof(base_name), "%.*s", (int)(suffix - name),
               name);
      name = base_name;
      materialized = true;
   }
   suffix = strstr(name, "-boundaries");
   if (suffix && suffix[11] == '\0') {
      snprintf(base_name, sizeof(base_name), "%.*s", (int)(suffix - name),
               name);
      name = base_name;
      boundaries = true;
   }
   suffix = strstr(name, "-reduced");
   if (suffix && suffix[8] == '\0') {
      snprintf(base_name, sizeof(base_name), "%.*s", (int)(suffix - name),
               name);
      name = base_name;
      reduced = true;
   }
   unsigned op;
   for (op = 0; op < 10; ++op)
      if (!strcmp(name, names[op]))
         break;
   if (op == 10)
      return 0;

   static const char *const functions[] = {
      "inversesqrt", "sqrt",  "exp2",      "log2", "floor",
      "ceil",        "trunc", "roundEven", "sin",  "cos",
   };
   enum { COUNT = 4096, GUARD = 64, WORDS = COUNT * 4 + GUARD * 2 };
   const uint32_t poison = 0xdeadbeef;
   uint32_t input[COUNT], output[WORDS];
   static const uint32_t directed[] = {
      0,          0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x3f800000,
      0xbf800000, 0x40000000, 0x40800000, 0x3f000000, 0xbf000000, 0x3fc00000,
      0xbfc00000, 0x40200000, 0xc0200000, 0x3f7fffff, 0x3f800001, 0x00800000,
      0x7f7fffff, 0x80800000, 0xff7fffff, 0x00000001, 0x007fffff, 0x80000001,
      0x807fffff, 0x39800000, 0x397fffff, 0x39800001, 0x3f490fda, 0x3f490fdb,
      0x3f490fdc, 0x3fc90fda, 0x3fc90fdb, 0x3fc90fdc,
   };
   uint32_t random = 0x6d2b79f5;
   for (unsigned i = 0; i < COUNT; ++i) {
      random = random * 1664525u + 1013904223u;
      input[i] = i < sizeof(directed) / sizeof(directed[0]) ? directed[i]
                 : op == 2 ? bits(((int)(random % 100001) - 50000) / 400.0f)
                           : random;
   }
   if (boundaries) {
      const double half_pi = 0x1.921fb54442d18p+0;
      for (unsigned i = 0; i < COUNT; ++i) {
         unsigned group = i / 16;
         float center = group < 128 ? (float)(group * half_pi)
                                    : (float)ldexp(half_pi, (int)group - 128);
         uint32_t u = bits(center);
         int delta = (int)(i % 8) - 4;
         if (u > 4 && u < 0x7f7ffffb)
            u += delta;
         input[i] = u | ((i & 8) ? 0x80000000 : 0);
      }
   }
   /* A dense sweep exercises the factor and its complement across every
    * quadrant, including both signs and the small-argument reduction. */
   if (reduced) {
      for (unsigned i = 0; i < COUNT; ++i)
         input[i] = bits(
            (float)(((double)i / (COUNT - 1) * 2 - 1) * 0x1.921fb54442d18p+1));
   }
   for (unsigned i = 0; i < WORDS; ++i)
      output[i] = poison;

   /* Separate repeated loads exercise pending and durable sources, while x
    * remains live across its SFU use. Results also feed an ordinary FMA. */
   char source[2048];
   snprintf(
      source, sizeof(source),
      "#version 310 es\nprecision highp float; precision highp int;\n"
      "layout(local_size_x=32) in;\n"
      "layout(std430,binding=0) readonly buffer I { uint input_data[]; };\n"
      "layout(std430,binding=1) buffer O { uint output_data[]; };\n"
      "void main() { uint i=gl_GlobalInvocationID.x;"
      "float x=uintBitsToFloat(input_data[i])%s;"
      "float y=%s(x);"
      "output_data[%uu+i*4u]=floatBitsToUint(y);"
      "output_data[%uu+i*4u]=floatBitsToUint(x);"
      "float z=%s(uintBitsToFloat(input_data[(i+37u)&4095u]));"
      "output_data[%uu+i*4u]=floatBitsToUint(z);"
      "output_data[%uu+i*4u]=floatBitsToUint(y*0.5+0.25); }",
      materialized ? "+0.25" : "", functions[op], GUARD, GUARD + 1,
      functions[op], GUARD + 2, GUARD + 3);
   GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
   const char *ptr = source;
   glShaderSource(shader, 1, &ptr, NULL);
   glCompileShader(shader);
   GLint okay = 0;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
   if (!okay) {
      char log[4096];
      glGetShaderInfoLog(shader, sizeof(log), NULL, log);
      fprintf(stderr, "%s: compile failed: %s\n%s\n", name, log, source);
      glDeleteShader(shader);
      return -1;
   }
   GLuint program = glCreateProgram();
   glAttachShader(program, shader);
   glLinkProgram(program);
   glGetProgramiv(program, GL_LINK_STATUS, &okay);
   if (!okay) {
      char log[4096];
      glGetProgramInfoLog(program, sizeof(log), NULL, log);
      fprintf(stderr, "%s: link failed: %s\n", name, log);
      glDeleteProgram(program);
      glDeleteShader(shader);
      return -1;
   }

   GLuint buffers[2];
   glGenBuffers(2, buffers);
   for (unsigned i = 0; i < 2; ++i) {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[i]);
      glBufferData(GL_SHADER_STORAGE_BUFFER, i ? sizeof(output) : sizeof(input),
                   i ? output : input, GL_DYNAMIC_COPY);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffers[i]);
   }
   glUseProgram(program);
   glDispatchCompute(COUNT / 32, 1, 1);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
   glFinish();
   unsigned failures = 0, component_failures[4] = {0};
   const uint32_t *read = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                           sizeof(output), GL_MAP_READ_BIT);
   if (!read) {
      fprintf(stderr, "%s: output mapping failed\n", name);
      failures++;
   } else {
      for (unsigned i = 0; i < WORDS; ++i) {
         if (i < GUARD || i >= WORDS - GUARD) {
            failures += read[i] != poison;
            continue;
         }
         unsigned lane = (i - GUARD) / 4, component = (i - GUARD) % 4;
         float x =
            from_bits(input[component == 2 ? (lane + 37) % COUNT : lane]);
         if (materialized && component != 2) {
            if (fpclassify(x) == FP_SUBNORMAL)
               x = copysignf(0, x);
            x += 0.25f;
         }
         float expected = component == 1 ? x : reference(op, x);
         unsigned tolerance = op < 4 || op >= 8 ? 2 : 0;
         if (component == 3) {
            /* y was independently checked above. Check its later consumer
             * exactly against that rounded SFU result: measuring error in
             * ULPs of y*0.5+0.25 exaggerates error near cancellation. */
            expected = fmaf(from_bits(read[i - 3]), 0.5f, 0.25f);
            tolerance = 0;
         }
         bool match =
            component == 1
               ? (isnan(x) ? isnan(from_bits(read[i])) : read[i] == bits(x))
               : matches(from_bits(read[i]), expected, tolerance);
         if (!match)
            component_failures[component]++;
         if (!match && failures++ < 12)
            fprintf(
               stderr,
               "%s lane=%u component=%u input=%08x got=%08x expected=%08x\n",
               name, lane, component, bits(x), read[i], bits(expected));
      }
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
   }
   /* An input write or guard corruption cannot hide behind a numerical pass. */
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
   read = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(input),
                           GL_MAP_READ_BIT);
   failures += !read || memcmp(read, input, sizeof(input)) != 0;
   if (read)
      glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
   failures += glGetError() != GL_NO_ERROR;
   glDeleteBuffers(2, buffers);
   glDeleteProgram(program);
   glDeleteShader(shader);
   fprintf(stderr, "%s: %u mismatches across %u values plus guards\n", name,
           failures, COUNT * 4);
   fprintf(stderr,
           "components: result=%u retained=%u final-use=%u consumer=%u\n",
           component_failures[0], component_failures[1], component_failures[2],
           component_failures[3]);
   return failures ? -1 : 1;
}
