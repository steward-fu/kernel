/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option)any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/backlight.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <sound/dmaengine_pcm.h>
#include <linux/gpio.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/gpio.h>
#include <asm/arch-suniv/dma.h>
#include <asm/arch-suniv/cpu.h>
#include <asm/arch-suniv/gpio.h>
#include <asm/arch-suniv/clock.h>
#include <asm/arch-suniv/codec.h>
#include <asm/arch-suniv/common.h>

#define DEBUG_LOG 0

extern int suniv_variant;

static int l2r2 = 0;
static int lock = 0;
static int myperiod = 30;
static struct input_dev *mydev;
static struct timer_list mytimer;

static uint32_t R_UP     = 0x0001;
static uint32_t R_DOWN   = 0x0002;
static uint32_t R_LEFT   = 0x0004;
static uint32_t R_RIGHT  = 0x0008;
static uint32_t R_A      = 0x0010;
static uint32_t R_B      = 0x0020;
static uint32_t R_X      = 0x0040;
static uint32_t R_Y      = 0x0080;
static uint32_t R_SELECT = 0x0100;
static uint32_t R_START  = 0x0200;
static uint32_t R_L1     = 0x0400;
static uint32_t R_R1     = 0x0800;
static uint32_t R_MENU   = 0x1000;
static uint32_t R_L2     = 0x2000;
static uint32_t R_R2     = 0x4000;

static uint32_t I_UP     = 0;
static uint32_t I_DOWN   = 0;
static uint32_t I_LEFT   = 0;
static uint32_t I_RIGHT  = 0;
static uint32_t I_A      = 0;
static uint32_t I_B      = 0;
static uint32_t I_X      = 0;
static uint32_t I_Y      = 0;
static uint32_t I_SELECT = 0;
static uint32_t I_START  = 0;
static uint32_t I_L1     = 0;
static uint32_t I_R1     = 0;
static uint32_t I_MENU   = 0;
static uint32_t I_L2     = 0;
static uint32_t I_R2     = 0;

static int do_input_request(uint32_t pin, const char *name)
{
    if(gpio_request(pin, name) < 0) {
        printk("failed to request gpio: %s\n", name);
        return -1;
    }
    gpio_direction_input(pin);
    return 0;
}

static int do_output_request(uint32_t pin, const char *name)
{
    if(gpio_request(pin, name) < 0) {
        printk("failed to request gpio: %s\n", name);
        return -1;
    }
    gpio_direction_output(pin, 1);
    return 0;
}

static ssize_t lock_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", lock);
}

static ssize_t lock_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int rc = -1;
    unsigned long v = 0;

    rc = kstrtoul(buf, 0, &v);
    if(rc) {
        return rc;
    }
    lock = v;
    return count;
}
static DEVICE_ATTR_RW(lock);

static ssize_t l2r2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", l2r2);
}

static ssize_t l2r2_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int rc = -1;
    static int cfg = 0;
    unsigned long v = 0;

    rc = kstrtoul(buf, 0, &v);
    if(rc) {
        return rc;
    }

    l2r2 = v;
    if((l2r2 > 0) && (cfg == 0)) {
        cfg = 1;
        do_input_request(I_L2, "l2");
        do_input_request(I_R2, "r2");
    }
    return count;
}
static DEVICE_ATTR_RW(l2r2);

static void print_key(uint32_t val, uint8_t is_pressed)
{
#if DEBUG_LOG
    uint32_t i = 0;
    uint32_t map_val[] = {R_UP, R_DOWN, R_LEFT, R_RIGHT, R_A, R_B, R_X, R_Y, R_SELECT, R_START, R_L1, R_R1, R_MENU, R_L2, R_R2, -1};
    char *map_key[] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "SELECT", "START", "L1", "R1", "MENU", "L2", "R2", ""};

    for(i = 0; map_val[i] != -1; i++) {
        if(map_val[i] == val) {
            printk("%s: %s\n", map_key[i], is_pressed ? "DOWN" : "UP");
            break;
        }
    }
#endif
}

