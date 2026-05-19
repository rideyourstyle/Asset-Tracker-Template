/*
 * Cloud REST module - sends GNSS position data to the rideyourstyle tracking REST API.
 *
 * This module replaces the nRF Cloud CoAP cloud module. It:
 *  - Subscribes to location_chan for GNSS position data
 *  - Optionally caches environmental (temp/pressure) and power (battery) data
 *  - POSTs position data to POST /v1/tracks/positions via plain HTTP
 *  - Publishes CLOUD_CONNECTED / CLOUD_DISCONNECTED based on network state
 *  - Stubs out the FOTA channel (FOTA is not supported without nRF Cloud)
 *  - Responds to shadow requests with "empty" responses so main.c can proceed
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/http/method.h>
#include <hw_id.h>

#include "app_common.h"
#include "cloud.h"
#include "fota.h"
#include "network.h"
#include "location.h"

#if defined(CONFIG_APP_ENVIRONMENTAL)
#include "environmental.h"
#endif
#if defined(CONFIG_APP_POWER)
#include "power.h"
#endif

LOG_MODULE_REGISTER(cloud, CONFIG_APP_CLOUD_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Stub fota_chan: the FOTA module is disabled without nRF Cloud.
 * We define the channel here and publish FOTA_MODULE_READY at startup so that
 * main.c's module-ready check does not block indefinitely.
 */
ZBUS_CHAN_DEFINE(fota_chan,
		struct fota_msg,
		NULL,
		NULL,
		ZBUS_OBSERVERS_EMPTY,
		ZBUS_MSG_INIT(0));

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(cloud_subscriber);

/* Channels this module subscribes to */
#define CHANNEL_LIST(X)							\
	X(network_chan,		struct network_msg)			\
	X(cloud_chan,		struct cloud_msg)			\
	X(location_chan,	struct location_msg)			\
	IF_ENABLED(CONFIG_APP_ENVIRONMENTAL,				\
		(X(environmental_chan,	struct environmental_msg)))	\
	IF_ENABLED(CONFIG_APP_POWER,					\
		(X(power_chan,		struct power_msg)))

#define MAX_MSG_SIZE		MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, cloud_subscriber, 0);
CHANNEL_LIST(ADD_OBSERVERS)

/* Define cloud_chan - the public output channel of this module */
ZBUS_CHAN_DEFINE(cloud_chan,
		struct cloud_msg,
		NULL,
		NULL,
		ZBUS_OBSERVERS_EMPTY,
		ZBUS_MSG_INIT(0));

/* Cached sensor data - updated whenever environmental/power messages arrive */
static struct {
	int temperature_celsius;
	int pressure_pa;
	bool has_env;
	int battery_mv;
	bool has_battery;
} sensor_cache;

/* Tracker ID retrieved from modem IMEI at startup */
static char tracker_id[HW_ID_LEN];

/* -------------------------------------------------------------------------- */
/* State machine declarations                                                  */
/* -------------------------------------------------------------------------- */

static void state_disconnected_entry(void *o);
static enum smf_state_result state_disconnected_run(void *o);
static void state_connected_entry(void *o);
static enum smf_state_result state_connected_run(void *o);

enum cloud_module_state {
	STATE_DISCONNECTED,
	STATE_CONNECTED,
};

struct cloud_state {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
};

static const struct smf_state states[] = {
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		state_disconnected_entry, state_disconnected_run, NULL, NULL, NULL),
	[STATE_CONNECTED] = SMF_CREATE_STATE(
		state_connected_entry, state_connected_run, NULL, NULL, NULL),
};

/* -------------------------------------------------------------------------- */
/* HTTP helpers                                                                */
/* -------------------------------------------------------------------------- */

static void format_timestamp(char *buf, size_t len,
			      const struct location_datetime *dt)
{
	if (dt->valid) {
		snprintk(buf, len, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
			 dt->year, dt->month, dt->day,
			 dt->hour, dt->minute, dt->second, dt->ms);
	} else {
		strncpy(buf, "1970-01-01T00:00:00.000Z", len);
		buf[len - 1] = '\0';
	}
}

static int http_response_cb(struct http_response *rsp,
			    enum http_final_call final_data,
			    void *user_data)
{
	if (final_data == HTTP_DATA_FINAL) {
		if (rsp->http_status_code >= 200 && rsp->http_status_code < 300) {
			LOG_INF("Position accepted (HTTP %d)", rsp->http_status_code);
		} else {
			LOG_WRN("Server returned HTTP %d: %s",
				rsp->http_status_code, rsp->http_status);
		}
	}
	return 0;
}

