#import <Foundation/Foundation.h>
#include <numeric>
#include <vector>
extern "C" NSInteger demoNativeChecksum(NSArray<NSNumber *> *values) {
 std::vector<long> items; for (NSNumber *n in values) items.push_back(n.longValue);
 return std::accumulate(items.begin(),items.end(),0L);
}
