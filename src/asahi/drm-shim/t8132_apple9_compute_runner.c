/* SPDX-License-Identifier: MIT */

/* Native Piglit runner for GLES 3.1 -> NIR -> Apple9 compute tests. */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t8132_apple9_compute_cases.h"

#define VALUE_COUNT     16384u
#define LOCAL_SIZE      256u
#define MIN_GUARD_BYTES 256u

enum workload {
   WORKLOAD_CONSTANT,
   WORKLOAD_CONSTANT32,
   WORKLOAD_CONSTANT32_SPARSE,
   WORKLOAD_GID,
   WORKLOAD_MAD,
   WORKLOAD_DAG,
   WORKLOAD_REUSE_DAG,
   WORKLOAD_SELECT_DAG,
   WORKLOAD_COMPARE_DAG,
   WORKLOAD_COMPARE_COMPLETE,
   WORKLOAD_DEEP_INT_DAG,
   WORKLOAD_DIAMOND_INT_DAG,
   WORKLOAD_FANOUT_INT_DAG,
   WORKLOAD_LOGIC_LIFETIME_DAG,
   WORKLOAD_PRESSURE_INT_DAG,
   WORKLOAD_MINMAX_INT_DAG,
   WORKLOAD_NESTED_SELECT_DAG,
   WORKLOAD_DEEP_FLOAT_DAG,
   WORKLOAD_FANOUT_FLOAT_DAG,
   WORKLOAD_MIXED_DOMAIN_DAG,
   WORKLOAD_RADIX_ALTERNATING_DAG,
   WORKLOAD_SELECT_ALL_LIVE_DAG,
   WORKLOAD_MINMAX_NESTED_LIVE_DAG,
   WORKLOAD_FMA_ALL_LIVE_DAG,
   WORKLOAD_FLOAT_CACHE_RING_DAG,
   WORKLOAD_CROSS_DOMAIN_CACHE_DAG,
   WORKLOAD_LOGIC_MINMAX_SELECT_DAG,
   WORKLOAD_CACHE_PRESSURE_DAG,
   WORKLOAD_ADD,
   WORKLOAD_SUB,
   WORKLOAD_RSUB,
   WORKLOAD_MUL,
   WORKLOAD_AND,
   WORKLOAD_OR,
   WORKLOAD_XOR,
   WORKLOAD_NOT,
   WORKLOAD_INEG,
   WORKLOAD_U2F,
   WORKLOAD_U2F_LAST_USE,
   WORKLOAD_I2F,
   WORKLOAD_I2F_RETAINED,
   WORKLOAD_F2I,
   WORKLOAD_F2U,
   WORKLOAD_SHL,
   WORKLOAD_ASHR,
   WORKLOAD_USHR,
   WORKLOAD_IMIN,
   WORKLOAD_IMAX,
   WORKLOAD_UMIN,
   WORKLOAD_UMAX,
   WORKLOAD_FADD,
   WORKLOAD_FSUB,
   WORKLOAD_RFSUB,
   WORKLOAD_FMUL,
   WORKLOAD_FMIN,
   WORKLOAD_FMAX,
   WORKLOAD_FABS,
   WORKLOAD_FNEG,
   WORKLOAD_FMA,
   WORKLOAD_FMA_NAN_MUL,
   WORKLOAD_ARCHIVE_CROSS_0,
   WORKLOAD_ARCHIVE_CROSS_1,
   WORKLOAD_ARCHIVE_CROSS_2,
   WORKLOAD_ARCHIVE_CROSS_3,
   WORKLOAD_ARCHIVE_CROSS_4,
   WORKLOAD_ARCHIVE_CROSS_5,
   WORKLOAD_ARCHIVE_CROSS_6,
   WORKLOAD_ARCHIVE_CROSS_7,
   WORKLOAD_COUNT,
};

static const char *workload_names[WORKLOAD_COUNT] = {
   [WORKLOAD_CONSTANT] = "constant",
   [WORKLOAD_CONSTANT32] = "constant32",
   [WORKLOAD_CONSTANT32_SPARSE] = "constant32-sparse",
   [WORKLOAD_GID] = "gid",
   [WORKLOAD_MAD] = "mad",
   [WORKLOAD_DAG] = "dag",
   [WORKLOAD_REUSE_DAG] = "reuse-dag",
   [WORKLOAD_SELECT_DAG] = "select-dag",
   [WORKLOAD_COMPARE_DAG] = "compare-dag",
   [WORKLOAD_COMPARE_COMPLETE] = "compare-complete",
   [WORKLOAD_DEEP_INT_DAG] = "deep-int-dag",
   [WORKLOAD_DIAMOND_INT_DAG] = "diamond-int-dag",
   [WORKLOAD_FANOUT_INT_DAG] = "fanout-int-dag",
   [WORKLOAD_LOGIC_LIFETIME_DAG] = "logic-lifetime-dag",
   [WORKLOAD_PRESSURE_INT_DAG] = "pressure-int-dag",
   [WORKLOAD_MINMAX_INT_DAG] = "minmax-int-dag",
   [WORKLOAD_NESTED_SELECT_DAG] = "nested-select-dag",
   [WORKLOAD_DEEP_FLOAT_DAG] = "deep-float-dag",
   [WORKLOAD_FANOUT_FLOAT_DAG] = "fanout-float-dag",
   [WORKLOAD_MIXED_DOMAIN_DAG] = "mixed-domain-dag",
   [WORKLOAD_RADIX_ALTERNATING_DAG] = "radix-alternating-dag",
   [WORKLOAD_SELECT_ALL_LIVE_DAG] = "select-all-live-dag",
   [WORKLOAD_MINMAX_NESTED_LIVE_DAG] = "minmax-nested-live-dag",
   [WORKLOAD_FMA_ALL_LIVE_DAG] = "fma-all-live-dag",
   [WORKLOAD_FLOAT_CACHE_RING_DAG] = "float-cache-ring-dag",
   [WORKLOAD_CROSS_DOMAIN_CACHE_DAG] = "cross-domain-cache-dag",
   [WORKLOAD_LOGIC_MINMAX_SELECT_DAG] = "logic-minmax-select-dag",
   [WORKLOAD_CACHE_PRESSURE_DAG] = "cache-pressure-dag",
   [WORKLOAD_ADD] = "add",
   [WORKLOAD_SUB] = "sub",
   [WORKLOAD_RSUB] = "rsub",
   [WORKLOAD_MUL] = "mul",
   [WORKLOAD_AND] = "and",
   [WORKLOAD_OR] = "or",
   [WORKLOAD_XOR] = "xor",
   [WORKLOAD_NOT] = "not",
   [WORKLOAD_INEG] = "ineg",
   [WORKLOAD_U2F] = "u2f",
   [WORKLOAD_U2F_LAST_USE] = "u2f-last-use",
   [WORKLOAD_I2F] = "i2f",
   [WORKLOAD_I2F_RETAINED] = "i2f-retained",
   [WORKLOAD_F2I] = "f2i",
   [WORKLOAD_F2U] = "f2u",
   [WORKLOAD_SHL] = "shl",
   [WORKLOAD_ASHR] = "ashr",
   [WORKLOAD_USHR] = "ushr",
   [WORKLOAD_IMIN] = "imin",
   [WORKLOAD_IMAX] = "imax",
   [WORKLOAD_UMIN] = "umin",
   [WORKLOAD_UMAX] = "umax",
   [WORKLOAD_FADD] = "fadd",
   [WORKLOAD_FSUB] = "fsub",
   [WORKLOAD_RFSUB] = "rfsub",
   [WORKLOAD_FMUL] = "fmul",
   [WORKLOAD_FMIN] = "fmin",
   [WORKLOAD_FMAX] = "fmax",
   [WORKLOAD_FABS] = "fabs",
   [WORKLOAD_FNEG] = "fneg",
   [WORKLOAD_FMA] = "fma",
   [WORKLOAD_FMA_NAN_MUL] = "fma-nan-mul",
   [WORKLOAD_ARCHIVE_CROSS_0] = "archive-cross-0",
   [WORKLOAD_ARCHIVE_CROSS_1] = "archive-cross-1",
   [WORKLOAD_ARCHIVE_CROSS_2] = "archive-cross-2",
   [WORKLOAD_ARCHIVE_CROSS_3] = "archive-cross-3",
   [WORKLOAD_ARCHIVE_CROSS_4] = "archive-cross-4",
   [WORKLOAD_ARCHIVE_CROSS_5] = "archive-cross-5",
   [WORKLOAD_ARCHIVE_CROSS_6] = "archive-cross-6",
   [WORKLOAD_ARCHIVE_CROSS_7] = "archive-cross-7",
};

