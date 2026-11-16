/* Native helper for tests/guest/cxxbool.mm (see cxxboolhelper.h). */
#import "cxxboolhelper.h"

@implementation CBCell
@end

int cb_set_text(CBCell *cell, NSString *text, BOOL clear, BOOL plain)
{
    cell.value = (int)text.length * 100 + (clear ? 10 : 0) + (plain ? 1 : 0);
    return cell.value;
}

int cb_mixed(bool cxx, BOOL objc, int v) { return v * 4 + (cxx ? 2 : 0) + (objc ? 1 : 0); }

void cb_store(BOOL *out, BOOL v) { *out = !v; }

namespace cbns {
int Table::pick(int a, int b, BOOL first) { return first ? a : b; }
template <class T> T choose(T a, T b, BOOL first) { return first ? a : b; }
template int choose<int>(int, int, BOOL);
template double choose<double>(double, double, BOOL);
}

int cb_plain(bool v) { return v ? 7 : 3; }
