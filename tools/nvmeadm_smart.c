// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

static void print_critical_warning(UBYTE warning)
{
    print_kv_hex8((CONST_STRPTR)"Critical Warning:", (ULONG)warning);
    if (warning == 0)
    {
        Printf((CONST_STRPTR)"  none\n");
        return;
    }

    if ((warning & (1U << 0)) != 0)
        Printf((CONST_STRPTR)"  available spare below threshold\n");
    if ((warning & (1U << 1)) != 0)
        Printf((CONST_STRPTR)"  temperature threshold exceeded\n");
    if ((warning & (1U << 2)) != 0)
        Printf((CONST_STRPTR)"  device reliability degraded\n");
    if ((warning & (1U << 3)) != 0)
        Printf((CONST_STRPTR)"  media is read-only\n");
    if ((warning & (1U << 4)) != 0)
        Printf((CONST_STRPTR)"  volatile memory backup failed\n");
    if ((warning & (1U << 5)) != 0)
        Printf((CONST_STRPTR)"  persistent memory region degraded\n");
}

static void print_endurance_group_warning(UBYTE warning)
{
    print_kv_hex8((CONST_STRPTR)"Endurance Warnings:", (ULONG)warning);
    if (warning == 0)
    {
        Printf((CONST_STRPTR)"  none\n");
        return;
    }

    if ((warning & (1U << 0)) != 0)
        Printf((CONST_STRPTR)"  endurance-group spare below threshold\n");
    if ((warning & (1U << 2)) != 0)
        Printf((CONST_STRPTR)"  endurance-group reliability degraded\n");
    if ((warning & (1U << 3)) != 0)
        Printf((CONST_STRPTR)"  endurance-group media is read-only\n");
}

/* prints "<temp> K (<temp> C)" with the negative-Celsius branch; the caller
   emits the label prefix so this stays usable for both the main reading and
   the per-sensor lines without changing their (intentionally unnormalized)
   column. */
static void print_temperature_celsius(UWORD temp_k)
{
    LONG temp_c = (LONG)temp_k - 273L;

    if (temp_c < 0)
        Printf((CONST_STRPTR)"%lu K (-%lu C)\n",
               (ULONG)temp_k, (ULONG)(0L - temp_c));
    else
        Printf((CONST_STRPTR)"%lu K (%lu C)\n",
               (ULONG)temp_k, (ULONG)temp_c);
}

static void print_temperature_sensor_readings(const __le16 *sensors, ULONG count)
{
    BOOL printed_any = FALSE;

    for (ULONG i = 0; i < count; i++)
    {
        UWORD temp_k = read_le16((const UBYTE *)&sensors[i]);

        if (temp_k == 0)
            continue;

        if (!printed_any)
        {
            Printf((CONST_STRPTR)"Temperature Sensors:\n");
            printed_any = TRUE;
        }

        Printf((CONST_STRPTR)"  Sensor %lu:            ", i + 1UL);
        print_temperature_celsius(temp_k);
    }
}

static void print_smart_u32_value(CONST_STRPTR label, ULONG value,
                                  CONST_STRPTR units,
                                  CONST_STRPTR zero_text)
{
    if (value == 0 && zero_text)
    {
        Printf((CONST_STRPTR)"%-24s %s\n",
               (ULONG)label, (ULONG)zero_text);
        return;
    }

    if (units)
    {
        Printf((CONST_STRPTR)"%-24s %lu %s\n",
               (ULONG)label, value, (ULONG)units);
    }
    else
    {
        Printf((CONST_STRPTR)"%-24s %lu\n",
               (ULONG)label, value);
    }
}

static void print_percentage_used(UBYTE value)
{
    if (value == 255U)
    {
        Printf((CONST_STRPTR)"Percentage Used:        >=255 %%\n");
        return;
    }

    Printf((CONST_STRPTR)"Percentage Used:        %lu %%\n",
           (ULONG)value);
}

