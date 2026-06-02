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

#include "scrcpy-session.h"

#include <obs-module.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include <plugin-support.h>

#include <util/platform.h>

#include <process.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define DEFAULT_SCRCPY_VERSION "4.0"
#define SCRCPY_META_DEVICE_NAME_SIZE 64
#define SCRCPY_SESSION_PACKET_SIZE 12
#define SCRCPY_FRAME_HEADER_SIZE 12
#define SCRCPY_DEFAULT_PORT 27183
#define SCRCPY_COMMAND_TIMEOUT_MS 15000

struct scrcpy_session {
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
	uint32_t scid;
	bool hw_decoding;
	enum AVHWDeviceType hw_device_type;

	scrcpy_session_frame_callback on_frame;
	void *on_frame_opaque;
	char socket_name[64];

	HANDLE worker_thread;
	HANDLE server_process;
	SOCKET video_socket;
	volatile LONG stop_requested;
	volatile LONG running;
};

static unsigned __stdcall scrcpy_session_worker(void *opaque);
static bool scrcpy_command_step(struct scrcpy_session *session, const char *step, const char *command_line);
static bool scrcpy_command_fire_and_forget(struct scrcpy_session *session, const char *step, const char *command_line);
static bool scrcpy_run_process(struct scrcpy_session *session, const char *step, const char *command_line,
			       bool wait_for_exit, bool treat_missing_exit_as_success, DWORD *exit_code);
static void scrcpy_log_pipe_output(const char *prefix, HANDLE pipe_handle);
static bool scrcpy_should_stop(struct scrcpy_session *session);
static bool scrcpy_open_video_socket(struct scrcpy_session *session);
static bool scrcpy_read_exact(struct scrcpy_session *session, SOCKET sock, void *buffer, size_t size);
static void scrcpy_log_socket_available(const char *label, SOCKET sock);
static bool scrcpy_read_handshake(struct scrcpy_session *session, enum AVCodecID *codec_id, uint32_t *width,
				  uint32_t *height);
static uint32_t scrcpy_read_be32(const uint8_t *data);
static bool scrcpy_init_decoder(struct scrcpy_session *session, enum AVCodecID codec_id, AVCodecContext **decoder_context);
static bool scrcpy_decode_loop(struct scrcpy_session *session, AVCodecContext *decoder_context, uint32_t width,
			       uint32_t height);
static void scrcpy_close_stream_handles(struct scrcpy_session *session);

#pragma comment(lib, "Ws2_32.lib")

static void scrcpy_copy_config(struct scrcpy_session *session, const struct scrcpy_session_config *config)
{
	bfree(session->adb_path);
	bfree(session->device_serial);
	bfree(session->server_jar_path);
	bfree(session->scrcpy_version);
	bfree(session->video_codec);
	bfree(session->video_source);
	bfree(session->camera_id);
	bfree(session->camera_size);

	session->adb_path = bstrdup(config->adb_path ? config->adb_path : "adb.exe");
	session->device_serial = bstrdup(config->device_serial ? config->device_serial : "");
	session->server_jar_path = bstrdup(config->server_jar_path ? config->server_jar_path : "scrcpy-server.jar");
	session->scrcpy_version =
		bstrdup(config->scrcpy_version && config->scrcpy_version[0] ? config->scrcpy_version : DEFAULT_SCRCPY_VERSION);
	session->video_codec =
		bstrdup(config->video_codec && config->video_codec[0] ? config->video_codec : "h264");
	session->video_source =
		bstrdup(config->video_source && config->video_source[0] ? config->video_source : "display");
	session->camera_id =
		bstrdup(config->camera_id && config->camera_id[0] ? config->camera_id : "0");
	session->camera_size =
		bstrdup(config->camera_size && config->camera_size[0] ? config->camera_size : "1920x1080");
	session->local_port = config->local_port ? config->local_port : SCRCPY_DEFAULT_PORT;
	session->video_bit_rate = config->video_bit_rate ? config->video_bit_rate : 8000000;
	session->max_size = config->max_size;
	session->hw_decoding = config->hw_decoding;
	session->scid = (GetCurrentProcessId() ^ GetTickCount()) & 0x7fffffffU;
	session->on_frame = config->on_frame;
	session->on_frame_opaque = config->on_frame_opaque;
	_snprintf_s(session->socket_name, sizeof(session->socket_name), _TRUNCATE, "scrcpy_%08x", session->scid);
}

struct scrcpy_session *scrcpy_session_create(void)
{
	return bzalloc(sizeof(struct scrcpy_session));
}

void scrcpy_session_destroy(struct scrcpy_session *session)
{
	if (!session)
		return;

	scrcpy_close_stream_handles(session);
	scrcpy_session_stop(session);
	bfree(session->adb_path);
	bfree(session->device_serial);
	bfree(session->server_jar_path);
	bfree(session->scrcpy_version);
	bfree(session->video_codec);
	bfree(session->video_source);
	bfree(session->camera_id);
	bfree(session->camera_size);
	bfree(session);
}

