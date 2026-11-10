/* libaggaliaspub.dylib for tests/guest/aggalias.sh: re-exports libaggaliasimpl.dylib's functions
 * under other names (the link line's -alias _aai_X _aap_X), and defines one function of its own. */
int aap_own(int v) { return v + 1; }
