/*
 * Cloud REST module — sends tracker records to the rideyourstyle tracking REST API.
 *
 * Recording path (runs in connected AND disconnected state):
 *   location_chan LOCATION_GNSS_DATA
 *     → combine GNSS + cached env/battery into compact tracker_record
 *     → publish tracker_record_chan  →  storage module persists to flash
 *     → cache as last_known_position for the next status PUT
 *
 * Sending path (triggered by main.c every CONFIG_APP_CLOUD_UPDATE_INTERVAL_SECONDS):
 *   STORAGE_BATCH_AVAILABLE / STORAGE_BATCH_EMPTY
 *     → one PUT /trackers/{id}   (status: lastPosition + battery; once per batch)
 *     → one POST /tracks/positions per stored record (peek-and-ack protocol)
 *     → on HTTP 2xx: STORAGE_BATCH_ACK  (record deleted from flash)
 *     → on error:    STORAGE_BATCH_CLOSE (record kept, retry next interval)
 *
 * Sleep path (triggered by CLOUD_GOING_TO_SLEEP from main.c before the batch):
 *   → sets going_to_sleep flag; next send_status(NULL) uses "noMotionSleep"
 *   → NETWORK_DISCONNECT is issued by sleeping_entry in main.c after the batch
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
	int battery_soc;  /* 0–100 %, -1 if not yet received */
	bool has_battery;
} sensor_cache;

/* Last GNSS fix — used as lastPosition in the status PUT */
static struct tracker_record last_known_position;
static bool last_known_position_valid;

/* Tracker ID: last 8 digits of the modem IMEI (unique serial number portion) */
#define TRACKER_ID_LEN 9  /* 8 chars + null */
static char tracker_id[TRACKER_ID_LEN];

/* Watchdog channel ID — made file-scope so state handlers can feed it during batch loops */
static int cloud_task_wdt_id = -1;

/* Set to true by the CLOUD_GOING_TO_SLEEP handler before a sleep-induced batch send.
 * Consumed by send_status(NULL) to select "noMotionSleep" instead of "active". */
static bool going_to_sleep;

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
	bool batch_status_sent;   /* status PUT sent for current batch session */
	bool batch_fetch_config;  /* server flagged fetchConfig:true in this batch */
	bool batch_first_record;  /* true for the initial BATCH_AVAILABLE after REQUEST;
				   * more_data is not reliable there — storage doesn't set it */
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
			LOG_INF("HTTP %d", rsp->http_status_code);
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

/* Open a TCP (or TLS) connection to the configured REST server.
 * Returns a connected socket fd ≥ 0, or -1 on error. Caller must zsock_close(). */
static int cloud_connect(void)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res;
	char port_str[8];
	int sock;
	int err;

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
		if (!err) {
			err = zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
					       &peer_verify, sizeof(peer_verify));
		}
		if (err) {
			LOG_ERR("TLS setup failed: %d", errno);
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

	return sock;
}

/* Fetch new config from server and publish CLOUD_CONFIG_UPDATE.
 * Called when the status PUT response contains "fetchConfig":true.
 * Uses GET /trackers/{id}/config?ack=true so the server marks the config as fetched.
 */
