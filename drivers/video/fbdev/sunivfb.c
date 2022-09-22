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

#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/console.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/lcm.h>
#include <linux/clk-provider.h>
#include <video/of_display_timing.h>
#include <linux/gpio.h>
#include <linux/omapfb.h>
#include <linux/compiler.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <asm/io.h>
#include <asm/gpio.h>

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
#include <asm/arch-suniv/cpu.h>
#include <asm/arch-suniv/dma.h>
#include <asm/arch-suniv/gpio.h>
#include <asm/arch-suniv/intc.h>
#include <asm/arch-suniv/lcdc.h>
#include <asm/arch-suniv/debe.h>
#include <asm/arch-suniv/codec.h>
#include <asm/arch-suniv/clock.h>
#include <asm/arch-suniv/common.h>

#include "hex_splash.h"

#define PALETTE_SIZE 256
#define DRIVER_NAME  "suniv-fb"

#define FBIO_SET_DEBE_MODE _IOWR(0x1000, 0, unsigned long)

DECLARE_WAIT_QUEUE_HEAD(wait_vsync_queue);

struct myfb_app {
    uint32_t yoffset;
    uint32_t vsync_count;
};

struct myfb_par {
    struct device *dev;
    struct platform_device *pdev;

    resource_size_t p_palette_base;
    unsigned short *v_palette_base;

    void *vram_virt;
    uint32_t vram_size;
    dma_addr_t vram_phys;

    struct myfb_app *app_virt;

    int bpp;
    int lcdc_irq;
    int gpio_irq;
    int lcdc_ready;
    u32 pseudo_palette[16];
    struct fb_videomode mode;
};

struct suniv_iomm {
    uint8_t *dma;
    uint8_t *ccm;
    uint8_t *gpio;
    uint8_t *lcdc;
    uint8_t *debe;
    uint8_t *intc;
    uint8_t *timer;
};

extern int suniv_variant;

static struct timer_list mytimer;
static struct suniv_iomm iomm = {0};
static struct myfb_par *mypar = NULL;
static struct fb_var_screeninfo myfb_var = {0};

static struct fb_fix_screeninfo myfb_fix = {
    .id = DRIVER_NAME,
    .type = FB_TYPE_PACKED_PIXELS,
    .type_aux = 0,
    .visual = FB_VISUAL_TRUECOLOR,
    .xpanstep = 0,
    .ypanstep = 1,
    .ywrapstep = 0,
    .accel = FB_ACCEL_NONE
};

static uint32_t swapRB(uint16_t v)
{
    return ((v & 0x001f) << 11) | (v & 0x07e0) | ((v & 0xf800) >> 11);
}

static ssize_t variant_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", suniv_variant);
}

static ssize_t variant_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int rc = -1;
    unsigned long v = 0;

    rc = kstrtoul(buf, 0, &v);
    if(rc) {
        return rc;
    }
    return count;
}
static DEVICE_ATTR_RW(variant);

static int wait_for_vsync(struct myfb_par *par)
{
    uint32_t count = par->app_virt->vsync_count;
    long t = wait_event_interruptible_timeout(wait_vsync_queue, count != par->app_virt->vsync_count, HZ / 10);
    return t > 0 ? 0 : (t < 0 ? (int)t : -ETIMEDOUT);
}

static void pocketgo_gpio_init(void)
{
    uint32_t r = 0;

    r = readl(iomm.gpio + PD_CFG0);
    r &= 0x0000000f;
    r |= 0x22222220;
    writel(r, iomm.gpio + PD_CFG0);

    r = readl(iomm.gpio + PD_CFG1);
    r &= 0x000000f0;
    r |= 0x22222202;
    writel(r, iomm.gpio + PD_CFG1);

    r = readl(iomm.gpio + PD_CFG2);
    r &= 0xff000000;
    r |= 0x000222222;
    writel(r, iomm.gpio + PD_CFG2);

    r = readl(iomm.gpio + PD_PUL1);
    r &= 0xfffff0ff;
    r |= 0x00000500;
    writel(r, iomm.gpio + PD_PUL1);

    r = readl(iomm.gpio + PE_CFG1);
    r &= 0xffff0fff;
    r |= 0x00001000;
    writel(r, iomm.gpio + PE_CFG1);

    suniv_clrbits(iomm.gpio + PE_DATA, (1 << 11));
    mdelay(150);
    suniv_setbits(iomm.gpio + PE_DATA, (1 << 11));
    mdelay(50);
}

static void spi_9bits_write(uint32_t val)
{
    uint8_t cnt = 0;
    uint32_t tmp = readl(iomm.gpio + PD_DATA);

    tmp &= ~(1 << 11);
    writel(tmp, iomm.gpio + PD_DATA);
    for(cnt = 0; cnt < 9; cnt++) {
        tmp &= ~(1 << 10);
        if(val & 0x100) {
            tmp |= (1 << 10);
        }
        val <<= 1;

        tmp &= ~(1 << 9);
        writel(tmp, iomm.gpio + PD_DATA);
        tmp |= (1 << 9);
        writel(tmp, iomm.gpio + PD_DATA);
    }
    tmp |= (1 << 11);
    writel(tmp, iomm.gpio + PD_DATA);
}

static void trimui_gpio_init(void)
{
    uint32_t r = 0;

    r = readl(iomm.gpio + PD_CFG0);
    r &= 0x00000fff;
    r |= 0x22222000;
    writel(r, iomm.gpio + PD_CFG0);

    r = readl(iomm.gpio + PD_CFG1);
    r &= 0xffff0000;
    r |= 0x00001112;
    writel(r, iomm.gpio + PD_CFG1);

    r = readl(iomm.gpio + PD_CFG2);
    r &= 0xff00f0ff;
    r |= 0x00220200;
    writel(r, iomm.gpio + PD_CFG2);

    r = readl(iomm.gpio + PC_PUL0);
    r &= 0xffffff00;
    r |= 0x00000055;
    writel(r, iomm.gpio + PC_PUL0);
}

static void fc3000_gpio_init(void)
{
    uint32_t r = 0;

    r = readl(iomm.gpio + PD_CFG0);
    r &= 0x0000000f;
    r |= 0x22222220;
    writel(r, iomm.gpio + PD_CFG0);

    r = readl(iomm.gpio + PD_CFG1);
    r &= 0x000000f0;
    r |= 0x22222202;
    writel(r, iomm.gpio + PD_CFG1);

    r = readl(iomm.gpio + PD_CFG2);
    r &= 0xff000000;
    r |= 0x00222222;
    writel(r, iomm.gpio + PD_CFG2);

    r = readl(iomm.gpio + PE_CFG1);
    r &= 0xffff0fff;
    r |= 0x00001000;
    writel(r, iomm.gpio + PE_CFG1);

    suniv_clrbits(iomm.gpio + PE_DATA, (1 << 11));
    mdelay(150);
    suniv_setbits(iomm.gpio + PE_DATA, (1 << 11));
    mdelay(50);
}

static uint32_t lcdc_wait_busy(void)
{
    uint32_t cnt = 0;

    suniv_setbits(iomm.lcdc + TCON0_CPU_IF_REG, (1 << 0));
    ndelay(10);
    while(1) {
        if(readl(iomm.lcdc + TCON0_CPU_IF_REG) & 0x00c00000) {
            if(cnt > 200) {
                return -1;
            }
            else {
                cnt += 1;
            }
        }
        break;
    }
    return 0;
}

static uint32_t extend_16b_to_24b(uint32_t value)
{
    return ((value & 0xfc00) << 8) | ((value & 0x0300) << 6) | ((value & 0x00e0) << 5) | ((value & 0x001f) << 3);
}

