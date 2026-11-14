// SPDX-License-Identifier: GPL-2.0
/*
 * BeagleBone Black - 5643AS 4-digit 7-segment display driver
 *
 * 5643AS is a bare common-cathode module with no driver IC:
 *   8 segment lines (a-g, dp) + 4 digit commons = 12 GPIOs.
 * Only one digit can be driven at a time, so a kernel thread
 * scans D1 -> D2 -> D3 -> D4 (1ms per digit, 250Hz per digit)
 * and persistence of vision makes all 4 digits look lit.
 *
 * The external interface is unchanged from the single-LED days:
 *   /dev/bbb_led, write "1"/"0", BBB_LED_IOC_SET/GET/TOGGLE.
 *   state 0 (BBB_LED_STATE_BLANK) = display blanked (LED OFF)
 *   state 1 (BBB_LED_STATE_ON)    = display shows " On " (LED ON)
 *   state 2 (BBB_LED_STATE_ERR)   = display shows "Err " (bad command)
 * So the user application keeps using LED ON / LED OFF over TCP
 * and sends state 2 when it rejects a command.
 *
 * Hardware notes:
 *   - Segments are active-high (common cathode): GPIO high = lit.
 *   - Each segment line needs its own 220-330 ohm resistor.
 *   - A digit common sinks the sum of up to 8 segment currents,
 *     far more than a BBB GPIO can handle. Drive each digit pin
 *     through an NPN transistor (or ULN2803): BBB GPIO high then
 *     selects the digit. The DT GPIO_ACTIVE_* flags absorb the
 *     polarity, so this driver always uses logical 1 = selected.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>   /* gpiod API (레거시 gpio API는 사용 금지) */
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include "bbb_led.h"

#define BBB_SEG_NR_SEGMENTS	8	/* a, b, c, d, e, f, g, dp */
#define BBB_SEG_NR_DIGITS	4	/* D1, D2, D3, D4 */
#define BBB_SEG_SCAN_US		1000	/* 1ms per digit -> 250Hz refresh */

struct bbb_led {
	struct gpio_descs *seg;		/* seg-gpios: a..g, dp */
	struct gpio_descs *digit;	/* digit-gpios: D1..D4 */
	struct miscdevice misc;
	struct mutex lock;
	struct task_struct *scan;	/* multiplexing thread */
	int value;			/* BBB_LED_STATE_*: 0=blank, 1=" On ", 2="Err " */
	unsigned int cur;		/* digit currently being scanned */
};

/* Segment bits: bit0=a, bit1=b, bit2=c, bit3=d, bit4=e, bit5=f, bit6=g, bit7=dp */
#define BBB_SEG_CHAR_O		0x3f	/* a,b,c,d,e,f */
#define BBB_SEG_CHAR_N		0x54	/* c,e,g       */
#define BBB_SEG_CHAR_E		0x79	/* a,d,e,f,g   */
#define BBB_SEG_CHAR_R		0x50	/* e,g (lowercase r) */
#define BBB_SEG_CHAR_BLANK	0x00

/* Text shown per display state, D1 -> D4 (left to right). */
static const u8 bbb_seg_texts[][BBB_SEG_NR_DIGITS] = {
	[BBB_LED_STATE_BLANK] = {
		BBB_SEG_CHAR_BLANK, BBB_SEG_CHAR_BLANK,
		BBB_SEG_CHAR_BLANK, BBB_SEG_CHAR_BLANK
	},	/* LED OFF: nothing lit */
	[BBB_LED_STATE_ON] = {
		BBB_SEG_CHAR_BLANK, BBB_SEG_CHAR_O,
		BBB_SEG_CHAR_N, BBB_SEG_CHAR_BLANK
	},	/* " On " */
	[BBB_LED_STATE_ERR] = {
		BBB_SEG_CHAR_E, BBB_SEG_CHAR_R,
		BBB_SEG_CHAR_R, BBB_SEG_CHAR_BLANK
	},	/* "Err " */
};

static void bbb_led_blank(struct bbb_led *led)
{
	int i;

	for (i = 0; i < BBB_SEG_NR_DIGITS; i++)
		gpiod_set_value_cansleep(led->digit->desc[i], 0);
	for (i = 0; i < BBB_SEG_NR_SEGMENTS; i++)
		gpiod_set_value_cansleep(led->seg->desc[i], 0);
}

/*
 * Multiplexing loop. Runs in process context so _cansleep GPIO
 * accessors and usleep_range() are fine here.
 *
 * Each step: blank all digits first (prevents ghosting), apply
 * the segment pattern, then enable only the current digit.
 */
static int bbb_led_scan(void *data)
{
	struct bbb_led *led = data;
	u8 pattern;
	int i, value;

	while (!kthread_should_stop()) {
		mutex_lock(&led->lock);
		value = led->value;
		mutex_unlock(&led->lock);

		if (value >= BBB_LED_STATE_BLANK && value <= BBB_LED_STATE_ERR)
			pattern = bbb_seg_texts[value][led->cur];
		else
			pattern = BBB_SEG_CHAR_BLANK;

		for (i = 0; i < BBB_SEG_NR_DIGITS; i++)
			gpiod_set_value_cansleep(led->digit->desc[i], 0);

		for (i = 0; i < BBB_SEG_NR_SEGMENTS; i++)
			gpiod_set_value_cansleep(led->seg->desc[i],
						 (pattern >> i) & 1);

		/* Blank digits draw nothing; skip selecting them. */
		if (pattern)
			gpiod_set_value_cansleep(led->digit->desc[led->cur], 1);

		led->cur = (led->cur + 1) % BBB_SEG_NR_DIGITS;
		usleep_range(BBB_SEG_SCAN_US, BBB_SEG_SCAN_US + 200);
	}

	bbb_led_blank(led);
	return 0;
}