static const char *workload_expressions[WORKLOAD_COUNT] = {
   [WORKLOAD_CONSTANT] = "42u",
   [WORKLOAD_CONSTANT32] = "0xdeadbeefu",
   [WORKLOAD_CONSTANT32_SPARSE] = "0x10000001u",
   [WORKLOAD_GID] = "gid",
   [WORKLOAD_MAD] = "gid * 0x01020305u + 0xdeadbeefu",
   [WORKLOAD_DAG] = "((gid * 3u) ^ (gid + 7u)) + 11u",
   [WORKLOAD_REUSE_DAG] = "(gid + 3u) ^ (gid * 2u)",
   [WORKLOAD_SELECT_DAG] =
      "((gid + 3u) < (gid * 2u)) ? (gid ^ 0x55u) : (gid + 100u)",
   [WORKLOAD_COMPARE_DAG] = "uint((gid + 3u) < (gid * 2u))",
   [WORKLOAD_ADD] = "gid + 0x12345678u",
   [WORKLOAD_SUB] = "gid - 0x12345678u",
   [WORKLOAD_RSUB] = "0x12345678u - gid",
   [WORKLOAD_MUL] = "gid * 0x01020305u",
   [WORKLOAD_AND] = "gid & 0x5a5aa5a5u",
   [WORKLOAD_OR] = "gid | 0x5a5aa5a5u",
   [WORKLOAD_XOR] = "gid ^ 0x5a5aa5a5u",
   [WORKLOAD_NOT] = "~gid",
   [WORKLOAD_INEG] = "uint(-int(gid))",
   [WORKLOAD_U2F] = "floatBitsToUint(float(gid))",
   [WORKLOAD_U2F_LAST_USE] =
      "floatBitsToUint(float(gid ^ 0x80000000u))",
   [WORKLOAD_I2F] = "floatBitsToUint(float(int(gid)-8192))",
   [WORKLOAD_I2F_RETAINED] =
      "floatBitsToUint(float(int(gid)-8192)) ^ uint(int(gid)-8192)",
   [WORKLOAD_F2I] =
      "uint(int(uintBitsToFloat(0x3f000000u|((gid&0x3ffu)<<12u))*37.0-20.0))",
   [WORKLOAD_F2U] =
      "uint(uintBitsToFloat(0x3f000000u|((gid&0x3ffu)<<12u))*37.0)",
   [WORKLOAD_SHL] = "gid << 9u",
   [WORKLOAD_ASHR] = "uint(int(gid^0x80000000u)>>7)",
   [WORKLOAD_USHR] = "(gid^0x80000000u)>>7u",
   [WORKLOAD_IMIN] = "uint(min(int(gid), -7))",
   [WORKLOAD_IMAX] = "uint(max(int(gid), 123))",
   [WORKLOAD_UMIN] = "min(gid, 1234u)",
   [WORKLOAD_UMAX] = "max(gid, 1234u)",
   [WORKLOAD_FADD] = "floatBitsToUint(uintBitsToFloat(gid) + 1.25)",
   [WORKLOAD_FSUB] = "floatBitsToUint(uintBitsToFloat(gid) - 1.0)",
   [WORKLOAD_RFSUB] = "floatBitsToUint(1.0 - uintBitsToFloat(gid))",
   [WORKLOAD_FMUL] = "floatBitsToUint(uintBitsToFloat(gid) * 2.0)",
   [WORKLOAD_FMIN] = "floatBitsToUint(min(uintBitsToFloat(gid), 0.0))",
   [WORKLOAD_FMAX] = "floatBitsToUint(max(uintBitsToFloat(gid), 0.0))",
   [WORKLOAD_FABS] = "floatBitsToUint(abs(uintBitsToFloat(gid)))",
   [WORKLOAD_FNEG] = "floatBitsToUint(-uintBitsToFloat(gid))",
   [WORKLOAD_FMA] = "floatBitsToUint(uintBitsToFloat(gid) * 2.0 + 1.0)",
   [WORKLOAD_FMA_NAN_MUL] = "floatBitsToUint(uintBitsToFloat(gid) * "
                            "uintBitsToFloat(0x7fc00000u) + 1.0)",
   [WORKLOAD_ARCHIVE_CROSS_0] = "gid + 0x0f1e2d3cu",
   [WORKLOAD_ARCHIVE_CROSS_1] = "gid + 0x10293847u",
   [WORKLOAD_ARCHIVE_CROSS_2] = "gid + 0x56473829u",
   [WORKLOAD_ARCHIVE_CROSS_3] = "gid + 0x89abcdefu",
   [WORKLOAD_ARCHIVE_CROSS_4] = "gid + 0xc001d00du",
   [WORKLOAD_ARCHIVE_CROSS_5] = "gid + 0x31415926u",
   [WORKLOAD_ARCHIVE_CROSS_6] = "gid + 0x27182818u",
   [WORKLOAD_ARCHIVE_CROSS_7] = "gid + 0xfeedfaceu",
};

/*
 * Larger graphs use named SSA values so fan-out and long live ranges survive
 * GLSL lowering.  The bodies still contain one ordinary SSBO store and pass
 * through the same Mesa NIR and Apple9 compiler path as the expression cases.
 */