static void lcdc_wr(uint8_t is_data, uint32_t data)
{
    while(lcdc_wait_busy());
    if(is_data) {
        suniv_setbits(iomm.lcdc + TCON0_CPU_IF_REG, (1 << 25));
    }
    else {
        suniv_clrbits(iomm.lcdc + TCON0_CPU_IF_REG, (1 << 25));
    }
    while(lcdc_wait_busy());
    writel(extend_16b_to_24b(data), iomm.lcdc + TCON0_CPU_WR_REG);
}

static void lcdc_wr_cmd(uint32_t cmd)
{
    lcdc_wr(0, cmd);
}

static void lcdc_wr_dat(uint32_t cmd)
{
    lcdc_wr(1, cmd);
}

static void flip_lcd(struct myfb_par *par)
{
#if 0
    static uint32_t report = 0;

    if((report++ % 60) == 0) {
        printk("%s, vsync\n", __func__);
    }
#endif
    suniv_clrbits(iomm.lcdc + TCON_INT_REG0, (1 << 15));
    suniv_clrbits(iomm.lcdc + TCON_CTRL_REG, (1 << 31));

    if(par->lcdc_ready) {
        if(suniv_variant == 0) {
            lcdc_wr_cmd(0x2c);
        }

        if(par->app_virt->yoffset == 0) {
            suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 8));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 9));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 10));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 11));
        }
        else if(par->app_virt->yoffset == 240) {
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 8));
            suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 9));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 10));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 11));
        }
        else if(par->app_virt->yoffset == 480) {
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 8));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 9));
            suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 10));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 11));
        }
        else if(par->app_virt->yoffset == 720) {
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 8));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 9));
            suniv_clrbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 10));
            suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 11));
        }
    }
    suniv_setbits(iomm.debe + DEBE_REGBUFF_CTRL_REG, (1 << 0));
    suniv_setbits(iomm.lcdc + TCON_CTRL_REG, (1 << 31));

    par->app_virt->vsync_count += 1;
    wake_up_interruptible_all(&wait_vsync_queue);
}

static irqreturn_t gpio_irq_handler(int irq, void *arg)
{
    flip_lcd(arg);
    return IRQ_HANDLED;
}

static irqreturn_t lcdc_irq_handler(int irq, void *arg)
{
    if((suniv_variant == 1) || (suniv_variant == 2) || (suniv_variant == 3) || (suniv_variant == 4) || (suniv_variant == 5)) {
        flip_lcd(arg);
    }
    suniv_clrbits(iomm.lcdc + TCON_INT_REG0, (1 << 15));
    return IRQ_HANDLED;
}

static void pocketgo_lcd_init(void)
{
    pocketgo_gpio_init();

    lcdc_wr_cmd(0x11);
    mdelay(50);

    lcdc_wr_cmd(0x36);
    lcdc_wr_dat(0xb0);

    lcdc_wr_cmd(0x3a);
    lcdc_wr_dat(0x05);

    lcdc_wr_cmd(0x2a);
    lcdc_wr_dat(0x00);
    lcdc_wr_dat(0x00);
    lcdc_wr_dat(0x01);
    lcdc_wr_dat(0x3f);

    lcdc_wr_cmd(0x2b);
    lcdc_wr_dat(0x00);
    lcdc_wr_dat(0x00);
    lcdc_wr_dat(0x00);
    lcdc_wr_dat(0xef);

    lcdc_wr_cmd(0xb2);
    lcdc_wr_dat(116);
    lcdc_wr_dat(16);
    lcdc_wr_dat(0x01);
    lcdc_wr_dat(0x33);
    lcdc_wr_dat(0x33);

    lcdc_wr_cmd(0xb7);
    lcdc_wr_dat(0x35);

    lcdc_wr_cmd(0xb8);
    lcdc_wr_dat(0x2f);
    lcdc_wr_dat(0x2b);
    lcdc_wr_dat(0x2f);

    lcdc_wr_cmd(0xbb);
    lcdc_wr_dat(0x15);

    lcdc_wr_cmd(0xc0);
    lcdc_wr_dat(0x3c);

    lcdc_wr_cmd(0x35);
    lcdc_wr_dat(0x00);

    lcdc_wr_cmd(0xc2);
    lcdc_wr_dat(0x01);

    lcdc_wr_cmd(0xc3);
    lcdc_wr_dat(0x13);

    lcdc_wr_cmd(0xc4);
    lcdc_wr_dat(0x20);

    lcdc_wr_cmd(0xc6);
    lcdc_wr_dat(0x07);

    lcdc_wr_cmd(0xd0);
    lcdc_wr_dat(0xa4);
    lcdc_wr_dat(0xa1);

    lcdc_wr_cmd(0xe8);
    lcdc_wr_dat(0x03);

    lcdc_wr_cmd(0xe9);
    lcdc_wr_dat(0x0d);
    lcdc_wr_dat(0x12);
    lcdc_wr_dat(0x00);

    lcdc_wr_cmd(0xe0);
    lcdc_wr_dat(0xd0);
    lcdc_wr_dat(0x08);
    lcdc_wr_dat(0x10);
    lcdc_wr_dat(0x0d);
    lcdc_wr_dat(0x0c);
    lcdc_wr_dat(0x07);
    lcdc_wr_dat(0x37);
    lcdc_wr_dat(0x53);
    lcdc_wr_dat(0x4c);
    lcdc_wr_dat(0x39);
    lcdc_wr_dat(0x15);
    lcdc_wr_dat(0x15);
    lcdc_wr_dat(0x2a);
    lcdc_wr_dat(0x2d);

    lcdc_wr_cmd(0xe1);
    lcdc_wr_dat(0xd0);
    lcdc_wr_dat(0x0d);
    lcdc_wr_dat(0x12);
    lcdc_wr_dat(0x08);
    lcdc_wr_dat(0x08);
    lcdc_wr_dat(0x15);
    lcdc_wr_dat(0x34);
    lcdc_wr_dat(0x34);
    lcdc_wr_dat(0x4a);
    lcdc_wr_dat(0x36);
    lcdc_wr_dat(0x12);
    lcdc_wr_dat(0x13);
    lcdc_wr_dat(0x2b);
    lcdc_wr_dat(0x2f);

    lcdc_wr_cmd(0x29);
    lcdc_wr_cmd(0x2c);
}

