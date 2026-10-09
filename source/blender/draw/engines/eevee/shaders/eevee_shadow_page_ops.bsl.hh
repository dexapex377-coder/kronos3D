/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_shadow_shared.hh"

#define max_page uint(SHADOW_MAX_PAGE)
/* SHADOW_MAX_PAGE is a power of two, so modulo is a bitwise AND. Some mobile drivers (e.g.
 * PowerVR BXM) refuse to compile the OpUMod instruction with a constant divisor in this shader
 * (fails at vkCreateComputePipelines with VK_ERROR_UNKNOWN). */
#define page_mask uint(SHADOW_MAX_PAGE - 1)

namespace eevee::shadow {

struct TileMaps {
  [[storage(8, read_write)]] ShadowTileMapData (&tilemaps_buf)[];
};

struct Tiles {
  [[storage(9, read_write)]] uint (&tiles_buf)[];
};

struct Statistics {
  [[storage(7, read_write)]] ShadowStatistics &statistics_buf;
};

/**
 * Operations to move virtual shadow map pages between heaps and tiles.
 * We reuse the blender::vector class denomination.
 *
 * A page is can be in 3 state (free, cached, acquired). Each one correspond to a different owner.
 */
struct PageAllocator {
  [[storage(2, read_write)]] ShadowPagesInfoData &pages_infos_buf;
  /* The tiles_buf only owns a page if it is used. If the page is cached, the tile contains a
   * reference index inside the pages_cached_buf.*/
  [[storage(1, read_write)]] uint (&tiles_buf)[];
  /* Free page stack containing only the page coordinates. */
  [[storage(3, read_write)]] uint (&pages_free_buf)[];
  /* The pages_cached_buf is a ring buffer where newly cached pages gets added at the end and the
   *  old cached pages gets defragmented at the start of the used portion. */
  [[storage(4, read_write)]] uint2 (&pages_cached_buf)[];

  /* Remove page ownership from the tile and append it to the cache. */
  void page_free(ShadowTileData &tile)
  {
    assert(tile.is_allocated);

    int index = atomicAdd(pages_infos_buf.page_free_count, 1);
    assert(index < SHADOW_MAX_PAGE);
    /* Insert in heap. */
    pages_free_buf[index] = shadow_page_pack(tile.page);
    /* Remove from tile. */
    tile.page = uint3(~0u);
    tile.is_cached = false;
    tile.is_allocated = false;
  }

  /* Remove last page from the free heap and give ownership to the tile. */
  void page_alloc(ShadowTileData &tile)
  {
    assert(!tile.is_allocated);

    int index = atomicAdd(pages_infos_buf.page_free_count, -1) - 1;
    /* This can easily happen in really big scene. */
    if (index < 0) {
      return;
    }
    /* Insert in tile. */
    tile.page = shadow_page_unpack(pages_free_buf[index]);
    tile.is_allocated = true;
    tile.do_update = true;
    /* Remove from heap. */
    pages_free_buf[index] = uint(-1);
  }

  /* Remove page ownership from the tile cache and append it to the cache. */
  void page_cache_append(ShadowTileData &tile, uint tile_index)
  {
    assert(tile.is_allocated);

    /* The page_cached_next is also wrapped in the defragment phase to avoid unsigned overflow. */
    uint index = atomicAdd(pages_infos_buf.page_cached_next, 1u) & page_mask;
    /* Insert in heap. */
    pages_cached_buf[index] = uint2(shadow_page_pack(tile.page), tile_index);
    /* Remove from tile. */
    tile.page = uint3(~0u);
    tile.cache_index = index;
    tile.is_cached = true;
    tile.is_allocated = false;
  }

  /* Remove page from cache and give ownership to the tile. */
  void page_cache_remove(ShadowTileData &tile)
  {
    assert(!tile.is_allocated);
    assert(tile.is_cached);

    uint index = tile.cache_index;
    /* Insert in tile. */
    tile.page = shadow_page_unpack(pages_cached_buf[index].x);
    tile.cache_index = uint(-1);
    tile.is_cached = false;
    tile.is_allocated = true;
    /* Remove from heap. Leaves hole in the buffer. This is handled by the defragment phase. */
    pages_cached_buf[index] = uint2(~0u);
  }

  /* Update cached page reference when a cached page moves inside the cached page buffer. */
  void page_cache_update_page_ref(uint page_index, uint new_page_index)
  {
    uint tile_index = pages_cached_buf[page_index].y;
    ShadowTileData tile = shadow_tile_unpack(tiles_buf[tile_index]);
    tile.cache_index = new_page_index;
    tiles_buf[tile_index] = shadow_tile_pack(tile);
  }

