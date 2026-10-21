#pragma once
#include "canvas/Geometry.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Typing in a text box on the picture: the caret, the selection, the keys that move them and the
// edits, over a text laid out by DirectWrite (Compose.cpp reports the lines and caret positions
// here). Plain data and maths, so the editing is unit-tested with a layout made by hand.
namespace sd::photo
{
    using canvas::PointD;
    using canvas::RectD;

    // One line of a laid-out text: the characters [start, end) without its line break, and where it
    // sits, relative to the layout's top-left corner (pixels).
    struct TextLine
    {
        size_t start = 0, end = 0;
        double top = 0, height = 0;
        double left = 0, right = 0;   // the x its characters span, trailing spaces included
    };

    struct TextLayoutInfo
    {
        double width = 0, height = 0;   // the text's own size (widest line, all lines)
        std::vector<TextLine> lines;
        // For every position 0..length, the caret's x (left edge of the character there).
        std::vector<double> caretX;

        bool Empty() const { return lines.empty(); }
        // The line a caret position is on: a position where a long line wrapped counts as the start
        // of the next line.
        size_t LineOf(size_t pos) const;
    };

    // The nearest caret position to a point (relative to the layout's top-left).
    size_t PositionAt(const TextLayoutInfo& layout, PointD at);

    struct CaretBox
    {
        double x = 0, top = 0, height = 0;
    };
    CaretBox CaretAt(const TextLayoutInfo& layout, size_t pos);

    // The highlight of [from, to): one rectangle per line it touches; a selected line break shows as
    // a small extra width.
    std::vector<RectD> SelectionRects(const TextLayoutInfo& layout, size_t from, size_t to, double newlineWidth);

    // The caret and the other end of the selection (equal when nothing is selected).
    struct TextCursor
    {
        size_t caret = 0, anchor = 0;

        bool HasSelection() const { return caret != anchor; }
        size_t Start() const { return caret < anchor ? caret : anchor; }
        size_t End() const { return caret < anchor ? anchor : caret; }
        void Collapse(size_t pos) { caret = anchor = pos; }
        void Clamp(const std::wstring& text);
    };

    enum class CaretMove
    {
        Left,
        Right,
        WordLeft,    // Ctrl+Left: to the start of this or the previous word
        WordRight,   // Ctrl+Right: to the start of the next word
        Home,        // the start of the line
        End,         // the end of the line
        Up,
        Down,
        TextStart,   // Ctrl+Home
        TextEnd,     // Ctrl+End
    };

    // Moves the caret; `extend` (Shift) keeps the anchor. Up and Down keep to the column in `goalX`
    // (set from the caret when it is negative, kept as it is otherwise) so a short line in between
    // doesn't pull the caret left. Left and Right without `extend` collapse a selection to its side.
    void MoveCaret(TextCursor& cursor, const std::wstring& text, const TextLayoutInfo& layout, CaretMove move, bool extend, double& goalX);

    // Text edits; each returns whether the text changed. `limit` is the most characters the text keeps.
    bool InsertText(std::wstring& text, TextCursor& cursor, std::wstring_view typed, size_t limit);
    bool DeleteBackward(std::wstring& text, TextCursor& cursor, bool word);   // Backspace, Ctrl+Backspace
    bool DeleteForward(std::wstring& text, TextCursor& cursor, bool word);    // Delete, Ctrl+Delete
    std::wstring SelectedText(const std::wstring& text, const TextCursor& cursor);

    // A double click: the word (or the run of spaces or of punctuation) around `pos`.
    void SelectWord(TextCursor& cursor, const std::wstring& text, size_t pos);

    // Pasted text as a text box takes it: CR LF and lone CRs become line breaks, tabs a space,
    // other control characters go.
    std::wstring CleanPasted(std::wstring_view text);
}