static void trimui_lcd_init(void)
{
    trimui_gpio_init();

    spi_9bits_write(0x00fe);
    spi_9bits_write(0x00ef);
    spi_9bits_write(0x0036);
    spi_9bits_write(0x0140);
    spi_9bits_write(0x003a);
    spi_9bits_write(0x0155);
    spi_9bits_write(0x0084);
    spi_9bits_write(0x0104);
    spi_9bits_write(0x0086);
    spi_9bits_write(0x01fb);
    spi_9bits_write(0x0087);
    spi_9bits_write(0x0179);
    spi_9bits_write(0x0089);
    spi_9bits_write(0x010b);
    spi_9bits_write(0x008a);
    spi_9bits_write(0x0120);
    spi_9bits_write(0x008b);
    spi_9bits_write(0x0180);
    spi_9bits_write(0x008d);
    spi_9bits_write(0x013b);
    spi_9bits_write(0x008e);
    spi_9bits_write(0x01cf);
    spi_9bits_write(0x00ec);
    spi_9bits_write(0x0133);
    spi_9bits_write(0x0102);
    spi_9bits_write(0x014c);
    spi_9bits_write(0x0098);
    spi_9bits_write(0x013e);
    spi_9bits_write(0x009c);
    spi_9bits_write(0x014b);
    spi_9bits_write(0x0099);
    spi_9bits_write(0x013e);
    spi_9bits_write(0x009d);
    spi_9bits_write(0x014b);
    spi_9bits_write(0x009b);
    spi_9bits_write(0x0155);
    spi_9bits_write(0x00e8);
    spi_9bits_write(0x0111);
    spi_9bits_write(0x0100);
    spi_9bits_write(0x00ff);
    spi_9bits_write(0x0162);
    spi_9bits_write(0x00c3);
    spi_9bits_write(0x0120);
    spi_9bits_write(0x00c4);
    spi_9bits_write(0x0103);
    spi_9bits_write(0x00c9);
    spi_9bits_write(0x010a);
    spi_9bits_write(0x003a);
    spi_9bits_write(0x0155);
    spi_9bits_write(0x0084);
    spi_9bits_write(0x0161);
    spi_9bits_write(0x008a);
    spi_9bits_write(0x0140);
    spi_9bits_write(0x00f6);
    spi_9bits_write(0x01c7);
    spi_9bits_write(0x00b0);
    spi_9bits_write(0x0163);
    spi_9bits_write(0x00b5);
    spi_9bits_write(0x0102);
    spi_9bits_write(0x0102);
    spi_9bits_write(0x0114);
    spi_9bits_write(0x00f0);
    spi_9bits_write(0x014a);
    spi_9bits_write(0x0110);
    spi_9bits_write(0x010a);
    spi_9bits_write(0x010a);
    spi_9bits_write(0x0126);
    spi_9bits_write(0x0139);
    spi_9bits_write(0x00f2);
    spi_9bits_write(0x014a);
    spi_9bits_write(0x0110);
    spi_9bits_write(0x010a);
    spi_9bits_write(0x010a);
    spi_9bits_write(0x0126);
    spi_9bits_write(0x0139);
    spi_9bits_write(0x00f1);
    spi_9bits_write(0x0150);
    spi_9bits_write(0x018f);
    spi_9bits_write(0x01af);
    spi_9bits_write(0x013b);
    spi_9bits_write(0x013f);
    spi_9bits_write(0x017f);
    spi_9bits_write(0x00f3);
    spi_9bits_write(0x0150);
    spi_9bits_write(0x018f);
    spi_9bits_write(0x01af);
    spi_9bits_write(0x013b);
    spi_9bits_write(0x013f);
    spi_9bits_write(0x017f);
    spi_9bits_write(0x00ba);
    spi_9bits_write(0x010a);
    spi_9bits_write(0x0035);
    spi_9bits_write(0x0100);
    spi_9bits_write(0x0021);
    spi_9bits_write(0x00fe);
    spi_9bits_write(0x00ee);
    spi_9bits_write(0x0011);
    spi_9bits_write(0x0029);
    spi_9bits_write(0x002c);
}

static void fc3000_RB411_11A_lcd_init(void)
{
    fc3000_gpio_init();

    lcdc_wr_cmd(0x800);
    lcdc_wr_dat(0x100);
    lcdc_wr_cmd(0x1000);
    lcdc_wr_dat(0x700);
    lcdc_wr_cmd(0x1800);
    lcdc_wr_dat(0xc002);
    lcdc_wr_cmd(0x2000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x4000);
    lcdc_wr_dat(0x1200);
    lcdc_wr_cmd(0x4800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x5000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x6000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x6800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x7800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8800);
    lcdc_wr_dat(0x3800);
    lcdc_wr_cmd(0x9000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x9800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x3800);
    lcdc_wr_dat(0x800);
    lcdc_wr_cmd(0x8000);
    lcdc_wr_dat(0x8682);
    lcdc_wr_cmd(0x8800);
    lcdc_wr_dat(0x3e60);
    lcdc_wr_cmd(0x9000);
    lcdc_wr_dat(0xc080);
    lcdc_wr_cmd(0x9800);
    lcdc_wr_dat(0x603);
    lcdc_wr_cmd(0x4820);
    lcdc_wr_dat(0xf000);
    lcdc_wr_cmd(0x5820);
    lcdc_wr_dat(0x7000);
    lcdc_wr_cmd(0x20);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x820);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8020);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8820);
    lcdc_wr_dat(0x3d00);
    lcdc_wr_cmd(0x9020);
    lcdc_wr_dat(0x2000);
    lcdc_wr_cmd(0xa820);
    lcdc_wr_dat(0x2a00);
    lcdc_wr_cmd(0xb020);
    lcdc_wr_dat(0x2000);
    lcdc_wr_cmd(0xb820);
    lcdc_wr_dat(0x3b00);
    lcdc_wr_cmd(0xc020);
    lcdc_wr_dat(0x1000);
    lcdc_wr_cmd(0xc820);
    lcdc_wr_dat(0x3f00);
    lcdc_wr_cmd(0xe020);
    lcdc_wr_dat(0x1500);
    lcdc_wr_cmd(0xe820);
    lcdc_wr_dat(0x2000);
    lcdc_wr_cmd(0x8040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8840);
    lcdc_wr_dat(0x78e0);
    lcdc_wr_cmd(0x9040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x9840);
    lcdc_wr_dat(0xf920);
    lcdc_wr_cmd(0x60);
    lcdc_wr_dat(0x714);
    lcdc_wr_cmd(0x860);
    lcdc_wr_dat(0x800);
    lcdc_wr_cmd(0x5060);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x80);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x880);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x1080);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x1880);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x2080);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x2880);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8080);
    lcdc_wr_dat(0x8000);
    lcdc_wr_cmd(0x9080);
    lcdc_wr_dat(0x600);
    lcdc_wr_cmd(0x1800);
    lcdc_wr_dat(0x4020);
    lcdc_wr_cmd(0x3800);
    lcdc_wr_dat(0x9920);
    lcdc_wr_cmd(0x8040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8840);
    lcdc_wr_dat(0x78e0);
    lcdc_wr_cmd(0x9040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x9840);
    lcdc_wr_dat(0xf920);
    lcdc_wr_cmd(0x20);
    lcdc_wr_dat(0x78e0);
    lcdc_wr_cmd(0x820);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x1020);
    mdelay(500);
}

