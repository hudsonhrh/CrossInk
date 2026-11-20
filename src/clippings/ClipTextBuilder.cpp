#include "ClipTextBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "ClippingPreview.h"

namespace ClipTextBuilder {
namespace {

bool hasEmSpace(const char* word) { return word[0] == '\xe2' && word[1] == '\x80' && word[2] == '\x83'; }

struct CleanWord {
  std::string text;
  bool leadingSpace = false;
  bool trailingSpace = false;
};

CleanWord cleanWordText(std::string_view word) {
  CleanWord result;
  auto& text = result.text;
  text.reserve(word.size());
  for (size_t i = 0; i < word.size();) {
    const char c = word[i];
    const size_t utf8Space = clippingPreview::utf8SpaceLength(word.data() + i, word.size() - i);
    if (utf8Space || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      if (text.empty()) {
        result.leadingSpace = true;
      } else if (c == ' ' || text.back() != ' ') {
        text += ' ';
      }
      i += utf8Space ? utf8Space : 1;
    } else {
      text += word[i++];
    }
  }
  result.trailingSpace = !text.empty() && text.back() == ' ';
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return result;
}

bool attachedInReadingOrder(const WordRef& previous, const WordRef& word) {
  if (previous.pageIdx != word.pageIdx) return false;
  // RTL selection visits the same block boundaries in reverse order.
  if (previous.pageWordIndex + 1 == word.pageWordIndex) return word.attachedToPrevious;
  return word.pageWordIndex + 1 == previous.pageWordIndex && previous.attachedToPrevious;
}

std::string_view selectedWordText(const ClipWordStore& wordStore, const WordRef& word,
                                  const SelectionBounds* selectionBounds) {
  const bool isFirstBound = selectionBounds && word.pageIdx == selectionBounds->firstPageIdx &&
                            word.pageWordIndex == selectionBounds->firstPageWordOrdinal;
  const bool isLastBound = selectionBounds && word.pageIdx == selectionBounds->lastPageIdx &&
                           word.pageWordIndex == selectionBounds->lastPageWordOrdinal;
  if (!selectionBounds || (!isFirstBound && !isLastBound)) {
    return {wordStore.text(word), word.textLength};
  }

  const size_t textLength = word.textLength;
  const size_t begin = isFirstBound ? std::min<size_t>(selectionBounds->firstWordByteOffset, textLength) : 0;
  const size_t end = isLastBound ? std::min<size_t>(selectionBounds->lastWordByteEndOffset, textLength) : textLength;
  if (end < begin) return {};
  return {wordStore.text(word) + begin, end - begin};
}

uint32_t countCharacters(const char* text, const size_t begin, const size_t end) {
  uint32_t count = 0;
  for (size_t i = begin; i < end; ++i)
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++count;
  return count;
}

bool selectedSourceRange(const ClipWordStore& store, const WordRef& word, const SelectionBounds* bounds,
                         uint32_t& start, uint32_t& end) {
  start = word.sourceStart;
  end = word.sourceEnd;
  if (!bounds) return true;
  const bool first = word.pageIdx == bounds->firstPageIdx && word.pageWordIndex == bounds->firstPageWordOrdinal;
  const bool last = word.pageIdx == bounds->lastPageIdx && word.pageWordIndex == bounds->lastPageWordOrdinal;
  const size_t begin = first ? std::min<size_t>(bounds->firstWordByteOffset, word.textLength) : 0;
  const size_t finish = last ? std::min<size_t>(bounds->lastWordByteEndOffset, word.textLength) : word.textLength;
  if (begin == 0 && finish == word.textLength) return true;
  const char* text = store.text(word);
  if (start >= end || end == UINT32_MAX || finish < begin || (static_cast<uint8_t>(text[begin]) & 0xC0) == 0x80 ||
      (static_cast<uint8_t>(text[finish]) & 0xC0) == 0x80)
    return false;
  const uint32_t indent = hasEmSpace(text) ? 1 : 0;
  const uint32_t insertedHyphen = word.endsWithInsertedHyphen ? 1 : 0;
  // Whole-word source ranges survive NFC composition. An interior byte bound
  // needs a one-to-one character map; never expand an ambiguous fragment.
  if (end - start != countCharacters(text, 0, word.textLength) - indent - insertedHyphen) return false;
  start += countCharacters(text, 0, begin) - (indent && begin >= 3 ? 1 : 0);
  end -= countCharacters(text, finish, word.textLength) - (insertedHyphen && finish < word.textLength ? 1 : 0);
  return start < end;
}

bool isSelectedTableWord(const WordRef& word, const uint16_t tableSelection) {
  return tableSelection == UINT16_MAX || word.tableSelection == tableSelection;
}

}  // namespace

AllocationBudget allocationBudget(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder,
                                  const int toOrder, const uint16_t tableSelection) {
  AllocationBudget budget;
  size_t longestWord = 0;
  for (int i = fromOrder; i <= toOrder; ++i) {
    const auto& word = wordStore.words[wordOrder[i]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    // Cleaning only removes bytes; allow one separator per selected word.
    budget.textCapacity += word.textLength + 1U;
    longestWord = std::max(longestWord, static_cast<size_t>(word.textLength));
  }
  // Keep conservative headroom for temporary strings during assembly.
  budget.temporaryBytes = 4 * (longestWord + 1U);
  return budget;
}

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder, const int toOrder,
                     const int startPageInSection, const int sectionPageCount, const SelectionBounds* selectionBounds,
                     const uint16_t tableSelection) {
  const auto& words = wordStore.words;
  std::string text;
  // Reserve once for the complete selection instead of repeatedly growing and
  // copying the result. The activity checks this same budget before assembly.
  text.reserve(allocationBudget(wordStore, wordOrder, fromOrder, toOrder, tableSelection).textCapacity);

  const WordRef& firstWord = words[wordOrder[fromOrder]];
  const WordRef& lastWord = words[wordOrder[toOrder]];
  uint16_t startPageWordIndex = firstWord.pageWordIndex;
  uint16_t endPageWordIndex = lastWord.pageWordIndex;
  uint16_t selectedWordCount = 0;
  uint32_t sourceStart = UINT32_MAX, sourceEnd = 0;
  bool allSourceRangesKnown = true;
  bool sourceMappingBlocked = false;
  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    selectedWordCount++;
    uint32_t selectedStart, selectedEnd;
    if (!selectedSourceRange(wordStore, word, selectionBounds, selectedStart, selectedEnd)) {
      allSourceRangesKnown = false;
      sourceMappingBlocked = true;
    } else if (selectedStart > selectedEnd || selectedEnd == UINT32_MAX) {
      allSourceRangesKnown = false;
    } else if (selectedStart < selectedEnd) {
      sourceStart = std::min(sourceStart, selectedStart);
      sourceEnd = std::max(sourceEnd, selectedEnd);
    }
    if (word.pageIdx == firstWord.pageIdx) {
      startPageWordIndex = std::min(startPageWordIndex, word.pageWordIndex);
    }
    if (word.pageIdx == lastWord.pageIdx) {
      endPageWordIndex = std::max(endPageWordIndex, word.pageWordIndex);
    }
  }

