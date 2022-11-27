/*
 * Battery and Power Management code for the Sharp SL-C7xx and SL-Cxx00
 * series of PDAs
 *
 * Copyright (c) 2004-2005 Richard Purdie
 *
 * Based on code written by Sharp for 2.4 kernels
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#undef DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/apm-emulation.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/suspend.h>
#include <linux/gpio.h>
#include <linux/io.h>

#include <asm/mach-types.h>
#include <mach/pm.h>
#include <mach/pxa2xx-regs.h>
#include <mach/regs-rtc.h>
#include <mach/sharpsl_pm.h>

/*
 * Constants
 */
#define SHARPSL_CHARGE_ON_TIME_INTERVAL        (msecs_to_jiffies(1*60*1000))  /* 1 min */
#define SHARPSL_CHARGE_FINISH_TIME             (msecs_to_jiffies(10*60*1000)) /* 10 min */
#define SHARPSL_BATCHK_TIME                    (msecs_to_jiffies(15*1000))    /* 15 sec */
#define SHARPSL_BATCHK_TIME_SUSPEND            (60*10)                        /* 10 min */

#define SHARPSL_WAIT_CO_TIME                   15  /* 15 sec */
#define SHARPSL_WAIT_DISCHARGE_ON              100 /* 100 msec */
#define SHARPSL_CHECK_BATTERY_WAIT_TIME_TEMP   10  /* 10 msec */
#define SHARPSL_CHECK_BATTERY_WAIT_TIME_VOLT   10  /* 10 msec */
#define SHARPSL_CHECK_BATTERY_WAIT_TIME_ACIN   10  /* 10 msec */
#define SHARPSL_CHARGE_WAIT_TIME               15  /* 15 msec */
#define SHARPSL_CHARGE_CO_CHECK_TIME           5   /* 5 msec */
#define SHARPSL_CHARGE_RETRY_CNT               1   /* eqv. 10 min */

#ifdef CONFIG_PM
static int sharpsl_off_charge_battery(void);
static int sharpsl_check_battery_voltage(void);
#endif
static int sharpsl_check_battery_temp(void);
static int sharpsl_ac_check(void);
static int sharpsl_average_value(int ad);
static void sharpsl_average_clear(void);
static void sharpsl_battery_thread(struct work_struct *private_);

struct sharpsl_pm_status sharpsl_pm;
static DECLARE_DELAYED_WORK(sharpsl_bat, sharpsl_battery_thread);
DEFINE_LED_TRIGGER(sharpsl_charge_led_trigger);

struct battery_thresh sharpsl_battery_levels_acin[] = {
    { 213, 100},
    { 212,  98},
    { 211,  95},
    { 210,  93},
    { 209,  90},
    { 208,  88},
    { 207,  85},
    { 206,  83},
    { 205,  80},
    { 204,  78},
    { 203,  75},
    { 202,  73},
    { 201,  70},
    { 200,  68},
    { 199,  65},
    { 198,  63},
    { 197,  60},
    { 196,  58},
    { 195,  55},
    { 194,  53},
    { 193,  50},
    { 192,  48},
    { 192,  45},
    { 191,  43},
    { 191,  40},
    { 190,  38},
    { 190,  35},
    { 189,  33},
    { 188,  30},
    { 187,  28},
    { 186,  25},
    { 185,  23},
    { 184,  20},
    { 183,  18},
    { 182,  15},
    { 181,  13},
    { 180,  10},
    { 179,   8},
    { 178,   5},
    {   0,   0},
};

struct battery_thresh sharpsl_battery_levels_noac[] = {
    { 213, 100},
    { 212,  98},
    { 211,  95},
    { 210,  93},
    { 209,  90},
    { 208,  88},
    { 207,  85},
    { 206,  83},
    { 205,  80},
    { 204,  78},
    { 203,  75},
    { 202,  73},
    { 201,  70},
    { 200,  68},
    { 199,  65},
    { 198,  63},
    { 197,  60},
    { 196,  58},
    { 195,  55},
    { 194,  53},
    { 193,  50},
    { 192,  48},
    { 191,  45},
    { 190,  43},
    { 189,  40},
    { 188,  38},
    { 187,  35},
    { 186,  33},
    { 185,  30},
    { 184,  28},
    { 183,  25},
    { 182,  23},
    { 181,  20},
    { 180,  18},
    { 179,  15},
    { 178,  13},
    { 177,  10},
    { 176,   8},
    { 175,   5},
    {   0,   0},
};

