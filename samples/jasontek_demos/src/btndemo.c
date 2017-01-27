#include <zephyr.h>
#include "../boards/arm/jasontek_f103rb/board.h"
#include <device.h>
#include <gpio.h>
#include <misc/util.h>
#include <misc/printk.h>
#include "btndemo.h"

#ifdef USR_GPIO_NAME
#define PORT	USR_GPIO_NAME
#else
#error USR_GPIO_NAME needs to be set in board.h
#endif

#ifdef USR_GPIO_PIN
#define PIN     USR_GPIO_PIN
#else
#error USR_GPIO_PIN needs to be set in board.h
#endif

/* change this to enable pull-up/pull-down */
#define PULL_UP 0

/* change this to use a different interrupt trigger */
#define EDGE    (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)

/* delay between greetings (in ms) */
#define SLEEP_TIME 500

void button_pressed(struct device *gpiob, struct gpio_callback *cb,
		    uint32_t pins)
{
	printk("Button pressed at %d\n", k_cycle_get_32());
}

static struct gpio_callback gpio_cb;

void button_main(void)
{
	struct device *gpioc;

	printk("Press the user defined button on the board\n");
	gpioc = device_get_binding(PORT);
	if (!gpioc) {
		printk("error\n");
		return;
	}

	gpio_pin_configure(gpioc, PIN,GPIO_DIR_IN | GPIO_INT |  PULL_UP | EDGE);

	gpio_init_callback(&gpio_cb, button_pressed, BIT(PIN));

	gpio_add_callback(gpioc, &gpio_cb);
	gpio_pin_enable_callback(gpioc, PIN);

	while (1) {
		uint32_t val = 0;

		gpio_pin_read(gpioc, PIN, &val);
		k_sleep(SLEEP_TIME);
	}
}