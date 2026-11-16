#include "hao/runtime.h"
#include "hao/bridge.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

#define FIELD(n) __typeof__(&n) n
static struct {
    FIELD(IORegistryEntryCreateCFProperty);
    FIELD(IORegistryEntryCreateCFProperties);
    FIELD(IORegistryEntrySearchCFProperty);
    FIELD(IORegistryEntryGetPath);
    FIELD(CFStringGetCString);
    FIELD(CFStringCreateWithCString);
    FIELD(CFArrayGetCount);
    FIELD(CFArrayGetValueAtIndex);
    FIELD(CFEqual);
    FIELD(CFDataCreate);
    FIELD(CFDictionaryGetValue);
    FIELD(CFDictionarySetValue);
    FIELD(CFRelease);
    bool ready;
} api;
static struct {
    FIELD(SCNetworkInterfaceCopyAll);
    FIELD(SCNetworkInterfaceGetInterfaceType);
    FIELD(SCNetworkInterfaceGetHardwareAddressString);
    const CFStringRef *wifi_type;
    bool ready;
} sc;
#undef FIELD
static pthread_once_t api_once = PTHREAD_ONCE_INIT, sc_once = PTHREAD_ONCE_INIT;

static void load_api(void)
{
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
    if (!io || !cf) return;
#define LOAD(h, n) do { api.n = dlsym(h, #n); if (!api.n) return; } while (0)
    LOAD(io, IORegistryEntryCreateCFProperty);
    LOAD(io, IORegistryEntryCreateCFProperties);
    LOAD(io, IORegistryEntrySearchCFProperty);
    LOAD(io, IORegistryEntryGetPath);
    LOAD(cf, CFStringGetCString);
    LOAD(cf, CFStringCreateWithCString);
    LOAD(cf, CFArrayGetCount);
    LOAD(cf, CFArrayGetValueAtIndex);
    LOAD(cf, CFEqual);
    LOAD(cf, CFDataCreate);
    LOAD(cf, CFDictionaryGetValue);
    LOAD(cf, CFDictionarySetValue);
    LOAD(cf, CFRelease);
#undef LOAD
    api.ready = true;
}

static void load_sc(void)
{
    void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return;
#define LOAD(n) do { sc.n = dlsym(h, #n); if (!sc.n) return; } while (0)
    LOAD(SCNetworkInterfaceCopyAll);
    LOAD(SCNetworkInterfaceGetInterfaceType);
    LOAD(SCNetworkInterfaceGetHardwareAddressString);
#undef LOAD
    sc.wifi_type = dlsym(h, "kSCNetworkInterfaceTypeIEEE80211");
    sc.ready = sc.wifi_type && *sc.wifi_type;
}

static bool chosen(io_registry_entry_t entry)
{
    io_string_t path;
    return api.IORegistryEntryGetPath(entry, "IODeviceTree", path) == KERN_SUCCESS &&
           !strcmp(path, "IODeviceTree:/chosen");
}

static bool wifi_key(CFStringRef key)
{
    char name[32];
    return key && api.CFStringGetCString(key, name, sizeof name, kCFStringEncodingASCII) &&
           !strcmp(name, "mac-address-wifi0");
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool mac_bytes(CFStringRef value, UInt8 bytes[6])
{
    char text[18];
    if (!value || !api.CFStringGetCString(value, text, sizeof text, kCFStringEncodingASCII) || strlen(text) != 17)
        return false;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 6; i++) {
        int hi = hex_digit(text[i * 3]), lo = hex_digit(text[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && text[i * 3 + 2] != ':')) return false;
        bytes[i] = (UInt8)((hi << 4) | lo);
        nonzero |= bytes[i];
    }
    return nonzero && !(bytes[0] & 1);
}

static CFDataRef copy_wifi(CFAllocatorRef allocator)
{
    pthread_once(&sc_once, load_sc);
    if (!sc.ready) return NULL;
    CFArrayRef interfaces = sc.SCNetworkInterfaceCopyAll();
    if (!interfaces) return NULL;
    unsigned count = 0;
    bool valid = false;
    UInt8 bytes[6];
    for (CFIndex i = 0; i < api.CFArrayGetCount(interfaces); i++) {
        SCNetworkInterfaceRef item = (SCNetworkInterfaceRef)api.CFArrayGetValueAtIndex(interfaces, i);
        CFStringRef type = sc.SCNetworkInterfaceGetInterfaceType(item);
        if (!type || !api.CFEqual(type, *sc.wifi_type)) continue;
        if (++count > 1) break;
        valid = mac_bytes(sc.SCNetworkInterfaceGetHardwareAddressString(item), bytes);
    }
    CFDataRef result = count == 1 && valid ? api.CFDataCreate(allocator, bytes, 6) : NULL;
    api.CFRelease(interfaces);
    return result;
}

static void trap_ioreg(hao_thread *t, uint64_t op)
{
    hao_cpu *c = &t->cpu;
    pthread_once(&api_once, load_api);
    if (!api.ready) {
        c->x[0] = op == 1 ? (uint32_t)kIOReturnUnsupported : 0;
        return;
    }
    io_registry_entry_t entry = (io_registry_entry_t)c->x[0];
    if (op == 1) {
        CFMutableDictionaryRef *out = (void *)(uintptr_t)hao_tbi(c->x[1]);
        CFAllocatorRef allocator = (void *)(uintptr_t)c->x[2];
        IOOptionBits options = (IOOptionBits)c->x[3];
        kern_return_t kr = api.IORegistryEntryCreateCFProperties(entry, out, allocator, options);
        if (kr == KERN_SUCCESS && !options && out && *out && chosen(entry)) {
            CFStringRef key = api.CFStringCreateWithCString(NULL, "mac-address-wifi0", kCFStringEncodingASCII);
            if (key) {
                if (!api.CFDictionaryGetValue(*out, key)) {
                    CFDataRef value = copy_wifi(allocator);
                    if (value) { api.CFDictionarySetValue(*out, key, value); api.CFRelease(value); }
                }
                api.CFRelease(key);
            }
        }
        c->x[0] = (uint32_t)kr;
        return;
    }
    CFStringRef key = (void *)(uintptr_t)c->x[op == 2 ? 2 : 1];
    CFAllocatorRef allocator = (void *)(uintptr_t)c->x[op == 2 ? 3 : 2];
    IOOptionBits options = (IOOptionBits)c->x[op == 2 ? 4 : 3];
    const char *plane = op == 2 ? (void *)(uintptr_t)hao_tbi(c->x[1]) : NULL;
    CFTypeRef result = op == 2 ? api.IORegistryEntrySearchCFProperty(entry, plane, key, allocator, options) :
                                api.IORegistryEntryCreateCFProperty(entry, key, allocator, options);

    if (!result && !options && wifi_key(key) && chosen(entry) &&
        (op != 2 || (plane && !strcmp(plane, "IODeviceTree"))))
        result = copy_wifi(allocator);
    c->x[0] = (uint64_t)(uintptr_t)result;
}

static const char *const names[] = {
    "_IORegistryEntryCreateCFProperty", "_IORegistryEntryCreateCFProperties", "_IORegistryEntrySearchCFProperty"
};
static hao_name_index names_index = HAO_NAME_INDEX(names, NULL);

bool hao_ioreg_interpose(const char *name, uint64_t *addr)
{
    long i = hao_name_find(&names_index, name);
    if (i >= 0) { *addr = hao_trap_address(name, trap_ioreg, (uint64_t)i); return true; }
    return false;
}

void hao_ioreg_interpose_names(hao_name_fn cb, void *ctx)
{
    hao_name_each(&names_index, cb, ctx);
}
