/*
 * scrcpy source scaffold
 * Copyright (C) 2026 NanKill <nankill@nankill.xyz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>
 */

#include <obs-module.h>

#include <plugin-support.h>

#include "scrcpy-session.h"
#include "scrcpy-source.h"

#ifdef _WIN32
#include <stdio.h>
#endif

#define SETTING_ADB_PATH "adb_path"
#define SETTING_DEVICE_SERIAL "device_serial"
#define SETTING_SERVER_JAR_PATH "server_jar_path"
#define SETTING_SCRCPY_VERSION "scrcpy_version"
#define SETTING_LOCAL_PORT "local_port"
#define SETTING_VIDEO_CODEC "video_codec"
#define SETTING_VIDEO_BIT_RATE "video_bit_rate"
#define SETTING_MAX_SIZE "max_size"
#define SETTING_VIDEO_SOURCE "video_source"
#define SETTING_CAMERA_ID "camera_id"
#define SETTING_CAMERA_SIZE "camera_size"
#define SETTING_HW_DECODING "hw_decoding"

static const char *const DEFAULT_ADB_PATH = "adb.exe";
static const char *const DEFAULT_SCRCPY_VERSION = "4.0";

struct scrcpy_source {
	obs_source_t *source;
	struct scrcpy_session *session;
	char *adb_path;
	char *device_serial;
	char *server_jar_path;
	char *scrcpy_version;
	char *video_codec;
	char *video_source;
	char *camera_id;
	char *camera_size;
	uint16_t local_port;
	uint32_t video_bit_rate;
	uint16_t max_size;
	uint32_t frame_width;
	uint32_t frame_height;
	bool hw_decoding;
	bool active;
};

static void scrcpy_source_update(void *data, obs_data_t *settings);
static int scrcpy_refresh_device_list(struct scrcpy_source *context, obs_property_t *list);
static bool scrcpy_refresh_button_clicked(obs_properties_t *props, obs_property_t *button, void *data);
static void scrcpy_source_start_session(struct scrcpy_source *context);
static void scrcpy_source_stop_session(struct scrcpy_source *context);
static void scrcpy_source_on_frame(void *opaque, const struct obs_source_frame *frame);

static uint32_t scrcpy_source_get_width(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return 0;
	return context->frame_width;
}

static uint32_t scrcpy_source_get_height(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return 0;
	return context->frame_height;
}

static const char *scrcpy_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "scrcpy Camera";
}

static void *scrcpy_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct scrcpy_source *context = bzalloc(sizeof(*context));
	context->source = source;
	context->session = scrcpy_session_create();
	context->frame_width = 0;
	context->frame_height = 0;

	obs_log(LOG_INFO, "creating scrcpy source scaffold");
	scrcpy_source_update(context, settings);
	return context;
}

static void scrcpy_source_destroy(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	bfree(context->adb_path);
	bfree(context->device_serial);
	bfree(context->server_jar_path);
	bfree(context->scrcpy_version);
	bfree(context->video_codec);
	bfree(context->video_source);
	bfree(context->camera_id);
	bfree(context->camera_size);
	scrcpy_session_destroy(context->session);
	bfree(context);
}