  const WordRef* previousWord = nullptr;
  bool previousInsertedHyphen = false;
  bool previousTrailingSpace = false;

  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    auto clean = cleanWordText(selectedWordText(wordStore, word, selectionBounds));
    if (clean.text.empty()) {
      previousTrailingSpace |= clean.leadingSpace;
      continue;
    }

    const bool paragraphStart =
        previousWord && (hasEmSpace(wordStore.text(word)) || word.paragraphStart ||
                         (word.pageIdx == previousWord->pageIdx && word.y > previousWord->y + previousWord->h));
    if (previousWord && !text.empty() && !previousInsertedHyphen) {
      if (paragraphStart) {
        text += '\n';
      } else {
        // A literal hyphen can also be a legal wrap point inside a compound word.
        const bool wrappedHyphen = text.back() == '-' &&
                                   (previousWord->pageIdx != word.pageIdx || previousWord->y != word.y) &&
                                   (previousWord->sourceEnd == UINT32_MAX || word.sourceStart == UINT32_MAX ||
                                    previousWord->sourceEnd == word.sourceStart) &&
                                   !std::ispunct(static_cast<unsigned char>(clean.text.front()));
        if (previousTrailingSpace || clean.leadingSpace ||
            (!wrappedHyphen && !attachedInReadingOrder(*previousWord, word))) {
          text += ' ';
        }
      }
    }
    previousInsertedHyphen = word.endsWithInsertedHyphen && clean.text.back() == '-';
    if (previousInsertedHyphen) clean.text.pop_back();
    text += clean.text;
    previousWord = &word;
    previousTrailingSpace = clean.trailingSpace;
  }

  ClippingResult result{std::move(text),
                        static_cast<uint16_t>(startPageInSection + firstWord.pageIdx),
                        static_cast<uint16_t>(startPageInSection + lastWord.pageIdx),
                        static_cast<uint16_t>(std::max(1, sectionPageCount)),
                        startPageWordIndex,
                        endPageWordIndex,
                        UINT16_MAX,
                        tableSelection,
                        selectedWordCount};
  if (allSourceRangesKnown && sourceStart < sourceEnd) {
    result.sourceStart = sourceStart;
    result.sourceEnd = sourceEnd;
  } else if (sourceMappingBlocked) {
    result.sourceEnd = 0;  // Explicitly unresolvable: do not migrate to the whole outer word.
  }
  return result;
}

}  // namespace ClipTextBuilder