bool scrcpy_session_is_running(const struct scrcpy_session *session)
{
	return session && InterlockedCompareExchange((LONG *)&session->running, 0, 0) != 0;
}

int scrcpy_session_start(struct scrcpy_session *session, const struct scrcpy_session_config *config)
{
	uintptr_t thread_handle;

	if (!session || !config)
		return -1;

	if (!config->device_serial || !config->device_serial[0]) {
		obs_log(LOG_WARNING, "scrcpy session start skipped: no device serial selected");
		return -2;
	}

	scrcpy_session_stop(session);
	scrcpy_copy_config(session, config);

	InterlockedExchange(&session->stop_requested, 0);
	InterlockedExchange(&session->running, 1);

	thread_handle = _beginthreadex(NULL, 0, scrcpy_session_worker, session, 0, NULL);
	if (!thread_handle) {
		InterlockedExchange(&session->running, 0);
		obs_log(LOG_ERROR, "failed to create scrcpy session worker thread");
		return -3;
	}

	session->worker_thread = (HANDLE)thread_handle;
	obs_log(LOG_INFO, "scrcpy session worker started for device '%s'", session->device_serial);
	return 0;
}

void scrcpy_session_stop(struct scrcpy_session *session)
{
	if (!session)
		return;

	InterlockedExchange(&session->stop_requested, 1);
	scrcpy_close_stream_handles(session);

	if (!session->worker_thread) {
		InterlockedExchange(&session->running, 0);
		return;
	}

	WaitForSingleObject(session->worker_thread, INFINITE);
	CloseHandle(session->worker_thread);
	session->worker_thread = NULL;
	InterlockedExchange(&session->running, 0);
	obs_log(LOG_INFO, "scrcpy session worker stopped");
}

static bool scrcpy_should_stop(struct scrcpy_session *session)
{
	return InterlockedCompareExchange(&session->stop_requested, 0, 0) != 0;
}

static void scrcpy_close_stream_handles(struct scrcpy_session *session)
{
	if (!session)
		return;

	if (session->video_socket != INVALID_SOCKET) {
		shutdown(session->video_socket, SD_BOTH);
		closesocket(session->video_socket);
		session->video_socket = INVALID_SOCKET;
	}

	if (session->server_process) {
		TerminateProcess(session->server_process, 0);
		CloseHandle(session->server_process);
		session->server_process = NULL;
	}
}

static bool scrcpy_command_step(struct scrcpy_session *session, const char *step, const char *command)
{
	return scrcpy_run_process(session, step, command, true, false, NULL);
}

static bool scrcpy_command_fire_and_forget(struct scrcpy_session *session, const char *step, const char *command)
{
	return scrcpy_run_process(session, step, command, false, false, NULL);
}