static void fetch_and_apply_config(void)
{
	static uint8_t cfg_recv_buf[512];
	char url_buf[sizeof(CONFIG_APP_CLOUD_REST_API_PATH) + TRACKER_ID_LEN +
		     sizeof("/config?ack=true") + 2];
	char api_key_hdr[sizeof("X-Api-Key: \r\n") + sizeof(CONFIG_APP_CLOUD_REST_API_KEY)];
	const char *api_key_hdrs[2] = { NULL, NULL };
	int http_status = 0;
	int sock;
	int err;

	if (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1) {
		snprintk(api_key_hdr, sizeof(api_key_hdr), "X-Api-Key: %s\r\n",
			 CONFIG_APP_CLOUD_REST_API_KEY);
		api_key_hdrs[0] = api_key_hdr;
	}

	snprintk(url_buf, sizeof(url_buf), "%s/%s/config?ack=true",
		 CONFIG_APP_CLOUD_REST_API_PATH, tracker_id);

	sock = cloud_connect();
	if (sock < 0) {
		LOG_ERR("fetch_config: connect failed");
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

/* PUT /trackers/{id} — status update.
 * tracker_status: NULL = auto (active/activeWithoutFix), or explicit string.
 * Includes lastPosition (if a GNSS fix has been cached) and battery.
 * Returns 0: success, 1: fetchConfig:true, -1: error.
 */
static int send_status(const char *tracker_status)
{
	static char json_buf[CONFIG_APP_CLOUD_REST_JSON_BUFFER_SIZE];
	static uint8_t recv_buf[256];
	char url_buf[sizeof(CONFIG_APP_CLOUD_REST_API_PATH) + TRACKER_ID_LEN + 2];
	char api_key_hdr[sizeof("X-Api-Key: \r\n") + sizeof(CONFIG_APP_CLOUD_REST_API_KEY)];
	const char *api_key_hdrs[2] = { NULL, NULL };
	char timestamp[32];
	int http_status = 0;
	int json_len;
	bool comma = false;
	int sock;
	int err;

	if (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1) {
		snprintk(api_key_hdr, sizeof(api_key_hdr), "X-Api-Key: %s\r\n",
			 CONFIG_APP_CLOUD_REST_API_KEY);
		api_key_hdrs[0] = api_key_hdr;
	}

	snprintk(url_buf, sizeof(url_buf), "%s/%s", CONFIG_APP_CLOUD_REST_API_PATH, tracker_id);

	/* Determine tracker status: explicit > sleep flag > auto from fix availability */
	const char *status = tracker_status;

	if (status == NULL) {
		if (going_to_sleep) {
			status = "noMotionSleep";
			going_to_sleep = false;
		} else {
			status = last_known_position_valid ? "active" : "activeWithoutFix";
		}
	}

	/* Build JSON body */
	json_len = snprintk(json_buf, sizeof(json_buf), "{");

	json_len += snprintk(json_buf + json_len, sizeof(json_buf) - json_len,
			     "\"trackerStatus\":\"%s\"", status);
	comma = true;

	if (last_known_position_valid) {
		format_timestamp(timestamp, sizeof(timestamp), &last_known_position);
		json_len += snprintk(json_buf + json_len, sizeof(json_buf) - json_len,
				     "%s\"lastPosition\":{"
				     "\"sampleTimestamp\":\"%s\","
				     "\"latitude\":%.7f,"
				     "\"longitude\":%.7f,"
				     "\"gnssAcc\":%d,"
				     "\"pressure\":%d,"
				     "\"speed\":0,"
				     "\"temperature\":%d"
				     "}",
				     comma ? "," : "",
				     timestamp,
				     last_known_position.latitude,
				     last_known_position.longitude,
				     (int)last_known_position.accuracy_m,
				     last_known_position.pressure_pa,
				     last_known_position.temperature_c);
		comma = true;
	}

	if (sensor_cache.has_battery) {
		json_len += snprintk(json_buf + json_len, sizeof(json_buf) - json_len,
				     "%s\"battery\":%d",
				     comma ? "," : "",
				     sensor_cache.battery_mv);
		if (sensor_cache.battery_soc >= 0) {
			json_len += snprintk(json_buf + json_len, sizeof(json_buf) - json_len,
					     ",\"batterySoc\":%d", sensor_cache.battery_soc);
		}
		comma = true;
	}

	json_len += snprintk(json_buf + json_len, sizeof(json_buf) - json_len, "}");

	if (json_len < 0 || json_len >= (int)sizeof(json_buf)) {
		LOG_ERR("send_status: JSON buffer overflow (%d bytes)", json_len);
		return -1;
	}

	LOG_DBG("PUT %s (%s)", url_buf, status);
	LOG_DBG("  body=%s", json_buf);

	sock = cloud_connect();
	if (sock < 0) {
		return -1;
	}

	memset(recv_buf, 0, sizeof(recv_buf));

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
		LOG_ERR("send_status: http_client_req failed: %d", err);
		return -1;
	}

	if (http_status >= 200 && http_status < 300) {
		bool fetch_config = strstr((char *)recv_buf, "\"fetchConfig\":true") != NULL ||
				    strstr((char *)recv_buf, "\"fetchConfig\": true") != NULL;

		return fetch_config ? 1 : 0;
	}

	return -1;
}

/* POST /tracks/positions — send accumulated positions JSON body.
 * json_body must be a complete, null-terminated JSON string.
 * Returns 0: success, -1: error.
 */
static int send_positions_batch_post(const char *json_body)
{
	static uint8_t recv_buf[128];
	char api_key_hdr[sizeof("X-Api-Key: \r\n") + sizeof(CONFIG_APP_CLOUD_REST_API_KEY)];
	const char *api_key_hdrs[2] = { NULL, NULL };
	int http_status = 0;
	int sock;
	int err;

	if (sizeof(CONFIG_APP_CLOUD_REST_API_KEY) > 1) {
		snprintk(api_key_hdr, sizeof(api_key_hdr), "X-Api-Key: %s\r\n",
			 CONFIG_APP_CLOUD_REST_API_KEY);
		api_key_hdrs[0] = api_key_hdr;
	}

	LOG_DBG("POST %s — %d bytes", CONFIG_APP_CLOUD_REST_POSITIONS_API_PATH,
		(int)strlen(json_body));

	sock = cloud_connect();
	if (sock < 0) {
		return -1;
	}

	memset(recv_buf, 0, sizeof(recv_buf));

	struct http_request req = {
		.method             = HTTP_POST,
		.url                = CONFIG_APP_CLOUD_REST_POSITIONS_API_PATH,
		.protocol           = "HTTP/1.1",
		.host               = CONFIG_APP_CLOUD_REST_SERVER_HOST,
		.content_type_value = "application/json",
		.payload            = json_body,
		.payload_len        = strlen(json_body),
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
		LOG_ERR("send_positions_batch_post: http_client_req failed: %d", err);
		return -1;
	}

	return (http_status >= 200 && http_status < 300) ? 0 : -1;
}

/* Positions JSON accumulation buffer — filled record by record, POSTed when batch ends */
static char positions_batch_buf[CONFIG_APP_CLOUD_REST_POSITIONS_BUFFER_SIZE];
static int  positions_batch_len;
static int  positions_batch_count;

/* Maximum bytes one position record can occupy (including leading comma) */
#define POSITIONS_RECORD_MAX_BYTES 220

/* Append one position record to the ongoing batch buffer.
 * Starts a new batch automatically; handles overflow by flushing a partial batch first.
 */
static void positions_batch_append(const struct tracker_record *rec)
{
	char timestamp[32];

	format_timestamp(timestamp, sizeof(timestamp), rec);

	/* Flush partial batch if insufficient space for the next record + closing "]}" */
	if (positions_batch_count > 0 &&
	    (positions_batch_len + POSITIONS_RECORD_MAX_BYTES + 2) >=
		    (int)sizeof(positions_batch_buf)) {
		LOG_WRN("Positions buffer full at %d records — flushing early",
			positions_batch_count);
		snprintk(positions_batch_buf + positions_batch_len,
			 sizeof(positions_batch_buf) - positions_batch_len, "]}");
		send_positions_batch_post(positions_batch_buf);
		positions_batch_len = 0;
		positions_batch_count = 0;
	}

	if (positions_batch_count == 0) {
		positions_batch_len = snprintk(positions_batch_buf,
					       sizeof(positions_batch_buf),
					       "{\"trackerId\":\"%s\",\"positions\":[",
					       tracker_id);
	} else {
		positions_batch_buf[positions_batch_len++] = ',';
	}

	positions_batch_len += snprintk(
		positions_batch_buf + positions_batch_len,
		sizeof(positions_batch_buf) - positions_batch_len,
		"{\"sampleTimestamp\":\"%s\","
		"\"latitude\":%.7f,"
		"\"longitude\":%.7f,"
		"\"gnssAcc\":%d,"
		"\"pressure\":%d,"
		"\"speed\":0,"
		"\"temperature\":%d}",
		timestamp,
		rec->latitude,
		rec->longitude,
		(int)rec->accuracy_m,
		rec->pressure_pa,
		rec->temperature_c);

	positions_batch_count++;
}

/* Close and POST the accumulated positions batch, then reset the buffer. */
static void positions_batch_flush(void)
{
	if (positions_batch_count == 0) {
		return;
	}

	snprintk(positions_batch_buf + positions_batch_len,
		 sizeof(positions_batch_buf) - positions_batch_len, "]}");

	LOG_INF("Posting %d position(s)", positions_batch_count);
	send_positions_batch_post(positions_batch_buf);

	positions_batch_len = 0;
	positions_batch_count = 0;
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

	/* Going to sleep while disconnected: just take modem offline (no HTTP possible) */
	if (s->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)s->msg_buf;

		if (msg->type == CLOUD_GOING_TO_SLEEP) {
			struct network_msg net_msg = { .type = NETWORK_DISCONNECT };

			zbus_chan_pub(&network_chan, &net_msg, K_SECONDS(1));
			return SMF_EVENT_HANDLED;
		}
	}

	/* Record GNSS fixes and keep sensor cache fresh even when offline */
	return handle_common_channels(s);
}