static void print_smart_counter(CONST_STRPTR label, const UBYTE *value,
                                CONST_STRPTR units)
{
    char decimal[40];

    format_le128_decimal(value, decimal, sizeof(decimal));
    if (units)
    {
        Printf((CONST_STRPTR)"%-24s %s %s\n",
               (ULONG)label, (ULONG)decimal, (ULONG)units);
    }
    else
    {
        Printf((CONST_STRPTR)"%-24s %s\n",
               (ULONG)label, (ULONG)decimal);
    }
}

int run_smart(void)
{
    struct nvme_smart_log *log = (struct nvme_smart_log *)nvmeadm_read_log(
        (CONST_STRPTR)"SMART log", NVME_NSID_ALL, NVME_LOG_SMART,
        0, 0, sizeof(*log));
    if (!log)
        return RETURN_FAIL;

    Printf((CONST_STRPTR)"SMART / Health Information\n");
    print_critical_warning(log->critical_warning);

    UWORD temp_k = read_le16(log->temperature);
    if (temp_k == 0)
    {
        Printf((CONST_STRPTR)"Temperature:            not reported\n");
    }
    else
    {
        Printf((CONST_STRPTR)"Temperature:            ");
        print_temperature_celsius(temp_k);
    }
    Printf((CONST_STRPTR)"Available Spare:        %lu %%\n",
           (ULONG)log->avail_spare);
    Printf((CONST_STRPTR)"Spare Threshold:        %lu %%\n",
           (ULONG)log->spare_thresh);
    print_percentage_used(log->percent_used);
    print_endurance_group_warning(log->endu_grp_crit_warn_sumry);
    print_smart_u32_value((CONST_STRPTR)"Warning Temp Time:",
                          read_le32((const UBYTE *)&log->warning_temp_time),
                          (CONST_STRPTR)"minutes",
                          (CONST_STRPTR)"0 (none or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Critical Comp Time:",
                          read_le32((const UBYTE *)&log->critical_comp_time),
                          (CONST_STRPTR)"minutes",
                          (CONST_STRPTR)"0 (none or unsupported)");
    print_smart_data_amount((CONST_STRPTR)"Data Units Read:",
                            log->data_units_read);
    print_smart_data_amount((CONST_STRPTR)"Data Units Written:",
                            log->data_units_written);
    print_smart_counter((CONST_STRPTR)"Host Read Commands:",
                        log->host_reads, NULL);
    print_smart_counter((CONST_STRPTR)"Host Write Commands:",
                        log->host_writes, NULL);
    print_smart_counter((CONST_STRPTR)"Controller Busy Time:",
                        log->ctrl_busy_time,
                        (CONST_STRPTR)"minutes");
    print_smart_counter((CONST_STRPTR)"Power Cycles:",
                        log->power_cycles, NULL);
    print_smart_counter((CONST_STRPTR)"Power On Hours:",
                        log->power_on_hours,
                        (CONST_STRPTR)"hours");
    print_smart_counter((CONST_STRPTR)"Unsafe Shutdowns:",
                        log->unsafe_shutdowns, NULL);
    print_smart_counter((CONST_STRPTR)"Media/Data Errors:",
                        log->media_errors, NULL);
    print_smart_counter((CONST_STRPTR)"Error Log Entries:",
                        log->num_err_log_entries, NULL);
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp1 Count:",
                          read_le32((const UBYTE *)&log->thm_temp1_trans_count),
                          NULL,
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp2 Count:",
                          read_le32((const UBYTE *)&log->thm_temp2_trans_count),
                          NULL,
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp1 Time:",
                          read_le32((const UBYTE *)&log->thm_temp1_total_time),
                          (CONST_STRPTR)"seconds",
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp2 Time:",
                          read_le32((const UBYTE *)&log->thm_temp2_total_time),
                          (CONST_STRPTR)"seconds",
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_temperature_sensor_readings(log->temp_sensor, 8UL);

    FreeMem(log, sizeof(*log));
    return RETURN_OK;
}
