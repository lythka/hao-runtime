#include "../../src/ioreg.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t hao_trap_address(const char *n, hao_trap_fn f, uint64_t a) { abort(); }

long hao_name_find(hao_name_index *i, const char *n) { abort(); }
void hao_name_each(hao_name_index *i, hao_name_fn cb, void *ctx) { abort(); }
static CFArrayRef snapshot;
static CFTypeRef native_value;
static kern_return_t native_status;
static CFStringRef expected_key;
static CFAllocatorRef expected_allocator;
static const CFStringRef wifi_type = CFSTR("IEEE80211");

static kern_return_t fake_path(io_registry_entry_t e, const io_name_t plane, io_string_t out)
{
    if (e == 99) return kIOReturnNotFound;
    strcpy(out, e == 1 ? "IODeviceTree:/chosen" : "IODeviceTree:/");
    return KERN_SUCCESS;
}
static CFTypeRef fake_property(io_registry_entry_t e, CFStringRef key, CFAllocatorRef a, IOOptionBits o)
{
    if (expected_key) { assert(key == expected_key); assert(a == expected_allocator); }
    return native_value ? CFRetain(native_value) : NULL;
}
static CFTypeRef fake_search(io_registry_entry_t e, const io_name_t p, CFStringRef k, CFAllocatorRef a, IOOptionBits o)
{ return fake_property(e, k, a, o); }
static kern_return_t fake_properties(io_registry_entry_t e, CFMutableDictionaryRef *out, CFAllocatorRef a, IOOptionBits o)
{
    if (native_status) return native_status;
    *out = CFDictionaryCreateMutable(a, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(*out, CFSTR("unrelated"), CFSTR("preserved"));
    if (native_value) CFDictionarySetValue(*out, CFSTR("mac-address-wifi0"), native_value);
    return KERN_SUCCESS;
}
static CFArrayRef fake_interfaces(void) { return snapshot ? CFRetain(snapshot) : NULL; }
static CFStringRef fake_type(SCNetworkInterfaceRef item)
{ return CFDictionaryGetValue((CFDictionaryRef)item, CFSTR("type")); }
static CFStringRef fake_address(SCNetworkInterfaceRef item)
{ return CFDictionaryGetValue((CFDictionaryRef)item, CFSTR("address")); }
static void configure(const char *address, unsigned wifi_count, bool ethernet)
{
    if (snapshot) CFRelease(snapshot);
    CFMutableArrayRef items = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (unsigned i = 0; i < wifi_count + ethernet; i++) {
        CFMutableDictionaryRef item = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(item, CFSTR("type"), i < wifi_count ? wifi_type : CFSTR("Ethernet"));
        if (address) {
            CFStringRef s = CFStringCreateWithCString(NULL, address, kCFStringEncodingASCII);
            CFDictionarySetValue(item, CFSTR("address"), s); CFRelease(s);
        }
        CFArrayAppendValue(items, item); CFRelease(item);
    }
    snapshot = items;
}
static CFTypeRef query(unsigned op, unsigned entry, CFStringRef key, unsigned options, CFAllocatorRef allocator)
{
    hao_thread *t = calloc(1, sizeof *t);
    t->cpu.x[0] = entry;
    t->cpu.x[1] = (uintptr_t)(op == 2 ? (const void *)"IODeviceTree" : key);
    t->cpu.x[op == 2 ? 2 : 1] = (uintptr_t)key;
    t->cpu.x[op == 2 ? 3 : 2] = (uintptr_t)allocator;
    t->cpu.x[op == 2 ? 4 : 3] = options;
    trap_ioreg(t, op);
    CFTypeRef result = (CFTypeRef)(uintptr_t)t->cpu.x[0];
    free(t); return result;
}
static void check_wifi(CFTypeRef value)
{
    static const UInt8 expected[] = {2, 0x12, 0x34, 0x56, 0x78, 0x9a};
    assert(value && CFGetTypeID(value) == CFDataGetTypeID());
    assert(CFDataGetLength(value) == 6 && !memcmp(CFDataGetBytePtr(value), expected, 6));
}
static void *worker(void *unused)
{
    for (int i = 0; i < 100; i++) {
        CFTypeRef v = query(0, 1, CFSTR("mac-address-wifi0"), 0, NULL);
        check_wifi(v); CFRelease(v);
    }
    return NULL;
}
static unsigned allocated, deallocated;
static void *allocate(CFIndex size, CFOptionFlags hint, void *info) { allocated++; return malloc(size); }
static void deallocate(void *ptr, void *info) { deallocated++; free(ptr); }
static void *reallocate(void *ptr, CFIndex size, CFOptionFlags hint, void *info) { return realloc(ptr, size); }

int main(void)
{
    pthread_once(&api_once, load_api); assert(api.ready);
    pthread_once(&sc_once, load_sc); assert(sc.ready);
    api.IORegistryEntryGetPath = fake_path;
    api.IORegistryEntryCreateCFProperty = fake_property;
    api.IORegistryEntryCreateCFProperties = fake_properties;
    api.IORegistryEntrySearchCFProperty = fake_search;
    sc.SCNetworkInterfaceCopyAll = fake_interfaces;
    sc.SCNetworkInterfaceGetInterfaceType = fake_type;
    sc.SCNetworkInterfaceGetHardwareAddressString = fake_address;
    sc.wifi_type = &wifi_type;
    configure("02:12:34:56:78:9A", 1, true);
    for (unsigned op = 0; op <= 2; op += 2) {
        CFTypeRef v = query(op, 1, CFSTR("mac-address-wifi0"), 0, NULL); check_wifi(v); CFRelease(v);
        assert(!query(op, 2, CFSTR("mac-address-wifi0"), 0, NULL));
        assert(!query(op, 99, CFSTR("mac-address-wifi0"), 0, NULL));
        assert(!query(op, 1, CFSTR("board-id"), 0, NULL));
        assert(!query(op, 1, CFSTR("mac-address-wifi0"), 1, NULL));
    }
    native_value = CFSTR("native-wins-even-with-another-type");
    CFTypeRef v = query(0, 1, CFSTR("mac-address-wifi0"), 0, NULL);
    assert(v == native_value); CFRelease(v);

    expected_key = (CFStringRef)(uintptr_t)0xab00000000123451ull;
    expected_allocator = (CFAllocatorRef)(uintptr_t)UINT64_MAX;
    v = query(0, 1, expected_key, 0, expected_allocator);
    assert(v == native_value); CFRelease(v);
    expected_key = NULL; expected_allocator = NULL; native_value = NULL;

    CFAllocatorContext context = {0, NULL, NULL, NULL, NULL, allocate, reallocate, deallocate, NULL};
    CFAllocatorRef allocator = CFAllocatorCreate(NULL, &context);
    v = query(0, 1, CFSTR("mac-address-wifi0"), 0, allocator); check_wifi(v);
    CFRelease(v); assert(allocated && allocated == deallocated); CFRelease(allocator);

    hao_thread *t = calloc(1, sizeof *t);
    CFMutableDictionaryRef dictionary = NULL;
    t->cpu.x[0] = 1; t->cpu.x[1] = (uintptr_t)&dictionary;
    trap_ioreg(t, 1); assert(!t->cpu.x[0] && dictionary);
    check_wifi(CFDictionaryGetValue(dictionary, CFSTR("mac-address-wifi0")));
    assert(CFEqual(CFDictionaryGetValue(dictionary, CFSTR("unrelated")), CFSTR("preserved")));
    CFRelease(dictionary);
    native_status = kIOReturnNotPermitted; dictionary = NULL;
    t->cpu.x[0] = 1; trap_ioreg(t, 1);
    assert((uint32_t)t->cpu.x[0] == (uint32_t)native_status && !dictionary);
    native_status = 0; free(t);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    const char *bad[] = {NULL, "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "01:12:34:56:78:9a", "02:12:34:56:78", "+2:12:34:56:78:9a", "02:12:34:56:78:9g"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        configure(bad[i], 1, false); assert(!query(0, 1, CFSTR("mac-address-wifi0"), 0, NULL));
    }
    configure("02:12:34:56:78:9a", 2, false);
    assert(!query(0, 1, CFSTR("mac-address-wifi0"), 0, NULL));
    configure("02:12:34:56:78:9a", 0, true);
    assert(!query(0, 1, CFSTR("mac-address-wifi0"), 0, NULL));
    CFRelease(snapshot); snapshot = NULL;
    assert(!query(0, 1, CFSTR("mac-address-wifi0"), 0, NULL));
    puts("ioreg: native precedence, scoped fallback, missing/ambiguous input, ownership and concurrency passed");
    return 0;
}