/* Handle channels that must be processed in BOTH connected and disconnected state:
 *   - location_chan: record GNSS fix to flash (offline buffering) + cache last position
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
				.battery_soc     = sensor_cache.has_battery
						   ? sensor_cache.battery_soc : -1,
				.year            = gnss->datetime.year,
				.month           = gnss->datetime.month,
				.day             = gnss->datetime.day,
				.hour            = gnss->datetime.hour,
				.minute          = gnss->datetime.minute,
				.second          = gnss->datetime.second,
				.ms              = gnss->datetime.ms,
				.timestamp_valid = gnss->datetime.valid ? 1u : 0u,
			};

			/* Cache for the next status PUT */
			last_known_position = rec;
			last_known_position_valid = true;

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
			/* BME680 driver reports SENSOR_CHAN_PRESS in kPa; convert to Pa */
			sensor_cache.pressure_pa         = (int)(msg->pressure * 1000.0);
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
			sensor_cache.battery_soc = (int)msg->percentage;
			sensor_cache.has_battery = true;
			LOG_DBG("Cached battery: %d mV  %d%%",
				sensor_cache.battery_mv, sensor_cache.battery_soc);
		}
		return SMF_EVENT_HANDLED;
	}
#endif

	return SMF_EVENT_PROPAGATE;
}