static const char *workload_bodies[WORKLOAD_COUNT] = {
   [WORKLOAD_COMPARE_COMPLETE] = "uint ua=gid*65793u+0x80001000u;"
                                 "uint ub=(gid^0xdeadbeefu)+0x1234u;"
                                 "int ia=int(ua);int ib=int(ub);"
                                 "float fa=float(int(gid&255u)-128);"
                                 "float fb=float(int((gid*37u)&255u)-128);"
                                 "uint r=uint(ua<ub)|(uint(ua>=ub)<<1u)|"
                                 "(uint(ua==ub)<<2u)|(uint(ua!=ub)<<3u)|"
                                 "(uint(ia<ib)<<4u)|(uint(ia>=ib)<<5u)|"
                                 "(uint(ia==ib)<<6u)|(uint(ia!=ib)<<7u)|"
                                 "(uint(fa<fb)<<8u)|(uint(fa>=fb)<<9u)|"
                                 "(uint(fa==fb)<<10u)|(uint(fa!=fb)<<11u);"
                                 "output0.v[gid]=r;",
   [WORKLOAD_DEEP_INT_DAG] = "uint a=gid*3u+0x00010203u;"
                             "uint b=(a^0xa5a5a5a5u)+gid*5u;"
                             "uint c=(b|(gid+17u))^(a&0x00ff00ffu);"
                             "uint d=c*9u+(b^0xdeadbeefu);"
                             "uint e=(d-(a|0x1234u))^(c+0x76543210u);"
                             "output0.v[gid]=e*7u+(d^b);",
   [WORKLOAD_DIAMOND_INT_DAG] =
      "uint root=gid*13u+0x10203040u;"
      "uint left0=(root+0x11111111u)^0x55aa55aau;"
      "uint right0=(root^0xa5a5a5a5u)+0x01020305u;"
      "uint left1=left0*3u+(right0^root);"
      "uint right1=right0*5u^(left0+root);"
      "uint join0=(left1^right1)+(left0|right0);"
      "uint join1=(left1+root)^(right1+left0);"
      "output0.v[gid]=(join0*7u)^(join1*11u)^(left1+right0);",
   [WORKLOAD_FANOUT_INT_DAG] = "uint base=gid*257u+17u;"
                               "uint a=base+0x11111111u;"
                               "uint b=base^0xa5a5a5a5u;"
                               "uint c=base*7u;"
                               "uint d=base|0x01010101u;"
                               "uint p=(a^b)+(c^d);"
                               "uint q=(a+c)^(b+d);"
                               "uint r=(a|d)^(b&c);"
                               "output0.v[gid]=(p*3u+q*5u)^r;",
   [WORKLOAD_LOGIC_LIFETIME_DAG] = "uint a=gid+3u;"
                                   "uint b=gid*5u;"
                                   "uint x=a^b;"
                                   "uint y=a|0x55aa55aau;"
                                   "uint z=b&0xf0f00f0fu;"
                                   "uint w=(x^y)+(z^a);"
                                   "output0.v[gid]=(w|b)^(x&(y+z));",
   [WORKLOAD_PRESSURE_INT_DAG] = "uint a0=gid+1u;"
                                 "uint a1=gid*3u;"
                                 "uint a2=gid^0x13579bdfu;"
                                 "uint a3=gid|0x01010101u;"
                                 "uint a4=gid&0xfefefefeu;"
                                 "uint a5=gid+0x2468ace0u;"
                                 "uint a6=gid*11u;"
                                 "uint a7=~gid;"
                                 "uint p=(a0^a1)+(a2^a3)+(a4^a5)+(a6^a7);"
                                 "uint q=(a0+a2)^(a1+a3)^(a4+a6)^(a5+a7);"
                                 "output0.v[gid]=p+q;",
   [WORKLOAD_MINMAX_INT_DAG] = "uint a=gid*65793u+0x80001000u;"
                               "uint b=(gid^0xdeadbeefu)+0x1234u;"
                               "uint u0=min(a,b);"
                               "uint u1=max(a,b);"
                               "uint i0=uint(min(int(a),int(b)));"
                               "uint i1=uint(max(int(a),int(b)));"
                               "output0.v[gid]=(u0^i1)+(u1^i0)+(a^b);",
   [WORKLOAD_NESTED_SELECT_DAG] = "uint a=gid*3u+5u;"
                                  "uint b=(gid^0x55aa55aau)+7u;"
                                  "uint c=gid+100u;"
                                  "uint d=gid*2u+1u;"
                                  "uint s0=(a<b)?(a^c):(b+d);"
                                  "uint s1=(c<d)?(s0+a):(s0^b);"
                                  "uint s2=(s0<s1)?(s1+d):(s0+c);"
                                  "output0.v[gid]=s2^(a+b);",
   [WORKLOAD_DEEP_FLOAT_DAG] = "float x=uintBitsToFloat(gid|0x3f800000u);"
                               "float a=x*2.0+0.5;"
                               "float b=x*0.5+0.25;"
                               "float c=max(a,b);"
                               "float d=min(a+b,c*2.0);"
                               "float e=abs((d-4.0)+(a-b));"
                               "output0.v[gid]=floatBitsToUint(e*0.5+0.125);",
   [WORKLOAD_FANOUT_FLOAT_DAG] = "float x=uintBitsToFloat(gid|0x3f800000u);"
                                 "float a=x*2.0+0.25;"
                                 "float b=x*0.5+0.125;"
                                 "float c=a+b;"
                                 "float d=a-b;"
                                 "float e=a*b;"
                                 "float f=max(c,e);"
                                 "float g=min(d+2.0,e*0.5);"
                                 "output0.v[gid]=floatBitsToUint((f+g)*d);",
   [WORKLOAD_MIXED_DOMAIN_DAG] =
      "uint u=(gid*0x00010203u)^0x5a5aa5a5u;"
      "float x=uintBitsToFloat((u&0x007fffffu)|0x3f800000u);"
      "float a=x*2.0+0.5;"
      "float b=max(a,x+0.25);"
      "uint bits=floatBitsToUint(b*0.5);"
      "output0.v[gid]=(bits^u)+gid*7u;",
   [WORKLOAD_RADIX_ALTERNATING_DAG] = "uint a=gid*0x10101010u+0xf000000fu;"
                                      "uint b=(gid^0x10010001u)+0x0f0000f0u;"
                                      "uint c=a*0x01000101u+(b^0x90000009u);"
                                      "output0.v[gid]=(c+a)^(b*0x00100001u);",
   [WORKLOAD_SELECT_ALL_LIVE_DAG] = "uint a=gid*3u+5u;"
                                    "uint b=(gid^0x55aa55aau)+7u;"
                                    "uint t=a^(gid+0x101u);"
                                    "uint f=b+(gid*2u+9u);"
                                    "uint s=(a<b)?t:f;"
                                    "output0.v[gid]=s^(a*5u+b*7u+t*11u+f*13u);",
   [WORKLOAD_MINMAX_NESTED_LIVE_DAG] =
      "uint a=gid*65793u+0x80001000u;"
      "uint b=(gid^0xdeadbeefu)+0x1234u;"
      "uint c=gid*17u+0x10203040u;"
      "uint d=(gid+0x76543210u)^0xa5a5a5a5u;"
      "uint lo0=min(a,b);"
      "uint hi0=max(a,b);"
      "uint lo1=min(lo0,c);"
      "uint hi1=max(hi0,d);"
      "uint slo=uint(min(int(a),int(c)));"
      "uint shi=uint(max(int(b),int(d)));"
      "output0.v[gid]=(lo1^hi1)+(slo^shi)+(a+b+c+d+lo0+hi0);",
   [WORKLOAD_FMA_ALL_LIVE_DAG] =
      "float x=uintBitsToFloat((gid&0x3fffu)|0x3f800000u);"
      "float a=x+0.25;"
      "float b=x*0.5+0.125;"
      "float c=x*2.0-0.5;"
      "precise float r=fma(a,b,c);"
      "precise float q=fma(c,a,b);"
      "output0.v[gid]=floatBitsToUint((r+q)+(a-b)+(c*0.25));",
   [WORKLOAD_FLOAT_CACHE_RING_DAG] =
      "float x=uintBitsToFloat((gid&0x3fffu)|0x3f800000u);"
      "float a=x*2.0+0.5;"
      "float b=x*0.5+0.25;"
      "float c=x+0.125;"
      "precise float p0=fma(a,b,c);"
      "float p1=max(p0,a);"
      "precise float p2=fma(p1,b,c);"
      "float p3=min(p2,p0);"
      "precise float p4=fma(p3,a,p1);"
      "output0.v[gid]=floatBitsToUint((p4+p2)-(p3-b));",
   [WORKLOAD_CROSS_DOMAIN_CACHE_DAG] =
      "float x=uintBitsToFloat((gid&0x3fffu)|0x3f800000u);"
      "float fa=x*2.0+0.25;"
      "float fb=x*0.5+0.125;"
      "uint a=floatBitsToUint(fa);"
      "uint b=floatBitsToUint(fb);"
      "uint lo=min(a,b);"
      "uint hi=max(a,b);"
      "uint t=(lo<hi)?(a^gid):(b+gid);"
      "output0.v[gid]=(t+lo)^(hi+a+b);",
   [WORKLOAD_LOGIC_MINMAX_SELECT_DAG] =
      "uint a=gid*9u+0x10203u;"
      "uint b=(gid^0xa5a55a5au)+0x12345u;"
      "uint x=a^b;"
      "uint y=(a|0x55005500u)+(b&0x0ff00ff0u);"
      "uint lo=min(x,y);"
      "uint hi=max(x,y);"
      "uint t=x+lo;"
      "uint f=y^hi;"
      "uint s=(lo<hi)?t:f;"
      "output0.v[gid]=s^(x+y+lo+hi+a+b+t+f);",
   [WORKLOAD_CACHE_PRESSURE_DAG] =
      "uint a0=gid+0x101u;"
      "uint a1=gid*3u+0x202u;"
      "uint a2=(gid^0x13579bdfu)+0x303u;"
      "uint a3=(gid|0x01010101u)+0x404u;"
      "uint a4=(gid&0xfefefefeu)+0x505u;"
      "uint a5=gid*11u+0x606u;"
      "uint lo0=min(a0,a1);"
      "uint hi0=max(a2,a3);"
      "uint lo1=min(a4,a5);"
      "uint s=(lo0<hi0)?lo1:a5;"
      "output0.v[gid]=s+a0+(a1^lo0)+(a2^hi0)+a3+a4+a5+lo1;",
};

