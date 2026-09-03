/*
 * Copyright 2026 Asahi Linux Contributors
 * SPDX-License-Identifier: MIT
 */

#include "agx_apple9.h"

#include <limits.h>
#include <string.h>

static void
put_u32(uint8_t *out, uint32_t value)
{
   memcpy(out, &value, sizeof(value));
}

static void
put_u64(uint8_t *out, uint64_t value)
{
   memcpy(out, &value, sizeof(value));
}

bool
agx_apple9_build_compute_geometry_fields(
   void *record_, size_t record_size, uint64_t record_address,
   const struct agx_apple9_compute_geometry *geometry)
{
   if (!record_ || !geometry || record_size < 0x78 ||
       record_address > UINT64_MAX - 0x6c)
      return false;

   uint8_t *record = record_;
   switch (geometry->mode) {
   case AGX_APPLE9_COMPUTE_GEOMETRY_DIRECT: {
      uint64_t local_threads = 1;
      for (unsigned d = 0; d < 3; ++d) {
         if (!geometry->threads[d] || !geometry->local[d] ||
             geometry->local[d] > 1024 / local_threads)
            return false;
         local_threads *= geometry->local[d];
      }
      put_u64(record + 0x00, record_address + 0x60);
      put_u64(record + 0x08, record_address + 0x6c);
      for (unsigned d = 0; d < 3; ++d) {
         put_u32(record + 0x60 + d * sizeof(uint32_t), geometry->threads[d]);
         put_u32(record + 0x6c + d * sizeof(uint32_t), 1);
      }
      return true;
   }

   case AGX_APPLE9_COMPUTE_GEOMETRY_INDIRECT: {
      if (!geometry->group_counts || (geometry->group_counts & 3))
         return false;
      uint64_t local_threads = 1;
      for (unsigned d = 0; d < 3; ++d) {
         if (!geometry->local[d] || geometry->local[d] > 1024 / local_threads)
            return false;
         local_threads *= geometry->local[d];
      }
      put_u64(record + 0x00, geometry->group_counts);
      put_u64(record + 0x08, record_address + 0x6c);
      for (unsigned d = 0; d < 3; ++d) {
         put_u32(record + 0x60 + d * sizeof(uint32_t), 0);
         put_u32(record + 0x6c + d * sizeof(uint32_t), geometry->local[d]);
      }
      return true;
   }

   default:
      return false;
   }
}