static void report_key(uint32_t btn, uint32_t mask, uint8_t key)
{
    static uint32_t btn_pressed = 0;
    static uint32_t btn_released = 0xffff;

    if(btn & mask) {
        btn_released &= ~mask;
        if((btn_pressed & mask) == 0) {
            btn_pressed |= mask;
            input_report_key(mydev, key, 1);
            print_key(mask, 1);
        }
    }
    else {
        btn_pressed &= ~mask;
        if((btn_released & mask) == 0) {
            btn_released |= mask;
            input_report_key(mydev, key, 0);
            print_key(mask, 0);
        }
    }
}

static void pocketgo_handler(unsigned long unused)
{
    uint32_t val = 0;
    static uint32_t pre = 0;

    if(lock) {
        val = 0;
        if(gpio_get_value(I_MENU) == 0) {
            val |= R_MENU;
        }
    }
    else {
        if(gpio_get_value(I_UP) == 0) {
            val |= R_UP;
        }
        if(gpio_get_value(I_DOWN) == 0) {
            val |= R_DOWN;
        }
        if(gpio_get_value(I_LEFT) == 0) {
            val |= R_LEFT;
        }
        if(gpio_get_value(I_RIGHT) == 0) {
            val |= R_RIGHT;
        }
        if(gpio_get_value(I_A) == 0) {
            val |= R_A;
        }
        if(gpio_get_value(I_B) == 0) {
            val |= R_B;
        }
        if(gpio_get_value(I_X) == 0) {
            val |= R_X;
        }
        if(gpio_get_value(I_Y) == 0) {
            val |= R_Y;
        }
        if(gpio_get_value(I_L1) == 0) {
            val |= R_L1;
        }
        if(gpio_get_value(I_R1) == 0) {
            val |= R_R1;
        }
        if(gpio_get_value(I_SELECT) == 0) {
            val |= R_SELECT;
        }
        if(gpio_get_value(I_START) == 0) {
            val |= R_START;
        }
        if(gpio_get_value(I_MENU) == 0) {
            val |= R_MENU;
        }

        if(l2r2) {
            if(gpio_get_value(I_L2) == 0) {
                val |= R_L2;
            }
            if(gpio_get_value(I_R2) == 0) {
                val |= R_R2;
            }
        }
    }

    if(pre != val) {
        pre = val;
        report_key(pre, R_UP,     KEY_UP);
        report_key(pre, R_DOWN,   KEY_DOWN);
        report_key(pre, R_LEFT,   KEY_LEFT);
        report_key(pre, R_RIGHT,  KEY_RIGHT);
        report_key(pre, R_A,      KEY_LEFTCTRL);
        report_key(pre, R_B,      KEY_LEFTALT);
        report_key(pre, R_X,      KEY_SPACE);
        report_key(pre, R_Y,      KEY_LEFTSHIFT);
        report_key(pre, R_L1,     KEY_TAB);
        report_key(pre, R_R1,     KEY_BACKSPACE);
        report_key(pre, R_SELECT, KEY_ESC);
        report_key(pre, R_START,  KEY_ENTER);
        report_key(pre, R_MENU,   KEY_RIGHTCTRL);
        report_key(pre, R_L2,     KEY_PAGEUP);
        report_key(pre, R_R2,     KEY_PAGEDOWN);
        input_sync(mydev);
    }
    mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
}