static void
fail(const char *message)
{
   fprintf(stderr, "T8132_GLES_COMPUTE_FAIL: %s (EGL=%#x GL=%#x)\n", message,
           eglGetError(), glGetError());
   exit(1);
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
build_program(enum workload workload)
{
   const char *expression = workload_expressions[workload];
   const char *body = workload_bodies[workload];
   char statement[3072];
   if (body) {
      snprintf(statement, sizeof(statement), "%s", body);
   } else {
      snprintf(statement, sizeof(statement), "output0.v[gid]=%s;", expression);
   }

   unsigned glsl_version = (workload == WORKLOAD_FMA_ALL_LIVE_DAG ||
                            workload == WORKLOAD_FLOAT_CACHE_RING_DAG)
                              ? 320
                              : 310;
   char source[4096];
   snprintf(source, sizeof(source),
            "#version %u es\n"
            "layout(local_size_x=256) in;\n"
            "layout(std430, binding=0) buffer Output { uint v[]; } output0;\n"
            "void main() { uint gid=gl_GlobalInvocationID.x; %s }\n",
            glsl_version, statement);

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
      fprintf(stderr, "compute compile failed: %.*s\n", size, log);
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
      fprintf(stderr, "compute link failed: %.*s\n", size, log);
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
expected(enum workload workload, uint32_t gid)
{
   float value = bits_float(gid);
   switch (workload) {
   case WORKLOAD_CONSTANT:
      return 42;
   case WORKLOAD_CONSTANT32:
      return 0xdeadbeef;
   case WORKLOAD_CONSTANT32_SPARSE:
      return 0x10000001;
   case WORKLOAD_GID:
      return gid;
   case WORKLOAD_MAD:
      return gid * 0x01020305u + 0xdeadbeefu;
   case WORKLOAD_DAG:
      return ((gid * 3u) ^ (gid + 7u)) + 11u;
   case WORKLOAD_REUSE_DAG:
      return (gid + 3u) ^ (gid * 2u);
   case WORKLOAD_SELECT_DAG:
      return (gid + 3u) < (gid * 2u) ? (gid ^ 0x55u) : (gid + 100u);
   case WORKLOAD_COMPARE_DAG:
      return (gid + 3u) < (gid * 2u);
   case WORKLOAD_COMPARE_COMPLETE: {
      uint32_t ua = gid * 65793u + 0x80001000u;
      uint32_t ub = (gid ^ 0xdeadbeefu) + 0x1234u;
      int32_t ia = (int32_t)ua;
      int32_t ib = (int32_t)ub;
      float fa = (float)((int32_t)(gid & 255u) - 128);
      float fb = (float)((int32_t)((gid * 37u) & 255u) - 128);
      return ((uint32_t)(ua < ub) << 0) | ((uint32_t)(ua >= ub) << 1) |
             ((uint32_t)(ua == ub) << 2) | ((uint32_t)(ua != ub) << 3) |
             ((uint32_t)(ia < ib) << 4) | ((uint32_t)(ia >= ib) << 5) |
             ((uint32_t)(ia == ib) << 6) | ((uint32_t)(ia != ib) << 7) |
             ((uint32_t)(fa < fb) << 8) | ((uint32_t)(fa >= fb) << 9) |
             ((uint32_t)(fa == fb) << 10) | ((uint32_t)(fa != fb) << 11);
   }
   case WORKLOAD_DEEP_INT_DAG: {
      uint32_t a = gid * 3u + 0x00010203u;
      uint32_t b = (a ^ 0xa5a5a5a5u) + gid * 5u;
      uint32_t c = (b | (gid + 17u)) ^ (a & 0x00ff00ffu);
      uint32_t d = c * 9u + (b ^ 0xdeadbeefu);
      uint32_t e = (d - (a | 0x1234u)) ^ (c + 0x76543210u);
      return e * 7u + (d ^ b);
   }
   case WORKLOAD_DIAMOND_INT_DAG: {
      uint32_t root = gid * 13u + 0x10203040u;
      uint32_t left0 = (root + 0x11111111u) ^ 0x55aa55aau;
      uint32_t right0 = (root ^ 0xa5a5a5a5u) + 0x01020305u;
      uint32_t left1 = left0 * 3u + (right0 ^ root);
      uint32_t right1 = right0 * 5u ^ (left0 + root);
      uint32_t join0 = (left1 ^ right1) + (left0 | right0);
      uint32_t join1 = (left1 + root) ^ (right1 + left0);
      return (join0 * 7u) ^ (join1 * 11u) ^ (left1 + right0);
   }
   case WORKLOAD_FANOUT_INT_DAG: {
      uint32_t base = gid * 257u + 17u;
      uint32_t a = base + 0x11111111u;
      uint32_t b = base ^ 0xa5a5a5a5u;
      uint32_t c = base * 7u;
      uint32_t d = base | 0x01010101u;
      uint32_t p = (a ^ b) + (c ^ d);
      uint32_t q = (a + c) ^ (b + d);
      uint32_t r = (a | d) ^ (b & c);
      return (p * 3u + q * 5u) ^ r;
   }
   case WORKLOAD_LOGIC_LIFETIME_DAG: {
      uint32_t a = gid + 3u;
      uint32_t b = gid * 5u;
      uint32_t x = a ^ b;
      uint32_t y = a | 0x55aa55aau;
      uint32_t z = b & 0xf0f00f0fu;
      uint32_t w = (x ^ y) + (z ^ a);
      return (w | b) ^ (x & (y + z));
   }
   case WORKLOAD_PRESSURE_INT_DAG: {
      uint32_t a0 = gid + 1u;
      uint32_t a1 = gid * 3u;
      uint32_t a2 = gid ^ 0x13579bdfu;
      uint32_t a3 = gid | 0x01010101u;
      uint32_t a4 = gid & 0xfefefefeu;
      uint32_t a5 = gid + 0x2468ace0u;
      uint32_t a6 = gid * 11u;
      uint32_t a7 = ~gid;
      uint32_t p = (a0 ^ a1) + (a2 ^ a3) + (a4 ^ a5) + (a6 ^ a7);
      uint32_t q = (a0 + a2) ^ (a1 + a3) ^ (a4 + a6) ^ (a5 + a7);
      return p + q;
   }
   case WORKLOAD_MINMAX_INT_DAG: {
      uint32_t a = gid * 65793u + 0x80001000u;
      uint32_t b = (gid ^ 0xdeadbeefu) + 0x1234u;
      uint32_t u0 = a < b ? a : b;
      uint32_t u1 = a > b ? a : b;
      uint32_t i0 = (int32_t)a < (int32_t)b ? a : b;
      uint32_t i1 = (int32_t)a > (int32_t)b ? a : b;
      return (u0 ^ i1) + (u1 ^ i0) + (a ^ b);
   }
   case WORKLOAD_NESTED_SELECT_DAG: {
      uint32_t a = gid * 3u + 5u;
      uint32_t b = (gid ^ 0x55aa55aau) + 7u;
      uint32_t c = gid + 100u;
      uint32_t d = gid * 2u + 1u;
      uint32_t s0 = a < b ? a ^ c : b + d;
      uint32_t s1 = c < d ? s0 + a : s0 ^ b;
      uint32_t s2 = s0 < s1 ? s1 + d : s0 + c;
      return s2 ^ (a + b);
   }
   case WORKLOAD_DEEP_FLOAT_DAG: {
      float x = bits_float(gid | 0x3f800000u);
      float a = x * 2.0f + 0.5f;
      float b = x * 0.5f + 0.25f;
      float c = a > b ? a : b;
      float sum = a + b;
      float twice = c * 2.0f;
      float d = sum < twice ? sum : twice;
      float e = (d - 4.0f) + (a - b);
      if (e < 0.0f)
         e = -e;
      return float_bits(e * 0.5f + 0.125f);
   }
   case WORKLOAD_FANOUT_FLOAT_DAG: {
      float x = bits_float(gid | 0x3f800000u);
      float a = x * 2.0f + 0.25f;
      float b = x * 0.5f + 0.125f;
      float c = a + b;
      float d = a - b;
      float e = a * b;
      float f = c > e ? c : e;
      float g0 = d + 2.0f;
      float g1 = e * 0.5f;
      float g = g0 < g1 ? g0 : g1;
      return float_bits((f + g) * d);
   }
   case WORKLOAD_MIXED_DOMAIN_DAG: {
      uint32_t u = (gid * 0x00010203u) ^ 0x5a5aa5a5u;
      float x = bits_float((u & 0x007fffffu) | 0x3f800000u);
      float a = x * 2.0f + 0.5f;
      float other = x + 0.25f;
      float b = a > other ? a : other;
      uint32_t bits = float_bits(b * 0.5f);
      return (bits ^ u) + gid * 7u;
   }
   case WORKLOAD_RADIX_ALTERNATING_DAG: {
      uint32_t a = gid * 0x10101010u + 0xf000000fu;
      uint32_t b = (gid ^ 0x10010001u) + 0x0f0000f0u;
      uint32_t c = a * 0x01000101u + (b ^ 0x90000009u);
      return (c + a) ^ (b * 0x00100001u);
   }
   case WORKLOAD_SELECT_ALL_LIVE_DAG: {
      uint32_t a = gid * 3u + 5u;
      uint32_t b = (gid ^ 0x55aa55aau) + 7u;
      uint32_t t = a ^ (gid + 0x101u);
      uint32_t f = b + (gid * 2u + 9u);
      uint32_t s = a < b ? t : f;
      return s ^ (a * 5u + b * 7u + t * 11u + f * 13u);
   }
   case WORKLOAD_MINMAX_NESTED_LIVE_DAG: {
      uint32_t a = gid * 65793u + 0x80001000u;
      uint32_t b = (gid ^ 0xdeadbeefu) + 0x1234u;
      uint32_t c = gid * 17u + 0x10203040u;
      uint32_t d = (gid + 0x76543210u) ^ 0xa5a5a5a5u;
      uint32_t lo0 = a < b ? a : b;
      uint32_t hi0 = a > b ? a : b;
      uint32_t lo1 = lo0 < c ? lo0 : c;
      uint32_t hi1 = hi0 > d ? hi0 : d;
      uint32_t slo = (int32_t)a < (int32_t)c ? a : c;
      uint32_t shi = (int32_t)b > (int32_t)d ? b : d;
      return (lo1 ^ hi1) + (slo ^ shi) + (a + b + c + d + lo0 + hi0);
   }
   case WORKLOAD_FMA_ALL_LIVE_DAG: {
      float x = bits_float((gid & 0x3fffu) | 0x3f800000u);
      float a = x + 0.25f;
      float b = x * 0.5f + 0.125f;
      float c = x * 2.0f - 0.5f;
      float r = fmaf(a, b, c);
      float q = fmaf(c, a, b);
      return float_bits((r + q) + (a - b) + (c * 0.25f));
   }
   case WORKLOAD_FLOAT_CACHE_RING_DAG: {
      float x = bits_float((gid & 0x3fffu) | 0x3f800000u);
      float a = x * 2.0f + 0.5f;
      float b = x * 0.5f + 0.25f;
      float c = x + 0.125f;
      float p0 = fmaf(a, b, c);
      float p1 = p0 > a ? p0 : a;
      float p2 = fmaf(p1, b, c);
      float p3 = p2 < p0 ? p2 : p0;
      float p4 = fmaf(p3, a, p1);
      return float_bits((p4 + p2) - (p3 - b));
   }
   case WORKLOAD_CROSS_DOMAIN_CACHE_DAG: {
      float x = bits_float((gid & 0x3fffu) | 0x3f800000u);
      float fa = x * 2.0f + 0.25f;
      float fb = x * 0.5f + 0.125f;
      uint32_t a = float_bits(fa);
      uint32_t b = float_bits(fb);
      uint32_t lo = a < b ? a : b;
      uint32_t hi = a > b ? a : b;
      uint32_t t = lo < hi ? a ^ gid : b + gid;
      return (t + lo) ^ (hi + a + b);
   }
   case WORKLOAD_LOGIC_MINMAX_SELECT_DAG: {
      uint32_t a = gid * 9u + 0x10203u;
      uint32_t b = (gid ^ 0xa5a55a5au) + 0x12345u;
      uint32_t x = a ^ b;
      uint32_t y = (a | 0x55005500u) + (b & 0x0ff00ff0u);
      uint32_t lo = x < y ? x : y;
      uint32_t hi = x > y ? x : y;
      uint32_t t = x + lo;
      uint32_t f = y ^ hi;
      uint32_t s = lo < hi ? t : f;
      return s ^ (x + y + lo + hi + a + b + t + f);
   }
   case WORKLOAD_CACHE_PRESSURE_DAG: {
      uint32_t a0 = gid + 0x101u;
      uint32_t a1 = gid * 3u + 0x202u;
      uint32_t a2 = (gid ^ 0x13579bdfu) + 0x303u;
      uint32_t a3 = (gid | 0x01010101u) + 0x404u;
      uint32_t a4 = (gid & 0xfefefefeu) + 0x505u;
      uint32_t a5 = gid * 11u + 0x606u;
      uint32_t lo0 = a0 < a1 ? a0 : a1;
      uint32_t hi0 = a2 > a3 ? a2 : a3;
      uint32_t lo1 = a4 < a5 ? a4 : a5;
      uint32_t s = lo0 < hi0 ? lo1 : a5;
      return s + a0 + (a1 ^ lo0) + (a2 ^ hi0) + a3 + a4 + a5 + lo1;
   }
   case WORKLOAD_ADD:
      return gid + 0x12345678u;
   case WORKLOAD_SUB:
      return gid - 0x12345678u;
   case WORKLOAD_RSUB:
      return 0x12345678u - gid;
   case WORKLOAD_MUL:
      return gid * 0x01020305u;
   case WORKLOAD_AND:
      return gid & 0x5a5aa5a5u;
   case WORKLOAD_OR:
      return gid | 0x5a5aa5a5u;
   case WORKLOAD_XOR:
      return gid ^ 0x5a5aa5a5u;
   case WORKLOAD_NOT:
      return ~gid;
   case WORKLOAD_INEG:
      return 0u - gid;
   case WORKLOAD_U2F:
      return float_bits((float)gid);
   case WORKLOAD_U2F_LAST_USE:
      return float_bits((float)(gid ^ 0x80000000u));
   case WORKLOAD_I2F:
      return float_bits((float)((int32_t)gid - 8192));
   case WORKLOAD_I2F_RETAINED: {
      int32_t value = (int32_t)gid - 8192;
      return float_bits((float)value) ^ (uint32_t)value;
   }
   case WORKLOAD_F2I: {
      float x = bits_float(0x3f000000u | ((gid & 0x3ffu) << 12u));
      return (uint32_t)(int32_t)(x * 37.0f - 20.0f);
   }
   case WORKLOAD_F2U: {
      float x = bits_float(0x3f000000u | ((gid & 0x3ffu) << 12u));
      return (uint32_t)(x * 37.0f);
   }
   case WORKLOAD_SHL:
      return gid << 9;
   case WORKLOAD_ASHR:
      return (uint32_t)((int32_t)(gid ^ 0x80000000u) >> 7);
   case WORKLOAD_USHR:
      return (gid ^ 0x80000000u) >> 7;
   case WORKLOAD_IMIN:
      return (uint32_t)(((int32_t)gid < -7) ? (int32_t)gid : -7);
   case WORKLOAD_IMAX:
      return (uint32_t)(((int32_t)gid > 123) ? (int32_t)gid : 123);
   case WORKLOAD_UMIN:
      return gid < 1234 ? gid : 1234;
   case WORKLOAD_UMAX:
      return gid > 1234 ? gid : 1234;
   case WORKLOAD_FADD:
      return float_bits(value + 1.25f);
   case WORKLOAD_FSUB:
      return float_bits(value - 1.0f);
   case WORKLOAD_RFSUB:
      return float_bits(1.0f - value);
   /* Apple9 flushes the gid-derived subnormal input to zero for fmul. */
   case WORKLOAD_FMUL:
      return 0;
   case WORKLOAD_FMIN:
      return float_bits(value < 0.0f ? value : 0.0f);
   case WORKLOAD_FMAX:
      return 0; /* gid bit patterns are positive subnormals. */
   case WORKLOAD_FABS:
      return gid & 0x7fffffffu;
   case WORKLOAD_FNEG:
      return gid ^ 0x80000000u;
   case WORKLOAD_FMA:
      return float_bits(value * 2.0f + 1.0f);
   case WORKLOAD_FMA_NAN_MUL:
      return 0x7fc00000u;
   case WORKLOAD_ARCHIVE_CROSS_0:
      return gid + 0x0f1e2d3cu;
   case WORKLOAD_ARCHIVE_CROSS_1:
      return gid + 0x10293847u;
   case WORKLOAD_ARCHIVE_CROSS_2:
      return gid + 0x56473829u;
   case WORKLOAD_ARCHIVE_CROSS_3:
      return gid + 0x89abcdefu;
   case WORKLOAD_ARCHIVE_CROSS_4:
      return gid + 0xc001d00du;
   case WORKLOAD_ARCHIVE_CROSS_5:
      return gid + 0x31415926u;
   case WORKLOAD_ARCHIVE_CROSS_6:
      return gid + 0x27182818u;
   case WORKLOAD_ARCHIVE_CROSS_7:
      return gid + 0xfeedfaceu;
   case WORKLOAD_COUNT:
      break;
   }
   abort();
}

static const char *
workload_name(enum workload workload)
{
   return workload_names[workload];
}

struct output_layout {
   size_t alignment;
   size_t guard_bytes;
   size_t slot_stride;
   size_t slot_count;
   size_t buffer_bytes;
};

static size_t
align_up_size(size_t value, size_t alignment)
{
   if (!alignment || value > SIZE_MAX - (alignment - 1))
      fail("output layout overflow");

   return ((value + alignment - 1) / alignment) * alignment;
}

static struct output_layout
make_output_layout(size_t slot_count, size_t segment_bytes)
{
   GLint queried_alignment = 0;
   glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &queried_alignment);
   if (glGetError() != GL_NO_ERROR || queried_alignment <= 0)
      fail("query SSBO alignment");

   size_t alignment = queried_alignment;
   size_t guard_bytes = align_up_size(MIN_GUARD_BYTES, alignment);
   if (segment_bytes > SIZE_MAX - 2 * guard_bytes)
      fail("output slot overflow");
   size_t slot_stride =
      align_up_size(guard_bytes + segment_bytes + guard_bytes, alignment);
   if (!slot_count || slot_count > SIZE_MAX / slot_stride)
      fail("output buffer overflow");

   return (struct output_layout){
      .alignment = alignment,
      .guard_bytes = guard_bytes,
      .slot_stride = slot_stride,
      .slot_count = slot_count,
      .buffer_bytes = slot_count * slot_stride,
   };
}

static size_t
slot_output_offset(const struct output_layout *layout, size_t slot)
{
   if (slot >= layout->slot_count)
      fail("output slot out of range");

   return slot * layout->slot_stride + layout->guard_bytes;
}

static uint32_t
guard_word(size_t slot, unsigned side, size_t word, unsigned generation)
{
   uint32_t mixed = UINT32_C(0x6d2b79f5) * (uint32_t)(slot + 1) ^
                    UINT32_C(0x9e3779b9) * (generation + 1) ^
                    UINT32_C(0x85ebca6b) * (uint32_t)(word + 1) ^
                    (side ? UINT32_C(0xc3a5c85c) : UINT32_C(0x4cf5ad43));
   return mixed ?: UINT32_C(0x13579bdf);
}

static uint32_t
poison_word(enum workload workload, uint32_t gid, size_t slot,
            unsigned generation)
{
   /* The nonzero xor mask guarantees that the seed differs from the exact
    * oracle, including workloads whose correct result is entirely zero. */
   uint32_t mask =
      (UINT32_C(0xa5a5a5a4) ^ UINT32_C(0x9e3779b9) * (uint32_t)(slot + 1) ^
       UINT32_C(0x7f4a7c15) * (generation + 1) ^ gid) |
      1u;
   return expected(workload, gid) ^ mask;
}

static void
seed_output_slot(uint8_t *seed, const struct output_layout *layout, size_t slot,
                 enum workload workload, unsigned generation)
{
   const size_t output_offset = slot_output_offset(layout, slot);
   const size_t guard_words = layout->guard_bytes / sizeof(uint32_t);
   uint32_t *before = (uint32_t *)(seed + output_offset - layout->guard_bytes);
   uint32_t *output = (uint32_t *)(seed + output_offset);
   uint32_t *after =
      (uint32_t *)(seed + output_offset + VALUE_COUNT * sizeof(uint32_t));

   for (size_t i = 0; i < guard_words; ++i) {
      before[i] = guard_word(slot, 0, i, generation);
      after[i] = guard_word(slot, 1, i, generation);
   }
   for (uint32_t i = 0; i < VALUE_COUNT; ++i)
      output[i] = poison_word(workload, i, slot, generation);
}

static bool
verify_output_slot(const uint8_t *mapped, const struct output_layout *layout,
                   size_t slot, enum workload workload, unsigned generation,
                   const char *mode, unsigned ordinal)
{
   const size_t output_offset = slot_output_offset(layout, slot);
   const size_t guard_words = layout->guard_bytes / sizeof(uint32_t);
   const uint32_t *before =
      (const uint32_t *)(mapped + output_offset - layout->guard_bytes);
   const uint32_t *output = (const uint32_t *)(mapped + output_offset);
   const uint32_t *after = (const uint32_t *)(mapped + output_offset +
                                              VALUE_COUNT * sizeof(uint32_t));

   for (size_t i = 0; i < guard_words; ++i) {
      uint32_t before_want = guard_word(slot, 0, i, generation);
      uint32_t after_want = guard_word(slot, 1, i, generation);
      if (before[i] != before_want || after[i] != after_want) {
         fprintf(stderr,
                 "%s %u workload %s slot %zu guard %zu changed: "
                 "before=%#x/%#x after=%#x/%#x\n",
                 mode, ordinal, workload_name(workload), slot, i, before[i],
                 before_want, after[i], after_want);
         return false;
      }
   }

   for (uint32_t i = 0; i < VALUE_COUNT; ++i) {
      uint32_t want = expected(workload, i);
      if (output[i] != want) {
         fprintf(stderr,
                 "%s %u workload %s slot %zu word %u=%#x expected=%#x "
                 "seed=%#x\n",
                 mode, ordinal, workload_name(workload), slot, i, output[i],
                 want, poison_word(workload, i, slot, generation));
         return false;
      }
   }
   return true;
}

static bool
run_formula_case(enum workload workload)
{
   const size_t segment_bytes = VALUE_COUNT * sizeof(uint32_t);
   struct output_layout layout = make_output_layout(1, segment_bytes);
   uint8_t *seed = malloc(layout.buffer_bytes);
   if (!seed)
      fail("allocate formula seed");
   seed_output_slot(seed, &layout, 0, workload, 0);

   GLuint buffer = 0;
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes, seed,
                GL_DYNAMIC_COPY);
   GLuint program = build_program(workload);
   glUseProgram(program);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffer,
                     slot_output_offset(&layout, 0), segment_bytes);
   glDispatchCompute(VALUE_COUNT / LOCAL_SIZE, 1, 1);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                   GL_SHADER_STORAGE_BARRIER_BIT);
   glFinish();

   const uint8_t *mapped = glMapBufferRange(
      GL_SHADER_STORAGE_BUFFER, 0, layout.buffer_bytes, GL_MAP_READ_BIT);
   if (!mapped)
      fail("map formula result");
   bool passed =
      verify_output_slot(mapped, &layout, 0, workload, 0, "formula", 1);
   glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
   glDeleteProgram(program);
   glDeleteBuffers(1, &buffer);
   free(seed);
   return passed;
}