static bool scrcpy_run_process(struct scrcpy_session *session, const char *step, const char *command_line,
			       bool wait_for_exit, bool treat_missing_exit_as_success, DWORD *exit_code)
{
	STARTUPINFOA startup_info;
	PROCESS_INFORMATION process_info;
	HANDLE stdout_read = NULL;
	HANDLE stdout_write = NULL;
	HANDLE stderr_read = NULL;
	HANDLE stderr_write = NULL;
	char buffer[1024];
	char full_command[4096];
	DWORD process_exit_code = 0;
	uint64_t start_ns = os_gettime_ns();
	uint64_t deadline_ns = start_ns + (uint64_t)SCRCPY_COMMAND_TIMEOUT_MS * 1000000ULL;
	bool success = false;

	if (scrcpy_should_stop(session))
		return false;

	obs_log(LOG_INFO, "scrcpy step: %s", step);

	if (wait_for_exit) {
		SECURITY_ATTRIBUTES security_attributes;

		ZeroMemory(&security_attributes, sizeof(security_attributes));
		security_attributes.nLength = sizeof(security_attributes);
		security_attributes.bInheritHandle = TRUE;
		security_attributes.lpSecurityDescriptor = NULL;

		if (!CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0)) {
			obs_log(LOG_ERROR, "failed to create stdout pipe for step '%s' (error %lu)", step, GetLastError());
			return false;
		}

		if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
			obs_log(LOG_ERROR, "failed to configure stdout pipe for step '%s' (error %lu)", step, GetLastError());
			goto done;
		}

		if (!CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0)) {
			obs_log(LOG_ERROR, "failed to create stderr pipe for step '%s' (error %lu)", step, GetLastError());
			goto done;
		}

		if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
			obs_log(LOG_ERROR, "failed to configure stderr pipe for step '%s' (error %lu)", step, GetLastError());
			goto done;
		}

		ZeroMemory(&startup_info, sizeof(startup_info));
		startup_info.cb = sizeof(startup_info);
		startup_info.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
		startup_info.wShowWindow = SW_HIDE;
		startup_info.hStdOutput = stdout_write;
		startup_info.hStdError = stderr_write;
		startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	} else {
		ZeroMemory(&startup_info, sizeof(startup_info));
		startup_info.cb = sizeof(startup_info);
		startup_info.dwFlags = STARTF_USESHOWWINDOW;
		startup_info.wShowWindow = SW_HIDE;
	}
	ZeroMemory(&process_info, sizeof(process_info));

	_snprintf_s(full_command, sizeof(full_command), _TRUNCATE, "cmd.exe /c \"%s\"", command_line);
	obs_log(LOG_DEBUG, "scrcpy command line: %s", full_command);
	if (!CreateProcessA(NULL, full_command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup_info,
			    &process_info)) {
		obs_log(LOG_ERROR, "failed to start process for step '%s' (error %lu)", step, GetLastError());
		goto done;
	}

	CloseHandle(process_info.hThread);

	if (wait_for_exit) {
		for (;;) {
			DWORD wait_result = WaitForSingleObject(process_info.hProcess, 100);
			DWORD bytes_available = 0;

			if (stdout_read && PeekNamedPipe(stdout_read, NULL, 0, NULL, &bytes_available, NULL) && bytes_available > 0) {
				DWORD bytes_read = 0;
				if (ReadFile(stdout_read, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
					buffer[bytes_read] = '\0';
					obs_log(LOG_INFO, "scrcpy adb: %s", buffer);
				}
			}

			bytes_available = 0;
			if (stderr_read && PeekNamedPipe(stderr_read, NULL, 0, NULL, &bytes_available, NULL) && bytes_available > 0) {
				DWORD bytes_read = 0;
				if (ReadFile(stderr_read, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
					buffer[bytes_read] = '\0';
					obs_log(LOG_INFO, "scrcpy adb: %s", buffer);
				}
			}

			if (wait_result == WAIT_OBJECT_0)
				break;

			if (scrcpy_should_stop(session)) {
				obs_log(LOG_WARNING, "scrcpy command aborted by stop request: %s", step);
				TerminateProcess(process_info.hProcess, 1);
				break;
			}

			if (os_gettime_ns() > deadline_ns) {
				obs_log(LOG_WARNING, "scrcpy command timed out after %u ms: %s", SCRCPY_COMMAND_TIMEOUT_MS, step);
				TerminateProcess(process_info.hProcess, 1);
				break;
			}
		}

		if (!GetExitCodeProcess(process_info.hProcess, &process_exit_code)) {
			obs_log(LOG_WARNING, "failed to read exit code for step '%s' (error %lu)", step, GetLastError());
			process_exit_code = 1;
		}

		if (exit_code)
			*exit_code = process_exit_code;
		if (process_exit_code != 0) {
			if (treat_missing_exit_as_success && process_exit_code == 1) {
				success = true;
			} else {
				obs_log(LOG_WARNING, "command returned non-zero exit code (%lu): %s", process_exit_code,
					command_line);
				success = false;
			}
		} else {
			success = true;
		}
		obs_log(LOG_DEBUG, "scrcpy step completed in %.3f sec: %s", (double)(os_gettime_ns() - start_ns) / 1e9,
			step);
	} else {
		session->server_process = process_info.hProcess;
		process_info.hProcess = NULL;
		success = true;
		obs_log(LOG_DEBUG, "scrcpy fire-and-forget launched in %.3f sec: %s",
			(double)(os_gettime_ns() - start_ns) / 1e9, step);
		obs_log(LOG_DEBUG, "scrcpy process handle retained for background server step: %s", step);
	}

done:
	if (stdout_write)
		CloseHandle(stdout_write);
	if (stdout_read)
		CloseHandle(stdout_read);
	if (stderr_write)
		CloseHandle(stderr_write);
	if (stderr_read)
		CloseHandle(stderr_read);
	if (process_info.hProcess)
		CloseHandle(process_info.hProcess);
	if (!success && process_info.hProcess == session->server_process) {
		session->server_process = NULL;
	}
	return success;
}

static void scrcpy_log_socket_available(const char *label, SOCKET sock)
{
	unsigned long available = 0;

	if (sock == INVALID_SOCKET)
		return;

	if (ioctlsocket(sock, FIONREAD, &available) == 0) {
		obs_log(LOG_DEBUG, "scrcpy socket state [%s]: %lu byte(s) buffered", label, available);
	} else {
		obs_log(LOG_DEBUG, "scrcpy socket state [%s]: FIONREAD failed (%d)", label, WSAGetLastError());
	}
}

static bool scrcpy_open_video_socket(struct scrcpy_session *session)
{
	SOCKET sock = INVALID_SOCKET;
	struct sockaddr_in addr;
	int timeout_ms = 250;
	int attempt;

	if (!session)
		return false;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(session->local_port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	for (attempt = 0; attempt < 5; ++attempt) {
		if (scrcpy_should_stop(session))
			return false;

		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
			return false;

		if (connect(sock, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
			setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
			session->video_socket = sock;
			obs_log(LOG_DEBUG, "connected to scrcpy TCP port %hu", session->local_port);
			scrcpy_log_socket_available("after connect", sock);
			return true;
		}

		closesocket(sock);
		sock = INVALID_SOCKET;
		Sleep(50);
	}

	obs_log(LOG_ERROR, "unable to connect to local scrcpy video socket on tcp:%hu", session->local_port);
	return false;
}

static bool scrcpy_read_exact(struct scrcpy_session *session, SOCKET sock, void *buffer, size_t size)
{
	uint8_t *cursor = buffer;
	size_t remaining = size;

			// obs_log(LOG_INFO, "[DEBUG] scrcpy_read_exact requesting %zu bytes", size);

	while (remaining > 0) {
		int received;
		int last_error;

		if (scrcpy_should_stop(session))
			return false;

		received = recv(sock, (char *)cursor, (int)remaining, 0);
		last_error = WSAGetLastError();
		if (received > 0) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy recv returned %d bytes (remaining: %zu -> %zu)", received, remaining, remaining - received);
			cursor += received;
			remaining -= (size_t)received;
			continue;
		}

		if (received == 0) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy socket closed (0 returned from recv) while waiting for %zu byte(s) out of %zu", remaining, size);
			return false;
		}

		if (last_error == WSAETIMEDOUT) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy recv timed out, retrying... (remaining=%zu)", remaining); // Might spam too much
			continue;
		}

			// obs_log(LOG_INFO, "[DEBUG] scrcpy recv failed: received=%d remaining=%zu error=%d", received, remaining, last_error);

		return false;
	}

			// obs_log(LOG_INFO, "[DEBUG] scrcpy_read_exact successfully read %zu bytes", size);
	return true;
}

