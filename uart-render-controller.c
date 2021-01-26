/**
 * UART-to-video rendering controller.
 *
 * Copyright © 2018 IBM Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

/*
 * We monitor two things: host state, and the VGA password register.
 * State diagram:
 *
 *    ┌──────────┐
 *    │host idle │←──┐
 *    └──────────┘   │
 *         │         │
 *        host      host
 *         on       !on
 *         ⭣         │
 *    ┌──────────┐   │
 *    │ GPU idle │───┤
 *    └──────────┘   │
 *         │         │
 *       vga pw      │
 *        set        │
 *         ⭣         │
 *    ┌──────────┐   │
 *    │ running  │───┘
 *    └──────────┘
 *
 * We only want the renderer running while in GPU idle state (host may be
 * outputting UART data, but isn't yet outputting video). That maps the GPU
 * state transitions to renderer state transitions as follows:
 *
 *  host on:    start renderer
 *  host !on:   stop renderer
 *  vga pw set: stop renderer
 *
 * Even though there are only two states for the renderer, we keep track of the
 * three host states, so we know when to monitor the VGA pw register.
 */

enum host_state {
	STATE_HOST_IDLE,
	STATE_HOST_GPU_IDLE,
	STATE_HOST_GPU_RUNNING,
};

struct render_ctx {
	enum host_state	state;
	int		vga_pw_fd;
	int		video_mode_fd;
	sd_bus		*bus;

	const char	*service_name;
};

static const int scratch_poll_ms = 100;

static const char *host_bus_name = "xyz.openbmc_project.State.Host";
static const char *host_iface_name = "xyz.openbmc_project.State.Host";
static const char *host_obj_path = "/xyz/openbmc_project/state/host0";
static const char *host_state_prop = "CurrentHostState";
static const char *host_state_prop_running =
	"xyz.openbmc_project.State.Host.HostState.Running";

static const char *dbus_iface_props = "org.freedesktop.DBus.Properties";

static const char *systemd_bus_name = "org.freedesktop.systemd1";
static const char *systemd_obj = "/org/freedesktop/systemd1";
static const char *systemd_iface = "org.freedesktop.systemd1.Manager";

static void set_renderer_running(struct render_ctx *ctx, bool running)
{
	char buf[] = { '0', '\n' };
	sd_bus_error e = {0};
	int rc;

	fprintf(stderr, "setting render state to %s\n",
			running ? "running" : "stopped");

	rc = sd_bus_call_method(ctx->bus,
			systemd_bus_name,
			systemd_obj,
			systemd_iface,
			running ? "StartUnit" : "StopUnit",
			&e,
			NULL,
			"ss",
			ctx->service_name,
			"replace");

	if (rc < 0) {
		warnx("failed to %s %s: %s\n",
				running ? "start" : "stop",
				ctx->service_name,
				sd_bus_error_is_set(&e) ?
					e.name : strerror(rc));
		return;
	}

	/* 1 for local output, 0 for host */
	if (running)
		buf[0] = '1';

	rc = lseek(ctx->video_mode_fd, 0, SEEK_SET);
	if (rc == -1) {
		warn("Can't seek video mode reg interface");
		return;
	}

	rc = write(ctx->video_mode_fd, buf, sizeof(buf));
	if (rc < 0)
		warn("Can't write video mode reg interface");
}

static int read_vga_pw_reg(struct render_ctx *ctx, uint8_t *regp)
{
	char buf[11]; /* Cover largest 32 bit unsigned int as decimal */
	char *endp;
	unsigned long tmp;
	int rc;

	rc = lseek(ctx->vga_pw_fd, 0, SEEK_SET);
	if (rc == -1) {
		warn("Can't seek VGA scratch reg interface");
		return -1;
	}

	rc = read(ctx->vga_pw_fd, buf, sizeof(buf));
	if (rc < 1) {
		warn("Can't read VGA scratch register");
		return -1;
	}

	errno = 0;
	tmp = strtoul(buf, &endp, 0);
	if (errno || endp == buf) {
		warnx("Can't parse VGA scratch register value");
		return -1;
	}

	*regp = tmp & 0xff;

	return 0;
}

static void handle_gpu_state(struct render_ctx *ctx, bool running)
{
	switch (ctx->state) {
	case STATE_HOST_GPU_IDLE:
		if (running) {
			set_renderer_running(ctx, false);
			ctx->state = STATE_HOST_GPU_RUNNING;
		}
		break;
	case STATE_HOST_GPU_RUNNING:
		/* we don't transition here, as the host will never cede
		 * control of the GPU while running */
		break;
	case STATE_HOST_IDLE:
		/* can't have the GPU running with the host off */
		break;
	}
}

static void query_scratch_reg_state(struct render_ctx *ctx)
{
	bool gpu_running;
	uint8_t reg;
	int rc;

	rc = read_vga_pw_reg(ctx, &reg);
	if (rc) {
		warnx("error reading VGA scratch register, assuming active");
		gpu_running = true;
	} else {
		gpu_running = reg == 0xa8;
	}

	handle_gpu_state(ctx, gpu_running);
}

static void handle_host_state(struct render_ctx *ctx, bool running)
{
	switch (ctx->state) {
	case STATE_HOST_IDLE:
		if (running) {
			ctx->state = STATE_HOST_GPU_IDLE;
			query_scratch_reg_state(ctx);
			if (ctx->state == STATE_HOST_GPU_IDLE)
				set_renderer_running(ctx, true);
		}
		break;

	case STATE_HOST_GPU_IDLE:
	case STATE_HOST_GPU_RUNNING:
		if (!running) {
			ctx->state = STATE_HOST_IDLE;
			set_renderer_running(ctx, false);
		}
		break;
	}
}