static void fc3000_lcd_init(void)
{
    fc3000_gpio_init();

    lcdc_wr_cmd(0x800);
    lcdc_wr_dat(0x100);
    lcdc_wr_cmd(0x1000);
    lcdc_wr_dat(0x700);
    lcdc_wr_cmd(0x1800);
    lcdc_wr_dat(0xc002);
    lcdc_wr_cmd(0x2000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x4000);
    lcdc_wr_dat(0x1200);
    lcdc_wr_cmd(0x4800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x5000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x6000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x6800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x7800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8800);
    lcdc_wr_dat(0x3800);
    lcdc_wr_cmd(0x9000);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x9800);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x3800);
    lcdc_wr_dat(0x800);
    lcdc_wr_cmd(0x8000);
    lcdc_wr_dat(0x8682);
    lcdc_wr_cmd(0x8800);
    lcdc_wr_dat(0x3e60);
    lcdc_wr_cmd(0x9000);
    lcdc_wr_dat(0xc080);
    lcdc_wr_cmd(0x9800);
    lcdc_wr_dat(0x603);
    lcdc_wr_cmd(0x4820);
    lcdc_wr_dat(0xf000);
    lcdc_wr_cmd(0x5820);
    lcdc_wr_dat(0x7000);
    lcdc_wr_cmd(0x20);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x820);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8020);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8820);
    lcdc_wr_dat(0x3d00);
    lcdc_wr_cmd(0x9020);
    lcdc_wr_dat(0x2000);
    lcdc_wr_cmd(0xa820);
    lcdc_wr_dat(0x2a00);
    lcdc_wr_cmd(0xb020);
    lcdc_wr_dat(0x2000);
    lcdc_wr_cmd(0xb820);
    lcdc_wr_dat(0x3b00);
    lcdc_wr_cmd(0xc020);
    lcdc_wr_dat(0x1000);
    lcdc_wr_cmd(0xc820);
    lcdc_wr_dat(0x3f00);
    lcdc_wr_cmd(0xe020);
    lcdc_wr_dat(0x1500);
    lcdc_wr_cmd(0xe820);
    lcdc_wr_dat(0x2000);
    lcdc_wr_cmd(0x8040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8840);
    lcdc_wr_dat(0x78e0);
    lcdc_wr_cmd(0x9040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x9840);
    lcdc_wr_dat(0xf920);
    lcdc_wr_cmd(0x60);
    lcdc_wr_dat(0x714);
    lcdc_wr_cmd(0x860);
    lcdc_wr_dat(0x800);
    lcdc_wr_cmd(0x5060);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x80);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x880);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x1080);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x1880);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x2080);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x2880);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8080);
    lcdc_wr_dat(0x8000);
    lcdc_wr_cmd(0x9080);
    lcdc_wr_dat(0x600);
    lcdc_wr_cmd(0x1800);
    lcdc_wr_dat(0x4020);
    lcdc_wr_cmd(0x3800);
    lcdc_wr_dat(0x9920);
    lcdc_wr_cmd(0x8040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x8840);
    lcdc_wr_dat(0x78e0);
    lcdc_wr_cmd(0x9040);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x9840);
    lcdc_wr_dat(0xf920);
    lcdc_wr_cmd(0x20);
    lcdc_wr_dat(0x78e0);
    lcdc_wr_cmd(0x820);
    lcdc_wr_dat(0x0);
    lcdc_wr_cmd(0x1020);
    mdelay(500);
}

static void fc3000_WL_28H105_A1_lcd_init(void)
{
    int cc = 0;

    fc3000_gpio_init();

    //
    // Settings found on few websites, (xx) means default
    //
    lcdc_wr_cmd(swapRB(0x01));    //SW Reset
    lcdc_wr_cmd(swapRB(0x28));    //Display off
    lcdc_wr_cmd(swapRB(0x11));    //Disable sleep mode

    lcdc_wr_cmd(swapRB(0x36));    //MADCTL - Memory Data Access Control
    lcdc_wr_dat(swapRB(0x6C));    //D7-D0 = MY MX MV ML RGB MH ** ** = 01101100

    lcdc_wr_cmd(swapRB(0x3A));    //COLMOD - Interface Pixel format
    lcdc_wr_dat(swapRB(0x55));    //D7-D0 = 01010101 = 0x55 = 65k color, 16bit

    lcdc_wr_cmd(swapRB(0xB2));    //PORCTRL - Porch Setting
    lcdc_wr_dat(swapRB(0x0C));    //BPA
    lcdc_wr_dat(swapRB(0x0C));    //FPA
    lcdc_wr_dat(swapRB(0x00));    //PSEN
    lcdc_wr_dat(swapRB(0x33));    //FPB
    lcdc_wr_dat(swapRB(0x33));    //BPC

    lcdc_wr_cmd(swapRB(0xB7));    //GCTRL - Gate control
    lcdc_wr_dat(swapRB(0x35));    //D7=0,D6-D4=VGHS,D3=0,D2-D0=VGLS

    lcdc_wr_cmd(swapRB(0xBB));    //VCOMS - Vcom Setting
    lcdc_wr_dat(swapRB(0x19));    //D7-D6=0,D5-D0=VCOM (2B)

    lcdc_wr_cmd(swapRB(0xC0));    //LCMCTRL - LCM Control
    lcdc_wr_dat(swapRB(0x0C));    //XOR command 0x36 paramters

    lcdc_wr_cmd(swapRB(0xC2));    //VDVVRHEN - VDV and VRH command enable
    lcdc_wr_dat(swapRB(0x01));    //enable/disable
    lcdc_wr_dat(swapRB(0xFF));    //

    lcdc_wr_cmd(swapRB(0xC3));    //VRHS - VRH set
    lcdc_wr_dat(swapRB(0x10));    // (11)

    lcdc_wr_cmd(swapRB(0xC4));    //VDVS - VDV set
    lcdc_wr_dat(swapRB(0x20));    //

    lcdc_wr_cmd(swapRB(0xC6));    // FPS
    lcdc_wr_dat(swapRB(0x0F));    // 0F=60, 0B=69, 0A=72, 15=50

    lcdc_wr_cmd(swapRB(0xD0));    //PWCTRL1 - Power control 1
    lcdc_wr_dat(swapRB(0xA4));    //
    lcdc_wr_dat(swapRB(0xA1));    //D7-D6=AVDD,D5-D4=AVCL,D3-D2=0,D1-D0=VDS

    //
    // Gamma is set by dafault also at reset
    //
    /*
    lcdc_wr_cmd(swapRB(0xE0));    //PVGAMCTRL - Positive Power Gamma control
    lcdc_wr_dat(swapRB(0xD0));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0x02));    // (05)
    lcdc_wr_dat(swapRB(0x07));    // (0E)
    lcdc_wr_dat(swapRB(0x0A));    // (15)
    lcdc_wr_dat(swapRB(0x28));    // (0D)
    lcdc_wr_dat(swapRB(0x32));    // (37)
    lcdc_wr_dat(swapRB(0x44));    // (43)
    lcdc_wr_dat(swapRB(0x42));    // (47)
    lcdc_wr_dat(swapRB(0x06));    // (09)
    lcdc_wr_dat(swapRB(0x0E));    // (15)
    lcdc_wr_dat(swapRB(0x12));
    lcdc_wr_dat(swapRB(0x14));    // (16)
    lcdc_wr_dat(swapRB(0x17));    // (19)

    lcdc_wr_cmd(swapRB(0xE1));    //NVGAMCTRL - Negative Power Gamma control
    lcdc_wr_dat(swapRB(0xD0));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0x02));    // (05)
    lcdc_wr_dat(swapRB(0x07));    // (0D)
    lcdc_wr_dat(swapRB(0x0A));    // (0C)
    lcdc_wr_dat(swapRB(0x28));    // (06)
    lcdc_wr_dat(swapRB(0x31));    // (2D)
    lcdc_wr_dat(swapRB(0x54));    // (44)
    lcdc_wr_dat(swapRB(0x47));    // (40)
    lcdc_wr_dat(swapRB(0x0E));
    lcdc_wr_dat(swapRB(0x1C));
    lcdc_wr_dat(swapRB(0x17));    // (18)
    lcdc_wr_dat(swapRB(0x1B));    // (16)
    lcdc_wr_dat(swapRB(0x1E));    // (19)
    */

    lcdc_wr_cmd(swapRB(0x2A));    //CASET - Column Adress set
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0x01));
    lcdc_wr_dat(swapRB(0x3F));

    lcdc_wr_cmd(swapRB(0x2B));    //RASET - Row Adress set
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_dat(swapRB(0xEF));

    //lcdc_wr_cmd(swapRB(0x55));    //Color Enhancement
    //lcdc_wr_dat(swapRB(0xB3));

    lcdc_wr_cmd(swapRB(0x51));
    lcdc_wr_dat(swapRB(0xff));

    lcdc_wr_cmd(swapRB(0x21));    //Invert Display
    lcdc_wr_cmd(swapRB(0x2C));    //Enable Write Ram
    //mdelay(500);

    for(cc = 0; cc < (320 * 240); cc++) {
        lcdc_wr_dat(0x00);
    }
    lcdc_wr_cmd(swapRB(0x29));    //Display on
    lcdc_wr_cmd(swapRB(0x2C));    //Enable Write Ram
}

