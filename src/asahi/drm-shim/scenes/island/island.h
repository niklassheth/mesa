/* SPDX-License-Identifier: MIT */
/* Ordinary GLES scene. No model data or special cases enter the driver. */
static int
run_island(unsigned width, unsigned height)
{
   const char *path = getenv("T8132_GLES_ISLAND");
   FILE *input = fopen(path, "rb");
   uint32_t header[4];
   if (!input || fread(header, sizeof(header), 1, input) != 1 ||
       memcmp(header, "A9M1", 4) || header[3] || !header[1] ||
       header[1] > 1000000 || !header[2] || header[2] > 3000000 || header[2] % 3)
      fail("invalid island mesh header");
   unsigned vertices = header[1], count = header[2];
   size_t vertex_bytes = (size_t)vertices * 9 * sizeof(float);
   float *data = malloc(vertex_bytes);
   uint32_t *indices = malloc((size_t)count * 2 * sizeof(uint32_t));
   if (!data || !indices || fread(data, 1, vertex_bytes, input) != vertex_bytes ||
       fread(indices, sizeof(uint32_t), count, input) != count ||
       fgetc(input) != EOF)
      fail("invalid island mesh payload");
   fclose(input);
   for (unsigned i = 0; i < count; ++i)
      if (indices[i] >= vertices)
         fail("island index out of range");
   for (unsigned t = 0; t < count / 3; ++t)
      memcpy(indices + count + 3 * t, indices + count - 3 * (t + 1), 12);

   GLuint program = link_program(0, true, VERTEX_VARIANT_NORMAL, NULL);
   glUseProgram(program);
   GLuint vb, ib;
   glGenBuffers(1, &vb);
   glBindBuffer(GL_ARRAY_BUFFER, vb);
   glBufferData(GL_ARRAY_BUFFER, vertex_bytes, data, GL_STATIC_DRAW);
   for (unsigned i = 0; i < 3; ++i) {
      glEnableVertexAttribArray(i);
      glVertexAttribPointer(i, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
                            (void *)(uintptr_t)(i * 3 * sizeof(float)));
   }
   glGenBuffers(1, &ib);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)count * 2 * sizeof(uint32_t),
                indices, GL_STATIC_DRAW);
   free(data);
   free(indices);

   GLuint fb, textures[2];
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glGenTextures(2, textures);
   for (unsigned i = 0; i < 2; ++i) {
      glBindTexture(GL_TEXTURE_2D, textures[i]);
      glTexStorage2D(GL_TEXTURE_2D, 1, i ? GL_DEPTH_COMPONENT32F : GL_RGBA8,
                     width, height);
      glFramebufferTexture2D(GL_FRAMEBUFFER,
                            i ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, textures[i], 0);
   }
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      fail("island framebuffer incomplete");
   glViewport(0, 0, width, height);
   glDisable(GL_CULL_FACE);
   glDisable(GL_BLEND);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
   glDepthMask(GL_TRUE);
   glClearDepthf(1);
   GLint transform = glGetUniformLocation(program, "u_transform");
   GLint light = glGetUniformLocation(program, "u_light");
   if (transform < 0 || light < 0)
      fail("island uniforms optimized out");
   /* Fixed world-space sun; camera motion does not rotate the light. */
   float sun[3] = {-.4f, .8f, .45f};
   float length = sqrtf(sun[0]*sun[0] + sun[1]*sun[1] + sun[2]*sun[2]);
   glUniform4f(light, sun[0]/length, sun[1]/length, sun[2]/length, .28f);
   unsigned views = read_dimension("T8132_GLES_VIEWS", 48);
   unsigned frames = read_dimension("T8132_GLES_FRAMES", views * 2);
   const char *metadata = getenv("T8132_GLES_FRAME_DATA");
   FILE *matrices = metadata ? fopen(metadata, "wb") : NULL;
   if (metadata && !matrices)
      fail("open island frame metadata");
   for (unsigned frame = 0; frame < frames; ++frame) {
      float azimuth = .65f + (frame / 2) * (6.283185307179586f / views);
      float elevation = .65f;
      float ca = cosf(azimuth), sa = sinf(azimuth);
      float ce = cosf(elevation), se = sinf(elevation);
      /* Orthographic look-at, orbiting Y-up origin. The four-times-wider
       * depth interval accommodates the broad display base when zoomed in. */
      float matrix[16] = {ca, -se*sa, -ce*sa*.25f, 0,
                         0, ce, -se*.25f, 0,
                         -sa, -se*ca, -ce*ca*.25f, 0,
                         0, 0, 0, 1};
      glUniformMatrix4fv(transform, 1, GL_FALSE, matrix);
      if (matrices) {
         float record[20];
         memcpy(record, matrix, sizeof(matrix));
         record[16] = sun[0]/length;
         record[17] = sun[1]/length;
         record[18] = sun[2]/length;
         record[19] = .28f;
         if (fwrite(record, sizeof(record), 1, matrices) != 1)
            fail("write island frame metadata");
      }
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT,
                     (void *)(uintptr_t)((frame % 2) * count * sizeof(uint32_t)));
      if (getenv("T8132_GLES_BUILD_ONLY")) {
         printf("T8132_GLES_ISLAND_BUILD_OK vertices=%u triangles=%u\n", vertices, count/3);
         fflush(NULL);
         _Exit(0);
      }
      glFinish();
      if (glGetError() != GL_NO_ERROR)
         fail("island draw");
      printf("T8132_GLES_ISLAND_FRAME frame=%u view=%u order=%s\n", frame,
             frame / 2, frame % 2 ? "reverse" : "forward");
      fflush(stdout);
   }
   if (matrices)
      fclose(matrices);
   printf("T8132_GLES_ISLAND_OK vertices=%u triangles=%u frames=%u\n",
          vertices, count/3, frames);
   glDeleteBuffers(1, &vb);
   glDeleteBuffers(1, &ib);
   glDeleteTextures(2, textures);
   glDeleteFramebuffers(1, &fb);
   glDeleteProgram(program);
   return 0;
}