/* MAX1111 Commands */
#define MAXCTRL_PD0      (1u << 0)
#define MAXCTRL_PD1      (1u << 1)
#define MAXCTRL_SGL      (1u << 2)
#define MAXCTRL_UNI      (1u << 3)
#define MAXCTRL_SEL_SH   4
#define MAXCTRL_STR      (1u << 7)

extern int max1111_read_channel(int);

int sharpsl_pm_pxa_read_max1111(int channel)
{
    /* Ugly, better move this function into another module */
    if (machine_is_tosa()) {
        return 0;
    }

    /* max1111 accepts channels from 0-3, however,
     * it is encoded from 0-7 here in the code.
     */
    return max1111_read_channel(channel >> 1);
}

static int get_percentage(int voltage)
{
    int i = sharpsl_pm.machinfo->bat_levels - 1;
    struct battery_thresh *thresh = sharpsl_pm.machinfo->bat_levels_noac;

    while (i > 0 && (voltage > thresh[i].voltage)) {
        i--;
    }

    return thresh[i].percentage;
}

static int get_apm_status(int voltage)
{
    int low_thresh, high_thresh;

    if (sharpsl_pm.charge_mode == CHRG_ON) {
        high_thresh = sharpsl_pm.machinfo->status_high_acin;
        low_thresh = sharpsl_pm.machinfo->status_low_acin;
    } else {
        high_thresh = sharpsl_pm.machinfo->status_high_noac;
        low_thresh = sharpsl_pm.machinfo->status_low_noac;
    }

    if (voltage >= high_thresh)
        return APM_BATTERY_STATUS_HIGH;
    if (voltage >= low_thresh)
        return APM_BATTERY_STATUS_LOW;
    return APM_BATTERY_STATUS_CRITICAL;
}

void sharpsl_battery_kick(void)
{
    schedule_delayed_work(&sharpsl_bat, msecs_to_jiffies(125));
}
EXPORT_SYMBOL(sharpsl_battery_kick);

static void sharpsl_charge_on(void)
{
    dev_dbg(sharpsl_pm.dev, "Turning Charger On\n");

    sharpsl_pm.full_count = 0;
    sharpsl_pm.machinfo->charge(1);
    sharpsl_pm_led(SHARPSL_LED_ON);
    sharpsl_pm.charge_mode = CHRG_ON;
}

static void sharpsl_charge_off(void)
{
    dev_dbg(sharpsl_pm.dev, "Turning Charger Off\n");

    sharpsl_pm.machinfo->charge(0);
    sharpsl_pm_led(SHARPSL_LED_OFF);
    sharpsl_pm.charge_mode = CHRG_OFF;
}