static void fc3000_handler(unsigned long unused)
{
    uint32_t val = 0, l = 0, r = 0;
    static uint32_t pre = 0, lr = 0, pre_l = 0, pre_r = 0, pre_select = 0, pre_start = 0;

    if(lock) {
        val = 0;
        l = gpio_get_value(I_L1);
        r = gpio_get_value(I_R1);
        if((l == 0) && (r == 0)) {
            val |= R_MENU;
        }
    }
    else {
        if(gpio_get_value(I_UP) == 0) {
            val |= R_UP;
        }
        if(gpio_get_value(I_DOWN) == 0) {
            val |= R_DOWN;
        }
        if(gpio_get_value(I_LEFT) == 0) {
            val |= R_LEFT;
        }
        if(gpio_get_value(I_RIGHT) == 0) {
            val |= R_RIGHT;
        }
        if(gpio_get_value(I_A) == 0) {
            val |= R_A;
        }
        if(gpio_get_value(I_B) == 0) {
            val |= R_B;
        }
        if(gpio_get_value(I_X) == 0) {
            val |= R_X;
        }
        if(gpio_get_value(I_Y) == 0) {
            val |= R_Y;
        }
        if(l2r2) {
            if(gpio_get_value(I_L2) == 0) {
                val |= R_L2;
            }
            if(gpio_get_value(I_R2) == 0) {
                val |= R_R2;
            }
        }

        lr ^= 1;
        if(lr) {
            l = gpio_get_value(I_L1);
            r = gpio_get_value(I_R1);
            if((l == 0) && (r == 0)) {
                val |= R_MENU;
            }
            else {
                pre_select = l;
                pre_start = r;
            }
            gpio_set_value(I_MENU, 1);
        }
        else {
            l = gpio_get_value(I_L1);
            r = gpio_get_value(I_R1);
            if((l == 0) && (r == 0)) {
                val |= R_MENU;
            }
            else {
                pre_l = l;
                pre_r = r;
            }
            //gpio_set_value(I_MENU, 0);
        }
        if(pre_select == 0) {
            val |= R_SELECT;
        }
        if(pre_start == 0) {
            val |= R_START;
        }
        if(pre_l == 0) {
            val |= R_L1;
        }
        if(pre_r == 0) {
            val |= R_R1;
        }
    }

    if(pre != val) {
        pre = val;
        report_key(pre, R_UP,     KEY_UP);
        report_key(pre, R_DOWN,   KEY_DOWN);
        report_key(pre, R_LEFT,   KEY_LEFT);
        report_key(pre, R_RIGHT,  KEY_RIGHT);
        report_key(pre, R_A,      KEY_LEFTCTRL);
        report_key(pre, R_B,      KEY_LEFTALT);
        report_key(pre, R_X,      KEY_SPACE);
        report_key(pre, R_Y,      KEY_LEFTSHIFT);
        report_key(pre, R_L1,     KEY_TAB);
        report_key(pre, R_R1,     KEY_BACKSPACE);
        report_key(pre, R_SELECT, KEY_ESC);
        report_key(pre, R_START,  KEY_ENTER);
        report_key(pre, R_MENU,   KEY_RIGHTCTRL);
        report_key(pre, R_L2,     KEY_PAGEUP);
        report_key(pre, R_R2,     KEY_PAGEDOWN);
        input_sync(mydev);
    }
    mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
}

static void fc3000_WL_28H105_A1_handler(unsigned long unused)
{
    uint32_t val = 0, l = 0, r = 0;
    static uint32_t pre = 0, lr = 0, pre_l = 0, pre_r = 0, pre_select = 0, pre_start = 0;

    //
    // FC3000 IPS V2 (WL-28H105-A1)
    // L1 / R1 - SELECT / START Fixed
    //
    if(lock) {
        val = 0;
        if(gpio_get_value(I_MENU) == 0) {
            val |= R_MENU;
        }
    }
    else {
        if(gpio_get_value(I_UP) == 0) {
            val |= R_UP;
        }
        if(gpio_get_value(I_DOWN) == 0) {
            val |= R_DOWN;
        }
        if(gpio_get_value(I_LEFT) == 0) {
            val |= R_LEFT;
        }
        if(gpio_get_value(I_RIGHT) == 0) {
            val |= R_RIGHT;
        }
        if(gpio_get_value(I_A) == 0) {
            val |= R_A;
        }
        if(gpio_get_value(I_B) == 0) {
            val |= R_B;
        }
        if(gpio_get_value(I_X) == 0) {
            val |= R_X;
        }
        if(gpio_get_value(I_Y) == 0) {
            val |= R_Y;
        }

        if(l2r2) {
            if(gpio_get_value(I_L2) == 0) {
                val |= R_L2;
            }
            if(gpio_get_value(I_R2) == 0) {
                val |= R_R2;
            }
        }

        lr ^= 1;
        if(lr) {
            l = gpio_get_value(I_L1);
            r = gpio_get_value(I_R1);
            if((l == 0) && (r == 0)) {
                val |= R_MENU;
                //val|= R_L1;
                //val|= R_R1;
            }
            else {
                pre_select = l;
                pre_start = r;
            }
            gpio_set_value(I_MENU, 1);
        }
        else {
            l = gpio_get_value(I_L1);
            r = gpio_get_value(I_R1);
            if((l == 0) && (r == 0)) {
                val |= R_MENU;
                //val|= R_L1;
                //val|= R_R1;
            }
            else {
                pre_l = l;
                pre_r = r;
            }
            gpio_set_value(I_MENU, 0);
        }

        if(pre_select == 0) {
            val |= R_SELECT;
        }
        if(pre_start == 0) {
            val |= R_START;
        }

        if(pre_l == 0) {
            val |= R_L1;
        }
        if(pre_r == 0) {
            val |= R_R1;
        }
    }

    if(pre != val) {
        pre = val;
        report_key(pre, R_UP,     KEY_UP);
        report_key(pre, R_DOWN,   KEY_DOWN);
        report_key(pre, R_LEFT,   KEY_LEFT);
        report_key(pre, R_RIGHT,  KEY_RIGHT);
        report_key(pre, R_A,      KEY_LEFTCTRL);
        report_key(pre, R_B,      KEY_LEFTALT);
        report_key(pre, R_X,      KEY_SPACE);
        report_key(pre, R_Y,      KEY_LEFTSHIFT);
        report_key(pre, R_L1,     KEY_TAB);
        report_key(pre, R_R1,     KEY_BACKSPACE);
        report_key(pre, R_SELECT, KEY_ESC);
        report_key(pre, R_START,  KEY_ENTER);
        report_key(pre, R_MENU,   KEY_RIGHTCTRL);
        report_key(pre, R_L2,     KEY_PAGEUP);
        report_key(pre, R_R2,     KEY_PAGEDOWN);
        input_sync(mydev);
    }
    mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
}

