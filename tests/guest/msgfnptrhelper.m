/* See msgfnptrhelper.h. */
#import "msgfnptrhelper.h"

@implementation MFHelper {
    mf_fn _fn;
    void *_ctx;
}
- (long)call:(long)a b:(long)b c:(long)c d:(long)d fn:(mf_fn)fn { return fn(a + b + c + d); }
- (long)stack:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(long)f fn:(mf_fn)fn { return fn(a + b + c + d + e + f); }
- (long)packed:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(char)f g:(int)g fn:(mf_fn)fn
{
    return fn(a + b + c + d + e + f + g);
}
- (double)fp:(double)x fn:(mf_dfn)fn y:(double)y { return fn(x) + y; }
- (long)rec:(MFRec)r { return r.fn(r.v); }
- (long)big:(MFBig)r { return r.fn(r.a + r.b + r.c); }
- (void)setFn:(mf_fn)fn { _fn = fn; }
- (mf_fn)fn { return _fn; }
- (long)callStored:(long)v { return _fn(v); }
- (void)setCtx:(void *)p { _ctx = p; }
- (void *)ctx { return _ctx; }
@end