static void sharpsl_battery_thread(struct work_struct *private_)
{
    int voltage, percent, apm_status, i, ac_in, ac_vol;

    if (!sharpsl_pm.machinfo)
        return;

    ac_vol = sharpsl_pm.machinfo->read_devdata(SHARPSL_ACIN_VOLT);
    ac_in = sharpsl_pm.battstat.ac_status = ac_vol > 32 ? APM_AC_ONLINE : APM_AC_OFFLINE;

    for (i = 0; i < 5; i++) {
        voltage = sharpsl_pm.machinfo->read_devdata(SHARPSL_BATT_VOLT);
        if (voltage > 0)
            break;
    }
    if (voltage <= 0) {
        voltage = sharpsl_pm.machinfo->bat_levels_noac[0].voltage;
        dev_warn(sharpsl_pm.dev, "Warning: Cannot read main battery!\n");
    }

    voltage = sharpsl_average_value(voltage);
    apm_status = get_apm_status(voltage);
    percent = get_percentage(voltage);
    
    if (ac_in == APM_AC_ONLINE) {
        if ((sharpsl_pm.charge_mode == CHRG_ON) && (voltage >= 215)) {
            sharpsl_charge_off();
        }
        else if ((sharpsl_pm.charge_mode != CHRG_ON) && (voltage <= 210)) {
            sharpsl_charge_on();
        }
    }
    else if (ac_in == APM_AC_OFFLINE) {
        if (sharpsl_pm.charge_mode == CHRG_ON) {
            sharpsl_charge_off();
        }
        else {
            sharpsl_pm.charge_mode = CHRG_OFF;
        }
    }

    /* At low battery voltages, the voltage has a tendency to start
       creeping back up so we try to avoid this here */
    if ((sharpsl_pm.battstat.ac_status == APM_AC_ONLINE)
        || (apm_status == APM_BATTERY_STATUS_HIGH)
        || percent <= sharpsl_pm.battstat.mainbat_percent)
    {
        sharpsl_pm.battstat.mainbat_voltage = voltage;
        sharpsl_pm.battstat.mainbat_status = apm_status;
        sharpsl_pm.battstat.mainbat_percent = percent;
    }

    dev_dbg(sharpsl_pm.dev, "bat_vol: %d, bat_per: %d%%, ac_in: %d, charge_mode: %d\n", voltage,
        sharpsl_pm.battstat.mainbat_percent, ac_in, sharpsl_pm.charge_mode);

    /* Suspend if critical battery level */
    if ((sharpsl_pm.battstat.ac_status != APM_AC_ONLINE)
         && (sharpsl_pm.battstat.mainbat_status == APM_BATTERY_STATUS_CRITICAL)
         && !(sharpsl_pm.flags & SHARPSL_APM_QUEUED))
    {
        sharpsl_pm.flags |= SHARPSL_APM_QUEUED;
        dev_err(sharpsl_pm.dev, "Fatal Off\n");
        apm_queue_event(APM_CRITICAL_SUSPEND);
    }
    schedule_delayed_work(&sharpsl_bat, SHARPSL_BATCHK_TIME);
}

void sharpsl_pm_led(int val)
{
    if (val == SHARPSL_LED_ERROR) {
        dev_err(sharpsl_pm.dev, "Charging Error!\n");
    } else if (val == SHARPSL_LED_ON) {
        dev_dbg(sharpsl_pm.dev, "Charge LED On\n");
        led_trigger_event(sharpsl_charge_led_trigger, LED_FULL);
    } else {
        dev_dbg(sharpsl_pm.dev, "Charge LED Off\n");
        led_trigger_event(sharpsl_charge_led_trigger, LED_OFF);
    }
}

/*
 * Maintain an average of the last 10 readings
 */
#define SHARPSL_CNV_VALUE_NUM    10
static int sharpsl_ad_index;

static void sharpsl_average_clear(void)
{
    sharpsl_ad_index = 0;
}

static int sharpsl_average_value(int ad)
{
    int i, ad_val = 0;
    static int sharpsl_ad[SHARPSL_CNV_VALUE_NUM+1];

    if (sharpsl_pm.battstat.mainbat_status != APM_BATTERY_STATUS_HIGH) {
        sharpsl_ad_index = 0;
        return ad;
    }

    sharpsl_ad[sharpsl_ad_index] = ad;
    sharpsl_ad_index++;
    if (sharpsl_ad_index >= SHARPSL_CNV_VALUE_NUM) {
        for (i = 0; i < (SHARPSL_CNV_VALUE_NUM-1); i++)
            sharpsl_ad[i] = sharpsl_ad[i+1];
        sharpsl_ad_index = SHARPSL_CNV_VALUE_NUM - 1;
    }
    for (i = 0; i < sharpsl_ad_index; i++)
        ad_val += sharpsl_ad[i];

    return ad_val / sharpsl_ad_index;
}

/*
 * Take an array of 5 integers, remove the maximum and minimum values
 * and return the average.
 */
static int get_select_val(int *val)
{
    int i, j, k, temp, sum = 0;

    /* Find MAX val */
    temp = val[0];
    j = 0;
    for (i = 1; i < 5; i++) {
        if (temp < val[i]) {
            temp = val[i];
            j = i;
        }
    }

    /* Find MIN val */
    temp = val[4];
    k = 4;
    for (i = 3; i >= 0; i--) {
        if (temp > val[i]) {
            temp = val[i];
            k = i;
        }
    }

    for (i = 0; i < 5; i++)
        if (i != j && i != k)
            sum += val[i];

    dev_dbg(sharpsl_pm.dev, "Average: %d from values: %d, %d, %d, %d, %d\n", sum/3, val[0], val[1], val[2], val[3], val[4]);

    return sum/3;
}