static void fc3000_T2812_M106_24C_7D_lcd_init(void)
{
    fc3000_gpio_init();
    lcdc_wr_cmd(swapRB(0x2e));
    lcdc_wr_dat(swapRB(0x89));
    lcdc_wr_cmd(swapRB(0x29));
    lcdc_wr_dat(swapRB(0x8f));
    lcdc_wr_cmd(swapRB(0x2b));
    lcdc_wr_dat(swapRB(0x02));
    lcdc_wr_cmd(swapRB(0xe2));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0xe4));
    lcdc_wr_dat(swapRB(0x01));
    lcdc_wr_cmd(swapRB(0xe5));
    lcdc_wr_dat(swapRB(0x10));
    lcdc_wr_cmd(swapRB(0xe6));
    lcdc_wr_dat(swapRB(0x01));
    lcdc_wr_cmd(swapRB(0xe7));
    lcdc_wr_dat(swapRB(0x10));
    lcdc_wr_cmd(swapRB(0xe8));
    lcdc_wr_dat(swapRB(0x70));
    lcdc_wr_cmd(swapRB(0xf2));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0xea));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0xeb));
    lcdc_wr_dat(swapRB(0x20));
    lcdc_wr_cmd(swapRB(0xec));
    lcdc_wr_dat(swapRB(0x3c));
    lcdc_wr_cmd(swapRB(0xed));
    lcdc_wr_dat(swapRB(0xc8));
    lcdc_wr_cmd(swapRB(0xe9));
    lcdc_wr_dat(swapRB(0x38));
    lcdc_wr_cmd(swapRB(0xf1));
    lcdc_wr_dat(swapRB(0x01));
    lcdc_wr_cmd(swapRB(0x40));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x41));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x42));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x43));
    lcdc_wr_dat(swapRB(0x15));
    lcdc_wr_cmd(swapRB(0x44));
    lcdc_wr_dat(swapRB(0x13));
    lcdc_wr_cmd(swapRB(0x45));
    lcdc_wr_dat(swapRB(0x3f));
    lcdc_wr_cmd(swapRB(0x47));
    lcdc_wr_dat(swapRB(0x55));
    lcdc_wr_cmd(swapRB(0x48));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x49));
    lcdc_wr_dat(swapRB(0x12));
    lcdc_wr_cmd(swapRB(0x4a));
    lcdc_wr_dat(swapRB(0x19));
    lcdc_wr_cmd(swapRB(0x4b));
    lcdc_wr_dat(swapRB(0x19));
    lcdc_wr_cmd(swapRB(0x4c));
    lcdc_wr_dat(swapRB(0x16));
    lcdc_wr_cmd(swapRB(0x50));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x51));
    lcdc_wr_dat(swapRB(0x2c));
    lcdc_wr_cmd(swapRB(0x52));
    lcdc_wr_dat(swapRB(0x2a));
    lcdc_wr_cmd(swapRB(0x53));
    lcdc_wr_dat(swapRB(0x3f));
    lcdc_wr_cmd(swapRB(0x54));
    lcdc_wr_dat(swapRB(0x3f));
    lcdc_wr_cmd(swapRB(0x55));
    lcdc_wr_dat(swapRB(0x3f));
    lcdc_wr_cmd(swapRB(0x56));
    lcdc_wr_dat(swapRB(0x2a));
    lcdc_wr_cmd(swapRB(0x57));
    lcdc_wr_dat(swapRB(0x7e));
    lcdc_wr_cmd(swapRB(0x58));
    lcdc_wr_dat(swapRB(0x09));
    lcdc_wr_cmd(swapRB(0x59));
    lcdc_wr_dat(swapRB(0x06));
    lcdc_wr_cmd(swapRB(0x5a));
    lcdc_wr_dat(swapRB(0x06));
    lcdc_wr_cmd(swapRB(0x5b));
    lcdc_wr_dat(swapRB(0x0d));
    lcdc_wr_cmd(swapRB(0x5c));
    lcdc_wr_dat(swapRB(0x1f));
    lcdc_wr_cmd(swapRB(0x5d));
    lcdc_wr_dat(swapRB(0xff));
    lcdc_wr_cmd(swapRB(0x1b));
    lcdc_wr_dat(swapRB(0x1a));
    lcdc_wr_cmd(swapRB(0x1a));
    lcdc_wr_dat(swapRB(0x02));
    lcdc_wr_cmd(swapRB(0x24));
    lcdc_wr_dat(swapRB(0x61));
    lcdc_wr_cmd(swapRB(0x25));
    lcdc_wr_dat(swapRB(0x5c));
    lcdc_wr_cmd(swapRB(0x23));
    lcdc_wr_dat(swapRB(0x62));
    lcdc_wr_cmd(swapRB(0x18));
    lcdc_wr_dat(swapRB(0x36));
    lcdc_wr_cmd(swapRB(0x19));
    lcdc_wr_dat(swapRB(0x01));
    lcdc_wr_cmd(swapRB(0x1f));
    lcdc_wr_dat(swapRB(0x88));
    lcdc_wr_cmd(swapRB(0x1f));
    lcdc_wr_dat(swapRB(0x80));
    lcdc_wr_cmd(swapRB(0x1f));
    lcdc_wr_dat(swapRB(0x90));
    lcdc_wr_cmd(swapRB(0x1f));
    lcdc_wr_dat(swapRB(0xd4));
    lcdc_wr_cmd(swapRB(0x17));
    lcdc_wr_dat(swapRB(0x05));
    lcdc_wr_cmd(swapRB(0x36));
    lcdc_wr_dat(swapRB(0x09));
    lcdc_wr_cmd(swapRB(0x28));
    lcdc_wr_dat(swapRB(0x38));
    lcdc_wr_cmd(swapRB(0x28));
    lcdc_wr_dat(swapRB(0x3c));

    lcdc_wr_cmd(swapRB(0x02));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x03));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x04));
    lcdc_wr_dat(swapRB(0x01));
    lcdc_wr_cmd(swapRB(0x05));
    lcdc_wr_dat(swapRB(0x3f));
    lcdc_wr_cmd(swapRB(0x06));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x07));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x08));
    lcdc_wr_dat(swapRB(0x00));
    lcdc_wr_cmd(swapRB(0x09));
    lcdc_wr_dat(swapRB(0xef));

    lcdc_wr_cmd(swapRB(0x16));
    lcdc_wr_dat(swapRB(0x28));
    lcdc_wr_cmd(swapRB(0x22));
}