static void send_position(const struct location_data *gnss_data)
{
	static char json_buf[CONFIG_APP_CLOUD_REST_JSON_BUFFER_SIZE];
	static uint8_t recv_buf[256];
	/* /v1/trackers/ + tracker_id (max HW_ID_LEN) + NUL */
	char url_buf[sizeof(CONFIG_APP_CLOUD_REST_API_PATH) + HW_ID_LEN + 2];

	/* Optional headers: API key (empty string = disabled, compile-time constant) */
	char api_key_hdr[sizeof("X-Api-Key: \r\n") + sizeof(CONFIG_APP_CLOUD_REST_API_KEY)];
	const char *api_key_hdrs[2] = { NULL, NULL };

	if (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1) {
		snprintk(api_key_hdr, sizeof(api_key_hdr), "X-Api-Key: %s\r\n",
			 CONFIG_APP_CLOUD_REST_API_KEY);
		api_key_hdrs[0] = api_key_hdr;
	}

	char timestamp[32];
	int json_len;
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	char port_str[8];
	int sock;
	int err;

	if (CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS > 0 &&
	    gnss_data->accuracy > CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS) {
		LOG_INF("Fix accuracy %.1f m worse than limit %d m, skipping",
			(double)gnss_data->accuracy,
			CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS);
		return;
	}

	format_timestamp(timestamp, sizeof(timestamp), &gnss_data->datetime);

	/* URL: /v1/trackers/{tracker_id} */
	snprintk(url_buf, sizeof(url_buf), "%s/%s",
		 CONFIG_APP_CLOUD_REST_API_PATH, tracker_id);

	/* Build flat JSON body (matches rideyourstyle tracker API) */
	json_len = snprintk(json_buf, sizeof(json_buf),
		"{"
		"\"sampleTimestamp\":\"%s\","
		"\"latitude\":%.7f,"
		"\"longitude\":%.7f,"
		"\"pressure\":%d,"
		"\"speed\":0,"
		"\"temperature\":%d,"
		"\"gnssAcc\":%d,"
		"\"battery\":%d,"
		"\"cellRssi\":0"
		"}",
		timestamp,
		gnss_data->latitude,
		gnss_data->longitude,
		sensor_cache.has_env ? sensor_cache.pressure_pa : 0,
		sensor_cache.has_env ? sensor_cache.temperature_celsius : 0,
		(int)gnss_data->accuracy,
		sensor_cache.has_battery ? sensor_cache.battery_mv : 0);

	if (json_len < 0 || json_len >= (int)sizeof(json_buf)) {
		LOG_ERR("JSON buffer too small (%d bytes needed)", json_len);
		return;
	}

	LOG_DBG("PUT %s  payload (%d bytes): %s", url_buf, json_len, json_buf);

	/* DNS lookup */
	snprintk(port_str, sizeof(port_str), "%d", CONFIG_APP_CLOUD_REST_SERVER_PORT);
	err = zsock_getaddrinfo(CONFIG_APP_CLOUD_REST_SERVER_HOST, port_str, &hints, &res);
	if (err) {
		LOG_ERR("DNS lookup for %s failed: %d",
			CONFIG_APP_CLOUD_REST_SERVER_HOST, err);
		return;
	}

	/* Create TCP socket */
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		zsock_freeaddrinfo(res);
		return;
	}

	/* Connect */
	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (err) {
		LOG_ERR("connect() to %s:%d failed: %d",
			CONFIG_APP_CLOUD_REST_SERVER_HOST,
			CONFIG_APP_CLOUD_REST_SERVER_PORT, errno);
		zsock_close(sock);
		return;
	}

	/* Send HTTP PUT using the Zephyr HTTP client stack */
	struct http_request req = {
		.method          = HTTP_PUT,
		.url             = url_buf,
		.protocol        = "HTTP/1.1",
		.host            = CONFIG_APP_CLOUD_REST_SERVER_HOST,
		.content_type_value = "application/json",
		.payload         = json_buf,
		.payload_len     = json_len,
		.response        = http_response_cb,
		.recv_buf        = recv_buf,
		.recv_buf_len    = sizeof(recv_buf),
		.optional_headers = (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1)
					? api_key_hdrs : NULL,
	};

	err = http_client_req(sock, &req,
			      CONFIG_APP_CLOUD_REST_HTTP_TIMEOUT_SECONDS * MSEC_PER_SEC,
			      NULL);
	if (err < 0) {
		LOG_ERR("http_client_req failed: %d", err);
	}

	LOG_DBG("Sent position: lat=%.5f lon=%.5f acc=%dm",
		gnss_data->latitude, gnss_data->longitude,
		(int)gnss_data->accuracy);

	zsock_close(sock);
}

/* -------------------------------------------------------------------------- */
/* State machine handlers                                                      */
/* -------------------------------------------------------------------------- */

