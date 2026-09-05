/* SPDX-License-Identifier: MIT */
/* GLES-only fixture. The driver never reads the scene mode or geometry. */
#include <math.h>

static int
run_mesh(unsigned width, unsigned height)
{
   const char *mode = getenv("T8132_GLES_MESH");
   bool cube = !strcmp(mode, "cube");
   bool depth = cube || !strcmp(mode, "depth");
   bool depth_states = getenv("T8132_GLES_DEPTH_STATES") != NULL;
   bool four = getenv("T8132_GLES_FOUR_BUFFERS") != NULL;
   unsigned position_attribute = four ? 3 : 0, color_attribute = four ? 9 : 1;
   bool u32 = getenv("T8132_GLES_INDEX32") != NULL;
   if (strcmp(mode, "quad") && strcmp(mode, "depth") && !cube)
      fail("unknown mesh mode");
   GLuint program = link_program(0, true, VERTEX_VARIANT_NORMAL, NULL);
   glUseProgram(program);

   const float quad[4][3] = {
      {-.75f, -.5f, 0},
      {.75f, -.5f, 0},
      {.75f, .5f, 0},
      {-.75f, .5f, 0},
   };
   const float corners[8][3] = {
      {-.6f, -.6f, -.6f}, {.6f, -.6f, -.6f}, {.6f, .6f, -.6f},
      {-.6f, .6f, -.6f},  {-.6f, -.6f, .6f}, {.6f, -.6f, .6f},
      {.6f, .6f, .6f},    {-.6f, .6f, .6f},
   };
   const uint16_t quad_indices[6] = {2, 0, 1, 0, 2, 3};
   const uint16_t cube_indices[36] = {
      0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
      3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
   };
   unsigned vertices = cube ? 8 : depth ? 6 : 4;
   unsigned count = cube ? 36 : 6;
   /* Deliberately different buffers, strides and nonzero offsets. */
   float positions[8 * 5 + 2], colors[8 * 6 + 2];
   for (unsigned i = 0; i < sizeof(positions) / sizeof(float); ++i)
      positions[i] = 99;
   for (unsigned i = 0; i < sizeof(colors) / sizeof(float); ++i)
      colors[i] = 99;
   for (unsigned i = 0; i < vertices; ++i) {
      float *p = &positions[2 + i * 5];
      float *c = &colors[2 + i * 6];
      if (depth && !cube) {
         unsigned k = i % 3;
         p[0] = k == 0 ? -.75f : k == 1 ? .75f : 0;
         p[1] = k == 2 ? .75f : -.75f;
         p[2] = i < 3 ? -.5f : .5f;
         c[0] = i < 3;
         c[1] = i >= 3;
         c[2] = .25f;
         c[3] = 1;
      } else {
         memcpy(p, cube ? corners[i] : quad[i], 3 * sizeof(float));
         c[0] = p[0] * .6f + .5f;
         c[1] = p[1] * .6f + .5f;
         c[2] = cube ? p[2] * .6f + .5f : .375f;
         c[3] = 1;
      }
   }
   GLuint vb[3], ib;
   glGenBuffers(3, vb);
   glBindBuffer(GL_ARRAY_BUFFER, vb[0]);
   glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
   glEnableVertexAttribArray(position_attribute);
   glVertexAttribPointer(position_attribute, 3, GL_FLOAT, GL_FALSE,
                         5 * sizeof(float), (void *)(2 * sizeof(float)));
   glBindBuffer(GL_ARRAY_BUFFER, vb[1]);
   glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
   glEnableVertexAttribArray(color_attribute);
   glVertexAttribPointer(color_attribute, 4, GL_FLOAT, GL_FALSE,
                         6 * sizeof(float), (void *)(2 * sizeof(float)));
   if (four) {
      float weights[8];
      for (unsigned i = 0; i < 8; ++i)
         weights[i] = 1 - i * .0625f;
      glBindBuffer(GL_ARRAY_BUFFER, vb[2]);
      glBufferData(GL_ARRAY_BUFFER, sizeof(weights), weights, GL_STATIC_DRAW);
      glEnableVertexAttribArray(13);
      glVertexAttribPointer(13, 1, GL_FLOAT, GL_FALSE, sizeof(float),
                            (void *)0);
   }
   glGenBuffers(1, &ib);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
   /* Leading/trailing sentinels exercise a nonzero API index offset. */
   uint16_t indices[2 + 72 + 2];
   indices[0] = indices[1] = 65534;
   for (unsigned i = 0; i < count; ++i)
      indices[2 + i] = cube ? cube_indices[i] : depth ? i : quad_indices[i];
   for (unsigned t = 0; t < count / 3; ++t)
      for (unsigned c = 0; c < 3; ++c)
         indices[2 + count + t * 3 + c] =
            indices[2 + (count / 3 - 1 - t) * 3 + c];
   indices[2 + count * 2] = indices[3 + count * 2] = 65534;
   if (u32) {
      uint32_t wide[2 + 72 + 2];
      for (unsigned i = 0; i < 4 + count * 2; ++i)
         wide[i] = indices[i];
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, (4 + count * 2) * sizeof(uint32_t),
                   wide, GL_STATIC_DRAW);
   } else {
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, (4 + count * 2) * sizeof(uint16_t),
                   indices, GL_STATIC_DRAW);
   }

   GLuint fb, texture, z = 0;
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          texture, 0);
   if (depth) {
      glGenTextures(1, &z);
      glBindTexture(GL_TEXTURE_2D, z);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, width, height);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             z, 0);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      glDepthMask(GL_TRUE);
      glClearDepthf(1);
   }
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      fail("mesh framebuffer incomplete");
   glViewport(0, 0, width, height);
   glDisable(GL_CULL_FACE);
   glDisable(GL_BLEND);
   GLint transform = glGetUniformLocation(program, "u_transform");
   GLint tint = glGetUniformLocation(program, "u_tint");
   if (transform < 0 || tint < 0)
      fail("mesh uniforms optimized out");
   unsigned frames = read_dimension("T8132_GLES_FRAMES", depth_states ? 10
                                                         : cube       ? 28
                                                                      : 2);
   for (unsigned frame = 0; frame < frames; ++frame) {
      float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      if (cube) {
         float a = .35f + (frame / 2) * .45f, b = .45f;
         float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
         /* R_y(a) R_x(b), column-major; GL depth remains [-1,1]. */
         float m[16] = {ca,      0,   -sa,     0, sa * sb, cb, ca * sb, 0,
                        sa * cb, -sb, ca * cb, 0, 0,       0,  0,       1};
         memcpy(matrix, m, sizeof(m));
      }
      glUniformMatrix4fv(transform, 1, GL_FALSE, matrix);
      glUniform4f(tint, 1, .875f, .75f, 1);
      if (depth_states) {
         glDepthMask(GL_TRUE);
         glClearDepthf(frame / 2 == 1 ? 0 : 1);
      }
      glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
      if (depth_states) {
         if (frame / 2 == 3)
            glDisable(GL_DEPTH_TEST);
         else
            glEnable(GL_DEPTH_TEST);
         glDepthFunc(frame / 2 == 1 ? GL_GREATER : GL_LESS);
         glDepthMask(frame / 2 == 2 ? GL_FALSE : GL_TRUE);
      }
      uintptr_t offset = (2 + (frame % 2) * count) * (u32 ? 4 : 2);
      if (depth_states && frame / 2 == 4) {
         glDepthMask(GL_FALSE);
         for (unsigned draw = 0; draw < 2; ++draw) {
            bool near_triangle = (draw == 0) != (frame % 2 != 0);
            glDepthFunc(near_triangle ? GL_ALWAYS : GL_NEVER);
            glDrawElements(GL_TRIANGLES, 3,
                           u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                           (void *)(offset + draw * 3 * (u32 ? 4 : 2)));
         }
      } else {
         glDrawElements(GL_TRIANGLES, count,
                        u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                        (void *)offset);
      }
      glFinish();
      if (glGetError() != GL_NO_ERROR)
         fail("mesh draw");
   }
   printf(
      "T8132_GLES_MESH_OK mode=%s vertices=%u triangles=%u indices=%u frames=%u depth=%u\n",
      mode, vertices, count / 3, u32 ? 32 : 16, frames, depth);
   glDeleteBuffers(3, vb);
   glDeleteBuffers(1, &ib);
   glDeleteTextures(1, &texture);
   if (z)
      glDeleteTextures(1, &z);
   glDeleteFramebuffers(1, &fb);
   glDeleteProgram(program);
   return 0;
}
