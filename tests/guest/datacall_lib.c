/* A native library that exports a function's name as data, as OCLP's
 * IO80211 wrapper (Modern Wireless, 13.7.2-25) exports
 * apple80211_roam_profile_typetoStr: a pointer in __DATA,__data. */
const char *placeholder_typetoStr = "placeholder_typetoStr";

const char *real_typetoStr(int t) { return t == 2 ? "two" : "other"; }