static void smartlcd_init(struct myfb_par *par)
{
    uint32_t ret = 0, p1 = 0, p2 = 0;

    writel(0, iomm.lcdc + TCON_CTRL_REG);
    writel(0, iomm.lcdc + TCON_INT_REG0);
    ret = readl(iomm.lcdc + TCON_CLK_CTRL_REG);
    ret &= ~(0xf << 28);
    writel(ret, iomm.lcdc + TCON_CLK_CTRL_REG);
    writel(0xffffffff, iomm.lcdc + TCON0_IO_CTRL_REG1);
    writel(0xffffffff, iomm.lcdc + TCON1_IO_CTRL_REG1);

    suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 0));
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY0_LINEWIDTH_REG);
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY1_LINEWIDTH_REG);
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY2_LINEWIDTH_REG);
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY3_LINEWIDTH_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_DISP_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY0_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY1_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY2_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY3_SIZE_REG);
    writel((5 << 8), iomm.debe + DEBE_LAY0_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY1_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY2_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY3_ATT_CTRL_REG1);
    suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 8));
    suniv_setbits(iomm.debe + DEBE_REGBUFF_CTRL_REG, (1 << 1));
    suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 1));

    ret = readl(iomm.lcdc + TCON_CTRL_REG);
    ret &= ~(1 << 0);
    writel(ret, iomm.lcdc + TCON_CTRL_REG);
    ret = (1 + 1 + 1);

    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 0) << 3, iomm.debe + DEBE_LAY0_FB_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 1) << 3, iomm.debe + DEBE_LAY1_FB_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 2) << 3, iomm.debe + DEBE_LAY2_FB_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 3) << 3, iomm.debe + DEBE_LAY3_FB_ADDR_REG);

    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 0) >> 29, iomm.debe + DEBE_LAY0_FB_HI_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 1) >> 29, iomm.debe + DEBE_LAY1_FB_HI_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 2) >> 29, iomm.debe + DEBE_LAY2_FB_HI_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 3) >> 29, iomm.debe + DEBE_LAY3_FB_HI_ADDR_REG);

    writel((1 << 31) | ((ret & 0x1f) << 4) | (1 << 24), iomm.lcdc + TCON0_CTRL_REG);
    if(suniv_variant == 0) {
        writel((0xf << 28) | (6 << 0), iomm.lcdc + TCON_CLK_CTRL_REG);
    }
    else if((suniv_variant == 2) || (suniv_variant == 3) || (suniv_variant == 4) || (suniv_variant == 5)) {
        //writel((0xf << 28) | (63 << 0), iomm.lcdc + TCON_CLK_CTRL_REG);
        writel((0xf << 28) | (127 << 0), iomm.lcdc + TCON_CLK_CTRL_REG);
    }
    writel((4 << 29) | (1 << 26), iomm.lcdc + TCON0_CPU_IF_REG);
    writel((1 << 28), iomm.lcdc + TCON0_IO_CTRL_REG0);

    p1 = par->mode.yres - 1;
    p2 = par->mode.xres - 1;
    writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG0);

    if(suniv_variant == 0) {
        p1 = 1 + 1;
        p2 = 1 + 1 + par->mode.xres + 2;
        writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG1);

        p1 = 1 + 1;
        p2 = (1 + 1 + par->mode.yres + 1 + 2) << 1;
        writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG2);

        p1 = 1 + 1;
        p2 = 1 + 1;
        writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG3);
    }
    else if((suniv_variant == 2) || (suniv_variant == 3) || (suniv_variant == 4) || (suniv_variant == 5)) {
        p1 = 1 + 1;
        p2 = 1 + 1 + par->mode.xres + 2;
        writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG1);

        p1 = 1 + 1;
        p2 = 1000;
        writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG2);

        p1 = 1 + 1;
        p2 = 1 + 1;
        writel((p2 << 16) | (p1 << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG3);
    }

    writel(0, iomm.lcdc + TCON0_HV_TIMING_REG);
    writel(0, iomm.lcdc + TCON0_IO_CTRL_REG1);

    suniv_setbits(iomm.lcdc + TCON_CTRL_REG, (1 << 31));
    if(suniv_variant == 0) {
        pocketgo_lcd_init();
    }
    else if(suniv_variant == 2) {
        fc3000_lcd_init();
    }
    else if(suniv_variant == 3) {
        fc3000_T2812_M106_24C_7D_lcd_init();
    }
    else if(suniv_variant == 4) {
        fc3000_WL_28H105_A1_lcd_init();
    }
    else if(suniv_variant == 5) {
        fc3000_RB411_11A_lcd_init();
    }

    suniv_setbits(iomm.lcdc + TCON_INT_REG0, (1 << 31));
    suniv_setbits(iomm.lcdc + TCON0_CPU_IF_REG, (1 << 28));
}