static uint32_t scrcpy_read_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
	       (uint32_t)data[3];
}

static bool scrcpy_read_handshake(struct scrcpy_session *session, enum AVCodecID *codec_id, uint32_t *width,
				  uint32_t *height)
{
	uint8_t dummy = 0;
	char device_name[SCRCPY_META_DEVICE_NAME_SIZE + 1];
	uint8_t codec_bytes[4];
	uint8_t session_packet[SCRCPY_SESSION_PACKET_SIZE];
	uint32_t codec = 0;

	/*
	 * Protocol lock: this parser intentionally targets scrcpy-server 4.0 behavior
	 * over adb forward with video enabled and audio/control disabled.
	 */
			// obs_log(LOG_INFO, "[DEBUG] waiting for scrcpy forward dummy byte");
	scrcpy_log_socket_available("before dummy byte", session->video_socket);
	if (!scrcpy_read_exact(session, session->video_socket, &dummy, sizeof(dummy))) {
		scrcpy_log_socket_available("dummy byte read failed", session->video_socket);
			// obs_log(LOG_INFO, "[DEBUG] failed to read scrcpy forward dummy byte (will retry)");
		return false;
	}
			// obs_log(LOG_INFO, "[DEBUG] scrcpy forward dummy byte value: %u", dummy);

			// obs_log(LOG_INFO, "[DEBUG] waiting for scrcpy 64-byte device metadata");
	if (!scrcpy_read_exact(session, session->video_socket, device_name, SCRCPY_META_DEVICE_NAME_SIZE)) {
		scrcpy_log_socket_available("device metadata read failed", session->video_socket);
		obs_log(LOG_ERROR, "failed to read scrcpy device metadata");
		return false;
	}

	device_name[SCRCPY_META_DEVICE_NAME_SIZE] = '\0';
	for (size_t i = 0; i < SCRCPY_META_DEVICE_NAME_SIZE; ++i) {
		if (device_name[i] == '\0')
			break;
		if (device_name[i] == '\n' || device_name[i] == '\r') {
			device_name[i] = '\0';
			break;
		}
	}
	obs_log(LOG_INFO, "scrcpy device metadata: '%s'", device_name);

	if (!scrcpy_read_exact(session, session->video_socket, codec_bytes, sizeof(codec_bytes))) {
		scrcpy_log_socket_available("codec id read failed", session->video_socket);
		obs_log(LOG_ERROR, "failed to read scrcpy codec id");
		return false;
	}

	codec = scrcpy_read_be32(codec_bytes);
	if (codec == 0x68323634U) {
		*codec_id = AV_CODEC_ID_H264;
		obs_log(LOG_INFO, "scrcpy codec id: h264");
	} else if (codec == 0x68323635U) {
		*codec_id = AV_CODEC_ID_HEVC;
		obs_log(LOG_INFO, "scrcpy codec id: h265");
	} else {
		obs_log(LOG_ERROR, "unsupported scrcpy codec id: 0x%08x", codec);
		return false;
	}

	/*
	 * Display mirroring sends a 0x80 session packet with initial
	 * dimensions before video frames. Camera mode does NOT — the
	 * stream goes straight to codec config / video frames.
	 */
	if (strcmp(session->video_source, "camera") == 0) {
		obs_log(LOG_INFO, "scrcpy camera mode: skipping session packet, dimensions from decoder");
		*width = 0;
		*height = 0;
	} else {
		if (!scrcpy_read_exact(session, session->video_socket,
				       session_packet, sizeof(session_packet))) {
			scrcpy_log_socket_available("session packet read failed",
						    session->video_socket);
			obs_log(LOG_ERROR, "failed to read scrcpy video session packet");
			return false;
		}
		if ((session_packet[0] & 0x80U) == 0) {
			obs_log(LOG_ERROR, "expected 0x80 session packet, got 0x%02x",
				session_packet[0]);
			return false;
		}
		*width = scrcpy_read_be32(session_packet + 4);
		*height = scrcpy_read_be32(session_packet + 8);
		obs_log(LOG_INFO, "scrcpy session dimensions: %ux%u",
			*width, *height);
	}
	return true;
}