static void
run_archive_cross_sequence(void)
{
   static const enum workload sequence[] = {
      WORKLOAD_ARCHIVE_CROSS_0, WORKLOAD_ARCHIVE_CROSS_1,
      WORKLOAD_ARCHIVE_CROSS_2, WORKLOAD_ARCHIVE_CROSS_3,
      WORKLOAD_ARCHIVE_CROSS_4, WORKLOAD_ARCHIVE_CROSS_5,
      WORKLOAD_ARCHIVE_CROSS_6, WORKLOAD_ARCHIVE_CROSS_7,
   };
   const size_t segment_bytes = VALUE_COUNT * sizeof(uint32_t);
   struct output_layout layout = make_output_layout(1, segment_bytes);
   uint8_t *seed = malloc(layout.buffer_bytes);
   if (!seed)
      fail("allocate archive sequence seed");
   GLuint buffer = 0;
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes, NULL,
                GL_DYNAMIC_COPY);

   GLuint programs[sizeof(sequence) / sizeof(sequence[0])];
   for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i)
      programs[i] = build_program(sequence[i]);

   for (unsigned round = 0; round < 2; ++round) {
      for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i) {
         enum workload workload = sequence[i];
         unsigned generation = round * 8 + i;
         seed_output_slot(seed, &layout, 0, workload, generation);
         glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
         glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, layout.buffer_bytes,
                         seed);
         glUseProgram(programs[i]);
         glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffer,
                           slot_output_offset(&layout, 0), segment_bytes);
         glDispatchCompute(VALUE_COUNT / LOCAL_SIZE, 1, 1);
         glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                         GL_SHADER_STORAGE_BARRIER_BIT);
         glFinish();
         const uint8_t *mapped = glMapBufferRange(
            GL_SHADER_STORAGE_BUFFER, 0, layout.buffer_bytes, GL_MAP_READ_BIT);
         if (!mapped)
            fail("map archive sequence result");
         if (!verify_output_slot(mapped, &layout, 0, workload, generation,
                                 "archive sequence", generation + 1))
            fail("archive sequence output mismatch");
         glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
      }
   }

   for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i)
      glDeleteProgram(programs[i]);
   glDeleteBuffers(1, &buffer);
   free(seed);
}