static void init_tracker_id(void)
{
	/* Called on first LTE connect — modem is ready for AT commands by then.
	 * Derive tracker ID from modem IMEI: last 8 digits (SNR + check digit),
	 * unique per physical device. */
	char imei_buf[HW_ID_LEN];
	int err = hw_id_get(imei_buf, sizeof(imei_buf));

	if (err == 0) {
		size_t imei_len = strlen(imei_buf);
		const char *src = imei_len >= 8 ? &imei_buf[imei_len - 8] : imei_buf;

		strncpy(tracker_id, src, sizeof(tracker_id) - 1);
		tracker_id[sizeof(tracker_id) - 1] = '\0';
	} else {
		LOG_ERR("hw_id_get failed (%d) — using fallback ID", err);
		strncpy(tracker_id, CONFIG_APP_CLOUD_REST_TRACKER_ID_FALLBACK,
			sizeof(tracker_id) - 1);
		tracker_id[sizeof(tracker_id) - 1] = '\0';
	}

	LOG_INF("Tracker ID: %s", tracker_id);
}

static void state_connected_entry(void *o)
{
	static bool initial_config_fetched;
	struct cloud_state *s = (struct cloud_state *)o;

	/* Reset batch-send state for the new connection */
	s->batch_status_sent = false;
	s->batch_fetch_config = false;

	if (tracker_id[0] == '\0') {
		init_tracker_id();
	}

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

	/* Storage batch:
	 *   1. First STORAGE_BATCH_AVAILABLE in a session: send PUT status (once per session).
	 *   2. Every STORAGE_BATCH_AVAILABLE: read record, POST positions, ACK.
	 *   3. STORAGE_BATCH_EMPTY / STORAGE_BATCH_ERROR: send PUT status if not yet sent,
	 *      fetch config if signalled, close batch.
	 */
	if (s->chan == &storage_chan) {
		const struct storage_msg *smsg = (const struct storage_msg *)s->msg_buf;

		if (smsg->type == STORAGE_BATCH_AVAILABLE) {
			struct storage_data_item item;
			struct storage_msg reply = { .session_id = smsg->session_id,
						     .data_type  = STORAGE_TYPE_TRACKER };
			int read_err;

			/* Detect new batch session → reset per-session flags */
			if (smsg->session_id != s->batch_session_id) {
				s->batch_status_sent = false;
				s->batch_fetch_config = false;
				s->batch_session_id = smsg->session_id;
				s->batch_first_record = true;
				positions_batch_len = 0;
				positions_batch_count = 0;
			}

			/* Send status PUT once per batch session */
			if (!s->batch_status_sent) {
				bool sleep_send = going_to_sleep; /* capture before consumed */
				int fc = send_status(NULL);

				s->batch_status_sent = true;
				if (fc < 0) {
					LOG_WRN("Status PUT failed — closing batch for retry");
					reply.type = STORAGE_BATCH_CLOSE;
					zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
					s->batch_session_id = 0;
					s->batch_status_sent = false;
					s->batch_first_record = false;
					return SMF_EVENT_HANDLED;
				}
				if (sleep_send) {
					/* noMotionSleep: close batch immediately.
					 * Stored positions are sent on the next wake-up
					 * when the device is connected anyway. */
					LOG_DBG("noMotionSleep — skipping positions, closing batch");
					reply.type = STORAGE_BATCH_CLOSE;
					zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
					s->batch_session_id = 0;
					s->batch_status_sent = false;
					s->batch_first_record = false;
					return SMF_EVENT_HANDLED;
				}
				if (fc == 1) {
					s->batch_fetch_config = true;
				}
			}

			read_err = storage_batch_read(&item, K_MSEC(500));
			if (read_err != 0) {
				LOG_ERR("storage_batch_read failed (%d), closing batch", read_err);
				reply.type = STORAGE_BATCH_CLOSE;
				zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
				s->batch_session_id = 0;
				s->batch_status_sent = false;
				s->batch_first_record = false;
				return SMF_EVENT_HANDLED;
			}

			/* Accumulate record into positions batch */
			positions_batch_append(&item.data.TRACKER);

			/* Flush when the storage module confirms this is the last record.
			 * The initial BATCH_AVAILABLE (after REQUEST) always has more_data=false
			 * regardless of record count — only trust it for post-ACK messages
			 * (batch_first_record=false). Remaining records are flushed in the
			 * STORAGE_BATCH_CLOSE handler below. */
			bool is_last = !smsg->more_data && !s->batch_first_record;

			s->batch_first_record = false;

			if (is_last) {
				positions_batch_flush();
				if (s->batch_fetch_config) {
					fetch_and_apply_config();
				}
				s->batch_status_sent = false;
				s->batch_fetch_config = false;
				s->batch_session_id = 0;
			}

			reply.type = STORAGE_BATCH_ACK;
			zbus_chan_pub(&storage_chan, &reply, K_SECONDS(1));
			return SMF_EVENT_HANDLED;
		}

		/* Storage sent BATCH_CLOSE after the last ACK (no more records).
		 * Flush any positions accumulated but not yet POSTed (single-record
		 * batch, or the is_last path wasn't reached due to more_data quirks). */
		if (smsg->type == STORAGE_BATCH_CLOSE) {
			if (positions_batch_count > 0) {
				positions_batch_flush();
			}
			if (s->batch_fetch_config) {
				fetch_and_apply_config();
			}
			s->batch_status_sent = false;
			s->batch_fetch_config = false;
			s->batch_first_record = false;
			positions_batch_len = 0;
			positions_batch_count = 0;
			s->batch_session_id = 0;
			return SMF_EVENT_PROPAGATE;  /* main.c handles the state transition */
		}

		if (smsg->type == STORAGE_BATCH_EMPTY || smsg->type == STORAGE_BATCH_ERROR) {
			struct storage_msg close = {
				.type       = STORAGE_BATCH_CLOSE,
				.session_id = smsg->session_id,
			};

			/* No records — still send status for this interval */
			if (!s->batch_status_sent) {
				int fc = send_status(NULL);

				if (fc == 1) {
					fetch_and_apply_config();
				}
			} else if (s->batch_fetch_config) {
				fetch_and_apply_config();
			}

			s->batch_status_sent = false;
			s->batch_fetch_config = false;
			s->batch_first_record = false;
			positions_batch_len = 0;
			positions_batch_count = 0;
			zbus_chan_pub(&storage_chan, &close, K_SECONDS(1));
			s->batch_session_id = 0;
			return SMF_EVENT_HANDLED;
		}

		return SMF_EVENT_PROPAGATE;
	}

	/* Shadow/config requests and sleep notification */
	if (s->chan == &cloud_chan) {
		const struct cloud_msg *msg = (const struct cloud_msg *)s->msg_buf;
		struct cloud_msg resp = { 0 };

		/* Device going to sleep: flag the next send_status(NULL) to use
		 * "noMotionSleep". The status is sent during the upcoming batch,
		 * and the network disconnect is issued by sleeping_entry in main.c. */
		if (msg->type == CLOUD_GOING_TO_SLEEP) {
			going_to_sleep = true;
			return SMF_EVENT_HANDLED;
		}

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
			LOG_ERR("task_wdt_feed, errrrror: %d", err);
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
