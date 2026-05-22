/*
 * Cloud REST module — sends tracker records to the rideyourstyle tracking REST API.
 *
 * Recording path (runs in connected AND disconnected state):
 *   location_chan LOCATION_GNSS_DATA
 *     → combine GNSS + cached env/battery into compact tracker_record
 *     → publish tracker_record_chan  →  storage module persists to flash
 *
 * Sending path (triggered by main.c every CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS):
 *   STORAGE_BATCH_AVAILABLE (one record at a time, peek-and-ack protocol)
 *     → HTTP PUT to dev.tracking.rideyourstyle.ch
 *     → on HTTP 2xx: STORAGE_BATCH_ACK  (record deleted from flash)
 *     → on error:    STORAGE_BATCH_CLOSE (record kept, retry next interval)
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
#include "tracker_record.h"
#include "storage.h"
#include "storage_data_types.h"

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

/* Channel published by this module: compact records ready for storage */
ZBUS_CHAN_DEFINE(tracker_record_chan,
		struct tracker_record,
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
	X(storage_chan,		struct storage_msg)			\
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

/* Watchdog channel ID — made file-scope so state handlers can feed it during batch loops */
static int cloud_task_wdt_id = -1;

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
	uint32_t batch_session_id;
};

static enum smf_state_result handle_common_channels(struct cloud_state *s);

static const struct smf_state states[] = {
	[STATE_DISCONNECTED] = SMF_CREATE_STATE(
		state_disconnected_entry, state_disconnected_run, NULL, NULL, NULL),
	[STATE_CONNECTED] = SMF_CREATE_STATE(
		state_connected_entry, state_connected_run, NULL, NULL, NULL),
};

/* -------------------------------------------------------------------------- */
/* HTTP helpers                                                                */
/* -------------------------------------------------------------------------- */

static void format_timestamp(char *buf, size_t len, const struct tracker_record *rec)
{
	if (rec->timestamp_valid) {
		snprintk(buf, len, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
			 rec->year, rec->month, rec->day,
			 rec->hour, rec->minute, rec->second, rec->ms);
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
		int *out_status = (int *)user_data;

		*out_status = rsp->http_status_code;

		if (rsp->http_status_code >= 200 && rsp->http_status_code < 300) {
			LOG_INF("Position accepted (HTTP %d)", rsp->http_status_code);
		} else {
			LOG_WRN("Server returned HTTP %d: %s",
				rsp->http_status_code, rsp->http_status);
		}
	}
	return 0;
}

/* Find integer value for "key" in a JSON string. Returns 0 on success. */
static int json_find_int(const char *json, const char *key, int32_t *out)
{
	char needle[48];
	const char *p;

	snprintk(needle, sizeof(needle), "\"%s\":", key);
	p = strstr(json, needle);
	if (!p) {
		return -ENOENT;
	}
	p += strlen(needle);
	while (*p == ' ') {
		p++;
	}
	if (*p < '0' || *p > '9') {
		return -EINVAL;
	}
	*out = 0;
	while (*p >= '0' && *p <= '9') {
		*out = *out * 10 + (*p - '0');
		p++;
	}
	return 0;
}

/* Fetch new config from server and publish CLOUD_CONFIG_UPDATE.
 * Called when the status PUT response contains "fetchConfig":true.
 * Uses GET /trackers/{id}/config?ack=true so the server marks the config as fetched.
 */