static void
run_repeated_range_dispatch(void)
{
   enum { SLOT_COUNT = 8 };
   const enum workload workload = WORKLOAD_CACHE_PRESSURE_DAG;
   const size_t segment_bytes = VALUE_COUNT * sizeof(uint32_t);
   struct output_layout layout = make_output_layout(SLOT_COUNT, segment_bytes);
   uint8_t *seed = malloc(layout.buffer_bytes);
   if (!seed)
      fail("allocate repeated-range seed");
   for (unsigned slot = 0; slot < SLOT_COUNT; ++slot)
      seed_output_slot(seed, &layout, slot, workload, slot);

   GLuint buffer = 0;
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, layout.buffer_bytes, seed,
                GL_DYNAMIC_COPY);
   GLuint program = build_program(workload);
   glUseProgram(program);
   for (unsigned slot = 0; slot < SLOT_COUNT; ++slot) {
      glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffer,
                        slot_output_offset(&layout, slot), segment_bytes);
      glDispatchCompute(VALUE_COUNT / LOCAL_SIZE, 1, 1);
   }
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                   GL_SHADER_STORAGE_BARRIER_BIT);
   glFinish();
   const uint8_t *mapped = glMapBufferRange(
      GL_SHADER_STORAGE_BUFFER, 0, layout.buffer_bytes, GL_MAP_READ_BIT);
   if (!mapped)
      fail("map repeated-range result");
   for (unsigned slot = 0; slot < SLOT_COUNT; ++slot)
      if (!verify_output_slot(mapped, &layout, slot, workload, slot,
                              "repeated range", slot + 1))
         fail("repeated range output mismatch");
   glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
   glDeleteProgram(program);
   glDeleteBuffers(1, &buffer);
   free(seed);
}

