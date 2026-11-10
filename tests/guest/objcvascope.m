#import "objcvascope.h"
#import <objc/message.h>
@interface HVGuestChild : HVScope
- (NSString *)viaSuper;
@end
@implementation HVGuestChild
- (NSString *)viaSuper { return [super localizedStringWithFormat:@"guest-super"]; }
@end
int main(int argc, const char **argv) {
 @autoreleasepool {
  if (argc != 2) return 2;
  NSString *mode = @(argv[1]);
  if ([mode isEqual:@"collision"]) {
   HVScope *o = [HVScope new];
   puts([o localizedStringWithFormat:@"fixed"].UTF8String);
   puts([HVScope localizedStringWithFormat:@"fixed"].UTF8String);
   puts([o stringWithFormat:@"fixed"].UTF8String);
   puts([HVScope stringWithFormat:@"fixed"].UTF8String);
   puts([o arrayWithObjects:@"fixed"].UTF8String);
   puts([o appendFormat:@"fixed"].UTF8String);
   puts([[HVScopeChild new] localizedStringWithFormat:@"fixed"].UTF8String);
   puts([[HVGuestChild new] viaSuper].UTF8String);
   HVScope *receiver = [HVScopeChild new];
   struct objc_super sup = {receiver, [HVScope class]};
   id (*send)(struct objc_super *, SEL, id) = (void *)objc_msgSendSuper;
   puts([send(&sup, @selector(localizedStringWithFormat:), @"raw-super") UTF8String]);
   puts([HVStringOverride localizedStringWithFormat:@"fixed"].UTF8String);
  } else if ([mode isEqual:@"controls"]) {
   puts([NSString localizedStringWithFormat:@"%@/%d/%.2f", @"value", 37, 1.25].UTF8String);
   puts([NSMutableString stringWithFormat:@"%@/%d/%.2f", @"value", 37, 1.25].UTF8String);
   NSArray *a = [NSArray arrayWithObjects:@"a", @"b", @"c", nil];
   NSArray *b = [[NSArray alloc] initWithObjects:@"a", @"b", @"c", nil];
   NSSet *s = [[NSSet alloc] initWithObjects:@"a", @"b", nil];
   NSOrderedSet *t = [[NSOrderedSet alloc] initWithObjects:@"a", @"b", nil];
   NSDictionary *d = [[NSDictionary alloc] initWithObjectsAndKeys:@"v", @"k", nil];
   printf("collections=%lu/%lu/%lu/%lu/%s\n", a.count,b.count,s.count,t.count,[d[@"k"] UTF8String]);
   NSMutableString *m = [NSMutableString stringWithString:@"prefix"];
   [m appendFormat:@"/%@/%d", @"value", 37];
   puts(m.UTF8String);
   puts([@"prefix" stringByAppendingFormat:@"/%@/%d", @"value", 37].UTF8String);
  } else return 2;
 }
 return 0;
}
