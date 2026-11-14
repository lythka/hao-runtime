/* Prototypes of the stand-ins in aggspi6helper.c (see there for where each comes from). */
#include <stdint.h>
#include <stdbool.h>
typedef struct { void (*a)(void); void (*b)(void); void (*c)(void); } SIFileOps;     /* {SIFileOps=^?^?^?} 24 B */
typedef struct { unsigned int val[8]; } audit_token_t;                               /* {?=[8I]} 32 B */
typedef struct { struct { double x, y; } origin; struct { double width, height; } size; } HRect; /* CGRect HFA */
typedef struct { uint64_t w[6]; } Params48;                                          /* 48 B */
int SIOpenIndex(void **, int, void *, void *, uint32_t, void *, SIFileOps, void *, void *, volatile int *);
int SICreateNewIndex(void **, int, void *, void *, uint32_t, SIFileOps, void *, void *, volatile int *);
void ess_notify_tcc_modify(const char *, const char *, int, int, audit_token_t, const audit_token_t *, int, int);
void ess_notify_od_group_add(audit_token_t, int, const char *, int, const char *, const char *, const char *);
void ess_notify_od_attribute_set(audit_token_t, int, int, const char *, const char *, const char *, const char *,
                                 const char *, const char *);
void QLPushCustomRoundedRect(void *, HRect, double, bool, bool, bool, bool, int, bool);
void *CGImageCreateCopyWithParameters(void *, void *, Params48, void *, void *, void *, void *, void *, void *, int,
                                      bool, int);
/* SIUserCtxCreate (SpotlightIndex, mds): 1104-byte credential and audit_token_t by value, then 9 more */
typedef struct { uint64_t w[138]; } SICred1104;                                     /* 0x450 B */
void *SIUserCtxCreate(void *, void *, SICred1104, audit_token_t, void *, void *, void *, void *, void *, void *,
                      void *, bool, void *);
/* NodeCreateSemanticNodeQuery (DesktopServicesPriv, Finder): five pointers, then a 24-byte client context
 * {?=^v^?^?} (info, retain, release) whose callbacks the callee calls */
typedef struct { void *info; void *(*retain)(void *); void (*release)(void *); } NodeClientContext;
void *NodeCreateSemanticNodeQuery(void *, void *, void *, void *, void *, NodeClientContext);
/* MRMediaRemoteRequestVideoThumbnails (MediaRemote): a 2064-byte record first, three pointers, then a completion
 * block the callee invokes as block(thumbnails, error) */
typedef struct { double times[256]; uint64_t count; float width, height; } MRThumbRequest;   /* {?=[256d]Qff} */
void MRMediaRemoteRequestVideoThumbnails(MRThumbRequest, void *, void *, void *, void (^)(void *, void *));
