/*
 * GSS-API layout crossing: the gss_OID_desc / gss_OID_set / channel-bindings
 * structures are laid out differently on x86_64 (pack(2)) and arm64, so OIDs
 * and OID sets must be converted at the boundary (src/gss.c). This program
 * exercises every direction and prints a stable transcript:
 *   - a native OID set reaching the guest (gss_indicate_mechs) and read back;
 *   - a static guest OID passed to native code (gss_test_oid_set_member,
 *     gss_oid_equal): the sshd-auth crash was here;
 *   - exported OID data (GSS_C_NT_* and gss_mech_krb5) read by the guest and
 *     handed back to native code (gss_import_name's name type);
 *   - a guest-built OID set modified by native code (gss_create_empty_oid_set,
 *     gss_add_oid_set_member, gss_release_oid_set);
 *   - gss_display_name / gss_oid_to_str string results.
 * Built arm64e (guest) and arm64 (reference); gss.sh diffs the transcripts.
 */
#include <GSS/GSS.h>
#include <stdio.h>
#include <string.h>

static void show_oid(const char *tag, gss_const_OID o)
{
    printf("%s len=%u:", tag, o ? o->length : 0);
    for (OM_uint32 k = 0; o && k < o->length; k++) printf(" %02x", ((unsigned char *)o->elements)[k]);
    putchar('\n');
}

int main(void)
{
    OM_uint32 maj, min;

    /* native OID set -> guest */
    gss_OID_set mechs = GSS_C_NO_OID_SET;
    maj = gss_indicate_mechs(&min, &mechs);
    printf("indicate_mechs maj=%u count=%zu\n", maj, mechs ? mechs->count : 0);
    for (size_t k = 0; mechs && k < mechs->count && k < 8; k++) {
        char buf[32];
        snprintf(buf, sizeof buf, "  mech[%zu]", k);
        show_oid(buf, &mechs->elements[k]);
    }

    /* static guest OID vs the native set (the sshd-auth path) and OID equality */
    gss_OID_desc krb5 = {9, (void *)"\x2a\x86\x48\x86\xf7\x12\x01\x02\x02"};
    int present = 0;
    maj = gss_test_oid_set_member(&min, &krb5, mechs, &present);
    printf("test_oid_set_member krb5 maj=%u present=%d\n", maj, present);
    gss_OID_desc bogus = {3, (void *)"\x2b\x05\x01"};
    present = 7;
    gss_test_oid_set_member(&min, &bogus, mechs, &present);
    printf("test_oid_set_member bogus present=%d\n", present);

    /* exported OID data read by the guest, then handed back to native code */
    show_oid("GSS_C_NT_HOSTBASED_SERVICE", GSS_C_NT_HOSTBASED_SERVICE);
    show_oid("GSS_C_NT_USER_NAME", GSS_C_NT_USER_NAME);
    printf("oid_equal(krb5, mech_krb5)=%d\n", gss_oid_equal(&krb5, gss_mech_krb5));

    gss_buffer_desc name_in = {sizeof("host@example.com") - 1, (void *)"host@example.com"};
    gss_name_t name = GSS_C_NO_NAME;
    maj = gss_import_name(&min, &name_in, GSS_C_NT_HOSTBASED_SERVICE, &name);
    printf("import_name maj=%u\n", maj);
    if (name) {
        gss_buffer_desc disp = GSS_C_EMPTY_BUFFER;
        gss_OID disp_type = GSS_C_NO_OID;
        maj = gss_display_name(&min, name, &disp, &disp_type);
        printf("display_name maj=%u value=%.*s\n", maj, (int)disp.length, (char *)disp.value);
        show_oid("  display_type", disp_type);
        gss_release_buffer(&min, &disp);
        gss_release_name(&min, &name);
    }

    /* guest-built OID set modified by native code */
    gss_OID_set built = GSS_C_NO_OID_SET;
    maj = gss_create_empty_oid_set(&min, &built);
    printf("create_empty_oid_set maj=%u count=%zu\n", maj, built ? built->count : 0);
    gss_add_oid_set_member(&min, &krb5, &built);
    gss_add_oid_set_member(&min, GSS_C_NT_HOSTBASED_SERVICE, &built);
    printf("after add count=%zu\n", built ? built->count : 0);
    present = 0;
    gss_test_oid_set_member(&min, &krb5, built, &present);
    printf("built contains krb5=%d\n", present);
    for (size_t k = 0; built && k < built->count; k++) {
        char buf[32];
        snprintf(buf, sizeof buf, "  built[%zu]", k);
        show_oid(buf, &built->elements[k]);
    }
    gss_release_oid_set(&min, &built);
    printf("after release built=%p\n", (void *)built);

    gss_buffer_desc s = GSS_C_EMPTY_BUFFER;
    maj = gss_oid_to_str(&min, &krb5, &s);
    printf("oid_to_str maj=%u value=%.*s\n", maj, (int)s.length, (char *)s.value);
    gss_release_buffer(&min, &s);

    gss_release_oid_set(&min, &mechs);
    printf("done\n");
    return 0;
}
