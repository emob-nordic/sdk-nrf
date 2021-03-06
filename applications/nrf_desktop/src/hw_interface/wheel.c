/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <assert.h>

#include <zephyr/types.h>

#include <soc.h>
#include <device.h>
#include <sensor.h>
#include <gpio.h>

#include "event_manager.h"
#include "wheel_event.h"
#include "power_event.h"

#define MODULE wheel
#include "module_state_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_DESKTOP_WHEEL_LOG_LEVEL);


#define SENSOR_IDLE_TIMEOUT K_SECONDS(CONFIG_DESKTOP_WHEEL_SENSOR_IDLE_TIMEOUT)


enum state {
	STATE_DISABLED,
	STATE_ACTIVE_IDLE,
	STATE_ACTIVE,
	STATE_SUSPENDED
};

static const u32_t qdec_pin[] = {
	DT_NORDIC_NRF_QDEC_QDEC_0_A_PIN,
	DT_NORDIC_NRF_QDEC_QDEC_0_B_PIN
};

static const struct sensor_trigger qdec_trig = {
	.type = SENSOR_TRIG_DATA_READY,
	.chan = SENSOR_CHAN_ROTATION,
};

static struct device *qdec_dev;
static struct device *gpio_dev;
static struct gpio_callback gpio_cbs[2];
static struct k_spinlock lock;
static struct k_delayed_work idle_timeout;
static bool qdec_triggered;
static enum state state;

static int enable_qdec(enum state next_state);


static void data_ready_handler(struct device *dev, struct sensor_trigger *trig)
{
	if (IS_ENABLED(CONFIG_ASSERT)) {
		k_spinlock_key_t key = k_spin_lock(&lock);

		__ASSERT_NO_MSG(state == STATE_ACTIVE);

		k_spin_unlock(&lock, key);
	}

	struct sensor_value value;

	int err = sensor_channel_get(qdec_dev, SENSOR_CHAN_ROTATION, &value);
	if (err) {
		LOG_ERR("Cannot get sensor value");
		return;
	}

	struct wheel_event *event = new_wheel_event();

	s32_t wheel = value.val1;

	if (!IS_ENABLED(CONFIG_DESKTOP_WHEEL_INVERT_AXIS)) {
		wheel *= -1;
	}

	static_assert(CONFIG_DESKTOP_WHEEL_SENSOR_VALUE_DIVIDER > 0,
		      "Divider must be non-negative");
	if (CONFIG_DESKTOP_WHEEL_SENSOR_VALUE_DIVIDER > 1) {
		wheel /= CONFIG_DESKTOP_WHEEL_SENSOR_VALUE_DIVIDER;
	}

	event->wheel = MAX(MIN(wheel, SCHAR_MAX), SCHAR_MIN);

	EVENT_SUBMIT(event);

	qdec_triggered = true;
}

static int wakeup_int_ctrl_nolock(bool enable)
{
	int err = 0;

	/* This must be done with irqs disabled to avoid pin callback
	 * being fired before others are still not set up.
	 */
	for (size_t i = 0; (i < ARRAY_SIZE(qdec_pin)) && !err; i++) {
		if (enable) {
			err = gpio_pin_enable_callback(gpio_dev, qdec_pin[i]);
		} else {
			err = gpio_pin_disable_callback(gpio_dev, qdec_pin[i]);
		}

		if (err) {
			LOG_ERR("Cannot control cb (pin:%zu)", i);
		}
	}

	return err;
}

static void wakeup_cb(struct device *gpio_dev, struct gpio_callback *cb,
		      u32_t pins)
{
	struct wake_up_event *event;
	int err;

	k_spinlock_key_t key = k_spin_lock(&lock);

	err = wakeup_int_ctrl_nolock(false);

	if (!err) {
		switch (state) {
		case STATE_ACTIVE_IDLE:
			err = enable_qdec(STATE_ACTIVE);
			break;

		case STATE_SUSPENDED:
			event = new_wake_up_event();
			EVENT_SUBMIT(event);
			break;

		case STATE_ACTIVE:
		case STATE_DISABLED:
		default:
			__ASSERT_NO_MSG(false);
			break;
		}
	}

	k_spin_unlock(&lock, key);

	if (err) {
		module_set_state(MODULE_STATE_ERROR);
	}
}

