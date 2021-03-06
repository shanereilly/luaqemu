/*
 * QTest testcase for VM Generation ID
 *
 * Copyright (c) 2016 Red Hat, Inc.
 * Copyright (c) 2017 Skyport Systems
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <glib.h>
#include <string.h>
#include <unistd.h>
#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/uuid.h"
#include "hw/acpi/acpi-defs.h"
#include "acpi-utils.h"
#include "libqtest.h"

#define VGID_GUID "324e6eaf-d1d1-4bf6-bf41-b9bb6c91fb87"
#define VMGENID_GUID_OFFSET 40   /* allow space for
                                  * OVMF SDT Header Probe Supressor
                                  */
#define RSDP_ADDR_INVALID 0x100000 /* RSDP must be below this address */
#define RSDP_SLEEP_US     100000   /* Sleep for 100ms between tries */
#define RSDP_TRIES_MAX    100      /* Max total time is 10 seconds */

typedef struct {
    AcpiTableHeader header;
    gchar name_op;
    gchar vgia[4];
    gchar val_op;
    uint32_t vgia_val;
} QEMU_PACKED VgidTable;

static uint32_t acpi_find_vgia(void)
{
    uint32_t rsdp_offset;
    uint32_t guid_offset = 0;
    AcpiRsdpDescriptor rsdp_table;
    uint32_t rsdt;
    AcpiRsdtDescriptorRev1 rsdt_table;
    int tables_nr;
    uint32_t *tables;
    AcpiTableHeader ssdt_table;
    VgidTable vgid_table;
    int i;

    /* Tables may take a short time to be set up by the guest */
    for (i = 0; i < RSDP_TRIES_MAX; i++) {
        rsdp_offset = acpi_find_rsdp_address();
        if (rsdp_offset < RSDP_ADDR_INVALID) {
            break;
        }
        g_usleep(RSDP_SLEEP_US);
    }
    g_assert_cmphex(rsdp_offset, <, RSDP_ADDR_INVALID);

    acpi_parse_rsdp_table(rsdp_offset, &rsdp_table);

    rsdt = rsdp_table.rsdt_physical_address;
    /* read the header */
    ACPI_READ_TABLE_HEADER(&rsdt_table, rsdt);
    ACPI_ASSERT_CMP(rsdt_table.signature, "RSDT");

    /* compute the table entries in rsdt */
    tables_nr = (rsdt_table.length - sizeof(AcpiRsdtDescriptorRev1)) /
                sizeof(uint32_t);
    g_assert_cmpint(tables_nr, >, 0);

    /* get the addresses of the tables pointed by rsdt */
    tables = g_new0(uint32_t, tables_nr);
    ACPI_READ_ARRAY_PTR(tables, tables_nr, rsdt);

    for (i = 0; i < tables_nr; i++) {
        ACPI_READ_TABLE_HEADER(&ssdt_table, tables[i]);
        if (!strncmp((char *)ssdt_table.oem_table_id, "VMGENID", 7)) {
            /* the first entry in the table should be VGIA
             * That's all we need
             */
            ACPI_READ_FIELD(vgid_table.name_op, tables[i]);
            g_assert(vgid_table.name_op == 0x08);  /* name */
            ACPI_READ_ARRAY(vgid_table.vgia, tables[i]);
            g_assert(memcmp(vgid_table.vgia, "VGIA", 4) == 0);
            ACPI_READ_FIELD(vgid_table.val_op, tables[i]);
            g_assert(vgid_table.val_op == 0x0C);  /* dword */
            ACPI_READ_FIELD(vgid_table.vgia_val, tables[i]);
            /* The GUID is written at a fixed offset into the fw_cfg file
             * in order to implement the "OVMF SDT Header probe suppressor"
             * see docs/specs/vmgenid.txt for more details
             */
            guid_offset = vgid_table.vgia_val + VMGENID_GUID_OFFSET;
            break;
        }
    }
    g_free(tables);
    return guid_offset;
}

static void read_guid_from_memory(QemuUUID *guid)
{
    uint32_t vmgenid_addr;
    int i;

    vmgenid_addr = acpi_find_vgia();
    g_assert(vmgenid_addr);

    /* Read the GUID directly from guest memory */
    for (i = 0; i < 16; i++) {
        guid->data[i] = readb(vmgenid_addr + i);
    }
    /* The GUID is in little-endian format in the guest, while QEMU
     * uses big-endian.  Swap after reading.
     */
    qemu_uuid_bswap(guid);
}

static void read_guid_from_monitor(QemuUUID *guid)
{
    QDict *rsp, *rsp_ret;
    const char *guid_str;

    rsp = qmp("{ 'execute': 'query-vm-generation-id' }");
    if (qdict_haskey(rsp, "return")) {
        rsp_ret = qdict_get_qdict(rsp, "return");
        g_assert(qdict_haskey(rsp_ret, "guid"));
        guid_str = qdict_get_str(rsp_ret, "guid");
        g_assert(qemu_uuid_parse(guid_str, guid) == 0);
    }
    QDECREF(rsp);
}

static void vmgenid_set_guid_test(void)
{
    QemuUUID expected, measured;
    gchar *cmd;

    g_assert(qemu_uuid_parse(VGID_GUID, &expected) == 0);

    cmd = g_strdup_printf("-machine accel=tcg -device vmgenid,id=testvgid,"
                          "guid=%s", VGID_GUID);
    qtest_start(cmd);

    /* Read the GUID from accessing guest memory */
    read_guid_from_memory(&measured);
    g_assert(memcmp(measured.data, expected.data, sizeof(measured.data)) == 0);

    qtest_quit(global_qtest);
    g_free(cmd);
}

static void vmgenid_set_guid_auto_test(void)
{
    const char *cmd;
    QemuUUID measured;

    cmd = "-machine accel=tcg -device vmgenid,id=testvgid," "guid=auto";
    qtest_start(cmd);

    read_guid_from_memory(&measured);

    /* Just check that the GUID is non-null */
    g_assert(!qemu_uuid_is_null(&measured));

    qtest_quit(global_qtest);
}

static void vmgenid_query_monitor_test(void)
{
    QemuUUID expected, measured;
    gchar *cmd;

    g_assert(qemu_uuid_parse(VGID_GUID, &expected) == 0);

    cmd = g_strdup_printf("-machine accel=tcg -device vmgenid,id=testvgid,"
                          "guid=%s", VGID_GUID);
    qtest_start(cmd);

    /* Read the GUID via the monitor */
    read_guid_from_monitor(&measured);
    g_assert(memcmp(measured.data, expected.data, sizeof(measured.data)) == 0);

    qtest_quit(global_qtest);
    g_free(cmd);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/vmgenid/vmgenid/set-guid",
                   vmgenid_set_guid_test);
    qtest_add_func("/vmgenid/vmgenid/set-guid-auto",
                   vmgenid_set_guid_auto_test);
    qtest_add_func("/vmgenid/vmgenid/query-monitor",
                   vmgenid_query_monitor_test);
    ret = g_test_run();

    return ret;
}
