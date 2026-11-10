#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int nativeBundle(const char *path) {
 @autoreleasepool {
  NSBundle *b=[NSBundle bundleWithPath:[NSString stringWithUTF8String:path]];
  NSError *error=nil;
  if (![b loadAndReturnError:&error]) { fprintf(stderr,"bundle load: %s\n",error.description.UTF8String); return 1; }
  Class principal=b.principalClass;
  long (*send)(id,SEL)=(void *)objc_msgSend;
  long a=principal?send(principal,sel_registerName("value")):-1;
  NSString *resource=[NSString stringWithContentsOfFile:[b pathForResource:@"payload" ofType:@"txt"] encoding:NSUTF8StringEncoding error:nil];
  int ok=[[NSBundle bundleForClass:principal].bundlePath isEqualToString:b.bundlePath] && [b classNamed:NSStringFromClass(principal)]==principal && a==41 && b.isLoaded && [resource isEqualToString:@"resource\n"];
  void *h=dlopen(b.executablePath.fileSystemRepresentation,RTLD_LAZY|RTLD_LOCAL);
  ok=ok && h;
  void *missing=dlsym(h,"deliberatelyMissingGuestSymbol");
  ok=ok && !missing;
  /* A later native error supersedes a pending guest error. */
  missing=dlsym(RTLD_DEFAULT,"deliberatelyMissingNativeSymbol");
  const char *why=dlerror();
  ok=ok && !missing && why && strstr(why,"deliberatelyMissingNativeSymbol") && !dlerror();
  (void)dlsym(RTLD_DEFAULT,"deliberatelyMissingNativeSymbol");
  int *data=dlsym(h,"bundleData");
  ok=ok && data && *data==19 && !dlerror();
  void *function=dlsym(h,"untypedExport");
  if (getenv("EXPECT_GUEST_BUNDLE")) ok=ok && !function && dlerror();
  else ok=ok && function && !dlerror();
  void *global=dlopen(b.executablePath.fileSystemRepresentation,RTLD_LAZY|RTLD_GLOBAL|RTLD_FIRST);
  data=dlsym(RTLD_DEFAULT,"bundleData");
  ok=ok && global && data && *data==19 && !dlerror();
  ok=ok && dlclose(h)==0 && dlclose(global)==0 && send(principal,sel_registerName("value"))==41;
  ok=ok && [b loadAndReturnError:&error] && send(principal,sel_registerName("value"))==41;
  printf("bundle %d\n",ok);
  return !ok;
 }
}
int nativeReject(const char *path) {
 void *h=dlopen(path,RTLD_LAZY|RTLD_LOCAL);
 const char *error=dlerror();
 int ok=!h && error && !dlerror();
 if (h) dlclose(h);
 printf("refused %d\n",ok);
 return !ok;
}