static bool host_state_is_running(const char *state)
{
	return !strcmp(state, host_state_prop_running);
}

/* returns:
 *  <0 on message error
 *   0 if the property isn't applicable
 *   1 if the property was processed
 */
static int handle_host_propchange_entry(struct render_ctx *ctx,
		sd_bus_message *m)
{
	const char *name, *type, *val;
	char c;
	int rc;

	rc = sd_bus_message_enter_container(m,
			SD_BUS_TYPE_DICT_ENTRY, "sv");
	if (rc <= 0)
		return -1;

	rc = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &name);
	if (rc < 0)
		goto out_exit;

	/* is this a host state change? */
	if (strcmp(name, host_state_prop)) {
		rc = 0;
		goto out_exit;
	}

	rc = sd_bus_message_peek_type(m, &c, &type);
	if (rc < 0)
		goto out_exit;

	/* host state change should be a string, but be paranoid and check
	 * anyway */
	if (c != SD_BUS_TYPE_VARIANT || *type != SD_BUS_TYPE_STRING) {
		char skip_type[] = { c, '\0' };
		sd_bus_message_skip(m, skip_type);
		rc = 0;
		goto out_exit;
	}

	rc = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, type);
	if (rc < 0)
		goto out_exit;

	rc = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &val);
	if (rc >= 0) {
		handle_host_state(ctx, host_state_is_running(val));
		rc = 1;
	}

	sd_bus_message_exit_container(m);

out_exit:
	sd_bus_message_exit_container(m);
	return rc;
}
static int handle_dbus_signal_host_propchange(sd_bus_message *m,
		void *data, sd_bus_error *err)
{
	struct render_ctx *ctx = data;
	const char *s;
	int rc;

	/* check that the member and signature indicate a property
	 * change event
	 */
	if (strcmp(sd_bus_message_get_interface(m), dbus_iface_props))
		return 0;

	if (strcmp(sd_bus_message_get_member(m), "PropertiesChanged"))
		return 0;

	/* is the property change part of the state interface? */
	rc = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &s);
	if (rc < 0 || strcmp(s, host_iface_name))
		return 0;

	rc = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
	if (rc < 0)
		return -1;

	for (;;) {
		rc = handle_host_propchange_entry(ctx, m);
		if (rc)
			break;
	}

	return 0;
}

static int handle_dbus_event(struct render_ctx *ctx)
{
	int rc;

	for (;;) {
		rc = sd_bus_process(ctx->bus, NULL);

		if (rc <= 0)
			return rc;
	}
}

static int run_daemon(struct render_ctx *ctx)
{
	struct pollfd pollfds[1];
	int rc, timeout;

	pollfds[0].fd = sd_bus_get_fd(ctx->bus);
	pollfds[0].events = POLLIN;

	for (;;) {
		if (ctx->state == STATE_HOST_GPU_IDLE)
			timeout = scratch_poll_ms;
		else
			timeout = -1;

		rc = poll(pollfds, 1, timeout);

		if (rc < 0) {
			warn("poll failure");
			return -1;
		}

		if (pollfds[0].revents)
			handle_dbus_event(ctx);

		if (ctx->state == STATE_HOST_GPU_IDLE)
			query_scratch_reg_state(ctx);
	}

	return 0;
}

static int query_initial_state(struct render_ctx *ctx)
{
	sd_bus_error sd_err = { 0 };
	char *host_state;
	int rc;

	ctx->state = STATE_HOST_IDLE;

	rc = sd_bus_get_property_string(ctx->bus,
			host_bus_name,
			host_obj_path,
			host_iface_name,
			host_state_prop,
			&sd_err,
			&host_state);

	if (rc < 0) {
		warnx("error querying host state: %d, %s\n",
				rc, sd_err.name);
		sd_bus_error_free(&sd_err);
		return rc;
	}

	handle_host_state(ctx, host_state_is_running(host_state));
	free(host_state);

	return 0;
}

static void usage(const char *progname)
{
	fprintf(stderr, "usage: %s <VGA-reg> <SCU-reg> <renderer-service>\n",
			progname);
}


int main(int argc, char **argv)
{
	const char *vga_pw_path, *video_mode_path;
	struct render_ctx _ctx, *ctx;
	char *match;
	int rc;

	ctx = &_ctx;

	if (argc != 4) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	vga_pw_path = argv[1];
	video_mode_path = argv[2];
	ctx->service_name = argv[3];

	ctx->vga_pw_fd = open(vga_pw_path, O_RDONLY);
	if (ctx->vga_pw_fd < 0)
		err(EXIT_FAILURE, "can't open VGA scratch regs interface at %s",
				vga_pw_path);

	ctx->video_mode_fd = open(video_mode_path, O_WRONLY);
	if (ctx->video_mode_fd < 0)
		err(EXIT_FAILURE, "can't open video mode reg interface at %s",
				video_mode_path);

	rc = sd_bus_default(&ctx->bus);
	if (rc < 0)
		err(EXIT_FAILURE, "can't connect to dbus");


	rc = query_initial_state(ctx);
	if (rc)
		return EXIT_FAILURE;

	rc = asprintf(&match,
			"type='signal',path='%s',interface='%s',member='%s'",
			host_obj_path,
			dbus_iface_props,
			"PropertiesChanged");

	if (rc < 0)
		return EXIT_FAILURE;

	rc = sd_bus_add_match(ctx->bus, NULL, match,
			handle_dbus_signal_host_propchange, ctx);
	if (rc < 0)
		errx(EXIT_FAILURE,
				"can't establish host property change handler");

	rc = run_daemon(ctx);

	return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