static int setup_wakeup(void)
{
	int err = gpio_pin_configure(gpio_dev, DT_NORDIC_NRF_QDEC_QDEC_0_ENABLE_PIN,
				     GPIO_DIR_OUT);
	if (err) {
		LOG_ERR("Cannot configure enable pin");
		return err;
	}

	err = gpio_pin_write(gpio_dev, DT_NORDIC_NRF_QDEC_QDEC_0_ENABLE_PIN, 0);
	if (err) {
		LOG_ERR("Failed to set enable pin");
		return err;
	}

	for (size_t i = 0; i < ARRAY_SIZE(qdec_pin); i++) {

		u32_t val;

		err = gpio_pin_read(gpio_dev, qdec_pin[i], &val);
		if (err) {
			LOG_ERR("Cannot read pin %zu", i);
			return err;
		}

		int flags = GPIO_DIR_IN | GPIO_INT | GPIO_INT_LEVEL;
		flags |= (val) ? GPIO_INT_ACTIVE_LOW : GPIO_INT_ACTIVE_HIGH;

		err = gpio_pin_configure(gpio_dev, qdec_pin[i], flags);
		if (err) {
			LOG_ERR("Cannot configure pin %zu", i);
			return err;
		}
	}

	err = wakeup_int_ctrl_nolock(true);

	return err;
}

static int enable_qdec(enum state next_state)
{
	__ASSERT_NO_MSG(next_state == STATE_ACTIVE);

	int err = device_set_power_state(qdec_dev, DEVICE_PM_ACTIVE_STATE,
					 NULL, NULL);
	if (err) {
		LOG_ERR("Cannot activate QDEC");
		return err;
	}

	err = sensor_trigger_set(qdec_dev, (struct sensor_trigger *)&qdec_trig,
				 data_ready_handler);
	if (err) {
		LOG_ERR("Cannot setup trigger");
	} else {
		state = next_state;
		if (SENSOR_IDLE_TIMEOUT > 0) {
			qdec_triggered = false;
			k_delayed_work_submit(&idle_timeout,
					      SENSOR_IDLE_TIMEOUT);
		}
	}

	return err;
}

static int disable_qdec(enum state next_state)
{
	if (SENSOR_IDLE_TIMEOUT > 0) {
		__ASSERT_NO_MSG((next_state == STATE_ACTIVE_IDLE) ||
				(next_state == STATE_SUSPENDED));
	} else {
		__ASSERT_NO_MSG(next_state == STATE_SUSPENDED);
	}

	int err = sensor_trigger_set(qdec_dev,
				     (struct sensor_trigger *)&qdec_trig, NULL);
	if (err) {
		LOG_ERR("Cannot disable trigger");
		return err;
	}

	err = device_set_power_state(qdec_dev, DEVICE_PM_SUSPEND_STATE,
				     NULL, NULL);
	if (err) {
		LOG_ERR("Cannot suspend QDEC");
	} else {
		err = setup_wakeup();
		if (!err) {
			if (SENSOR_IDLE_TIMEOUT > 0) {
				k_delayed_work_cancel(&idle_timeout);
			}
			state = next_state;
		}
	}

	return err;
}

static void idle_timeout_fn(struct k_work *work)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	__ASSERT_NO_MSG(state == STATE_ACTIVE);

	if (!qdec_triggered) {
		int err = disable_qdec(STATE_ACTIVE_IDLE);

		if (err) {
			module_set_state(MODULE_STATE_ERROR);
		}
	} else {
		qdec_triggered = false;
		k_delayed_work_submit(&idle_timeout, SENSOR_IDLE_TIMEOUT);
	}

	k_spin_unlock(&lock, key);
}