static void scrcpy_source_update(void *data, obs_data_t *settings)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	const char *adb_path = obs_data_get_string(settings, SETTING_ADB_PATH);
	const char *device_serial = obs_data_get_string(settings, SETTING_DEVICE_SERIAL);
	const char *server_jar_path = obs_data_get_string(settings, SETTING_SERVER_JAR_PATH);
	const char *scrcpy_version = obs_data_get_string(settings, SETTING_SCRCPY_VERSION);
	const char *video_codec = obs_data_get_string(settings, SETTING_VIDEO_CODEC);
	const char *video_source = obs_data_get_string(settings, SETTING_VIDEO_SOURCE);
	const char *camera_id = obs_data_get_string(settings, SETTING_CAMERA_ID);
	const char *camera_size = obs_data_get_string(settings, SETTING_CAMERA_SIZE);
	long long local_port = obs_data_get_int(settings, SETTING_LOCAL_PORT);
	long long video_bit_rate = obs_data_get_int(settings, SETTING_VIDEO_BIT_RATE);
	long long max_size = obs_data_get_int(settings, SETTING_MAX_SIZE);
	bool hw_decoding = obs_data_get_bool(settings, SETTING_HW_DECODING);

	bfree(context->adb_path);
	bfree(context->device_serial);
	bfree(context->server_jar_path);
	bfree(context->scrcpy_version);
	bfree(context->video_codec);
	bfree(context->video_source);
	bfree(context->camera_id);
	bfree(context->camera_size);
	context->adb_path = bstrdup(adb_path && adb_path[0] ? adb_path : DEFAULT_ADB_PATH);
	context->device_serial = bstrdup(device_serial ? device_serial : "");
	context->server_jar_path = bstrdup(server_jar_path && server_jar_path[0] ? server_jar_path : "scrcpy-server.jar");
	context->scrcpy_version = bstrdup(scrcpy_version && scrcpy_version[0] ? scrcpy_version : DEFAULT_SCRCPY_VERSION);
	context->video_codec = bstrdup(video_codec && video_codec[0] ? video_codec : "h264");
	context->video_source = bstrdup(video_source && video_source[0] ? video_source : "display");
	context->camera_id = bstrdup(camera_id && camera_id[0] ? camera_id : "0");
	context->camera_size = bstrdup(camera_size && camera_size[0] ? camera_size : "1920x1080");
	
	/* OBS editable combo box uses the display text. Extract just the ID. */
	char *space = strchr(context->camera_id, ' ');
	if (space)
		*space = '\0';

	if (local_port < 1 || local_port > 65535)
		local_port = 27183;
	context->local_port = (uint16_t)local_port;
	if (video_bit_rate < 1)
		video_bit_rate = 8;
	context->video_bit_rate = (uint32_t)(video_bit_rate * 1000000);
	context->max_size = (uint16_t)max_size;
	context->hw_decoding = hw_decoding;

	obs_log(LOG_INFO,
		"scrcpy source updated: device='%s', source=%s, codec=%s, bitrate=%uMbps, max_size=%hu, camera_size=%s",
		context->device_serial, context->video_source, context->video_codec, (uint32_t)video_bit_rate,
		context->max_size, context->camera_size);

	if (context->active) {
		scrcpy_source_stop_session(context);
		scrcpy_source_start_session(context);
	}
}

static void scrcpy_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_ADB_PATH, DEFAULT_ADB_PATH);
	obs_data_set_default_string(settings, SETTING_DEVICE_SERIAL, "");
	obs_data_set_default_string(settings, SETTING_SERVER_JAR_PATH, "scrcpy-server.jar");
	obs_data_set_default_string(settings, SETTING_SCRCPY_VERSION, DEFAULT_SCRCPY_VERSION);
	obs_data_set_default_string(settings, SETTING_VIDEO_CODEC, "h264");
	obs_data_set_default_int(settings, SETTING_LOCAL_PORT, 27183);
	obs_data_set_default_int(settings, SETTING_VIDEO_BIT_RATE, 8);
	obs_data_set_default_int(settings, SETTING_MAX_SIZE, 0);
	obs_data_set_default_string(settings, SETTING_VIDEO_SOURCE, "display");
	obs_data_set_default_string(settings, SETTING_CAMERA_ID, "0");
	obs_data_set_default_string(settings, SETTING_CAMERA_SIZE, "1920x1080");
	obs_data_set_default_bool(settings, SETTING_HW_DECODING, true);
}

static bool scrcpy_video_source_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const char *val = obs_data_get_string(settings, SETTING_VIDEO_SOURCE);
	bool is_camera = (val && strcmp(val, "camera") == 0);
	obs_property_t *cam_id_prop = obs_properties_get(props, SETTING_CAMERA_ID);
	obs_property_set_visible(cam_id_prop, is_camera);
	obs_property_t *cam_size_prop = obs_properties_get(props, SETTING_CAMERA_SIZE);
	obs_property_set_visible(cam_size_prop, is_camera);
	return true;
}