static int sharpsl_check_battery_temp(void)
{
    int val, i, buff[5];

    /* Check battery temperature */
    for (i = 0; i < 5; i++) {
        mdelay(SHARPSL_CHECK_BATTERY_WAIT_TIME_TEMP);
        sharpsl_pm.machinfo->measure_temp(1);
        mdelay(SHARPSL_CHECK_BATTERY_WAIT_TIME_TEMP);
        buff[i] = sharpsl_pm.machinfo->read_devdata(SHARPSL_BATT_TEMP);
        sharpsl_pm.machinfo->measure_temp(0);
    }

    val = get_select_val(buff);

    dev_dbg(sharpsl_pm.dev, "Temperature: %d\n", val);
    if (val > sharpsl_pm.machinfo->charge_on_temp) {
        printk(KERN_WARNING "Not charging: temperature out of limits.\n");
        return -1;
    }

    return 0;
}

#ifdef CONFIG_PM
static int sharpsl_check_battery_voltage(void)
{
    int val, i, buff[5];

    /* disable charge, enable discharge */
    sharpsl_pm.machinfo->charge(0);
    sharpsl_pm.machinfo->discharge(1);
    mdelay(SHARPSL_WAIT_DISCHARGE_ON);

    if (sharpsl_pm.machinfo->discharge1)
        sharpsl_pm.machinfo->discharge1(1);

    /* Check battery voltage */
    for (i = 0; i < 5; i++) {
        buff[i] = sharpsl_pm.machinfo->read_devdata(SHARPSL_BATT_VOLT);
        mdelay(SHARPSL_CHECK_BATTERY_WAIT_TIME_VOLT);
    }

    if (sharpsl_pm.machinfo->discharge1)
        sharpsl_pm.machinfo->discharge1(0);

    sharpsl_pm.machinfo->discharge(0);

    val = get_select_val(buff);
    dev_dbg(sharpsl_pm.dev, "Battery Voltage: %d\n", val);

    if (val < sharpsl_pm.machinfo->charge_on_volt)
        return -1;

    return 0;
}
#endif

static int sharpsl_ac_check(void)
{
    int temp, i, buff[5];

    for (i = 0; i < 5; i++) {
        buff[i] = sharpsl_pm.machinfo->read_devdata(SHARPSL_ACIN_VOLT);
        mdelay(SHARPSL_CHECK_BATTERY_WAIT_TIME_ACIN);
    }

    temp = get_select_val(buff);
    dev_dbg(sharpsl_pm.dev, "AC Voltage: %d\n", temp);

    if ((temp > sharpsl_pm.machinfo->charge_acin_high) || (temp < sharpsl_pm.machinfo->charge_acin_low)) {
        dev_err(sharpsl_pm.dev, "Error: AC check failed: voltage %d.\n", temp);
        return -1;
    }

    return 0;
}

#ifdef CONFIG_PM
static int sharpsl_pm_suspend(struct platform_device *pdev, pm_message_t state)
{
    sharpsl_pm.flags |= SHARPSL_SUSPENDED;
    flush_delayed_work(&sharpsl_bat);
    sharpsl_charge_off();
    return 0;
}

static int sharpsl_pm_resume(struct platform_device *pdev)
{
    /* Clear the reset source indicators as they break the bootloader upon reboot */
    RCSR = 0x0f;
    sharpsl_average_clear();
    sharpsl_pm.flags &= ~SHARPSL_APM_QUEUED;
    sharpsl_pm.flags &= ~SHARPSL_SUSPENDED;
    return 0;
}