static int init(void)
{
	__ASSERT_NO_MSG(state == STATE_DISABLED);

	if (SENSOR_IDLE_TIMEOUT > 0) {
		k_delayed_work_init(&idle_timeout, idle_timeout_fn);
	}

	qdec_dev = device_get_binding(DT_NORDIC_NRF_QDEC_QDEC_0_LABEL);
	if (!qdec_dev) {
		LOG_ERR("Cannot get QDEC device");
		return -ENXIO;
	}

	gpio_dev = device_get_binding(DT_GPIO_P0_DEV_NAME);
	if (!gpio_dev) {
		LOG_ERR("Cannot get GPIO device");
		return -ENXIO;
	}

	static_assert(ARRAY_SIZE(qdec_pin) == ARRAY_SIZE(gpio_cbs),
		      "Invalid array size");
	int err = 0;

	for (size_t i = 0; (i < ARRAY_SIZE(qdec_pin)) && !err; i++) {
		gpio_init_callback(&gpio_cbs[i], wakeup_cb, BIT(qdec_pin[i]));
		err = gpio_add_callback(gpio_dev, &gpio_cbs[i]);
		if (err) {
			LOG_ERR("Cannot configure cb (pin:%zu)", i);
		}
	}

	return err;
}

static bool event_handler(const struct event_header *eh)
{
	if (is_module_state_event(eh)) {
		struct module_state_event *event = cast_module_state_event(eh);

		if (check_state(event, MODULE_ID(main), MODULE_STATE_READY)) {
			int err = init();

			if (!err) {
				k_spinlock_key_t key = k_spin_lock(&lock);

				err = enable_qdec(STATE_ACTIVE);

				k_spin_unlock(&lock, key);
			}

			if (!err) {
				module_set_state(MODULE_STATE_READY);
			} else {
				module_set_state(MODULE_STATE_ERROR);
			}

			return false;
		}

		return false;
	}

	if (is_wake_up_event(eh)) {
		int err;

		k_spinlock_key_t key = k_spin_lock(&lock);

		switch (state) {
		case STATE_SUSPENDED:
			err = wakeup_int_ctrl_nolock(false);
			if (!err) {
				err = enable_qdec(STATE_ACTIVE);
			}

			if (!err) {
				module_set_state(MODULE_STATE_READY);
			} else {
				module_set_state(MODULE_STATE_ERROR);
			}
			break;

		case STATE_ACTIVE:
		case STATE_ACTIVE_IDLE:
			/* No action */
			break;

		case STATE_DISABLED:
		default:
			__ASSERT_NO_MSG(false);
			break;
		}

		k_spin_unlock(&lock, key);

		return false;
	}

	if (is_power_down_event(eh)) {
		int err;

		k_spinlock_key_t key = k_spin_lock(&lock);

		switch (state) {
		case STATE_ACTIVE:
			err = disable_qdec(STATE_SUSPENDED);

			if (!err) {
				module_set_state(MODULE_STATE_STANDBY);
			} else {
				module_set_state(MODULE_STATE_ERROR);
			}
			break;

		case STATE_ACTIVE_IDLE:
			state = STATE_SUSPENDED;
			break;

		case STATE_SUSPENDED:
			/* No action */
			break;

		case STATE_DISABLED:
		default:
			__ASSERT_NO_MSG(false);
			break;
		}

		k_spin_unlock(&lock, key);

		return false;
	}

	/* If event is unhandled, unsubscribe. */
	__ASSERT_NO_MSG(false);

	return false;
}
EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, module_state_event);
EVENT_SUBSCRIBE(MODULE, wake_up_event);
EVENT_SUBSCRIBE_EARLY(MODULE, power_down_event);
