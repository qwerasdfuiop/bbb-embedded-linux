#pragma once
#include <linux/ioctl.h>
#define BBB_LED_IOC_MAGIC  'L'
#define BBB_LED_IOC_SET    _IOW(BBB_LED_IOC_MAGIC, 0x00, int)  /* BBB_LED_STATE_* */
#define BBB_LED_IOC_GET    _IOR(BBB_LED_IOC_MAGIC, 0x01, int)
#define BBB_LED_IOC_TOGGLE _IO(BBB_LED_IOC_MAGIC, 0x02)

/* Display states for the 5643AS 7-segment display (/dev/bbb_led) */
#define BBB_LED_STATE_BLANK	0	/* nothing lit (LED OFF) */
#define BBB_LED_STATE_ON	1	/* shows " On " (LED ON) */
#define BBB_LED_STATE_ERR	2	/* shows "Err " (invalid command) */