static enum AVPixelFormat scrcpy_get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	struct scrcpy_session *session = ctx->opaque;
	const enum AVPixelFormat *p;
	char fmts_str[256] = {0};
	for (p = pix_fmts; *p != -1; p++) {
		const char *name = av_get_pix_fmt_name(*p);
		if (name) {
			strncat(fmts_str, name, sizeof(fmts_str) - strlen(fmts_str) - 1);
			strncat(fmts_str, ", ", sizeof(fmts_str) - strlen(fmts_str) - 1);
			
			if (session) {
				if (session->hw_device_type == AV_HWDEVICE_TYPE_D3D11VA) {
					if (strcmp(name, "d3d11") == 0 || strcmp(name, "d3d11va_vld") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				} else if (session->hw_device_type == AV_HWDEVICE_TYPE_DXVA2) {
					if (strcmp(name, "dxva2_vld") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				} else if (session->hw_device_type == AV_HWDEVICE_TYPE_CUDA) {
					if (strcmp(name, "cuda") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				} else if (session->hw_device_type == AV_HWDEVICE_TYPE_QSV) {
					if (strcmp(name, "qsv") == 0) {
						obs_log(LOG_INFO, "HW surface format selected: %s", name);
						return *p;
					}
				}
			}
		}
	}
	// We only log this once to avoid spamming
	static bool warned = false;
	if (!warned) {
		obs_log(LOG_WARNING, "failed to get HW surface format, available formats: %s. Hardware decoding will not be used", fmts_str);
		warned = true;
	}
	return pix_fmts[0];
}

static bool scrcpy_init_decoder(struct scrcpy_session *session, enum AVCodecID codec_id, AVCodecContext **decoder_context)
{
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	AVCodecContext *context;
	AVBufferRef *hw_device_ctx = NULL;

	if (!codec) {
		obs_log(LOG_ERROR, "ffmpeg decoder not found for codec id %d", (int)codec_id);
		return false;
	}

	session->hw_device_type = AV_HWDEVICE_TYPE_NONE;

	if (session->hw_decoding) {
		const char *hw_types[] = {"cuda", "qsv", "d3d11va", "dxva2", NULL};
		enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
		
		for (int i = 0; hw_types[i]; i++) {
			hw_type = av_hwdevice_find_type_by_name(hw_types[i]);
			if (hw_type != AV_HWDEVICE_TYPE_NONE) {
				int err = av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0);
				if (err == 0) {
					obs_log(LOG_INFO, "hardware decoding context created successfully: %s", hw_types[i]);
					session->hw_device_type = hw_type;
					break;
				}
			}
		}
		
		if (!hw_device_ctx) {
			obs_log(LOG_WARNING, "failed to create any hardware device context, falling back to software");
		}
	}

	context = avcodec_alloc_context3(codec);
	if (!context) {
		obs_log(LOG_ERROR, "failed to allocate ffmpeg decoder context");
		if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
		return false;
	}

	if (hw_device_ctx) {
		context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
		context->opaque = session;
		context->get_format = scrcpy_get_hw_format;
		av_buffer_unref(&hw_device_ctx);
	}

	context->thread_count = 1; // Explicitly set to 1 to disable frame-threading latency

	if (avcodec_open2(context, codec, NULL) < 0) {
		obs_log(LOG_ERROR, "failed to open ffmpeg decoder");
		avcodec_free_context(&context);
		return false;
	}

			// obs_log(LOG_INFO, "[DEBUG] ffmpeg decoder initialized successfully (hw_decoding=%d, threads=%d)", session->hw_decoding, context->thread_count);

	*decoder_context = context;
	return true;
}

static bool scrcpy_decode_loop(struct scrcpy_session *session, AVCodecContext *decoder_context, uint32_t width,
			       uint32_t height)
{
	uint8_t header[SCRCPY_FRAME_HEADER_SIZE];
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool warned_format = false;
	uint8_t *codec_config = NULL;
	size_t codec_config_size = 0;

	if (!packet || !frame) {
		obs_log(LOG_ERROR, "unable to allocate ffmpeg packet/frame objects");
		av_packet_free(&packet);
		av_frame_free(&frame);
		return false;
	}



	obs_log(LOG_INFO, "scrcpy decode loop starting (socket=%lld)", (long long)session->video_socket);

	while (!scrcpy_should_stop(session)) {
		uint32_t payload_size;
		bool is_session_packet;
		int send_ret;
		int last_error;

			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: waiting for %zu byte header...", sizeof(header));
		if (!scrcpy_read_exact(session, session->video_socket, header, sizeof(header))) {
			obs_log(LOG_WARNING, "scrcpy decode loop: header read failed (WSA=%d)",
				WSAGetLastError());
			break;
		}

			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: header read success. Header bytes: %02x %02x %02x %02x ...", header[0], header[1], header[2], header[3]);

		is_session_packet = (header[0] & 0x80U) != 0;
		last_error = WSAGetLastError();
		if (is_session_packet) {
			width = scrcpy_read_be32(header + 4);
			height = scrcpy_read_be32(header + 8);
			obs_log(LOG_INFO, "scrcpy session refresh: %ux%u", width, height);
			continue;
		}

		if (last_error == WSAETIMEDOUT)
			continue;

		payload_size = scrcpy_read_be32(header + 8);
			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: packet payload_size=%u bytes (flags=%02x)", payload_size, header[0]);
		if (payload_size == 0) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy packet with empty payload ignored");
			continue;
		}

		av_packet_unref(packet);
		if (av_new_packet(packet, (int)payload_size) < 0) {
			obs_log(LOG_ERROR, "scrcpy decode loop: av_new_packet failed for payload_size=%u", payload_size);
			break;
		}

			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: waiting for %u bytes of payload...", payload_size);
		if (!scrcpy_read_exact(session, session->video_socket, packet->data, payload_size)) {
			obs_log(LOG_WARNING, "scrcpy decode loop: failed to read payload (%u bytes)", payload_size);
			av_packet_unref(packet);
			break;
		}
			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: payload read successfully");

		if ((header[0] & 0x20U) != 0)
			packet->flags |= AV_PKT_FLAG_KEY;

		/*
		 * Codec config packets (flag 0x40) carry H.264 SPS/PPS
		 * parameter sets without picture data. Store them and
		 * prepend to the first keyframe so the decoder receives
		 * a complete access unit (SPS + PPS + IDR slice).
		 */
		if ((header[0] & 0x40U) != 0) {
			// obs_log(LOG_INFO, "[DEBUG] scrcpy codec config packet (%u bytes), storing for keyframe", payload_size);
			av_free(codec_config);
			codec_config = av_malloc(payload_size);
			if (!codec_config) {
				obs_log(LOG_ERROR, "scrcpy decode loop: failed to allocate codec config buffer");
				av_packet_unref(packet);
				break;
			}
			memcpy(codec_config, packet->data, payload_size);
			codec_config_size = payload_size;
			av_packet_unref(packet);
			continue;
		}

		/*
		 * If we have stored codec config and this is a keyframe,
		 * prepend the config data to create a complete access unit.
		 */
		if (codec_config && (header[0] & 0x20U) != 0) {
			size_t combined_size = codec_config_size + payload_size;
			AVPacket *combined = av_packet_alloc();

			// obs_log(LOG_INFO, "[DEBUG] scrcpy prepending %zu bytes codec config to %u byte keyframe",
			//	codec_config_size, payload_size);

			if (!combined || av_new_packet(combined, (int)combined_size) < 0) {
				obs_log(LOG_ERROR, "scrcpy decode loop: failed to allocate combined packet");
				av_packet_free(&combined);
				av_packet_unref(packet);
				break;
			}

			memcpy(combined->data, codec_config, codec_config_size);
			memcpy(combined->data + codec_config_size, packet->data, payload_size);
			combined->flags = packet->flags;
			av_packet_unref(packet);

			/* Swap: use the combined packet from here on. */
			av_packet_move_ref(packet, combined);
			av_packet_free(&combined);

			av_free(codec_config);
			codec_config = NULL;
			codec_config_size = 0;
		}

			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: sending packet to decoder (size=%d)", packet->size);
		send_ret = avcodec_send_packet(decoder_context, packet);
		av_packet_unref(packet);
		if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
			obs_log(LOG_ERROR, "scrcpy decode loop: avcodec_send_packet failed (%d)", send_ret);
			break;
		}
			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: sent packet to decoder (ret=%d)", send_ret);

		for (;;) {
			int recv_ret = avcodec_receive_frame(decoder_context, frame);
			if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
				// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: avcodec_receive_frame returned %d (no more frames for now)", recv_ret);
				break;
			}
			if (recv_ret < 0) {
				obs_log(LOG_ERROR, "[DEBUG] scrcpy decode loop: avcodec_receive_frame failed (%d)", recv_ret);
				av_frame_unref(frame);
				goto fail;
			}
			// obs_log(LOG_INFO, "[DEBUG] scrcpy decode loop: received decoded frame! (format=%d, size=%dx%d)", frame->format, frame->width, frame->height);

			AVFrame *output_frame = frame;
			AVFrame *sw_frame = NULL;

			if (frame->format != AV_PIX_FMT_YUV420P && frame->hw_frames_ctx) {
				sw_frame = av_frame_alloc();
				if (!sw_frame) {
					obs_log(LOG_ERROR, "scrcpy decode loop: failed to allocate sw frame");
					av_frame_unref(frame);
					break;
				}
				if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
					obs_log(LOG_ERROR, "scrcpy decode loop: failed to transfer hw frame to sw");
					av_frame_free(&sw_frame);
					av_frame_unref(frame);
					break;
				}
				output_frame = sw_frame;
			}

			if ((output_frame->format == AV_PIX_FMT_YUV420P || output_frame->format == AV_PIX_FMT_NV12) && session->on_frame) {
				struct obs_source_frame obs_frame;
				memset(&obs_frame, 0, sizeof(obs_frame));
				
				if (output_frame->format == AV_PIX_FMT_NV12) {
					obs_frame.format = VIDEO_FORMAT_NV12;
					obs_frame.data[0] = output_frame->data[0];
					obs_frame.data[1] = output_frame->data[1];
					obs_frame.linesize[0] = output_frame->linesize[0];
					obs_frame.linesize[1] = output_frame->linesize[1];
				} else {
					obs_frame.format = VIDEO_FORMAT_I420;
					obs_frame.data[0] = output_frame->data[0];
					obs_frame.data[1] = output_frame->data[1];
					obs_frame.data[2] = output_frame->data[2];
					obs_frame.linesize[0] = output_frame->linesize[0];
					obs_frame.linesize[1] = output_frame->linesize[1];
					obs_frame.linesize[2] = output_frame->linesize[2];
				}
				
				obs_frame.width = output_frame->width;
				obs_frame.height = output_frame->height;
				obs_frame.timestamp = os_gettime_ns();

				video_format_get_parameters_for_format(
					VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
					obs_frame.format, obs_frame.color_matrix,
					obs_frame.color_range_min,
					obs_frame.color_range_max);
				session->on_frame(session->on_frame_opaque, &obs_frame);
			} else if (!warned_format && session->on_frame) {
				obs_log(LOG_WARNING,
					"decoder output format %d is not AV_PIX_FMT_YUV420P or NV12; frame dropped without swscale",
					output_frame->format);
				warned_format = true;
			}

			if (sw_frame) {
				av_frame_free(&sw_frame);
			}
			av_frame_unref(frame);
		}
	}

	obs_log(LOG_DEBUG, "scrcpy decode loop exiting (stop_requested=%ld)",
		InterlockedCompareExchange(&session->stop_requested, 0, 0));
	av_free(codec_config);
	av_packet_unref(packet);
	av_frame_unref(frame);
	av_packet_free(&packet);
	av_frame_free(&frame);
	return true;

