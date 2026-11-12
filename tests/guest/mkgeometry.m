// MapKit geometry functions that pass or return MKMapRect / MKCoordinateRegion
// (four doubles, 32 bytes) by value: an HFA in v registers on arm64, MEMORY class
// (stack argument, sret result in rdi) on x86_64 SysV. Each must equal native arm64.
#import <MapKit/MapKit.h>
#include <stdio.h>

// No header declares these two (src/aggspi_gen.inc).
extern MKMapRect MKMapRectForCoordinateRegion(MKCoordinateRegion region);
extern MKMapRect MKMapRectBoundingMapPointsWrappingMeridian(const MKMapPoint *points, NSInteger count);

static void rect(const char *what, MKMapRect r)
{
    printf("%-36s %.6f %.6f %.6f %.6f\n", what, r.origin.x, r.origin.y, r.size.width, r.size.height);
}

int main(void)
{
    MKCoordinateRegion region = MKCoordinateRegionMake(CLLocationCoordinate2DMake(37.7955, -122.3937),
                                                       MKCoordinateSpanMake(0.01, 0.02));
    MKMapRect a = MKMapRectForCoordinateRegion(region);
    rect("MKMapRectForCoordinateRegion", a);

    MKMapRect b = MKMapRectMake(a.origin.x + a.size.width / 2, a.origin.y - 1000, a.size.width, a.size.height);
    rect("MKMapRectIntersection", MKMapRectIntersection(a, b));
    rect("MKMapRectRemainder", MKMapRectRemainder(MKMapRectMake(MKMapSizeWorld.width - 500, 1000, 2000, 300)));
    printf("%-36s %d %d\n", "MKMapRectSpans180thMeridian",
           MKMapRectSpans180thMeridian(MKMapRectMake(MKMapSizeWorld.width - 500, 1000, 2000, 300)),
           MKMapRectSpans180thMeridian(a));

    MKMapRect slice, remainder;
    MKMapRectDivide(a, &slice, &remainder, a.size.width / 3, CGRectMaxXEdge);
    rect("MKMapRectDivide slice", slice);
    rect("MKMapRectDivide remainder", remainder);

    MKMapPoint points[3] = {{1000.5, 2000.25}, {MKMapSizeWorld.width - 10, 2500}, {30, 1800}};
    rect("MKMapRectBoundingMapPointsWrapping", MKMapRectBoundingMapPointsWrappingMeridian(points, 3));

    // Already typed in sigdb_gen.inc: a control that the same shapes still agree.
    printf("%-36s %d\n", "MKMapRectContainsPoint", MKMapRectContainsPoint(a, MKMapPointMake(a.origin.x + 1, a.origin.y + 1)));
    rect("MKMapRectUnion", MKMapRectUnion(a, b));
    return 0;
}