static GLuint
build_publication_program(unsigned slot, uint32_t value)
{
   char source[512];
   int length = snprintf(source, sizeof(source),
                         "#version 310 es\n"
                         "layout(local_size_x=32) in;\n"
                         "layout(std430,binding=0) buffer O { uint v[]; } o;\n"
                         "void main(){uint i=gl_GlobalInvocationID.x;"
                         "o.v[%uu+i]=0x%08xu^(i*0x9e3779b9u);}\n",
                         slot * 32u, value);
   if (length < 0 || (size_t)length >= sizeof(source))
      fail("publication shader source overflow");
   GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
   const char *sources[] = {source};
   glShaderSource(shader, 1, sources, NULL);
   glCompileShader(shader);
   GLint ok = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok)
      fail("compile publication shader");
   GLuint program = glCreateProgram();
   glAttachShader(program, shader);
   glLinkProgram(program);
   glDeleteShader(shader);
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (!ok)
      fail("link publication shader");
   return program;
}

static void
run_program_lifecycle_stress(void)
{
   enum { PROGRAMS = 64 };
   const size_t words_per_program = 32;
   const size_t payload_bytes = PROGRAMS * words_per_program * sizeof(uint32_t);
   struct output_layout layout = make_output_layout(1, payload_bytes);
   const size_t output_offset = slot_output_offset(&layout, 0);
   const size_t total_bytes = layout.buffer_bytes;
   uint8_t *expected = malloc(total_bytes);
   if (!expected)
      fail("allocate publication oracle");
   for (size_t i = 0; i < total_bytes; ++i)
      expected[i] = (uint8_t)((i * 0x5du + 0xa7u) | 1u);

   GLuint buffer = 0;
   glGenBuffers(1, &buffer);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, total_bytes, expected,
                GL_DYNAMIC_COPY);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffer, output_offset,
                     payload_bytes);

   GLuint first = 0;
   for (unsigned i = 0; i < PROGRAMS; ++i) {
      uint32_t value = 0x10203040u ^ (i * 0x9e3779b9u);
      GLuint program = build_publication_program(i, value);
      if (i == 0)
         first = program;
      glUseProgram(program);
      glDispatchCompute(1, 1, 1);
      for (unsigned lane = 0; lane < words_per_program; ++lane) {
         uint32_t result = value ^ (lane * 0x9e3779b9u);
         memcpy(expected + output_offset +
                   (i * words_per_program + lane) * sizeof(result),
                &result, sizeof(result));
      }
      if (i != 0)
         glDeleteProgram(program);
      if (i == 0) {
         glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                         GL_SHADER_STORAGE_BARRIER_BIT);
         glFinish();
      }
   }

   uint32_t poison[32];
   for (unsigned lane = 0; lane < words_per_program; ++lane)
      poison[lane] = 0xfeedfaceu ^ lane;
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferSubData(GL_SHADER_STORAGE_BUFFER, output_offset, sizeof(poison),
                   poison);
   glUseProgram(first);
   glDispatchCompute(1, 1, 1);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT |
                   GL_SHADER_STORAGE_BARRIER_BIT);
   glFinish();

   const uint8_t *actual = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                            total_bytes, GL_MAP_READ_BIT);
   if (!actual)
      fail("map publication stress output");
   if (memcmp(actual, expected, total_bytes))
      fail("publication lifecycle output mismatch");
   glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
   glDeleteProgram(first);
   glDeleteBuffers(1, &buffer);
   free(expected);
}