static void fc3000_old_handler(unsigned long unused)
{
    uint32_t val = 0, l = 0, r = 0;
    static uint32_t pre = 0, lr = 0, pre_l = 0, pre_r = 0, pre_select = 0, pre_start = 0;

    if(lock) {
        val = 0;
        if(gpio_get_value(I_MENU) == 0) {
            val |= R_MENU;
        }
    }
    else {
        //
        // FC3000 V1 TFT (Old Version)
        //
        if(gpio_get_value(I_UP) == 0) {
            val |= R_UP;
        }
        if(gpio_get_value(I_DOWN) == 0) {
            val |= R_DOWN;
        }
        if(gpio_get_value(I_LEFT) == 0) {
            val |= R_LEFT;
        }
        if(gpio_get_value(I_RIGHT) == 0) {
            val |= R_RIGHT;
        }
        if(gpio_get_value(I_A) == 0) {
            val |= R_A;
        }
        if(gpio_get_value(I_B) == 0) {
            val |= R_B;
        }
        if(gpio_get_value(I_X) == 0) {
            val |= R_X;
        }
        if(gpio_get_value(I_Y) == 0) {
            val |= R_Y;
        }
        if(gpio_get_value(I_START) == 0) {
            val |= R_START;
        }
        if(gpio_get_value(I_SELECT) == 0) {
            val |= R_SELECT;
        }
        if(gpio_get_value(I_MENU) == 0) {
            val |= R_MENU;
        }

        //
        // L1 / R1 Simulation
        //
        l = gpio_get_value(I_Y);
        r = gpio_get_value(I_LEFT);
        if((l == 0) && (r == 0)) {
            val = 0;
            val |= R_L1;
        }
        l = gpio_get_value(I_Y);
        r = gpio_get_value(I_RIGHT);
        if((l == 0) && (r == 0)) {
            val = 0;
            val |= R_R1;
        }
    }

    if(pre != val) {
        pre = val;
        report_key(pre, R_UP,     KEY_UP);
        report_key(pre, R_DOWN,   KEY_DOWN);
        report_key(pre, R_LEFT,   KEY_LEFT);
        report_key(pre, R_RIGHT,  KEY_RIGHT);
        report_key(pre, R_A,      KEY_LEFTCTRL);
        report_key(pre, R_B,      KEY_LEFTALT);
        report_key(pre, R_X,      KEY_SPACE);
        report_key(pre, R_Y,      KEY_LEFTSHIFT);
        report_key(pre, R_L1,     KEY_TAB);
        report_key(pre, R_R1,     KEY_BACKSPACE);
        report_key(pre, R_SELECT, KEY_ESC);
        report_key(pre, R_START,  KEY_ENTER);
        report_key(pre, R_MENU,   KEY_RIGHTCTRL);
        report_key(pre, R_L2,     KEY_PAGEUP);
        report_key(pre, R_R2,     KEY_PAGEDOWN);
        input_sync(mydev);
    }
    mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
}