static void fetch_and_apply_config(void)
{
	static uint8_t cfg_recv_buf[512];
	char url_buf[sizeof(CONFIG_APP_CLOUD_REST_API_PATH) + HW_ID_LEN +
		     sizeof("/config?ack=true") + 2];
	char api_key_hdr[sizeof("X-Api-Key: \r\n") + sizeof(CONFIG_APP_CLOUD_REST_API_KEY)];
	const char *api_key_hdrs[2] = { NULL, NULL };
	int http_status = 0;
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res;
	char port_str[8];
	int sock;
	int err;

	if (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1) {
		snprintk(api_key_hdr, sizeof(api_key_hdr), "X-Api-Key: %s\r\n",
			 CONFIG_APP_CLOUD_REST_API_KEY);
		api_key_hdrs[0] = api_key_hdr;
	}

	snprintk(url_buf, sizeof(url_buf), "%s/%s/config?ack=true",
		 CONFIG_APP_CLOUD_REST_API_PATH, tracker_id);

	snprintk(port_str, sizeof(port_str), "%d", CONFIG_APP_CLOUD_REST_SERVER_PORT);
	err = zsock_getaddrinfo(CONFIG_APP_CLOUD_REST_SERVER_HOST, port_str, &hints, &res);
	if (err) {
		LOG_ERR("fetch_config: DNS lookup failed: %d", err);
		return;
	}

#if defined(CONFIG_APP_CLOUD_REST_TLS)
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
#else
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TCP);
#endif
	if (sock < 0) {
		LOG_ERR("fetch_config: socket() failed: %d", errno);
		zsock_freeaddrinfo(res);
		return;
	}

#if defined(CONFIG_APP_CLOUD_REST_TLS)
	{
		int peer_verify = TLS_PEER_VERIFY_NONE;

		err = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
				       CONFIG_APP_CLOUD_REST_SERVER_HOST,
				       sizeof(CONFIG_APP_CLOUD_REST_SERVER_HOST) - 1);
		if (err) {
			LOG_ERR("fetch_config: TLS_HOSTNAME failed: %d", errno);
			zsock_close(sock);
			zsock_freeaddrinfo(res);
			return;
		}
		err = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
				       &peer_verify, sizeof(peer_verify));
		if (err) {
			LOG_ERR("fetch_config: TLS_PEER_VERIFY failed: %d", errno);
			zsock_close(sock);
			zsock_freeaddrinfo(res);
			return;
		}
	}
#endif

	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (err) {
		LOG_ERR("fetch_config: connect() failed: %d", errno);
		zsock_close(sock);
		return;
	}

	memset(cfg_recv_buf, 0, sizeof(cfg_recv_buf));

	struct http_request req = {
		.method           = HTTP_GET,
		.url              = url_buf,
		.protocol         = "HTTP/1.1",
		.host             = CONFIG_APP_CLOUD_REST_SERVER_HOST,
		.response         = http_response_cb,
		.recv_buf         = cfg_recv_buf,
		.recv_buf_len     = sizeof(cfg_recv_buf),
		.optional_headers = (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1)
					? api_key_hdrs : NULL,
	};

	err = http_client_req(sock, &req,
			      CONFIG_APP_CLOUD_REST_HTTP_TIMEOUT_SECONDS * MSEC_PER_SEC,
			      &http_status);
	zsock_close(sock);

	if (err < 0 || http_status < 200 || http_status >= 300) {
		LOG_WRN("fetch_config: failed (err=%d, HTTP=%d)", err, http_status);
		return;
	}

	const char *body = strstr((char *)cfg_recv_buf, "{");

	if (!body) {
		LOG_WRN("fetch_config: no JSON body in response");
		return;
	}

	int32_t sample_interval = 0;
	int32_t transmit_interval = 0;

	json_find_int(body, "sampleInterval", &sample_interval);
	json_find_int(body, "transmitInterval", &transmit_interval);

	LOG_INF("Config fetched: sampleInterval=%d s, transmitInterval=%d s",
		sample_interval, transmit_interval);

	if (sample_interval > 0 && (3600 % sample_interval) != 0) {
		LOG_WRN("fetch_config: sampleInterval=%d not a divisor of 3600, ignoring",
			sample_interval);
		sample_interval = 0;
	}

	if (sample_interval == 0 && transmit_interval == 0) {
		LOG_DBG("fetch_config: no actionable values in response");
		return;
	}

	struct cloud_msg update = {
		.type = CLOUD_CONFIG_UPDATE,
		.config = {
			.sample_interval_sec   = (uint32_t)sample_interval,
			.transmit_interval_sec = (uint32_t)transmit_interval,
		},
	};

	int pub_err = zbus_chan_pub(&cloud_chan, &update, K_SECONDS(1));

	if (pub_err) {
		LOG_ERR("fetch_config: failed to publish CLOUD_CONFIG_UPDATE: %d", pub_err);
	}
}