/*
 * Only records the requested state; the scan thread does the
 * actual GPIO work. Safe to call from any file operation.
 */
static int bbb_led_set(struct bbb_led *led, int value)
{
	if (value < BBB_LED_STATE_BLANK || value > BBB_LED_STATE_ERR)
		return -EINVAL;

	mutex_lock(&led->lock);
	led->value = value;
	mutex_unlock(&led->lock);
	return 0;
}

static int bbb_led_get(struct bbb_led *led)
{
	int value;

	mutex_lock(&led->lock);
	value = led->value;
	mutex_unlock(&led->lock);
	return value;
}

static ssize_t bbb_led_write(struct file *f, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct bbb_led *led = container_of(f->private_data, struct bbb_led, misc);
	char c;
	int state, ret;

	if (!count || copy_from_user(&c, buf, 1))
		return -EFAULT;
	if (c == '0')
		state = BBB_LED_STATE_BLANK;
	else if (c == '1')
		state = BBB_LED_STATE_ON;
	else if (c == '2' || c == 'E')
		state = BBB_LED_STATE_ERR;
	else
		return -EINVAL;

	ret = bbb_led_set(led, state);
	return ret ? ret : (ssize_t)count;
}

static ssize_t bbb_led_read(struct file *f, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct bbb_led *led = container_of(f->private_data, struct bbb_led, misc);
	char str[2] = { '0' + bbb_led_get(led), '\n' };

	return simple_read_from_buffer(buf, count, ppos, str, sizeof(str));
}

static long bbb_led_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct bbb_led *led = container_of(f->private_data, struct bbb_led, misc);
	int value;

	switch (cmd) {
	case BBB_LED_IOC_SET:
		if (copy_from_user(&value, (int __user *)arg, sizeof(value)))
			return -EFAULT;
		return bbb_led_set(led, value);	/* range checked in bbb_led_set() */
	case BBB_LED_IOC_GET:
		value = bbb_led_get(led);
		return copy_to_user((int __user *)arg, &value, sizeof(value)) ? -EFAULT : 0;
	case BBB_LED_IOC_TOGGLE:
		/* ON <-> BLANK; from ERR, toggling turns the display on. */
		return bbb_led_set(led, bbb_led_get(led) == BBB_LED_STATE_ON ?
					BBB_LED_STATE_BLANK : BBB_LED_STATE_ON);
	}
	return -ENOTTY;
}

static const struct file_operations bbb_led_fops = {
	.owner = THIS_MODULE,
	.write = bbb_led_write,
	.read = bbb_led_read,
	.unlocked_ioctl = bbb_led_ioctl,
	.llseek = no_llseek,
};

static int bbb_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bbb_led *led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	int ret;

	if (!led)
		return -ENOMEM;

	/* DT의 seg-gpios (8개: a..g, dp), digit-gpios (4개: D1..D4) */
	led->seg = devm_gpiod_get_array(dev, "seg", GPIOD_OUT_LOW);
	if (IS_ERR(led->seg))
		return dev_err_probe(dev, PTR_ERR(led->seg), "failed to get seg gpios\n");
	if (led->seg->ndescs != BBB_SEG_NR_SEGMENTS)
		return dev_err_probe(dev, -EINVAL, "seg-gpios must have %d entries\n",
				     BBB_SEG_NR_SEGMENTS);

	led->digit = devm_gpiod_get_array(dev, "digit", GPIOD_OUT_LOW);
	if (IS_ERR(led->digit))
		return dev_err_probe(dev, PTR_ERR(led->digit), "failed to get digit gpios\n");
	if (led->digit->ndescs != BBB_SEG_NR_DIGITS)
		return dev_err_probe(dev, -EINVAL, "digit-gpios must have %d entries\n",
				     BBB_SEG_NR_DIGITS);

	mutex_init(&led->lock);
	led->value = 0;
	led->cur = 0;
	led->misc.minor = MISC_DYNAMIC_MINOR;
	led->misc.name = "bbb_led";          /* → /dev/bbb_led */
	led->misc.fops = &bbb_led_fops;
	led->misc.parent = dev;
	platform_set_drvdata(pdev, led);

	ret = misc_register(&led->misc);
	if (ret)
		return ret;

	led->scan = kthread_run(bbb_led_scan, led, "bbb_led_scan");
	if (IS_ERR(led->scan)) {
		ret = PTR_ERR(led->scan);
		led->scan = NULL;
		misc_deregister(&led->misc);
		return dev_err_probe(dev, ret, "failed to start scan thread\n");
	}

	dev_info(dev, "5643AS 4-digit 7-segment display ready (/dev/%s)\n",
		 led->misc.name);
	return 0;
}

static int bbb_led_remove(struct platform_device *pdev)
{
	struct bbb_led *led = platform_get_drvdata(pdev);

	bbb_led_set(led, 0);
	if (led->scan)
		kthread_stop(led->scan);	/* stops after blanking */
	misc_deregister(&led->misc);
	return 0;
}

static const struct of_device_id bbb_led_of_match[] = {
	{ .compatible = "bbb,ext-led" },
	{ }
};
MODULE_DEVICE_TABLE(of, bbb_led_of_match);

static struct platform_driver bbb_led_driver = {
	.probe = bbb_led_probe,
	.remove = bbb_led_remove,
	.driver = { .name = "bbb-led", .of_match_table = bbb_led_of_match },
};
module_platform_driver(bbb_led_driver);
MODULE_DESCRIPTION("BeagleBone Black 5643AS 4-digit 7-segment display driver");
MODULE_LICENSE("GPL");
