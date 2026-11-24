#include <ffi/ffi.h>
#include <cstdio>

static void raise_exception(ffi_cif *, void *, void **, void *)
{
    throw 17;
}

int main()
{
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &ffi_type_void, nullptr) != FFI_OK) return 2;
    void *code;
    auto *closure = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
    if (!closure || ffi_prep_closure_loc(closure, &cif, raise_exception, nullptr, code) != FFI_OK) return 3;
    try {
        ffi_call(&cif, FFI_FN(code), nullptr, nullptr);
    } catch (int value) {
        std::fprintf(stderr, "caught native-unwound value %d\n", value);
        ffi_closure_free(closure);
        return value == 17 ? 0 : 4;
    }
    return 5;
}