/* Send one tracker_record via HTTPS PUT.
 * Returns  0: success, no config fetch needed
 * Returns  1: success, server signalled fetchConfig:true
 * Returns -1: error (socket, timeout, non-2xx)
 */
static int send_record(const struct tracker_record *rec)
{
	static char json_buf[CONFIG_APP_CLOUD_REST_JSON_BUFFER_SIZE];
	static uint8_t recv_buf[256];
	char url_buf[sizeof(CONFIG_APP_CLOUD_REST_API_PATH) + HW_ID_LEN + 2];
	char api_key_hdr[sizeof("X-Api-Key: \r\n") + sizeof(CONFIG_APP_CLOUD_REST_API_KEY)];
	const char *api_key_hdrs[2] = { NULL, NULL };
	char timestamp[32];
	int json_len;
	int http_status = 0;
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	char port_str[8];
	int sock;
	int err;

	if (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1) {
		snprintk(api_key_hdr, sizeof(api_key_hdr), "X-Api-Key: %s\r\n",
			 CONFIG_APP_CLOUD_REST_API_KEY);
		api_key_hdrs[0] = api_key_hdr;
	}

	format_timestamp(timestamp, sizeof(timestamp), rec);

	snprintk(url_buf, sizeof(url_buf), "%s/%s", CONFIG_APP_CLOUD_REST_API_PATH, tracker_id);

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
		rec->latitude,
		rec->longitude,
		rec->pressure_pa,
		rec->temperature_c,
		(int)rec->accuracy_m,
		rec->battery_mv);

	if (json_len < 0 || json_len >= (int)sizeof(json_buf)) {
		LOG_ERR("JSON buffer too small (%d bytes needed)", json_len);
		return -1;
	}

	LOG_DBG("PUT %s (%d bytes)", url_buf, json_len);
	LOG_DBG("  timestamp=%s", timestamp);
	LOG_DBG("  lat=%.7f  lon=%.7f  acc=%dm",
		rec->latitude, rec->longitude, (int)rec->accuracy_m);
	LOG_DBG("  temp=%d°C  pressure=%dPa  battery=%dmV",
		rec->temperature_c, rec->pressure_pa, rec->battery_mv);

	snprintk(port_str, sizeof(port_str), "%d", CONFIG_APP_CLOUD_REST_SERVER_PORT);
	err = zsock_getaddrinfo(CONFIG_APP_CLOUD_REST_SERVER_HOST, port_str, &hints, &res);
	if (err) {
		LOG_ERR("DNS lookup for %s failed: %d", CONFIG_APP_CLOUD_REST_SERVER_HOST, err);
		return -1;
	}

#if defined(CONFIG_APP_CLOUD_REST_TLS)
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TLS_1_2);
#else
	sock = zsock_socket(res->ai_family, res->ai_socktype, IPPROTO_TCP);
#endif
	if (sock < 0) {
		LOG_ERR("socket() failed: %d", errno);
		zsock_freeaddrinfo(res);
		return -1;
	}

#if defined(CONFIG_APP_CLOUD_REST_TLS)
	{
		int peer_verify = TLS_PEER_VERIFY_NONE;

		err = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
				       CONFIG_APP_CLOUD_REST_SERVER_HOST,
				       sizeof(CONFIG_APP_CLOUD_REST_SERVER_HOST) - 1);
		if (err) {
			LOG_ERR("TLS_HOSTNAME setsockopt failed: %d", errno);
			zsock_close(sock);
			zsock_freeaddrinfo(res);
			return -1;
		}
		err = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
				       &peer_verify, sizeof(peer_verify));
		if (err) {
			LOG_ERR("TLS_PEER_VERIFY setsockopt failed: %d", errno);
			zsock_close(sock);
			zsock_freeaddrinfo(res);
			return -1;
		}
	}