static void corgi_goto_sleep(unsigned long alarm_time, unsigned int alarm_enable, suspend_state_t state)
{
    dev_dbg(sharpsl_pm.dev, "Time is: %08x\n", RCNR);

    dev_dbg(sharpsl_pm.dev, "Offline Charge Activate = %d\n", sharpsl_pm.flags & SHARPSL_DO_OFFLINE_CHRG);
    /* not charging and AC-IN! */

    if ((sharpsl_pm.flags & SHARPSL_DO_OFFLINE_CHRG) && (sharpsl_pm.machinfo->read_devdata(SHARPSL_ACIN_VOLT) > 32)) {
        dev_dbg(sharpsl_pm.dev, "Activating Offline Charger...\n");
        sharpsl_pm.charge_mode = CHRG_OFF;
        sharpsl_pm.flags &= ~SHARPSL_DO_OFFLINE_CHRG;
        sharpsl_off_charge_battery();
    }

    sharpsl_pm.machinfo->presuspend();

    PEDR = 0xffffffff; /* clear it */

    sharpsl_pm.flags &= ~SHARPSL_ALARM_ACTIVE;
    if ((sharpsl_pm.charge_mode == CHRG_ON) && ((alarm_enable && ((alarm_time - RCNR) > (SHARPSL_BATCHK_TIME_SUSPEND + 30))) || !alarm_enable)) {
        RTSR &= RTSR_ALE;
        RTAR = RCNR + SHARPSL_BATCHK_TIME_SUSPEND;
        dev_dbg(sharpsl_pm.dev, "Charging alarm at: %08x\n", RTAR);
        sharpsl_pm.flags |= SHARPSL_ALARM_ACTIVE;
    } else if (alarm_enable) {
        RTSR &= RTSR_ALE;
        RTAR = alarm_time;
        dev_dbg(sharpsl_pm.dev, "User alarm at: %08x\n", RTAR);
    } else {
        dev_dbg(sharpsl_pm.dev, "No alarms set.\n");
    }

    pxa_pm_enter(state);

    sharpsl_pm.machinfo->postsuspend();

    dev_dbg(sharpsl_pm.dev, "Corgi woken up from suspend: %08x\n", PEDR);
}

static int corgi_enter_suspend(unsigned long alarm_time, unsigned int alarm_enable, suspend_state_t state)
{
    if (!sharpsl_pm.machinfo->should_wakeup(!(sharpsl_pm.flags & SHARPSL_ALARM_ACTIVE) && alarm_enable)) {
        if (!(sharpsl_pm.flags & SHARPSL_ALARM_ACTIVE)) {
            dev_dbg(sharpsl_pm.dev, "No user triggered wakeup events and not charging. Strange. Suspend.\n");
            corgi_goto_sleep(alarm_time, alarm_enable, state);
            return 1;
        }
        if (sharpsl_off_charge_battery()) {
            dev_dbg(sharpsl_pm.dev, "Charging. Suspend...\n");
            corgi_goto_sleep(alarm_time, alarm_enable, state);
            return 1;
        }
        dev_dbg(sharpsl_pm.dev, "User triggered wakeup in offline charger.\n");
    }

    if ((!sharpsl_pm.machinfo->read_devdata(SHARPSL_STATUS_LOCK)) ||
        (!sharpsl_pm.machinfo->read_devdata(SHARPSL_STATUS_FATAL)))    {
        dev_err(sharpsl_pm.dev, "Fatal condition. Suspend.\n");
        corgi_goto_sleep(alarm_time, alarm_enable, state);
        return 1;
    }

    return 0;
}

static int corgi_pxa_pm_enter(suspend_state_t state)
{
    unsigned long alarm_time = RTAR;
    unsigned int alarm_status = ((RTSR & RTSR_ALE) != 0);

    dev_dbg(sharpsl_pm.dev, "SharpSL suspending for first time.\n");

    corgi_goto_sleep(alarm_time, alarm_status, state);

    while (corgi_enter_suspend(alarm_time, alarm_status, state))
        {}

    if (sharpsl_pm.machinfo->earlyresume)
        sharpsl_pm.machinfo->earlyresume();

    dev_dbg(sharpsl_pm.dev, "SharpSL resuming...\n");

    return 0;
}

static int sharpsl_off_charge_error(void)
{
    dev_err(sharpsl_pm.dev, "Offline Charger: Error occurred.\n");
    sharpsl_pm.machinfo->charge(0);
    sharpsl_pm_led(SHARPSL_LED_ERROR);
    sharpsl_pm.charge_mode = CHRG_ERROR;
    return 1;
}

/*
 * Charging Control while suspended
 * Return 1 - go straight to sleep
 * Return 0 - sleep or wakeup depending on other factors
 */