static void rgblcd_init(struct myfb_par *par)
{
    uint32_t ret = 0, p1 = 0, p2 = 0;
    uint32_t v_front_porch = 0x11e;
    uint32_t v_back_porch = 2;//0x06;
    uint32_t v_sync_len = 1;//0x04;
    uint32_t h_sync_len = 0x12;
    uint32_t h_back_porch = 2;//0x26;
    uint32_t h_front_porch = 0x465;

    if(suniv_variant == 1) {
        trimui_lcd_init();
    }
    ret = readl(iomm.lcdc + TCON_CTRL_REG);
    ret &= ~(0x1 << 0);
    writel(ret, iomm.lcdc + TCON_CTRL_REG);

    suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 0));
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY0_LINEWIDTH_REG);
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY1_LINEWIDTH_REG);
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY2_LINEWIDTH_REG);
    writel(par->mode.xres << 4, iomm.debe + DEBE_LAY3_LINEWIDTH_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_DISP_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY0_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY1_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY2_SIZE_REG);
    writel((((par->mode.yres) - 1) << 16) | (((par->mode.xres) - 1) << 0), iomm.debe + DEBE_LAY3_SIZE_REG);
    writel((5 << 8), iomm.debe + DEBE_LAY0_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY1_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY2_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY3_ATT_CTRL_REG1);
    suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 8));
    suniv_setbits(iomm.debe + DEBE_REGBUFF_CTRL_REG, (3 << 0));
    suniv_setbits(iomm.debe + DEBE_MODE_CTRL_REG, (1 << 1));

    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 0) << 3, iomm.debe + DEBE_LAY0_FB_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 1) << 3, iomm.debe + DEBE_LAY1_FB_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 2) << 3, iomm.debe + DEBE_LAY2_FB_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 3) << 3, iomm.debe + DEBE_LAY3_FB_ADDR_REG);

    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 0) >> 29, iomm.debe + DEBE_LAY0_FB_HI_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 1) >> 29, iomm.debe + DEBE_LAY1_FB_HI_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 2) >> 29, iomm.debe + DEBE_LAY2_FB_HI_ADDR_REG);
    writel((uint32_t)(par->vram_phys + 320 * 240 * 2 * 3) >> 29, iomm.debe + DEBE_LAY3_FB_HI_ADDR_REG);

    ret = v_front_porch + v_back_porch + v_sync_len;
    writel((1 << 31) | ((ret & 0x1f) << 4), iomm.lcdc + TCON0_CTRL_REG);
    writel((0xf << 28) | (25 << 0), iomm.lcdc + TCON_CLK_CTRL_REG);

    p1 = par->mode.yres;
    p2 = par->mode.xres;
    writel(((p2 - 1) << 16) | ((p1 - 1) << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG0);

    p1 = h_sync_len + h_back_porch;
    p2 = par->mode.xres + h_front_porch + p1;
    writel(((p2 - 1) << 16) | ((p1 - 1) << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG1);

    p1 = v_sync_len + v_back_porch;
    p2 = par->mode.yres + v_front_porch + p1;
    writel(((p2 - 1) << 16) | ((p1 - 1) << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG2);

    p1 = v_sync_len;
    p2 = h_sync_len;
    writel(((p2 - 1) << 16) | ((p1 - 1) << 0), iomm.lcdc + TCON0_BASIC_TIMING_REG3);

    writel((1 << 31), iomm.lcdc + TCON0_HV_TIMING_REG);
    writel(0, iomm.lcdc + TCON0_CPU_IF_REG);

    writel((1 << 28) | (1 << 25) | (1 << 24), iomm.lcdc + TCON0_IO_CTRL_REG0);
    writel(0, iomm.lcdc + TCON0_IO_CTRL_REG1);
    suniv_setbits(iomm.lcdc + TCON_INT_REG0, (1 << 31));
    suniv_setbits(iomm.lcdc + TCON_CTRL_REG, (1 << 31));
}

static void suniv_enable_irq(struct myfb_par *par)
{
    int ret = 0;

    par->lcdc_irq = platform_get_irq(par->pdev, 0);
    if(par->lcdc_irq < 0) {
        printk("%s, failed to get irq number for lcdc irq\n", __func__);
    }
    else {
        ret = request_irq(par->lcdc_irq, lcdc_irq_handler, IRQF_SHARED, "lcdc_irq", par);
        if(ret) {
            printk("%s, failed to register lcdc interrupt(%d)\n", __func__, par->lcdc_irq);
        }
    }

    if(suniv_variant == 0) {
        par->gpio_irq = gpio_to_irq(((32 * 4) + 10));
        if(par->gpio_irq < 0) {
            printk("%s, failed to get irq number for gpio irq\n", __func__);
        }
        else {
            ret = request_irq(par->gpio_irq, gpio_irq_handler, IRQF_TRIGGER_RISING, "gpio_irq", par);
            if(ret) {
                printk("%s, failed to register gpio interrupt(%d)\n", __func__, par->gpio_irq);
            }
        }
    }
}

static void suniv_cpu_init(struct myfb_par *par)
{
    uint32_t ret, i;

    if((suniv_variant == 1) || (suniv_variant == 2) || (suniv_variant == 3) || (suniv_variant == 4) || (suniv_variant == 5)) {
        writel((1 << 31) | (1 << 25) | (1 << 24) | (98 << 8) | (7 << 0), iomm.ccm + PLL_VIDEO_CTRL_REG);
    }

    while((readl(iomm.ccm + PLL_VIDEO_CTRL_REG) & (1 << 28)) == 0) {
    }
    while((readl(iomm.ccm + PLL_PERIPH_CTRL_REG) & (1 << 28)) == 0) {
    }

    ret = readl(iomm.ccm + DRAM_GATING_REG);
    ret |= (1 << 26) | (1 << 24);
    writel(ret, iomm.ccm + DRAM_GATING_REG);

    suniv_setbits(iomm.ccm + FE_CLK_REG, (1 << 31));
    suniv_setbits(iomm.ccm + BE_CLK_REG, (1 << 31));
    suniv_setbits(iomm.ccm + TCON_CLK_REG, (1 << 31) | (1 << 25));
    suniv_setbits(iomm.ccm + BUS_CLK_GATING_REG1, (1 << 14) | (1 << 12) | (1 << 4));
    suniv_setbits(iomm.ccm + BUS_SOFT_RST_REG1, (1 << 14) | (1 << 12) | (1 << 4));
    for(i = 0x0800; i < 0x1000; i += 4) {
        writel(0, iomm.debe + i);
    }
}

static void lcd_delay_init(unsigned long param)
{
    suniv_cpu_init(mypar);
    if((suniv_variant == 0) || (suniv_variant == 2) || (suniv_variant == 3) || (suniv_variant == 4) || (suniv_variant == 5)) {
        smartlcd_init(mypar);
    }
    else if(suniv_variant == 1) {
        rgblcd_init(mypar);
    }
    //
    // Enable Boot Logo
    //
    memcpy((uint8_t *)mypar->vram_virt + (320 * 240 * 2 * 1), hex_splash, 320 * 240 * 2);    // Logo Data
    mypar->app_virt->yoffset = 240;    // Show bootlogo
    //mypar->app_virt->yoffset = 0;        // Show Kernel msg
    mypar->lcdc_ready = 1;
    suniv_enable_irq(mypar);

    //
    // NEO_BL
    // Enable Backlight during Boot for boot logo display
    // Curently tested on FC3000, FC3000_OLD, FC3000_IPS
    //
    if((suniv_variant == 2) || (suniv_variant == 3) || (suniv_variant == 4) || (suniv_variant == 5)) {
        gpio_direction_output((32 * 4) + 6, 1);    // set PE6 as output
        gpio_set_value((32 * 4) + 6, 1);        // set PE6 high
    }

}

#define CNVT_TOHW(val, width) ((((val) << (width)) + 0x7FFF - (val)) >> 16)
static int myfb_setcolreg(unsigned regno, unsigned red, unsigned green, unsigned blue, unsigned transp, struct fb_info *info)
{
    red = CNVT_TOHW(red, info->var.red.length);
    blue = CNVT_TOHW(blue, info->var.blue.length);
    green = CNVT_TOHW(green, info->var.green.length);
    ((u32 *)(info->pseudo_palette))[regno] = (red << info->var.red.offset) | (green << info->var.green.offset) | (blue << info->var.blue.offset);
    return 0;
}
#undef CNVT_TOHW

static int myfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    int bpp = var->bits_per_pixel >> 3;
    struct myfb_par *par = info->par;
    unsigned long line_size = var->xres_virtual * bpp;

    if((var->xres != 320) || (var->yres != 240) || (var->bits_per_pixel != 16)) {
        return -EINVAL;
    }

    var->transp.offset = 0;
    var->transp.length = 0;
    var->red.offset = 11;
    var->red.length = 5;
    var->green.offset = 5;
    var->green.length = 6;
    var->blue.offset = 0;
    var->blue.length = 5;
    var->red.msb_right = 0;
    var->green.msb_right = 0;
    var->blue.msb_right = 0;
    var->transp.msb_right = 0;
    if(line_size * var->yres_virtual > par->vram_size) {
        var->yres_virtual = par->vram_size / line_size;
    }
    if(var->yres > var->yres_virtual) {
        var->yres = var->yres_virtual;
    }
    if(var->xres > var->xres_virtual) {
        var->xres = var->xres_virtual;
    }
    if(var->xres + var->xoffset > var->xres_virtual) {
        var->xoffset = var->xres_virtual - var->xres;
    }
    if(var->yres + var->yoffset > var->yres_virtual) {
        var->yoffset = var->yres_virtual - var->yres;
    }
    return 0;
}

static int myfb_set_par(struct fb_info *info)
{
    struct myfb_par *par = info->par;

    fb_var_to_videomode(&par->mode, &info->var);
    par->app_virt->yoffset = info->var.yoffset = 0;
    par->bpp = info->var.bits_per_pixel;
    info->fix.visual = FB_VISUAL_TRUECOLOR;
    info->fix.line_length = (par->mode.xres * par->bpp) / 8;

    writel((5 << 8), iomm.debe + DEBE_LAY0_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY1_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY2_ATT_CTRL_REG1);
    writel((5 << 8), iomm.debe + DEBE_LAY3_ATT_CTRL_REG1);
    return 0;
}

static int myfb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
    struct myfb_par *par = info->par;

    switch(cmd) {
    case FBIO_WAITFORVSYNC:
        wait_for_vsync(par);
        break;
    case FBIO_SET_DEBE_MODE:
        if(arg == 1) {
            writel((7 << 8) | 4, iomm.debe + DEBE_LAY0_ATT_CTRL_REG1);
            writel((7 << 8) | 4, iomm.debe + DEBE_LAY1_ATT_CTRL_REG1);
            writel((7 << 8) | 4, iomm.debe + DEBE_LAY2_ATT_CTRL_REG1);
            writel((7 << 8) | 4, iomm.debe + DEBE_LAY3_ATT_CTRL_REG1);
        }
        else {
            writel((5 << 8), iomm.debe + DEBE_LAY0_ATT_CTRL_REG1);
            writel((5 << 8), iomm.debe + DEBE_LAY1_ATT_CTRL_REG1);
            writel((5 << 8), iomm.debe + DEBE_LAY2_ATT_CTRL_REG1);
            writel((5 << 8), iomm.debe + DEBE_LAY3_ATT_CTRL_REG1);
        }
        break;
    default:
        return -1;
    }
    return 0;
}

