/**
 * @file
 * @brief Bluetooth BAP Broadcast Sink Sample LC3
 *
 * This files handles all the USB related functionality to audio out for the Sample
 *
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/sys_clock.h>
#include <zephyr/toolchain.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>

#include <sample_usbd.h>

#include "lc3.h"
#include "usb.h"

LOG_MODULE_REGISTER(usb, CONFIG_LOG_DEFAULT_LEVEL);

#define USB_ENQUEUE_COUNT        30U /* 30 times 1ms frames => 30ms */
#define USB_FRAME_DURATION_US    1000U
#define USB_SAMPLE_CNT           ((USB_FRAME_DURATION_US * USB_SAMPLE_RATE_HZ) / USEC_PER_SEC)
#define USB_BYTES_PER_SAMPLE     sizeof(int16_t)
#define USB_MONO_FRAME_SIZE      (USB_SAMPLE_CNT * USB_BYTES_PER_SAMPLE)
#define USB_CHANNELS             2U
#define USB_STEREO_FRAME_SIZE    (USB_MONO_FRAME_SIZE * USB_CHANNELS)
#define USB_OUT_RING_BUF_SIZE    (CONFIG_BT_ISO_RX_BUF_COUNT * LC3_MAX_NUM_SAMPLES_STEREO)
#define USB_IN_RING_BUF_SIZE     (USB_MONO_FRAME_SIZE * USB_ENQUEUE_COUNT)

#define IN_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(in_terminal))

struct decoded_sdu {
	int16_t right_frames[CONFIG_MAX_CODEC_FRAMES_PER_SDU][LC3_MAX_NUM_SAMPLES_MONO];
	int16_t left_frames[CONFIG_MAX_CODEC_FRAMES_PER_SDU][LC3_MAX_NUM_SAMPLES_MONO];
	size_t right_frames_cnt;
	size_t left_frames_cnt;
	size_t mono_frames_cnt;
	uint32_t ts;
} decoded_sdu;

RING_BUF_DECLARE(usb_out_ring_buf, USB_OUT_RING_BUF_SIZE);
K_MEM_SLAB_DEFINE_STATIC(usb_out_buf_pool, ROUND_UP(USB_STEREO_FRAME_SIZE, UDC_BUF_GRANULARITY),
			 USB_ENQUEUE_COUNT, UDC_BUF_ALIGN);
static volatile bool terminal_enabled;

/* USB consumer callback, called every 1ms, consumes data from ring-buffer */
static void uac2_sof_cb(const struct device *dev, void *user_data)
{
	void *pcm_buf;
	uint32_t size;
	int err;

	if (!terminal_enabled) {
		/* Simply discard the data then */
		(void)ring_buf_get(&usb_out_ring_buf, NULL, USB_STEREO_FRAME_SIZE);
		return;
	}

	err = k_mem_slab_alloc(&usb_out_buf_pool, &pcm_buf, K_NO_WAIT);
	if (err != 0) {
		LOG_WRN("Could not allocate pcm_buf");
		return;
	}

	size = ring_buf_get(&usb_out_ring_buf, pcm_buf, USB_STEREO_FRAME_SIZE);
	if (size != USB_STEREO_FRAME_SIZE) {
		/* If we could not fill the buffer, zero-fill the rest (possibly all) */
		memset(((uint8_t *)pcm_buf) + size, 0, USB_STEREO_FRAME_SIZE - size);
	}

	if (CONFIG_INFO_REPORTING_INTERVAL > 0) {
		if (size != 0) {
			static size_t cnt;

			if (++cnt % (CONFIG_INFO_REPORTING_INTERVAL * 10) == 0U) {
				LOG_INF("[%zu]: Sending USB audio", cnt);
			}
		} else {
			static size_t cnt;

			if (++cnt % (CONFIG_INFO_REPORTING_INTERVAL * 10) == 0U) {
				LOG_INF("[%zu]: Sending empty USB audio", cnt);
			}
		}
	}

	err = usbd_uac2_send(dev, IN_TERMINAL_ID, pcm_buf, USB_STEREO_FRAME_SIZE);
	if (err != 0) {
		if (CONFIG_INFO_REPORTING_INTERVAL > 0) {
			static size_t cnt;

			if (cnt++ % (CONFIG_INFO_REPORTING_INTERVAL * 10) == 0) {
				LOG_ERR("[%zu]: Failed to send USB audio: %d", cnt, err);
			}
		}

		k_mem_slab_free(&usb_out_buf_pool, pcm_buf);
	} /* USB owns the buffer which will be released in uac2_buf_release_cb */
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal, void *buf,
				void *user_data)
{
	k_mem_slab_free(&usb_out_buf_pool, buf);
}