#endif

	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	if (err) {
		LOG_ERR("connect() to %s:%d failed: %d",
			CONFIG_APP_CLOUD_REST_SERVER_HOST,
			CONFIG_APP_CLOUD_REST_SERVER_PORT, errno);
		zsock_close(sock);
		return -1;
	}

	struct http_request req = {
		.method             = HTTP_PUT,
		.url                = url_buf,
		.protocol           = "HTTP/1.1",
		.host               = CONFIG_APP_CLOUD_REST_SERVER_HOST,
		.content_type_value = "application/json",
		.payload            = json_buf,
		.payload_len        = json_len,
		.response           = http_response_cb,
		.recv_buf           = recv_buf,
		.recv_buf_len       = sizeof(recv_buf),
		.optional_headers   = (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1)
					? api_key_hdrs : NULL,
	};

	err = http_client_req(sock, &req,
			      CONFIG_APP_CLOUD_REST_HTTP_TIMEOUT_SECONDS * MSEC_PER_SEC,
			      &http_status);
	zsock_close(sock);

	if (err < 0) {
		LOG_ERR("http_client_req failed: %d", err);
		return -1;
	}

	if (http_status >= 200 && http_status < 300) {
		LOG_DBG("Sent: lat=%.5f lon=%.5f acc=%dm",
			rec->latitude, rec->longitude, (int)rec->accuracy_m);

		/* Check if the server wants us to fetch a new config */
		bool fetch_config = strstr((char *)recv_buf, "\"fetchConfig\":true") != NULL ||
				    strstr((char *)recv_buf, "\"fetchConfig\": true") != NULL;

		return fetch_config ? 1 : 0;
	}

	return -1;
}

/* Fetch config from server on first connect — cloud config is the primary source.
 * Compile-time values (CONFIG_APP_SAMPLING_INTERVAL_SECONDS etc.) act as fallback
 * until this call succeeds. Uses ?ack=true to acknowledge any pending config change.
 * Best-effort: failures are logged but do not block normal operation.
 */
static void fetch_initial_config(void)
{
	LOG_INF("Fetching initial config from server (compile-time values are fallback)");
	fetch_and_apply_config();
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

	/* Record GNSS fixes and keep sensor cache fresh even when offline */
	return handle_common_channels(s);
}

/* Handle channels that must be processed in BOTH connected and disconnected state:
 *   - location_chan: record GNSS fix to flash (offline buffering)
 *   - environmental_chan: update sensor cache
 *   - power_chan: update battery cache
 * Returns SMF_EVENT_HANDLED if the message was consumed, SMF_EVENT_PROPAGATE otherwise.
 */
static enum smf_state_result handle_common_channels(struct cloud_state *s)
{
	if (s->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)s->msg_buf;

		if (msg->type == LOCATION_GNSS_DATA) {
			const struct location_data *gnss = &msg->gnss_data;

			if (CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS > 0 &&
			    gnss->accuracy > CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS) {
				LOG_INF("Fix accuracy %.1f m > limit %d m, discarding",
					(double)gnss->accuracy,
					CONFIG_APP_CLOUD_REST_GNSS_MIN_ACCURACY_METERS);
				return SMF_EVENT_HANDLED;
			}

			struct tracker_record rec = {
				.latitude        = gnss->latitude,
				.longitude       = gnss->longitude,
				.accuracy_m      = gnss->accuracy,
				.temperature_c   = sensor_cache.has_env
						   ? sensor_cache.temperature_celsius : 0,
				.pressure_pa     = sensor_cache.has_env
						   ? sensor_cache.pressure_pa : 0,
				.battery_mv      = sensor_cache.has_battery
						   ? sensor_cache.battery_mv : 0,
				.year            = gnss->datetime.year,
				.month           = gnss->datetime.month,
				.day             = gnss->datetime.day,
				.hour            = gnss->datetime.hour,
				.minute          = gnss->datetime.minute,
				.second          = gnss->datetime.second,
				.ms              = gnss->datetime.ms,
				.timestamp_valid = gnss->datetime.valid ? 1u : 0u,
			};

			int pub_err = zbus_chan_pub(&tracker_record_chan, &rec, K_SECONDS(1));

			if (pub_err) {
				LOG_ERR("Failed to publish tracker_record: %d", pub_err);
			} else {
				LOG_DBG("Stored: lat=%.5f lon=%.5f acc=%dm",
					rec.latitude, rec.longitude, (int)rec.accuracy_m);
			}
		}
		return SMF_EVENT_HANDLED;
	}

