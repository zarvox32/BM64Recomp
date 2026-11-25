#ifndef __BM64_MENU_CURSOR_SEARCH_H__
#define __BM64_MENU_CURSOR_SEARCH_H__

#include <cstdint>

// Developer tool for finding where the game keeps each of its menus' cursors,
// so those menus can be made to speak. Enabled by setting the BM64_MENU_SEARCH
// environment variable.
//
// Nothing happens until the tester presses F9 in one of the game's menus.
// Then each Up/Down/Left/Right press is followed by a comparison of the game's
// RAM from before and after it. Values that moved one step with every press,
// and didn't change between presses, are the cursor candidates. Results are
// printed as [Search] lines on stderr. Events are spoken; tones only pace the
// presses:
//   normal beep        press a direction now (the only thing it means)
//   low tone           the last item: pressing past it didn't move the cursor,
//                      so the menu doesn't wrap. Once a menu's end is known,
//                      also played on arriving at its last item. A normal beep
//                      follows after a pause.
//   two short beeps    the cursor wrapped around: the menu wraps
//   low buzz           pressed before the beep; that press isn't measured
//   two falling notes  lost track of the cursor; press F9 to start over
// A, B or Start (choosing something or going back) stops the search for that
// menu, as does F9. Each measured press also prints a "[Search] shot <name>"
// line, so a screenshot of the menu can be taken at that moment.
namespace bm64::menu_cursor_search {
    // Records where the game's RAM is. Call once the game is running.
    void set_rdram(uint8_t* rdram);

    // Start and stop the search thread. Both do nothing unless
    // BM64_MENU_SEARCH is set.
    void start();
    void stop();
}

#endif
