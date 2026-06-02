/*
 * scrcpy session bootstrap
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

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct scrcpy_session;
struct obs_source_frame;

typedef void (*scrcpy_session_frame_callback)(void *opaque, const struct obs_source_frame *frame);

struct scrcpy_session_config {
	const char *adb_path;
	const char *device_serial;
	const char *server_jar_path;
	const char *scrcpy_version;
	const char *video_codec;
	const char *video_source;
	const char *camera_id;
	const char *camera_size;
	uint16_t local_port;
	uint32_t video_bit_rate;
	uint16_t max_size;
	bool hw_decoding;
	scrcpy_session_frame_callback on_frame;
	void *on_frame_opaque;
};

struct scrcpy_session *scrcpy_session_create(void);
void scrcpy_session_destroy(struct scrcpy_session *session);

int scrcpy_session_start(struct scrcpy_session *session, const struct scrcpy_session_config *config);
void scrcpy_session_stop(struct scrcpy_session *session);
bool scrcpy_session_is_running(const struct scrcpy_session *session);
