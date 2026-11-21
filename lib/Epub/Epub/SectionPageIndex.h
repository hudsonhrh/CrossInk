#pragma once

#include <ChunkedPageIndex.h>

struct SectionPageIndexEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
  uint32_t visibleTextOffset;
  // Recorded even when metadata is skipped (for example sleep-page rebuilds),
  // so the same cache remains usable when reopened with stable-page metadata.
  uint32_t referenceTextOffset = 0;
};

static_assert(sizeof(SectionPageIndexEntry) == 16, "Section page index entry size changed");

using SectionPageIndexAllocator = PageIndexAllocator;

template <typename Allocator = SectionPageIndexAllocator>
using BasicSectionPageIndex = ChunkedPageIndex<SectionPageIndexEntry, Allocator>;

using SectionPageIndex = BasicSectionPageIndex<>;