static int myfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
    const unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    const unsigned long size = vma->vm_end - vma->vm_start;

    if(offset + size > info->fix.smem_len) {
        return -EINVAL;
    }

    if(remap_pfn_range(vma, vma->vm_start, (info->fix.smem_start + offset) >> PAGE_SHIFT, size, vma->vm_page_prot)) {
        return -EAGAIN;
    }
    return 0;
}

static int myfb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct myfb_par *par = info->par;

    info->var.xoffset = var->xoffset;
    info->var.yoffset = var->yoffset;
    par->app_virt->yoffset = var->yoffset;
    return 0;
}

static struct fb_ops myfb_ops = {
    .owner          = THIS_MODULE,
    .fb_check_var   = myfb_check_var,
    .fb_set_par     = myfb_set_par,
    .fb_setcolreg   = myfb_setcolreg,
    .fb_pan_display = myfb_pan_display,
    .fb_ioctl       = myfb_ioctl,
    .fb_mmap        = myfb_mmap,

    .fb_fillrect  = sys_fillrect,
    .fb_copyarea  = sys_copyarea,
    .fb_imageblit = sys_imageblit,
};

static int myfb_probe(struct platform_device *device)
{
    int ret = 0;
    struct fb_info *info = NULL;
    struct myfb_par *par = NULL;
    struct fb_videomode *mode = NULL;

    mode = devm_kzalloc(&device->dev, sizeof(struct fb_videomode), GFP_KERNEL);
    if(mode == NULL) {
        return -ENOMEM;
    }
    mode->name = "320x240";
    mode->xres = 320;
    mode->yres = 240;
    mode->vmode = FB_VMODE_NONINTERLACED;
    pm_runtime_enable(&device->dev);
    pm_runtime_get_sync(&device->dev);

    info = framebuffer_alloc(sizeof(struct myfb_par), &device->dev);
    if(!info) {
        return -ENOMEM;
    }

    par = info->par;
    par->pdev = device;
    par->dev = &device->dev;
    par->bpp = 16;
    fb_videomode_to_var(&myfb_var, mode);

    par->vram_size = (320 * 240 * 2 * 4) + 4096;
    par->vram_virt = dma_alloc_coherent(NULL, par->vram_size, (resource_size_t *)&par->vram_phys, GFP_KERNEL | GFP_DMA);
    if(!par->vram_virt) {
        return -EINVAL;
    }
    info->screen_base = (char __iomem *)par->vram_virt;
    myfb_fix.smem_start = par->vram_phys;
    myfb_fix.smem_len = par->vram_size;
    myfb_fix.line_length = 320 * 2;
    par->app_virt = (struct myfb_app *)((uint8_t *)par->vram_virt + (320 * 240 * 2 * 4));

    par->v_palette_base = dma_alloc_coherent(NULL, PALETTE_SIZE, (resource_size_t *)&par->p_palette_base, GFP_KERNEL | GFP_DMA);
    if(!par->v_palette_base) {
        return -EINVAL;
    }
    memset(par->v_palette_base, 0, PALETTE_SIZE);
    myfb_var.grayscale = 0;
    myfb_var.bits_per_pixel = par->bpp;

    info->flags = FBINFO_FLAG_DEFAULT;
    info->fix = myfb_fix;
    info->var = myfb_var;
    info->fbops = &myfb_ops;
    info->pseudo_palette = par->pseudo_palette;
    info->fix.visual = (info->var.bits_per_pixel <= 8) ? FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR;
    ret = fb_alloc_cmap(&info->cmap, PALETTE_SIZE, 0);
    if(ret) {
        return -EINVAL;
    }
    info->cmap.len = 32;

    myfb_var.activate = FB_ACTIVATE_FORCE;
    fb_set_var(info, &myfb_var);
    dev_set_drvdata(&device->dev, info);
    if(register_framebuffer(info) < 0) {
        return -EINVAL;
    }

    mypar = par;
    mypar->lcdc_ready = 0;
    mypar->app_virt->vsync_count = 0;
    for(ret = 0; ret < of_clk_get_parent_count(device->dev.of_node); ret++) {
        clk_prepare_enable(of_clk_get(device->dev.of_node, ret));
    }


    setup_timer(&mytimer, lcd_delay_init, 0);
    mod_timer(&mytimer, jiffies + HZ);
    device_create_file(&device->dev, &dev_attr_variant);
    return 0;
}

static int myfb_remove(struct platform_device *dev)
{
    struct fb_info *info = dev_get_drvdata(&dev->dev);
    struct myfb_par *par = info->par;

    if(info) {
        del_timer(&mytimer);
        flush_scheduled_work();
        unregister_framebuffer(info);
        fb_dealloc_cmap(&info->cmap);
        dma_free_coherent(NULL, PALETTE_SIZE, par->v_palette_base, par->p_palette_base);
        dma_free_coherent(NULL, par->vram_size, par->vram_virt, par->vram_phys);
        pm_runtime_put_sync(&dev->dev);
        pm_runtime_disable(&dev->dev);
        framebuffer_release(info);
    }
    return 0;
}

static int myfb_suspend(struct platform_device *dev, pm_message_t state)
{
    struct fb_info *info = platform_get_drvdata(dev);

    console_lock();
    fb_set_suspend(info, 1);
    pm_runtime_put_sync(&dev->dev);
    console_unlock();
    return 0;
}

static int myfb_resume(struct platform_device *dev)
{
    struct fb_info *info = platform_get_drvdata(dev);

    console_lock();
    pm_runtime_get_sync(&dev->dev);
    fb_set_suspend(info, 0);
    console_unlock();
    return 0;
}

static const struct of_device_id fb_of_match[] = {{.compatible = "allwinner,suniv-f1c100s-tcon0",}, {}};
MODULE_DEVICE_TABLE(of, fb_of_match);

static struct platform_driver fb_driver = {
    .probe    = myfb_probe,
    .remove   = myfb_remove,
    .suspend  = myfb_suspend,
    .resume   = myfb_resume,
    .driver = {
        .name   = DRIVER_NAME,
        .owner  = THIS_MODULE,
        .of_match_table = of_match_ptr(fb_of_match),
    },
};

static void suniv_ioremap(void)
{
    iomm.ccm = (uint8_t *)ioremap(SUNIV_CCM_BASE, 1024);
    iomm.gpio = (uint8_t *)ioremap(SUNIV_GPIO_BASE, 1024);
    iomm.lcdc = (uint8_t *)ioremap(SUNIV_LCDC_BASE, 1024);
    iomm.debe = (uint8_t *)ioremap(SUNIV_DEBE_BASE, 4096);
}

static void suniv_iounmap(void)
{
    iounmap(iomm.ccm);
    iounmap(iomm.gpio);
    iounmap(iomm.lcdc);
    iounmap(iomm.debe);
}

static int __init fb_init(void)
{
    suniv_ioremap();
    return platform_driver_register(&fb_driver);
}

static void __exit fb_cleanup(void)
{
    suniv_iounmap();
    platform_driver_unregister(&fb_driver);
}

module_init(fb_init);
module_exit(fb_cleanup);

MODULE_DESCRIPTION("framebuffer driver for allwinner suniv handheld");
MODULE_AUTHOR("Steward Fu <steward.fu@gmail.com>");
MODULE_LICENSE("GPL");