static int sharpsl_off_charge_battery(void)
{
    int time;

    dev_dbg(sharpsl_pm.dev, "Charge Mode: %d\n", sharpsl_pm.charge_mode);

    if (sharpsl_pm.charge_mode == CHRG_OFF) {
        dev_dbg(sharpsl_pm.dev, "Offline Charger: Step 1\n");

        /* AC Check */
        if ((sharpsl_ac_check() < 0) || (sharpsl_check_battery_temp() < 0))
            return sharpsl_off_charge_error();

        /* Start Charging */
        sharpsl_pm_led(SHARPSL_LED_ON);
        sharpsl_pm.machinfo->charge(0);
        mdelay(SHARPSL_CHARGE_WAIT_TIME);
        sharpsl_pm.machinfo->charge(1);

        sharpsl_pm.charge_mode = CHRG_ON;
        sharpsl_pm.full_count = 0;

        return 1;
    } else if (sharpsl_pm.charge_mode != CHRG_ON) {
        return 1;
    }

    if (sharpsl_pm.full_count == 0) {
        int time;

        dev_dbg(sharpsl_pm.dev, "Offline Charger: Step 2\n");

        if ((sharpsl_check_battery_temp() < 0) || (sharpsl_check_battery_voltage() < 0))
            return sharpsl_off_charge_error();

        sharpsl_pm.machinfo->charge(0);
        mdelay(SHARPSL_CHARGE_WAIT_TIME);
        sharpsl_pm.machinfo->charge(1);
        sharpsl_pm.charge_mode = CHRG_ON;

        mdelay(SHARPSL_CHARGE_CO_CHECK_TIME);

        time = RCNR;
        while (1) {
            /* Check if any wakeup event had occurred */
            if (sharpsl_pm.machinfo->charger_wakeup() != 0)
                return 0;
            /* Check for timeout */
            if ((RCNR - time) > SHARPSL_WAIT_CO_TIME)
                return 1;
            if (sharpsl_pm.machinfo->read_devdata(SHARPSL_STATUS_CHRGFULL)) {
                dev_dbg(sharpsl_pm.dev, "Offline Charger: Charge full occurred. Retrying to check\n");
                sharpsl_pm.full_count++;
                sharpsl_pm.machinfo->charge(0);
                mdelay(SHARPSL_CHARGE_WAIT_TIME);
                sharpsl_pm.machinfo->charge(1);
                return 1;
            }
        }
    }

    dev_dbg(sharpsl_pm.dev, "Offline Charger: Step 3\n");

    mdelay(SHARPSL_CHARGE_CO_CHECK_TIME);

    time = RCNR;
    while (1) {
        /* Check if any wakeup event had occurred */
        if (sharpsl_pm.machinfo->charger_wakeup())
            return 0;
        /* Check for timeout */
        if ((RCNR-time) > SHARPSL_WAIT_CO_TIME) {
            if (sharpsl_pm.full_count > SHARPSL_CHARGE_RETRY_CNT) {
                dev_dbg(sharpsl_pm.dev, "Offline Charger: Not charged sufficiently. Retrying.\n");
                sharpsl_pm.full_count = 0;
            }
            sharpsl_pm.full_count++;
            return 1;
        }
        if (sharpsl_pm.machinfo->read_devdata(SHARPSL_STATUS_CHRGFULL)) {
            dev_dbg(sharpsl_pm.dev, "Offline Charger: Charging complete.\n");
            sharpsl_pm_led(SHARPSL_LED_OFF);
            sharpsl_pm.machinfo->charge(0);
            sharpsl_pm.charge_mode = CHRG_DONE;
            return 1;
        }
    }
}
#else
#define sharpsl_pm_suspend NULL
#define sharpsl_pm_resume  NULL
#endif

static ssize_t charge_show(struct device *dev, struct device_attribute *attr, char *buf)
{
  return sprintf(buf, "%d\n", sharpsl_pm.charge_mode);
}

static ssize_t charge_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
  int rc=-1;
  unsigned long v=0;

  rc = kstrtoul(buf, 0, &v);
  if (rc){
    return rc;
  }

  if(v){
    sharpsl_charge_on();
  }
  else{
    sharpsl_charge_off();
  }
  printk("%ld\n", v);
  return count;
}
static DEVICE_ATTR_RW(charge);

static ssize_t battery_percentage_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", sharpsl_pm.battstat.mainbat_percent);
}