static int __init kbd_init(void)
{
    uint32_t ret = 0;

    if(suniv_variant == 0) {
        I_UP     = ((32 * 4) + 2);
        I_DOWN   = ((32 * 4) + 3);
        I_LEFT   = ((32 * 4) + 4);
        I_RIGHT  = ((32 * 4) + 5);
        I_A      = ((32 * 4) + 9);
        I_B      = ((32 * 4) + 7);
        I_X      = ((32 * 3) + 9);
        I_Y      = ((32 * 4) + 8);
        I_SELECT = ((32 * 3) + 0);
        I_START  = ((32 * 4) + 0);
        I_L1     = ((32 * 2) + 1);
        I_R1     = ((32 * 2) + 2);
        I_MENU   = ((32 * 4) + 1);
        I_L2     = ((32 * 3) + 20);
        I_R2     = ((32 * 3) + 21);
        do_input_request(I_MENU, "menu");
    }
    else if(suniv_variant == 1) {
        I_UP     = ((32 * 0) + 0);
        I_DOWN   = ((32 * 4) + 2);
        I_LEFT   = ((32 * 4) + 4);
        I_RIGHT  = ((32 * 4) + 8);
        I_A      = ((32 * 3) + 19);
        I_B      = ((32 * 3) + 12);
        I_X      = ((32 * 3) + 2);
        I_Y      = ((32 * 3) + 17);
        I_SELECT = ((32 * 3) + 14);
        I_START  = ((32 * 3) + 13);
        I_L1     = ((32 * 3) + 0);
        I_R1     = ((32 * 3) + 1);
        I_MENU   = ((32 * 4) + 11);
        I_L2     = ((32 * 2) + 1);
        I_R2     = ((32 * 2) + 2);
        do_input_request(I_MENU, "menu");
    }
    else if(suniv_variant == 2 || suniv_variant == 5) {
        I_UP     = ((32 * 5) + 0);	//PF0
        I_DOWN   = ((32 * 5) + 5);	//PF5
        I_LEFT   = ((32 * 5) + 4);	//PF4
        I_RIGHT  = ((32 * 4) + 2);	//PE2
        I_A      = ((32 * 4) + 3);	//PE3
        I_B      = ((32 * 4) + 4);	//PE4
        I_X      = ((32 * 4) + 5);	//PE5
        I_Y      = ((32 * 0) + 3);	//PA3
        I_L1     = ((32 * 0) + 1);	//PE12=1,PA1
        I_R1     = ((32 * 0) + 2);	//PE12=1,PA2
        I_MENU   = ((32 * 4) + 12);	//PA1=,PA2=0
        I_L2     = ((32 * 4) + 10);
        I_R2     = ((32 * 4) + 7);
        do_output_request(I_MENU, "menu");
    }
    //
    // FC3000 V1 TFT (Old Version)
    //
    else if(suniv_variant == 3) {
        I_UP	= ((32 * 5) + 0);
        I_DOWN	= ((32 * 5) + 5);
        I_LEFT	= ((32 * 5) + 4);
        I_RIGHT	= ((32 * 4) + 2);
        //(32*4) 0,1,(2=RIGHT),(3=SELECT),(4=START),(5=A),(7=B),(8=Y),(9=X),(6=hangboot)
        //(32*0) 1,2 = Shutdown menu!?, (3=MENU)
        I_START = ((32 * 4) + 4);
        I_SELECT = ((32 * 4) + 3);
        I_A	= ((32 * 4) + 5);
        I_B	= ((32 * 4) + 7);
        I_X	= ((32 * 4) + 8);
        I_Y	= ((32 * 4) + 9);
        I_L1	= ((32 * 4) + 8);
        I_R1	= ((32 * 4) + 9);
        I_MENU	= ((32 * 0) + 3);
        do_input_request(I_MENU, "menu");
    }
    //
    // FC3000 V2 IPS (WL-28H105-A1)
    //
    else if(suniv_variant == 4) {
        I_UP     = ((32 * 5) + 0);	//PF0
        I_DOWN   = ((32 * 5) + 5);	//PF5
        I_LEFT   = ((32 * 5) + 4);	//PF4
        I_RIGHT  = ((32 * 4) + 2);	//PE2
        I_B      = ((32 * 4) + 3);	//PE3
        I_A      = ((32 * 4) + 4);	//PE4
        I_Y      = ((32 * 4) + 5);	//PE5
        I_X      = ((32 * 0) + 3);	//PA3
        I_L1     = ((32 * 0) + 1);	//PE12=1,PA1
        I_R1     = ((32 * 0) + 2);	//PE12=1,PA2
        I_MENU   = ((32 * 4) + 12);	//PA1=,PA2=0
        I_L2     = ((32 * 4) + 10);
        I_R2     = ((32 * 4) + 7);
        do_output_request(I_MENU, "menu");
    }

    do_input_request(I_UP,     "up");
    do_input_request(I_DOWN,   "down");
    do_input_request(I_LEFT,   "left");
    do_input_request(I_RIGHT,  "right");
    do_input_request(I_A,      "a");
    do_input_request(I_B,      "b");
    do_input_request(I_X,      "x");
    do_input_request(I_Y,      "y");
    do_input_request(I_SELECT, "select");
    do_input_request(I_START,  "start");
    do_input_request(I_L1,     "l1");
    do_input_request(I_R1,     "r1");

    mydev = input_allocate_device();
    set_bit(EV_KEY,         mydev-> evbit);
    set_bit(KEY_UP,         mydev->keybit);
    set_bit(KEY_DOWN,       mydev->keybit);
    set_bit(KEY_LEFT,       mydev->keybit);
    set_bit(KEY_RIGHT,      mydev->keybit);
    set_bit(KEY_LEFTCTRL,   mydev->keybit);
    set_bit(KEY_LEFTALT,    mydev->keybit);
    set_bit(KEY_SPACE,      mydev->keybit);
    set_bit(KEY_LEFTSHIFT,  mydev->keybit);
    set_bit(KEY_ENTER,      mydev->keybit);
    set_bit(KEY_ESC,        mydev->keybit);
    set_bit(KEY_TAB,        mydev->keybit);
    set_bit(KEY_BACKSPACE,  mydev->keybit);
    set_bit(KEY_RIGHTCTRL,  mydev->keybit);
    set_bit(KEY_PAGEUP,     mydev->keybit);
    set_bit(KEY_PAGEDOWN,   mydev->keybit);
    mydev->name = "suniv-keypad";
    mydev->id.bustype = BUS_HOST;
    ret = input_register_device(mydev);

    if(suniv_variant == 0) {
        setup_timer(&mytimer, pocketgo_handler, 0);
        printk("set pocketgo keypad handler\n");
    }
    else if(suniv_variant == 1) {
        setup_timer(&mytimer, pocketgo_handler, 0);
        printk("set trimui keypad handler\n");
    }
    else if(suniv_variant == 2 || suniv_variant == 5) {
        setup_timer(&mytimer, fc3000_handler, 0);
        printk("set fc3000 keypad handler\n");
    }
    else if(suniv_variant == 3) {
        setup_timer(&mytimer, fc3000_old_handler, 0);
        printk("set fc3000 tft v1 (old version) keypad handler\n");
    }
    else if(suniv_variant == 4) {
        setup_timer(&mytimer, fc3000_WL_28H105_A1_handler, 0);
        printk("set fc3000 ips v2 (WL-28H105-A1) keypad handler\n");
    }

    mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
    device_create_file(&mydev->dev, &dev_attr_lock);
    device_create_file(&mydev->dev, &dev_attr_l2r2);
    return 0;
}

static void __exit kbd_exit(void)
{
    input_unregister_device(mydev);
    del_timer(&mytimer);
}

module_init(kbd_init);
module_exit(kbd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steward Fu <steward.fu@gmail.com>");
MODULE_DESCRIPTION("keypad driver for allwinner suniv handheld");

