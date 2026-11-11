/*
 * hao — the code identity Security.framework gives a listed hao stub or re-signed native
 * program (docs/security-model.md, Identity; the lists: include/hao/idlist.h).
 *
 * The image's x86_64 Security.framework calls this after its own validity check, with the
 * status that check returned. Only a requirement failure (errSecCSReqFailed) of a listed
 * code is reconsidered: the requirement is evaluated against the Apple-signed reference
 * slice the list names, and that verdict is returned. Everything else is returned unchanged.
 *
 * For a running code (SecCodeRef) the code must first pass the check without a requirement
 * (the running code is the file's), and its cdhash is the one the kernel reports. The
 * reference is opened at its path and arch, and must have exactly the listed cdhash; its
 * signature is checked by Security (kSecCSDoNotValidateExecutable | kSecCSDoNotValidateResources:
 * the cdhash binds the code, the System volume is sealed). A reference that is itself listed
 * is never followed.
 *
 * Built as build/libhao_codeid.a (this file's code and src/idlist.c), no libhao dependency.
 * Not reentrant into the caller's check: it calls SecCodeCheckValidity with no requirement
 * and SecStaticCodeCheckValidityWithErrors on the reference, which reach it again only with a
 * status it returns unchanged.
 */
#ifndef HAO_CODEID_H
#define HAO_CODEID_H
#include <Security/Security.h>

/* code: a SecCodeRef or SecStaticCodeRef; requirement: the caller's (NULL: nothing to
 * reconsider); status: what the original check returned. *errors (if errors is not NULL)
 * is released and cleared when the answer becomes errSecSuccess. */
OSStatus hao_codeid_decide(SecStaticCodeRef code, SecRequirementRef requirement, OSStatus status, CFErrorRef *errors);

/* Where to call it: at the return of SecCodeCheckValidity(WithErrors) and
 * SecStaticCodeCheckValidity(WithErrors). SecTaskValidateForRequirement evaluates through
 * SecCodeCheckValidity in Apple's Security sources, so it needs no entry of its own if the
 * carrier hooks that function's body rather than its export. */
#endif