static void state_disconnected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_DBG("Cloud disconnected");

	struct cloud_msg msg = { .type = CLOUD_DISCONNECTED };
	int err = zbus_chan_pub(&cloud_chan, &msg, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub CLOUD_DISCONNECTED, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_disconnected_run(void *o)
{
	struct cloud_state *s = (struct cloud_state *)o;

	if (s->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)s->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			smf_set_state(SMF_CTX(s), &states[STATE_CONNECTED]);
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_connected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_INF("Cloud connected (REST mode, server: %s)",
		CONFIG_APP_CLOUD_REST_SERVER_HOST);

	struct cloud_msg msg = { .type = CLOUD_CONNECTED };
	int err = zbus_chan_pub(&cloud_chan, &msg, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub CLOUD_CONNECTED, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static enum smf_state_result state_connected_run(void *o)
{
	struct cloud_state *s = (struct cloud_state *)o;

	/* Network disconnect → go back to disconnected */
	if (s->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)s->msg_buf;

		if (msg->type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(s), &states[STATE_DISCONNECTED]);
			return SMF_EVENT_HANDLED;
		}
		return SMF_EVENT_PROPAGATE;
	}

	/* GNSS location data → send HTTP POST */
	if (s->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)s->msg_buf;

		if (msg->type == LOCATION_GNSS_DATA) {
			send_position(&msg->gnss_data);
		}
		return SMF_EVENT_HANDLED;
	}

#if defined(CONFIG_APP_ENVIRONMENTAL)
	/* Cache environmental data for next position report */
	if (s->chan == &environmental_chan) {
		const struct environmental_msg *msg =
			(const struct environmental_msg *)s->msg_buf;

		if (msg->type == ENVIRONMENTAL_SENSOR_SAMPLE_RESPONSE) {
			sensor_cache.temperature_celsius = (int)msg->temperature;
			sensor_cache.pressure_pa         = (int)msg->pressure;
			sensor_cache.has_env             = true;
			LOG_DBG("Cached env data: temp=%d°C pressure=%d Pa",
				sensor_cache.temperature_celsius,
				sensor_cache.pressure_pa);
		}
		return SMF_EVENT_HANDLED;
	}
#endif

#if defined(CONFIG_APP_POWER)
	if (s->chan == &power_chan) {
		const struct power_msg *msg = (const struct power_msg *)s->msg_buf;

		if (msg->type == POWER_BATTERY_PERCENTAGE_SAMPLE_RESPONSE) {
			sensor_cache.battery_mv = (int)(msg->voltage * 1000);
			sensor_cache.has_battery = true;
			LOG_DBG("Cached battery: %d mV", sensor_cache.battery_mv);
		}
		return SMF_EVENT_HANDLED;
	}
#endif

	/* Shadow/config requests: respond with empty responses so main.c can proceed
	 * with its default configuration values.
	 */
	if (s->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)s->msg_buf;
		struct cloud_msg resp = { 0 };

		switch (msg->type) {
		case CLOUD_SHADOW_GET_DESIRED:
			resp.type = CLOUD_SHADOW_RESPONSE_EMPTY_DESIRED;
			zbus_chan_pub(&cloud_chan, &resp, K_SECONDS(1));
			return SMF_EVENT_HANDLED;

		case CLOUD_SHADOW_GET_DELTA:
			resp.type = CLOUD_SHADOW_RESPONSE_EMPTY_DELTA;
			zbus_chan_pub(&cloud_chan, &resp, K_SECONDS(1));
			return SMF_EVENT_HANDLED;

		default:
			/* CLOUD_PAYLOAD_JSON, CLOUD_SHADOW_SET_REPORTED_CONFIG, etc.
			 * are silently ignored in REST mode.
			 */
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

/* -------------------------------------------------------------------------- */
/* Watchdog callback                                                           */
/* -------------------------------------------------------------------------- */

static void task_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));
	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* -------------------------------------------------------------------------- */
/* Module thread                                                               */
/* -------------------------------------------------------------------------- */

static void cloud_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_CLOUD_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_CLOUD_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	struct cloud_state cloud_state_obj = { 0 };

	LOG_DBG("Cloud REST module started");

	/* Obtain tracker ID */
	if (IS_ENABLED(CONFIG_APP_CLOUD_REST_TRACKER_ID_OVERRIDE)) {
		strncpy(tracker_id, CONFIG_APP_CLOUD_REST_TRACKER_ID_FALLBACK,
			sizeof(tracker_id) - 1);
		tracker_id[sizeof(tracker_id) - 1] = '\0';
	} else {
		err = hw_id_get(tracker_id, sizeof(tracker_id));
		if (err) {
			LOG_WRN("hw_id_get failed (%d), using fallback ID", err);
			strncpy(tracker_id, CONFIG_APP_CLOUD_REST_TRACKER_ID_FALLBACK,
				sizeof(tracker_id) - 1);
			tracker_id[sizeof(tracker_id) - 1] = '\0';
		} else {
			tracker_id[sizeof(tracker_id) - 1] = '\0';
		}
	}

	LOG_INF("Tracker ID: %s", tracker_id);

	/* Publish FOTA_MODULE_READY so main.c can leave STATE_WAITING_FOR_MODULES_INIT.
	 * FOTA is not supported in REST mode but the channel must signal ready.
	 */
	struct fota_msg fota_ready_msg = { .type = FOTA_MODULE_READY };

	err = zbus_chan_pub(&fota_chan, &fota_ready_msg, K_SECONDS(5));
	if (err) {
		LOG_ERR("Failed to publish FOTA_MODULE_READY: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&cloud_state_obj), &states[STATE_DISCONNECTED]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&cloud_subscriber, &cloud_state_obj.chan,
					cloud_state_obj.msg_buf, zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = smf_run_state(SMF_CTX(&cloud_state_obj));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(cloud_module_thread_id,
		CONFIG_APP_CLOUD_THREAD_STACK_SIZE,
		cloud_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