static obs_properties_t *scrcpy_source_properties(void *unused)
{
	struct scrcpy_source *context = unused;
	obs_property_t *codec_list;
	obs_property_t *max_size_list;
	obs_property_t *vsource_list;
	obs_property_t *cam_id_prop;

	obs_properties_t *props = obs_properties_create();
	obs_properties_add_path(props, SETTING_ADB_PATH, "ADB executable", OBS_PATH_FILE, "Executable (*.exe);;All Files (*.*)", NULL);

	obs_property_t *device_list = obs_properties_add_list(props, SETTING_DEVICE_SERIAL, "ADB device",
					       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (device_list)
		scrcpy_refresh_device_list(context, device_list);

	obs_properties_add_button(props, "refresh_devices", "Refresh device list", scrcpy_refresh_button_clicked);
	obs_properties_add_path(props, SETTING_SERVER_JAR_PATH, "scrcpy-server.jar path", OBS_PATH_FILE, "Jar Files (*.jar);;All Files (*.*)", NULL);
	obs_properties_add_text(props, SETTING_SCRCPY_VERSION, "scrcpy protocol version", OBS_TEXT_DEFAULT);

	vsource_list = obs_properties_add_list(props, SETTING_VIDEO_SOURCE, "Video source",
					       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(vsource_list, "Screen", "display");
	obs_property_list_add_string(vsource_list, "Camera", "camera");
	obs_property_set_modified_callback(vsource_list, scrcpy_video_source_changed);

	cam_id_prop = obs_properties_add_list(props, SETTING_CAMERA_ID, "Camera ID",
					      OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(cam_id_prop, "0 (Back)", "0");
	obs_property_list_add_string(cam_id_prop, "1 (Front)", "1");
	obs_property_list_add_string(cam_id_prop, "2", "2");
	obs_property_list_add_string(cam_id_prop, "3", "3");

	obs_properties_add_text(props, SETTING_CAMERA_SIZE, "Camera Size", OBS_TEXT_DEFAULT);

	codec_list = obs_properties_add_list(props, SETTING_VIDEO_CODEC, "Video codec",
					     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec_list, "H.264 (AVC)", "h264");
	obs_property_list_add_string(codec_list, "H.265 (HEVC)", "h265");

	obs_properties_add_bool(props, SETTING_HW_DECODING, "Use Hardware Decoding");

	obs_properties_add_int_slider(props, SETTING_VIDEO_BIT_RATE, "Video bitrate (Mbps)", 1, 50, 1);

	max_size_list = obs_properties_add_list(props, SETTING_MAX_SIZE, "Max resolution",
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(max_size_list, "Original (no limit)", 0);
	obs_property_list_add_int(max_size_list, "2560p (WQHD)", 2560);
	obs_property_list_add_int(max_size_list, "1920p (Full HD)", 1920);
	obs_property_list_add_int(max_size_list, "1280p (HD)", 1280);
	obs_property_list_add_int(max_size_list, "960p", 960);
	obs_property_list_add_int(max_size_list, "720p", 720);
	obs_property_list_add_int(max_size_list, "480p", 480);

	obs_properties_add_int(props, SETTING_LOCAL_PORT, "Local TCP port", 1, 65535, 1);

	return props;
}

static void scrcpy_source_start_session(struct scrcpy_source *context)
{
	struct scrcpy_session_config config;

	if (!context || !context->session)
		return;

	config.adb_path = context->adb_path;
	config.device_serial = context->device_serial;
	config.server_jar_path = context->server_jar_path;
	config.scrcpy_version = context->scrcpy_version;
	config.video_codec = context->video_codec;
	config.video_source = context->video_source;
	config.camera_id = context->camera_id;
	config.camera_size = context->camera_size;
	config.local_port = context->local_port;
	config.video_bit_rate = context->video_bit_rate;
	config.max_size = context->max_size;
	config.hw_decoding = context->hw_decoding;
	config.on_frame = scrcpy_source_on_frame;
	config.on_frame_opaque = context;

	if (scrcpy_session_start(context->session, &config) != 0)
		obs_log(LOG_WARNING, "unable to start scrcpy bootstrap session");
}

static void scrcpy_source_stop_session(struct scrcpy_source *context)
{
	if (!context || !context->session)
		return;

	scrcpy_session_stop(context->session);
}

static void scrcpy_source_on_frame(void *opaque, const struct obs_source_frame *frame)
{
	struct scrcpy_source *context = opaque;
	if (!context || !frame)
		return;

	context->frame_width = frame->width;
	context->frame_height = frame->height;
	obs_source_output_video(context->source, frame);
}

static void scrcpy_source_activate(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	context->active = true;
	scrcpy_source_start_session(context);
}

static void scrcpy_source_deactivate(void *data)
{
	struct scrcpy_source *context = data;
	if (!context)
		return;

	context->active = false;
	scrcpy_source_stop_session(context);
}

static int scrcpy_parse_adb_devices(FILE *pipe, obs_property_t *list)
{
	char line[1024];
	int found = 0;

	while (fgets(line, (int)sizeof(line), pipe) != NULL) {
		char *cursor = line;
		char *serial;
		char *state;

		while (*cursor == ' ' || *cursor == '\t')
			++cursor;

		if (!cursor[0] || cursor[0] == '\n' || cursor[0] == '\r')
			continue;

		if (!strncmp(cursor, "List of devices attached", 24))
			continue;

		if (cursor[0] == '*')
			continue;

		serial = strtok(cursor, " \t\r\n");
		state = strtok(NULL, " \t\r\n");
		if (!serial || !state)
			continue;

		if (strcmp(state, "device") != 0)
			continue;

		/* Trim trailing newlines in-place for a cleaner UI label. */
		for (size_t i = strlen(cursor); i > 0; --i) {
			char c = cursor[i - 1];
			if (c == '\n' || c == '\r')
				cursor[i - 1] = '\0';
			else
				break;
		}

		obs_property_list_add_string(list, serial, serial);
		++found;
	}

	return found;
}

static int scrcpy_refresh_device_list(struct scrcpy_source *context, obs_property_t *list)
{
	const char *adb_path = DEFAULT_ADB_PATH;
	int found = 0;

	if (!list)
		return 0;

	if (context && context->adb_path && context->adb_path[0])
		adb_path = context->adb_path;

	obs_property_list_clear(list);
	obs_property_list_add_string(list, "(Select device)", "");

#ifdef _WIN32
	char command[1024];
	FILE *pipe;

	_snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" devices -l 2>nul", adb_path);
	pipe = _popen(command, "r");
	if (!pipe) {
		obs_log(LOG_WARNING, "failed to run adb command using '%s'", adb_path);
		obs_property_list_add_string(list, "ADB command failed", "");
		return 0;
	}

	found = scrcpy_parse_adb_devices(pipe, list);
	_pclose(pipe);
#else
	UNUSED_PARAMETER(adb_path);
#endif

	if (found == 0)
		obs_property_list_add_string(list, "No online ADB devices", "");

	obs_log(LOG_INFO, "scrcpy discovered %d ADB device(s)", found);
	return found;
}

static bool scrcpy_refresh_button_clicked(obs_properties_t *props, obs_property_t *button, void *data)
{
	UNUSED_PARAMETER(button);

	obs_property_t *device_list = obs_properties_get(props, SETTING_DEVICE_SERIAL);
	scrcpy_refresh_device_list(data, device_list);
	return true;
}

static struct obs_source_info scrcpy_source_info = {
	.id = "scrcpy_camera_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = scrcpy_source_get_name,
	.create = scrcpy_source_create,
	.destroy = scrcpy_source_destroy,
	.activate = scrcpy_source_activate,
	.deactivate = scrcpy_source_deactivate,
	.update = scrcpy_source_update,
	.get_defaults = scrcpy_source_defaults,
	.get_properties = scrcpy_source_properties,
	.get_width = scrcpy_source_get_width,
	.get_height = scrcpy_source_get_height,
};

void scrcpy_source_register(void)
{
	obs_register_source(&scrcpy_source_info);
}