static void terminal_update_cb(const struct device *dev, uint8_t terminal, bool enabled,
			       bool microframes, void *user_data)
{
	terminal_enabled = enabled;
}

static void usb_send_frames_to_usb(void)
{
	const bool is_left_only =
		decoded_sdu.right_frames_cnt == 0U && decoded_sdu.mono_frames_cnt == 0U;
	const bool is_right_only =
		decoded_sdu.left_frames_cnt == 0U && decoded_sdu.mono_frames_cnt == 0U;
	const bool is_mono_only =
		decoded_sdu.left_frames_cnt == 0U && decoded_sdu.right_frames_cnt == 0U;
	const bool is_single_channel = is_left_only || is_right_only || is_mono_only;
	const size_t frame_cnt =
		MAX(decoded_sdu.mono_frames_cnt,
		    MAX(decoded_sdu.left_frames_cnt, decoded_sdu.right_frames_cnt));
	static size_t cnt;

	/* Send frames to USB - If we only have a single channel we mix it to stereo */
	for (size_t i = 0U; i < frame_cnt; i++) {
		static int16_t stereo_frame[LC3_MAX_NUM_SAMPLES_STEREO];
		const int16_t *right_frame = decoded_sdu.right_frames[i];
		const int16_t *left_frame = decoded_sdu.left_frames[i];
		const int16_t *mono_frame = decoded_sdu.left_frames[i]; /* use left as mono */
		static size_t fail_cnt;
		uint32_t rb_size;

		/* Not enough space to store data */
		if (ring_buf_space_get(&usb_out_ring_buf) < sizeof(stereo_frame)) {
			if (CONFIG_INFO_REPORTING_INTERVAL > 0 &&
			    (fail_cnt % CONFIG_INFO_REPORTING_INTERVAL) == 0U) {
				LOG_WRN("[%zu] Could not send more than %zu frames to USB",
					fail_cnt, i);
			}

			fail_cnt++;

			break;
		}

		fail_cnt = 0U;

		/* Generate the stereo frame
		 *
		 * If we only have single channel then we mix that to stereo
		 */
		for (int j = 0; j < LC3_MAX_NUM_SAMPLES_MONO; j++) {
			if (is_single_channel) {
				int16_t sample = 0;

				/* Mix to stereo as LRLRLRLR */
				if (is_left_only) {
					sample = left_frame[j];
				} else if (is_right_only) {
					sample = right_frame[j];
				} else if (is_mono_only) {
					sample = mono_frame[j];
				}

				stereo_frame[j * 2] = sample;
				stereo_frame[j * 2 + 1] = sample;
			} else {
				stereo_frame[j * 2] = left_frame[j];
				stereo_frame[j * 2 + 1] = right_frame[j];
			}
		}

		rb_size = ring_buf_put(&usb_out_ring_buf, (uint8_t *)stereo_frame,
				       sizeof(stereo_frame));
		if (rb_size != sizeof(stereo_frame)) {
			LOG_WRN("Failed to put frame on USB ring buf");

			break;
		}
	}

	if (CONFIG_INFO_REPORTING_INTERVAL > 0 && (++cnt % CONFIG_INFO_REPORTING_INTERVAL) == 0U) {
		LOG_INF("[%zu]: Sending %u USB audio frame", cnt, frame_cnt);
	}

	usb_clear_frames_to_usb();
}

static bool ts_overflowed(uint32_t ts)
{
	/* If the timestamp is a factor of 10 in difference, then we assume that TS overflowed
	 * We cannot simply check if `ts < decoded_sdu.ts` as that could also indicate old data
	 */
	return ((uint64_t)ts * 10 < decoded_sdu.ts);
}

