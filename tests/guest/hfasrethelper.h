/* Prototypes of the stand-ins in hfasrethelper.c (see there for where each comes from). */
#include <stdint.h>
typedef struct { double x, y, w, h; } HRect;                  /* CGRect: an HFA of four doubles */
typedef struct { double top, left, bottom, right; } HInsets;  /* UIEdgeInsets */
typedef struct { double a, b, c; } HVec3;                     /* three doubles: an HFA, 24 B */
typedef struct { double lat, lon; } HCoord;                   /* CLLocationCoordinate2D */
typedef struct { double w, h; } HSize;                        /* CGSize */
typedef struct HIter HIter;
HRect SLSWindowIteratorGetBounds(HIter *, long);
HRect SLSWindowIteratorGetScreenRect(HIter *, long);
HRect MKMapRectMakeWithRadialDistance(HCoord, double);
HInsets UIEdgeInsetsMakeWithEdges(unsigned long, double);
HVec3 QCVector_Make(double, double, double);
HRect QLContentRect(HSize, HSize);
HRect UIAccessibilityConvertFrameToScreenCoordinates(HRect, void *);
HRect _MKMapRectThatFits(HRect, HSize, HInsets);
HIter *hfasret_iter_new(void);
void *hfasret_region_new(void);
int hfasret_region_intact(void);
HRect GEOMapRectForMapRegion(void *);
