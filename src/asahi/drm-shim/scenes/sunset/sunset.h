/* SPDX-License-Identifier: MIT */
/* Ordinary GLES scene. No model data or special cases enter the driver. */
static int
run_sunset(unsigned width, unsigned height)
{
   const char *path = getenv("T8132_GLES_ISLAND");
   FILE *input = fopen(path, "rb");
   uint32_t header[4];
   if (!input || fread(header, sizeof(header), 1, input) != 1 ||
       memcmp(header, "A9M1", 4) || header[3] || !header[1] ||
       header[1] > 1000000 || !header[2] || header[2] > 3000000 || header[2] % 3)
      fail("invalid island mesh header");
   unsigned vertices = header[1], count = header[2];
   unsigned original_vertices = vertices, original_count = count;
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
   /* Pack specular weight / sky marker into color.w: still three attribute arguments. */
   float *expanded = calloc((vertices + 3) * 10, sizeof(float));
   if (!expanded) fail("sunset allocation");
   for (unsigned i = 0; i < vertices; ++i) {
      memcpy(expanded + i*10, data + i*9, 9*sizeof(float));
      expanded[i*10+9] = data[i*9+8] > data[i*9+6]*1.8f ? .8f : .08f;
      /* Extend the model's broad cyan display base into a water backdrop.
       * Small colored details keep their original geometry. */
      if (expanded[i*10+9] > .5f &&
          (fabsf(data[i*9]) > 2 || fabsf(data[i*9+2]) > 2)) {
         expanded[i*10] *= 12;
         expanded[i*10+2] *= 12;
      }
   }
   free(data);
   data = expanded;
   vertices += 3; count += 3;
   indices = realloc(indices, (size_t)count * 2 * sizeof(uint32_t));
   if (!indices) fail("sunset index allocation");
   for (unsigned i = 0; i < 3; ++i) indices[original_count+i] = original_vertices+i;
   vertex_bytes = (size_t)vertices * 10 * sizeof(float);
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
      glVertexAttribPointer(i, i == 2 ? 4 : 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                            (void *)(uintptr_t)(i * 3 * sizeof(float)));
   }
   glGenBuffers(1, &ib);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)count * 2 * sizeof(uint32_t),
                indices, GL_STATIC_DRAW);
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
   GLint eye = glGetUniformLocation(program, "u_eye");
   if (eye < 0) fail("sunset uniforms missing");
   unsigned views = read_dimension("T8132_GLES_VIEWS", 48);
   unsigned frames = read_dimension("T8132_GLES_FRAMES", views * 2);
   const char *metadata = getenv("T8132_GLES_FRAME_DATA");
   FILE *matrices = metadata ? fopen(metadata, "wb") : NULL;
   if (metadata && !matrices)
      fail("open island frame metadata");
   for (unsigned frame = 0; frame < frames; ++frame) {
      float azimuth = .65f + (frame / 2) * (6.283185307179586f / views);
      float elevation = .19f;
      float ca = cosf(azimuth), sa = sinf(azimuth);
      float ce = cosf(elevation), se = sinf(elevation);
      /* Perspective look-at: 45-degree vertical field of view, with a
       * finite 30-unit far plane beyond the widened water backdrop. */
      const float camera_distance=2.6f, near=.1f, far=30.f;
      const float focal=2.41421356237f;
      float a_depth=-(far+near)/(far-near), b_depth=-2*far*near/(far-near);
      float matrix[16] = {focal*ca, -focal*se*sa, a_depth*ce*sa, -ce*sa,
                         0, focal*ce, a_depth*se, -se,
                         -focal*sa, -focal*se*ca, a_depth*ce*ca, -ce*ca,
                         0, 0, a_depth*(-camera_distance)+b_depth, camera_distance};
      glUniformMatrix4fv(transform, 1, GL_FALSE, matrix);
      /* Keep the directional light aligned with the visible sun disk.
       * The orbit is equivalent to turning the island beneath this sky. */
      float sun_x=-.52f/focal, sun_y=.65f/focal;
      float sun[3] = {ca*sun_x-se*sa*sun_y-ce*sa,
                      ce*sun_y-se,
                      -sa*sun_x-se*ca*sun_y-ce*ca};
      float length=sqrtf(sun[0]*sun[0]+sun[1]*sun[1]+sun[2]*sun[2]);
      glUniform4f(light,sun[0]/length,sun[1]/length,sun[2]/length,.28f);
      glUniform3f(eye, ce*sa*camera_distance, se*camera_distance, ce*ca*camera_distance);
      /* One camera-facing sky triangle shares the island pipeline. */
      const float sky[3][2] = {{-1,-1},{3,-1},{-1,3}};
      for (unsigned k=0; k<3; ++k) {
         float *v = data + (original_vertices+k)*10;
         /* Inverse perspective camera: sky plane just inside the far
          * clip plane. Pixel shader receives normalized screen coordinates. */
         float sx=sky[k][0]*29.f/focal, sy=sky[k][1]*29.f/focal;
         float backward=camera_distance-29.f;
         v[0]=ca*sx-se*sa*sy+ce*sa*backward;
         v[1]=ce*sy+se*backward;
         v[2]=-sa*sx-se*ca*sy+ce*ca*backward;
         v[3]=sky[k][0]; v[4]=sky[k][1]; v[5]=1;
         v[6]=v[7]=v[8]=0; v[9]=-1;
      }
      glBindBuffer(GL_ARRAY_BUFFER, vb);
      glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_bytes, data);
      if (matrices) {
         float record[23];
         memcpy(record, matrix, sizeof(matrix));
         record[16] = sun[0]/length;
         record[17] = sun[1]/length;
         record[18] = sun[2]/length;
         record[19] = .28f;
         record[20] = ce*sa*camera_distance; record[21] = se*camera_distance; record[22] = ce*ca*camera_distance;
         if (fwrite(record, sizeof(record), 1, matrices) != 1)
            fail("write island frame metadata");
         if (fwrite(data, 1, vertex_bytes, matrices) != vertex_bytes)
            fail("write sunset vertex metadata");
      }
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT,
                     (void *)(uintptr_t)((frame % 2) * count * sizeof(uint32_t)));
      if (getenv("T8132_GLES_BUILD_ONLY")) {
         printf("T8132_GLES_SUNSET_BUILD_OK vertices=%u triangles=%u\n", vertices, count/3);
         fflush(NULL);
         _Exit(0);
      }
      glFinish();
      if (glGetError() != GL_NO_ERROR)
         fail("island draw");
      printf("T8132_GLES_SUNSET_FRAME frame=%u view=%u order=%s\n", frame,
             frame / 2, frame % 2 ? "reverse" : "forward");
      fflush(stdout);
   }
   if (matrices)
      fclose(matrices);
   printf("T8132_GLES_SUNSET_OK vertices=%u triangles=%u frames=%u\n",
          vertices, count/3, frames);
   free(data);
   glDeleteBuffers(1, &vb);
   glDeleteBuffers(1, &ib);
   glDeleteTextures(2, textures);
   glDeleteFramebuffers(1, &fb);
   glDeleteProgram(program);
   return 0;
}