int usb_add_frame_to_usb(enum bt_audio_location chan_allocation, const int16_t *frame,
			 size_t frame_size, uint32_t ts)
{
	const bool is_left = (chan_allocation & BT_AUDIO_LOCATION_FRONT_LEFT) != 0;
	const bool is_right = (chan_allocation & BT_AUDIO_LOCATION_FRONT_RIGHT) != 0;
	const bool is_mono = chan_allocation == BT_AUDIO_LOCATION_MONO_AUDIO;
	const uint8_t ts_jitter_us = 100; /* timestamps may have jitter */
	static size_t cnt;

	if (!terminal_enabled) {
		/* Simply discard the data then */
		/* TODO: Consider if we still want to decode the incoming audio */
		return 0;
	}

	if (CONFIG_INFO_REPORTING_INTERVAL > 0 && (++cnt % CONFIG_INFO_REPORTING_INTERVAL) == 0U) {
		LOG_INF("[%zu]: Adding USB audio frame", cnt);
	}

	if (frame_size > LC3_MAX_NUM_SAMPLES_MONO * sizeof(int16_t) || frame_size == 0U) {
		LOG_DBG("Invalid frame of size %zu", frame_size);

		return -EINVAL;
	}

	if (bt_audio_get_chan_count(chan_allocation) != 1) {
		LOG_DBG("Invalid channel allocation %d", chan_allocation);

		return -EINVAL;
	}

	if (((is_left || is_right) && decoded_sdu.mono_frames_cnt != 0) ||
	    (is_mono &&
	     (decoded_sdu.left_frames_cnt != 0U || decoded_sdu.right_frames_cnt != 0U))) {
		LOG_DBG("Cannot mix and match mono with left or right");

		return -EINVAL;
	}

	/* Check if the frame can be combined with a previous frame from another channel, of if
	 * we have to send previous data to USB and then store the current frame
	 *
	 * This is done by comparing the timestamps of the frames, and in the case that they are the
	 * same, there are additional checks to see if we have received more left than right frames,
	 * in which case we also send existing data
	 */

	if (ts + ts_jitter_us < decoded_sdu.ts && !ts_overflowed(ts)) {
		/* Old data, discard */
		return -ENOEXEC;
	} else if (ts > decoded_sdu.ts + ts_jitter_us || ts_overflowed(ts)) {
		/* We are getting new data - Send existing data to ring buffer */
		usb_send_frames_to_usb();
	} else { /* same timestamp */
		bool send = false;

		if (is_left && decoded_sdu.left_frames_cnt > decoded_sdu.right_frames_cnt) {
			/* We are receiving left again before a right, send to USB */
			send = true;
		} else if (is_right && decoded_sdu.right_frames_cnt > decoded_sdu.left_frames_cnt) {
			/* We are receiving right again before a left, send to USB */
			send = true;
		} else if (is_mono) {
			/* always send mono as it comes */
			send = true;
		}

		if (send) {
			usb_send_frames_to_usb();
		}
	}

	if (is_left) {
		if (decoded_sdu.left_frames_cnt >= ARRAY_SIZE(decoded_sdu.left_frames)) {
			LOG_WRN("Could not add more left frames");

			return -ENOMEM;
		}

		memcpy(decoded_sdu.left_frames[decoded_sdu.left_frames_cnt++], frame, frame_size);
	} else if (is_right) {
		if (decoded_sdu.right_frames_cnt >= ARRAY_SIZE(decoded_sdu.right_frames)) {
			LOG_WRN("Could not add more right frames");

			return -ENOMEM;
		}

		memcpy(decoded_sdu.right_frames[decoded_sdu.right_frames_cnt++], frame, frame_size);
	} else if (is_mono) {
		/* Use left as mono*/
		if (decoded_sdu.mono_frames_cnt >= ARRAY_SIZE(decoded_sdu.left_frames)) {
			LOG_WRN("Could not add more mono frames");

			return -ENOMEM;
		}

		memcpy(decoded_sdu.left_frames[decoded_sdu.mono_frames_cnt++], frame, frame_size);
	} else {
		/* Unsupported channel */
		LOG_DBG("Unsupported channel %d", chan_allocation);

		return -EINVAL;
	}

	decoded_sdu.ts = ts;

	return 0;
}

void usb_clear_frames_to_usb(void)
{
	decoded_sdu.mono_frames_cnt = 0U;
	decoded_sdu.right_frames_cnt = 0U;
	decoded_sdu.left_frames_cnt = 0U;
	decoded_sdu.ts = 0U;
}

int usb_init(void)
{
	const struct device *mic_dev = DEVICE_DT_GET(DT_NODELABEL(uac2_microphone));
	static struct uac2_ops usb_audio_ops = {
		.sof_cb = uac2_sof_cb,
		.buf_release_cb = uac2_buf_release_cb,
		.terminal_update_cb = terminal_update_cb,
	};
	struct usbd_context *sample_usbd;
	static bool initialized;
	int err;

	if (initialized) {
		return -EALREADY;
	}

	if (!device_is_ready(mic_dev)) {
		LOG_ERR("Cannot get USB Microphone Device");
		return -EIO;
	}

	usbd_uac2_set_ops(mic_dev, &usb_audio_ops, NULL);

	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		return -ENODEV;
	}

	err = usbd_enable(sample_usbd);
	if (err != 0) {
		return err;
	}

	LOG_INF("USB initialized");
	initialized = true;

	return 0;
}