fail:
	av_free(codec_config);
	av_packet_unref(packet);
	av_frame_unref(frame);
	av_packet_free(&packet);
	av_frame_free(&frame);
	return false;
}

static unsigned __stdcall scrcpy_session_worker(void *opaque)
{
	struct scrcpy_session *session = opaque;
	char command[2048];
	enum AVCodecID codec_id = AV_CODEC_ID_NONE;
	AVCodecContext *decoder_context = NULL;
	uint32_t width = 0;
	uint32_t height = 0;
	WSADATA wsadata;
	int reconnect_attempts = 0;

	if (!session)
		return 0;

	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
		obs_log(LOG_ERROR, "WSAStartup failed");
		InterlockedExchange(&session->running, 0);
		return 0;
	}

	while (!scrcpy_should_stop(session)) {
		session->video_socket = INVALID_SOCKET;
		session->server_process = NULL;
		codec_id = AV_CODEC_ID_NONE;
		decoder_context = NULL;
		width = 0;
		height = 0;

		_snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" start-server", session->adb_path);
		if (!scrcpy_command_step(session, "Start ADB server", command))
			goto cleanup_winsock;

	_snprintf_s(command, sizeof(command), _TRUNCATE,
		    "\"%s\" -s %s push \"%s\" /data/local/tmp/scrcpy-server.jar", session->adb_path,
		    session->device_serial, session->server_jar_path);
	if (!scrcpy_command_step(session, "Push scrcpy server", command))
		goto cleanup_winsock;

	_snprintf_s(command, sizeof(command), _TRUNCATE,
		    "\"%s\" -s %s forward tcp:%hu localabstract:%s", session->adb_path, session->device_serial,
		    session->local_port, session->socket_name);
	if (!scrcpy_command_step(session, "Configure adb forward", command))
		goto cleanup_winsock;

	_snprintf_s(command, sizeof(command), _TRUNCATE,
		    "\"%s\" -s %s shell CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / "
		    "com.genymobile.scrcpy.Server %s scid=%08x tunnel_forward=true audio=false control=false "
		    "video_codec=%s",
		    session->adb_path, session->device_serial, session->scrcpy_version, session->scid,
		    session->video_codec);

	{
		size_t len = strlen(command);
		if (session->video_bit_rate != 8000000) {
			len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
					   " video_bit_rate=%u", session->video_bit_rate);
		}

		uint16_t max_size = session->max_size;
		bool has_camera_size = (strcmp(session->video_source, "camera") == 0 &&
					session->camera_size && session->camera_size[0]);

		if (strcmp(session->video_source, "camera") == 0 && !has_camera_size && max_size == 0) {
			/*
			 * Default to 1920 in camera mode if no limit is set. Cameras often support extreme
			 * raw resolutions (e.g. 4000x3000) which the device's hardware H.264 encoder cannot
			 * handle, leading to "Camera configuration error" on startup.
			 */
			max_size = 1920;
		}

		if (max_size > 0 && !has_camera_size) {
			len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
					   " max_size=%hu", max_size);
		}
		if (strcmp(session->video_source, "camera") == 0) {
			len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
					   " video_source=camera camera_id=%s", session->camera_id);
			if (has_camera_size) {
				len += _snprintf_s(command + len, sizeof(command) - len, _TRUNCATE,
						   " camera_size=%s", session->camera_size);
			}
		}
	}

	obs_log(LOG_INFO, "scrcpy server command: ...%s",
		strlen(command) > 80 ? command + strlen(command) - 80 : command);
	if (!scrcpy_command_fire_and_forget(session, "Start scrcpy app_process server", command))
		goto cleanup_winsock;

	/*
	 * The scrcpy server takes some time to start on the device after
	 * fire-and-forget launch. connect() may succeed immediately because
	 * adb forward is already listening, but the server hasn't yet bound
	 * the abstract socket. When that happens, adb closes the forwarded
	 * connection and the dummy byte read returns EOF. Retry the full
	 * connect+handshake sequence to account for this race.
	 */
	{
		int max_attempts = 15;
		int connect_attempt;
		bool handshake_ok = false;

		/* Camera startup is slower: Camera2 API init, capture session
		 * setup, and encoder configuration can take several seconds. */
		if (strcmp(session->video_source, "camera") == 0) {
			max_attempts = 25;
		}

		for (connect_attempt = 0; connect_attempt < max_attempts; ++connect_attempt) {
			if (scrcpy_should_stop(session))
				goto cleanup_winsock;

			if (!scrcpy_open_video_socket(session))
				goto cleanup_winsock;

			if (scrcpy_read_handshake(session, &codec_id, &width, &height)) {
				handshake_ok = true;
				reconnect_attempts = 0;
				break;
			}

			/* Handshake failed - server probably not ready. Close socket and retry. */
			obs_log(LOG_DEBUG,
				"scrcpy handshake attempt %d/%d failed, retrying in 500ms",
				connect_attempt + 1, max_attempts);
			if (session->video_socket != INVALID_SOCKET) {
				shutdown(session->video_socket, SD_BOTH);
				closesocket(session->video_socket);
				session->video_socket = INVALID_SOCKET;
			}
			Sleep(500);
		}
		if (!handshake_ok) {
			obs_log(LOG_ERROR, "scrcpy handshake failed after %d attempts", connect_attempt);
			goto cleanup_winsock;
		}
	}

	if (!scrcpy_init_decoder(session, codec_id, &decoder_context))
		goto cleanup_winsock;

	if (!scrcpy_decode_loop(session, decoder_context, width, height))
		obs_log(LOG_WARNING, "scrcpy decode loop exited due to stream error or decode failure");

	obs_log(LOG_INFO, "scrcpy bootstrap finished for device '%s' on tcp:%hu", session->device_serial,
		session->local_port);

