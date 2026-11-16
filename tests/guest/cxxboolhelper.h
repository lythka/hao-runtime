/* Native helper for tests/guest/cxxbool.mm. Built universal (x86_64 + arm64
 * + arm64e) from ObjC++: ObjC BOOL is bool on arm64 and signed char on
 * x86_64, so each C++ function below with BOOL in its signature is exported
 * as ..b.. by the arm64 slices and ..a.. by the x86_64 one. Its ObjC class
 * keeps the library native under hao (a C++ library without ObjC or Swift
 * metadata would load as guest code). */
#ifndef CXXBOOLHELPER_H
#define CXXBOOLHELPER_H
#import <Foundation/Foundation.h>

@interface CBCell : NSObject
@property int value;
@end

/* __Z..P6CBCellP8NSStringbb: two BOOLs, as a cell setter */
int cb_set_text(CBCell *cell, NSString *text, BOOL clear, BOOL plain);
/* a C++ bool next to a BOOL: ..bb on arm64, ..ba on x86_64 */
int cb_mixed(bool cxx, BOOL objc, int v);
/* BOOL behind a pointer: Pb / Pa */
void cb_store(BOOL *out, BOOL v);
/* a nested name and a template function (its result type is in the name) */
namespace cbns {
struct Table {
    static int pick(int a, int b, BOOL first);
};
template <class T> T choose(T a, T b, BOOL first);
}
/* C++ bool only: the same name in every slice */
int cb_plain(bool v);
#endif