 private:
  /* Update cached page reference when a tile referencing a cached page moves inside the tile-map.
   */
  void page_cache_update_tile_ref(uint page_index, uint new_tile_index)
  {
    pages_cached_buf[page_index].y = new_tile_index;
  }

  void find_first_valid(uint &src, uint dst)
  {
    for (uint i = src; i < dst; i++) {
      if (pages_cached_buf[i & page_mask].x != uint(-1)) {
        src = i;
        return;
      }
    }

    src = dst;
  }

  void free_cached_page(uint page_index)
  {
    uint tile_index = pages_cached_buf[page_index].y;
    ShadowTileData tile = shadow_tile_unpack(tiles_buf[tile_index]);

    page_cache_remove(tile);
    page_free(tile);

    tiles_buf[tile_index] = shadow_tile_pack(tile);
  }

  /* Can be used to debug heap and invalid pages inside the free buffer. */
  bool check_heap_integrity(int start, int size, const uint invalid_val)
  {
    auto &heap = pages_free_buf;
    bool result = true;
    for (int i = 0; i < int(max_page); i++) {
      if ((i >= start) && (i < (start + size))) {
        result = result && (heap[i] != invalid_val);
      }
      else {
        result = result && (heap[i] == invalid_val);
      }
    }
    return result;
  }

 public:
  void defrag()
  {
    /* Pages we need to get off the cache for the allocation pass. */
    int additional_pages = pages_infos_buf.page_alloc_count - pages_infos_buf.page_free_count;

    uint src = pages_infos_buf.page_cached_start;
    uint end = pages_infos_buf.page_cached_end;

    find_first_valid(src, end);

#if 0 /* Debug */
    if (!check_heap_integrity(0, pages_infos_buf.page_free_count, uint(-1))) {
      printf("Corrupted heap before defrag.");
    }
#endif

    /* First free as much pages as needed from the end of the cached range to fulfill the
     * allocation. Avoid defragmenting to then free them. */
    for (; additional_pages > 0 && src < end; additional_pages--) {
      free_cached_page(src & page_mask);
      find_first_valid(src, end);
    }

    /* Defragment page in "old" range.
     * Forward/ascending traversal: scan holes from low to high and fill each with the
     * first cached page. The previous implementation used a descending outer loop
     * (`dst--`) with a nested advancing counter, a pattern the PowerVR BXM driver
     * rejects at pipeline creation (VK_ERROR_UNKNOWN). The forward variant is
     * equivalent: same set of cached pages, same relative order, packed contiguous
     * (probe p2c confirmed the driver compiles it). */
    bool is_empty = (src == end);
    if (!is_empty) {
      for (uint dst = src + 1; dst < end; dst++) {
        /* Find hole. */
        if (pages_cached_buf[dst & page_mask].x != uint(-1)) {
          continue;
        }
        uint old_page_idx = src & page_mask;
        uint dst_page_idx = dst & page_mask;
        /* Update corresponding reference in tile. */
        page_cache_update_page_ref(old_page_idx, dst_page_idx);
        /* Move page, one component at a time (uint2 copies rejected by driver). */
        uint page_coord = pages_cached_buf[old_page_idx].x;
        uint tile_index = pages_cached_buf[old_page_idx].y;
        pages_cached_buf[dst_page_idx].x = page_coord;
        pages_cached_buf[dst_page_idx].y = tile_index;
        pages_cached_buf[old_page_idx].x = uint(-1);
        pages_cached_buf[old_page_idx].y = uint(-1);

        /* Advance src to the next valid page. */
        while (src < dst) {
          src++;
          if (pages_cached_buf[src & page_mask].x != uint(-1)) {
            break;
          }
        }
      }
    }

    end = pages_infos_buf.page_cached_next;
    /* Free pages in the "new" range (these are compact). */
    for (; additional_pages > 0 && src < end; additional_pages--, src++) {
      free_cached_page(src & page_mask);
    }

#if 0 /* Debug */
    if (!check_heap_integrity(0, pages_infos_buf.page_free_count, uint(-1))) {
      printf("Corrupted heap after defrag.");
    }
#endif

    pages_infos_buf.page_cached_start = src;
    pages_infos_buf.page_cached_end = end;
    pages_infos_buf.page_alloc_count = 0;

    /* Wrap the cursor to avoid unsigned overflow. We do not do modulo arithmetic because it would
     * produce a 0 length buffer if the buffer is full. */
    if (pages_infos_buf.page_cached_start > max_page) {
      pages_infos_buf.page_cached_next -= max_page;
      pages_infos_buf.page_cached_start -= max_page;
      pages_infos_buf.page_cached_end -= max_page;
    }
  }
};

#undef max_page
#undef page_mask

}  // namespace eevee::shadow