cleanup_winsock:
		if (decoder_context)
			avcodec_free_context(&decoder_context);

		scrcpy_close_stream_handles(session);

		_snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" -s %s forward --remove tcp:%hu", session->adb_path,
			    session->device_serial, session->local_port);
		if (!scrcpy_run_process(session, "Remove adb forward", command, true, true, NULL)) {
			obs_log(LOG_INFO, "adb forward removal is optional during cleanup for tcp:%hu", session->local_port);
		}

		if (scrcpy_should_stop(session))
			break;

		reconnect_attempts++;
		if (reconnect_attempts >= 100) {
			obs_log(LOG_ERROR, "scrcpy auto-reconnect failed 100 times. Giving up.");
			break;
		}

		int delay_ms = 2000;
		if (reconnect_attempts >= 50) {
			delay_ms = 10000;
		} else if (reconnect_attempts >= 15) {
			delay_ms = 5000;
		}
		
		obs_log(LOG_INFO, "scrcpy auto-reconnect attempt %d, waiting %d ms...", reconnect_attempts, delay_ms);
		
		for (int i = 0; i < delay_ms; i += 200) {
			if (scrcpy_should_stop(session)) break;
			Sleep(200);
		}
	}

	WSACleanup();

	InterlockedExchange(&session->running, 0);
	return 0;
}