static const char *const lifecycle_case_names[] = {
   "archive-cross-program-sequence",
   "repeated-range-dispatch",
   "program-lifecycle-stress",
};

static int
run_named_case(const char *name)
{
   for (unsigned i = 0; i < WORKLOAD_ARCHIVE_CROSS_0; ++i) {
      if (!strcmp(name, workload_names[i])) {
         return run_formula_case((enum workload)i) ? 1 : -1;
      }
   }
   if (!strcmp(name, lifecycle_case_names[0])) {
      run_archive_cross_sequence();
      return 1;
   }
   if (!strcmp(name, lifecycle_case_names[1])) {
      run_repeated_range_dispatch();
      return 1;
   }
   if (!strcmp(name, lifecycle_case_names[2])) {
      run_program_lifecycle_stress();
      return 1;
   }

   size_t count = 0;
   const char *const *names = t8132_apple9_memory_case_names(&count);
   for (size_t i = 0; i < count; ++i) {
      if (!strcmp(name, names[i])) {
         t8132_apple9_run_memory_case(name);
         return 1;
      }
   }
   names = t8132_apple9_geometry_case_names(&count);
   for (size_t i = 0; i < count; ++i) {
      if (!strcmp(name, names[i])) {
         t8132_apple9_run_geometry_case(name);
         return 1;
      }
   }
   return 0;
}

static void
list_cases(void)
{
   for (unsigned i = 0; i < WORKLOAD_ARCHIVE_CROSS_0; ++i)
      puts(workload_names[i]);
   size_t count = 0;
   const char *const *names = t8132_apple9_memory_case_names(&count);
   for (size_t i = 0; i < count; ++i)
      puts(names[i]);
   names = t8132_apple9_geometry_case_names(&count);
   for (size_t i = 0; i < count; ++i)
      puts(names[i]);
   for (unsigned i = 0;
        i < sizeof(lifecycle_case_names) / sizeof(lifecycle_case_names[0]); ++i)
      puts(lifecycle_case_names[i]);
}

int
main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "--list")) {
      list_cases();
      return 0;
   }
   if (argc < 2) {
      fprintf(stderr, "usage: %s --list | CASE...\n", argv[0]);
      return 2;
   }

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

   bool failed = false;
   for (int i = 1; i < argc; ++i) {
      printf("PIGLIT TEST: %d - %s\n", i, argv[i]);
      fflush(stdout);
      int result = run_named_case(argv[i]);
      if (result == 0) {
         fprintf(stderr, "unknown Apple9 compute case: %s\n", argv[i]);
         return 2;
      }
      const char *status = result > 0 ? "pass" : "fail";
      failed |= result < 0;
      if (argc == 2)
         printf("PIGLIT: {\"result\": \"%s\"}\n", status);
      else
         printf("PIGLIT: {\"subtest\": {\"%s\": \"%s\"}}\n", argv[i], status);
      fflush(stdout);
   }

   printf("T8132_APPLE9_COMPUTE_RUNNER_%s cases=%d renderer=\"%s\" "
          "version=\"%s\"\n",
          failed ? "FAILED" : "OK", argc - 1, renderer, version);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return failed ? 1 : 0;
}