static ssize_t battery_voltage_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", sharpsl_pm.battstat.mainbat_voltage);
}

static DEVICE_ATTR(battery_percentage, 0444, battery_percentage_show, NULL);
static DEVICE_ATTR(battery_voltage, 0444, battery_voltage_show, NULL);

extern void (*apm_get_power_status)(struct apm_power_info *);

static void sharpsl_apm_get_power_status(struct apm_power_info *info)
{
    info->ac_line_status = sharpsl_pm.battstat.ac_status;

    if (sharpsl_pm.charge_mode == CHRG_ON) {
        info->battery_status = APM_BATTERY_STATUS_CHARGING;
    }
    else {
        info->battery_status = sharpsl_pm.battstat.mainbat_status;
    }

    info->battery_flag = (1 << info->battery_status);
    info->battery_life = sharpsl_pm.battstat.mainbat_percent;
}

#ifdef CONFIG_PM
static const struct platform_suspend_ops sharpsl_pm_ops = {
    .prepare = pxa_pm_prepare,
    .finish  = pxa_pm_finish,
    .enter   = corgi_pxa_pm_enter,
    .valid   = suspend_valid_only_mem,
};
#endif

static int sharpsl_pm_probe(struct platform_device *pdev)
{
    int ret = 0;

    if (!pdev->dev.platform_data) {
        return -EINVAL;
    }

    sharpsl_pm.dev = &pdev->dev;
    sharpsl_pm.machinfo = pdev->dev.platform_data;
    sharpsl_pm.charge_mode = CHRG_OFF;
    sharpsl_pm.flags = 0;
    led_trigger_register_simple("sharpsl-charge", &sharpsl_charge_led_trigger);
    sharpsl_pm.machinfo->init();
    gpio_request(sharpsl_pm.machinfo->gpio_acin, "AC IN");
    gpio_direction_input(sharpsl_pm.machinfo->gpio_acin);
    gpio_request(sharpsl_pm.machinfo->gpio_batfull, "Battery Full");
    gpio_direction_input(sharpsl_pm.machinfo->gpio_batfull);
    gpio_request(sharpsl_pm.machinfo->gpio_batlock, "Battery Lock");
    gpio_direction_input(sharpsl_pm.machinfo->gpio_batlock);

    ret = device_create_file(&pdev->dev, &dev_attr_battery_percentage);
    ret |= device_create_file(&pdev->dev, &dev_attr_battery_voltage);
    ret |= device_create_file(&pdev->dev, &dev_attr_charge);
    if (ret != 0) {
        dev_warn(&pdev->dev, "Failed to register attributes (%d)\n", ret);
    }
    apm_get_power_status = sharpsl_apm_get_power_status;

#ifdef CONFIG_PM
    suspend_set_ops(&sharpsl_pm_ops);
#endif

    return 0;
}

static int sharpsl_pm_remove(struct platform_device *pdev)
{
    suspend_set_ops(NULL);
    device_remove_file(&pdev->dev, &dev_attr_battery_percentage);
    device_remove_file(&pdev->dev, &dev_attr_battery_voltage);
    device_remove_file(&pdev->dev, &dev_attr_charge);
    led_trigger_unregister_simple(sharpsl_charge_led_trigger);
    gpio_free(sharpsl_pm.machinfo->gpio_batlock);
    gpio_free(sharpsl_pm.machinfo->gpio_batfull);
    gpio_free(sharpsl_pm.machinfo->gpio_acin);

    if (sharpsl_pm.machinfo->exit) {
        sharpsl_pm.machinfo->exit();
    }
    return 0;
}

static struct platform_driver sharpsl_pm_driver = {
    .probe    = sharpsl_pm_probe,
    .remove   = sharpsl_pm_remove,
    .suspend  = sharpsl_pm_suspend,
    .resume   = sharpsl_pm_resume,
    .driver   = { .name = "sharpsl-pm" },
};

static int sharpsl_pm_init(void)
{
    return platform_driver_register(&sharpsl_pm_driver);
}

static void sharpsl_pm_exit(void)
{
    platform_driver_unregister(&sharpsl_pm_driver);
}

late_initcall(sharpsl_pm_init);
module_exit(sharpsl_pm_exit);
