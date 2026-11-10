/* libaddrkeyspub.dylib for tests/guest/addrkeys.sh: renamed re-exports (the link line's -alias
 * of symbols other libraries define): akp_flag -> libaddrkeysimpl's akx_flag, and two functions
 * hao answers with a trap, under names hao has no entry for: akp_dladdr -> libdyld's dladdr,
 * akp_chkstk -> libcompiler_rt's ___chkstk_darwin (itself a re-export of libsystem_pthread's
 * ____chkstk_darwin). One function of its own. */
int akp_own(int v) { return v + 1; }