#if defined(CONFIG_APP_ENVIRONMENTAL)
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
			sensor_cache.battery_mv  = (int)(msg->voltage * 1000);
			sensor_cache.has_battery = true;
			LOG_DBG("Cached battery: %d mV", sensor_cache.battery_mv);
		}
		return SMF_EVENT_HANDLED;
	}
#endif

	return SMF_EVENT_PROPAGATE;
}

static void state_connected_entry(void *o)
{
	static bool initial_config_fetched;

	ARG_UNUSED(o);

	LOG_INF("Cloud connected (REST mode, server: %s)",
		CONFIG_APP_CLOUD_REST_SERVER_HOST);

	/* Publish CLOUD_CONNECTED first so main.c transitions to STATE_CONNECTED
	 * before CLOUD_CONFIG_UPDATE arrives (fetch_initial_config publishes it).
	 */
	struct cloud_msg msg = { .type = CLOUD_CONNECTED };
	int err = zbus_chan_pub(&cloud_chan, &msg, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub CLOUD_CONNECTED, error: %d", err);
		SEND_FATAL_ERROR();
	}

	/* Fetch cloud config once per boot. Compile-time values are the fallback
	 * until the server responds successfully.
	 */
	if (!initial_config_fetched) {
		initial_config_fetched = true;
		fetch_initial_config();
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

	/* GNSS recording and sensor cache — shared with disconnected state */
	{
		enum smf_state_result r = handle_common_channels(s);

		if (r == SMF_EVENT_HANDLED) {
			return SMF_EVENT_HANDLED;
		}
	}

	/* Storage batch: peek-and-ack — one record per BATCH_AVAILABLE message.
	 * Records stay in flash until ACKed (HTTP 2xx). On failure: CLOSE keeps the record.
	 */
	if (s->chan == &storage_chan) {
		const struct storage_msg *smsg = (const struct storage_msg *)s->msg_buf;

		if (smsg->type == STORAGE_BATCH_AVAILABLE) {
			struct storage_data_item item;
			struct storage_msg reply = { .session_id = smsg->session_id,
						     .data_type  = STORAGE_TYPE_TRACKER };
			int read_err;

			s->batch_session_id = smsg->session_id;

			read_err = storage_batch_read(&item, K_MSEC(500));
			if (read_err != 0) {
				LOG_ERR("storage_batch_read failed (%d), closing batch", read_err);
				reply.type = STORAGE_BATCH_CLOSE;
				zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
				s->batch_session_id = 0;
				return SMF_EVENT_HANDLED;
			}

			int send_result = send_record(&item.data.TRACKER);

			if (send_result < 0) {
				LOG_WRN("Send failed, keeping record for retry");
				reply.type = STORAGE_BATCH_CLOSE;
				zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
				s->batch_session_id = 0;
				return SMF_EVENT_HANDLED;
			}

			if (send_result == 1) {
				/* Server signalled fetchConfig:true — fetch after ACK */
				reply.type = STORAGE_BATCH_ACK;
				zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
				fetch_and_apply_config();
				return SMF_EVENT_HANDLED;
			}

			reply.type = STORAGE_BATCH_ACK;
			zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
			return SMF_EVENT_HANDLED;
		}

		if (smsg->type == STORAGE_BATCH_EMPTY || smsg->type == STORAGE_BATCH_ERROR) {
			struct storage_msg close = {
				.type       = STORAGE_BATCH_CLOSE,
				.session_id = smsg->session_id,
			};

			zbus_chan_pub(&storage_chan, &close, K_SECONDS(1));
			s->batch_session_id = 0;
			return SMF_EVENT_HANDLED;
		}

		return SMF_EVENT_PROPAGATE;
	}

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

	cloud_task_wdt_id = task_wdt_add(wdt_timeout_ms, task_wdt_callback,
					  (void *)k_current_get());
	if (cloud_task_wdt_id < 0) {
		LOG_ERR("task_wdt_add, error: %d", cloud_task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&cloud_state_obj), &states[STATE_DISCONNECTED]);

	while (true) {
		err = task_wdt_feed(cloud_task_wdt_id);
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
