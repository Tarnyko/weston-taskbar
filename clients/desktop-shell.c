/*
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <cairo.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/epoll.h> 
#include <linux/input.h>
#include <libgen.h>
#include <ctype.h>
#include <time.h>

#include <wayland-client.h>
#include "window.h"
#include "../shared/cairo-util.h"
#include "../shared/config-parser.h"

#include "desktop-shell-client-protocol.h"

extern char **environ; /* defined by libc */

struct desktop {
	struct display *display;
	struct desktop_shell *shell;
	uint32_t interface_version;
	struct unlock_dialog *unlock_dialog;
	struct task unlock_task;
	struct wl_list outputs;

	struct window *grab_window;
	struct widget *grab_widget;

	struct weston_config *config;
	int locking;

	enum cursor_type grab_cursor;

	int painted;
};

struct surface {
	void (*configure)(void *data,
			  struct desktop_shell *desktop_shell,
			  uint32_t edges, struct window *window,
			  int32_t width, int32_t height);
};

struct panel {
	struct surface base;
	struct window *window;
	struct widget *widget;
	struct wl_list launcher_list;
	struct panel_clock *clock;
	int painted;
	uint32_t color;
};

struct taskbar {
	struct surface base;
	struct window *window;
	struct widget *widget;
	struct wl_list handler_list;
	struct desktop *desktop;
	int painted;
	uint32_t color;
};

struct background {
	struct surface base;
	struct window *window;
	struct widget *widget;
	int painted;

	char *image;
	int type;
	uint32_t color;
};

struct output {
	struct wl_output *output;
	uint32_t server_output_id;
	struct wl_list link;

	struct panel *panel;
	struct taskbar *taskbar;
	struct background *background;
};

struct panel_launcher {
	struct widget *widget;
	struct panel *panel;
	cairo_surface_t *icon;
	int focused, pressed;
	char *path;
	struct wl_list link;
	struct wl_array envp;
	struct wl_array argv;
};

struct panel_clock {
	struct widget *widget;
	struct panel *panel;
	struct task clock_task;
	int clock_fd;
};

struct taskbar_handler {
	struct widget *widget;
	struct taskbar *taskbar;
	cairo_surface_t *icon;
	int focused, pressed;
	struct managed_surface *surface;
	char *title;
	int state;
	struct wl_list link;
};

struct unlock_dialog {
	struct window *window;
	struct widget *widget;
	struct widget *button;
	int button_focused;
	int closing;
	struct desktop *desktop;
};

static void
panel_add_launchers(struct panel *panel, struct desktop *desktop);

static void
sigchild_handler(int s)
{
	int status;
	pid_t pid;

	while (pid = waitpid(-1, &status, WNOHANG), pid > 0)
		fprintf(stderr, "child %d exited\n", pid);
}

static void
menu_func(struct window *window, struct input *input, int index, void *data)
{
	printf("Selected index %d from a panel menu.\n", index);
}

static void
show_menu(struct panel *panel, struct input *input, uint32_t time)
{
	int32_t x, y;
	static const char *entries[] = {
		"Roy", "Pris", "Leon", "Zhora"
	};

	input_get_position(input, &x, &y);
	window_show_menu(window_get_display(panel->window),
			 input, time, panel->window,
			 x - 10, y - 10, menu_func, entries, 4);
}

static void
update_window(struct window *window)
{
	struct rectangle allocation;
	window_get_allocation(window, &allocation);
	window_schedule_resize(window, allocation.width, allocation.height);
}

static int
is_desktop_painted(struct desktop *desktop)
{
	struct output *output;

	wl_list_for_each(output, &desktop->outputs, link) {
		if (output->panel && !output->panel->painted)
			return 0;
		if (output->taskbar && !output->taskbar->painted)
			return 0;
		if (output->background && !output->background->painted)
			return 0;
	}

	return 1;
}

static void
check_desktop_ready(struct window *window)
{
	struct display *display;
	struct desktop *desktop;

	display = window_get_display(window);
	desktop = display_get_user_data(display);

	if (!desktop->painted && is_desktop_painted(desktop)) {
		desktop->painted = 1;

		if (desktop->interface_version >= 2)
			desktop_shell_desktop_ready(desktop->shell);
	}
}

static void
panel_launcher_activate(struct panel_launcher *widget)
{
	char **argv;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork failed: %m\n");
		return;
	}

	if (pid)
		return;

	argv = widget->argv.data;
	if (execve(argv[0], argv, widget->envp.data) < 0) {
		fprintf(stderr, "execl '%s' failed: %m\n", argv[0]);
		exit(1);
	}
}

static void
panel_launcher_redraw_handler(struct widget *widget, void *data)
{
	struct panel_launcher *launcher = data;
	struct rectangle allocation;
	cairo_t *cr;

	cr = widget_cairo_create(launcher->panel->widget);

	widget_get_allocation(widget, &allocation);
	if (launcher->pressed) {
		allocation.x++;
		allocation.y++;
	}

	cairo_set_source_surface(cr, launcher->icon,
				 allocation.x, allocation.y);
	cairo_paint(cr);

	if (launcher->focused) {
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
		cairo_mask_surface(cr, launcher->icon,
				   allocation.x, allocation.y);
	}

	cairo_destroy(cr);
}

static int
panel_launcher_motion_handler(struct widget *widget, struct input *input,
			      uint32_t time, float x, float y, void *data)
{
	struct panel_launcher *launcher = data;

	widget_set_tooltip(widget, basename((char *)launcher->path), x, y);

	return CURSOR_LEFT_PTR;
}

static void
set_hex_color(cairo_t *cr, uint32_t color)
{
	cairo_set_source_rgba(cr, 
			      ((color >> 16) & 0xff) / 255.0,
			      ((color >>  8) & 0xff) / 255.0,
			      ((color >>  0) & 0xff) / 255.0,
			      ((color >> 24) & 0xff) / 255.0);
}

static void
panel_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	struct panel *panel = data;

	cr = widget_cairo_create(panel->widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_hex_color(cr, panel->color);
	cairo_paint(cr);

	cairo_destroy(cr);
	surface = window_get_surface(panel->window);
	cairo_surface_destroy(surface);
	panel->painted = 1;
	check_desktop_ready(panel->window);
}

static int
panel_launcher_enter_handler(struct widget *widget, struct input *input,
			     float x, float y, void *data)
{
	struct panel_launcher *launcher = data;

	launcher->focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
panel_launcher_leave_handler(struct widget *widget,
			     struct input *input, void *data)
{
	struct panel_launcher *launcher = data;

	launcher->focused = 0;
	widget_destroy_tooltip(widget);
	widget_schedule_redraw(widget);
}

static void
panel_launcher_button_handler(struct widget *widget,
			      struct input *input, uint32_t time,
			      uint32_t button,
			      enum wl_pointer_button_state state, void *data)
{
	struct panel_launcher *launcher;

	launcher = widget_get_user_data(widget);
	widget_schedule_redraw(widget);
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		panel_launcher_activate(launcher);

}

static void
panel_launcher_touch_down_handler(struct widget *widget, struct input *input,
				  uint32_t serial, uint32_t time, int32_t id,
				  float x, float y, void *data)
{
	struct panel_launcher *launcher;

	launcher = widget_get_user_data(widget);
	launcher->focused = 1;
	widget_schedule_redraw(widget);
}

static void
panel_launcher_touch_up_handler(struct widget *widget, struct input *input,
				uint32_t serial, uint32_t time, int32_t id, 
				void *data)
{
	struct panel_launcher *launcher;

	launcher = widget_get_user_data(widget);
	launcher->focused = 0;
	widget_schedule_redraw(widget);
	panel_launcher_activate(launcher);
}

static void
clock_func(struct task *task, uint32_t events)
{
	struct panel_clock *clock =
		container_of(task, struct panel_clock, clock_task);
	uint64_t exp;

	if (read(clock->clock_fd, &exp, sizeof exp) != sizeof exp)
		abort();
	widget_schedule_redraw(clock->widget);
}

static void
panel_clock_redraw_handler(struct widget *widget, void *data)
{
	struct panel_clock *clock = data;
	cairo_t *cr;
	struct rectangle allocation;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	time_t rawtime;
	struct tm * timeinfo;
	char string[128];

	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(string, sizeof string, "%a %b %d, %I:%M %p", timeinfo);

	widget_get_allocation(widget, &allocation);
	if (allocation.width == 0)
		return;

	cr = widget_cairo_create(clock->panel->widget);
	cairo_select_font_face(cr, "sans",
			       CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, string, &extents);
	cairo_font_extents (cr, &font_extents);
	cairo_move_to(cr, allocation.x + 5,
		      allocation.y + 3 * (allocation.height >> 2) + 1);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_show_text(cr, string);
	cairo_move_to(cr, allocation.x + 4,
		      allocation.y + 3 * (allocation.height >> 2));
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_show_text(cr, string);
	cairo_destroy(cr);
}

static int
clock_timer_reset(struct panel_clock *clock)
{
	struct itimerspec its;

	its.it_interval.tv_sec = 60;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = 60;
	its.it_value.tv_nsec = 0;
	if (timerfd_settime(clock->clock_fd, 0, &its, NULL) < 0) {
		fprintf(stderr, "could not set timerfd\n: %m");
		return -1;
	}

	return 0;
}

static void
panel_destroy_clock(struct panel_clock *clock)
{
	widget_destroy(clock->widget);

	close(clock->clock_fd);

	free(clock);
}

static void
panel_add_clock(struct panel *panel)
{
	struct panel_clock *clock;
	int timerfd;

	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (timerfd < 0) {
		fprintf(stderr, "could not create timerfd\n: %m");
		return;
	}

	clock = xzalloc(sizeof *clock);
	clock->panel = panel;
	panel->clock = clock;
	clock->clock_fd = timerfd;

	clock->clock_task.run = clock_func;
	display_watch_fd(window_get_display(panel->window), clock->clock_fd,
			 EPOLLIN, &clock->clock_task);
	clock_timer_reset(clock);

	clock->widget = widget_add_widget(panel->widget, clock);
	widget_set_redraw_handler(clock->widget, panel_clock_redraw_handler);
}

static void
panel_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button,
		     enum wl_pointer_button_state state, void *data)
{
	struct panel *panel = data;

	if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		show_menu(panel, input, time);
}

static void
panel_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct panel_launcher *launcher;
	struct panel *panel = data;
	int x, y, w, h;
	
	x = 10;
	y = 16;
	wl_list_for_each(launcher, &panel->launcher_list, link) {
		w = cairo_image_surface_get_width(launcher->icon);
		h = cairo_image_surface_get_height(launcher->icon);
		widget_set_allocation(launcher->widget,
				      x, y - h / 2, w + 1, h + 1);
		x += w + 10;
	}
	h=20;
	w=170;

	if (panel->clock)
		widget_set_allocation(panel->clock->widget,
				      width - w - 8, y - h / 2, w + 1, h + 1);
}

static void
panel_configure(void *data,
		struct desktop_shell *desktop_shell,
		uint32_t edges, struct window *window,
		int32_t width, int32_t height)
{
	struct surface *surface = window_get_user_data(window);
	struct panel *panel = container_of(surface, struct panel, base);

	window_schedule_resize(panel->window, width, 32);
}

static void
panel_destroy_launcher(struct panel_launcher *launcher)
{
	wl_array_release(&launcher->argv);
	wl_array_release(&launcher->envp);

	free(launcher->path);

	cairo_surface_destroy(launcher->icon);

	widget_destroy(launcher->widget);
	wl_list_remove(&launcher->link);

	free(launcher);
}

static void
panel_destroy(struct panel *panel)
{
	struct panel_launcher *tmp;
	struct panel_launcher *launcher;

	panel_destroy_clock(panel->clock);

	wl_list_for_each_safe(launcher, tmp, &panel->launcher_list, link)
		panel_destroy_launcher(launcher);

	widget_destroy(panel->widget);
	window_destroy(panel->window);

	free(panel);
}

static struct panel *
panel_create(struct desktop *desktop)
{
	struct panel *panel;
	struct weston_config_section *s;

	panel = xzalloc(sizeof *panel);

	panel->base.configure = panel_configure;
	panel->window = window_create_custom(desktop->display);
	panel->widget = window_add_widget(panel->window, panel);
	wl_list_init(&panel->launcher_list);

	window_set_title(panel->window, "panel");
	window_set_user_data(panel->window, panel);

	widget_set_redraw_handler(panel->widget, panel_redraw_handler);
	widget_set_resize_handler(panel->widget, panel_resize_handler);
	widget_set_button_handler(panel->widget, panel_button_handler);
	
	panel_add_clock(panel);

	s = weston_config_get_section(desktop->config, "shell", NULL, NULL);
	weston_config_section_get_uint(s, "panel-color",
				       &panel->color, 0xaa000000);

	panel_add_launchers(panel, desktop);

	return panel;
}

static void
taskbar_handler_activate(struct taskbar_handler *handler)
{
	 /* invert the handler state */
	if (handler->state == 0)
		handler->state = 1;
	else
		handler->state = 0;

	 /* request the compositor to minimize/raise the window */
	managed_surface_set_state(handler->surface, handler->state);
}

static void
taskbar_handler_redraw_handler(struct widget *widget, void *data)
{
	struct taskbar_handler *handler = data;
	struct rectangle allocation;
	cairo_t *cr;

	cr = widget_cairo_create(handler->taskbar->widget);

	widget_get_allocation(widget, &allocation);
	if (handler->pressed) {
		allocation.x++;
		allocation.y++;
	}

	cairo_set_source_surface(cr, handler->icon,
				 allocation.x, allocation.y);
	cairo_paint(cr);

	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
	/* cairo_set_font_size (cr, 20); */
	cairo_move_to (cr, allocation.x+20, allocation.y+12);
	cairo_show_text (cr, handler->title);

	if (handler->focused) {
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
		cairo_mask_surface(cr, handler->icon,
				   allocation.x, allocation.y);
	}

	cairo_destroy(cr);
}

static void
taskbar_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	struct taskbar *taskbar = data;

	cr = widget_cairo_create(taskbar->widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_hex_color(cr, taskbar->color);
	cairo_paint(cr);

	cairo_destroy(cr);
	surface = window_get_surface(taskbar->window);
	cairo_surface_destroy(surface);
	taskbar->painted = 1;
	check_desktop_ready(taskbar->window);
}

static int
taskbar_handler_enter_handler(struct widget *widget, struct input *input,
			     float x, float y, void *data)
{
	struct taskbar_handler *handler = data;

	handler->focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
taskbar_handler_leave_handler(struct widget *widget,
			     struct input *input, void *data)
{
	struct taskbar_handler *handler = data;

	handler->focused = 0;
	/* no tooltip yet... */
	/* widget_destroy_tooltip(widget); */
	widget_schedule_redraw(widget);
}

static void
taskbar_handler_button_handler(struct widget *widget,
			      struct input *input, uint32_t time,
			      uint32_t butt,
			      enum wl_pointer_button_state state, void *data)
{
	struct taskbar_handler *handler;

	handler = widget_get_user_data(widget);
	widget_schedule_redraw(widget);
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		taskbar_handler_activate(handler);
}

static void
taskbar_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct taskbar_handler *handler;
	struct taskbar *taskbar = data;
	cairo_t *cr;
	cairo_text_extents_t extents;
	int x, y, w, h;
	
	x = 10;
	y = 16;
	wl_list_for_each(handler, &taskbar->handler_list, link) {
		cr = cairo_create (handler->icon);
		cairo_text_extents (cr, handler->title, &extents);

		w = cairo_image_surface_get_width(handler->icon) + extents.width + 8;
		h = cairo_image_surface_get_height(handler->icon);
		widget_set_allocation(handler->widget,
				      x, y - h / 2, w + 1, h + 1);
		x += w + 10;

		cairo_destroy (cr);
	}
}

static void
taskbar_configure(void *data,
		struct desktop_shell *desktop_shell,
		uint32_t edges, struct window *window,
		int32_t width, int32_t height)
{
	struct surface *surface = window_get_user_data(window);
	struct taskbar *taskbar = container_of(surface, struct taskbar, base);

	window_schedule_resize(taskbar->window, width, 32);
}

static void
taskbar_destroy_handler(struct taskbar_handler *handler)
{
	free(handler->title);

	cairo_surface_destroy(handler->icon);

	widget_destroy(handler->widget);
	wl_list_remove(&handler->link);

	free(handler);
}

static void
taskbar_destroy(struct taskbar *taskbar)
{
	struct taskbar_handler *tmp;
	struct taskbar_handler *handler;

	wl_list_for_each_safe(handler, tmp, &taskbar->handler_list, link) {
		taskbar_destroy_handler(handler);
	}

	widget_destroy(taskbar->widget);
	window_destroy(taskbar->window);

	free(taskbar);
}

static struct taskbar *
taskbar_create(struct desktop *desktop)
{
	struct taskbar *taskbar;
	struct weston_config_section *s;

	taskbar = xzalloc(sizeof *taskbar);

	taskbar->base.configure = taskbar_configure;
	taskbar->desktop = desktop;
	taskbar->window = window_create_custom(desktop->display);
	taskbar->widget = window_add_widget(taskbar->window, taskbar);
	wl_list_init(&taskbar->handler_list);

	window_set_title(taskbar->window, "taskbar");
	window_set_user_data(taskbar->window, taskbar);

	widget_set_redraw_handler(taskbar->widget, taskbar_redraw_handler);
	widget_set_resize_handler(taskbar->widget, taskbar_resize_handler);

	s = weston_config_get_section(desktop->config, "shell", NULL, NULL);
	weston_config_section_get_uint(s, "taskbar-color",
				       &taskbar->color, 0xaabbbbbb);

	return taskbar;
}

static cairo_surface_t *
load_icon_or_fallback(const char *icon)
{
	cairo_surface_t *surface = cairo_image_surface_create_from_png(icon);
	cairo_status_t status;
	cairo_t *cr;

	status = cairo_surface_status(surface);
	if (status == CAIRO_STATUS_SUCCESS)
		return surface;

	cairo_surface_destroy(surface);
	fprintf(stderr, "ERROR loading icon from file '%s', error: '%s'\n",
		icon, cairo_status_to_string(status));

	/* draw fallback icon */
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
					     20, 20);
	cr = cairo_create(surface);

	cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 1);
	cairo_paint(cr);

	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_rectangle(cr, 0, 0, 20, 20);
	cairo_move_to(cr, 4, 4);
	cairo_line_to(cr, 16, 16);
	cairo_move_to(cr, 4, 16);
	cairo_line_to(cr, 16, 4);
	cairo_stroke(cr);

	cairo_destroy(cr);

	return surface;
}

static void
panel_add_launcher(struct panel *panel, const char *icon, const char *path)
{
	struct panel_launcher *launcher;
	char *start, *p, *eq, **ps;
	int i, j, k;

	launcher = xzalloc(sizeof *launcher);
	launcher->icon = load_icon_or_fallback(icon);
	launcher->path = xstrdup(path);

	wl_array_init(&launcher->envp);
	wl_array_init(&launcher->argv);
	for (i = 0; environ[i]; i++) {
		ps = wl_array_add(&launcher->envp, sizeof *ps);
		*ps = environ[i];
	}
	j = 0;

	start = launcher->path;
	while (*start) {
		for (p = start, eq = NULL; *p && !isspace(*p); p++)
			if (*p == '=')
				eq = p;

		if (eq && j == 0) {
			ps = launcher->envp.data;
			for (k = 0; k < i; k++)
				if (strncmp(ps[k], start, eq - start) == 0) {
					ps[k] = start;
					break;
				}
			if (k == i) {
				ps = wl_array_add(&launcher->envp, sizeof *ps);
				*ps = start;
				i++;
			}
		} else {
			ps = wl_array_add(&launcher->argv, sizeof *ps);
			*ps = start;
			j++;
		}

		while (*p && isspace(*p))
			*p++ = '\0';

		start = p;
	}

	ps = wl_array_add(&launcher->envp, sizeof *ps);
	*ps = NULL;
	ps = wl_array_add(&launcher->argv, sizeof *ps);
	*ps = NULL;

	launcher->panel = panel;
	wl_list_insert(panel->launcher_list.prev, &launcher->link);

	launcher->widget = widget_add_widget(panel->widget, launcher);
	widget_set_enter_handler(launcher->widget,
				 panel_launcher_enter_handler);
	widget_set_leave_handler(launcher->widget,
				   panel_launcher_leave_handler);
	widget_set_button_handler(launcher->widget,
				    panel_launcher_button_handler);
	widget_set_touch_down_handler(launcher->widget,
				      panel_launcher_touch_down_handler);
	widget_set_touch_up_handler(launcher->widget,
				    panel_launcher_touch_up_handler);
	widget_set_redraw_handler(launcher->widget,
				  panel_launcher_redraw_handler);
	widget_set_motion_handler(launcher->widget,
				  panel_launcher_motion_handler);
}

static const struct managed_surface_listener managed_surface_listener;

static void
taskbar_add_handler(struct taskbar *taskbar,
			        struct managed_surface *managed_surface, 
			        const char *title)
{
	struct taskbar_handler *handler;

	handler = xzalloc(sizeof *handler);
	handler->icon = load_icon_or_fallback(DATADIR "/weston/icon_window.png");
	handler->surface = managed_surface;
	handler->title = strdup(title);
	handler->state = 0;

	handler->taskbar = taskbar;
	wl_list_insert(taskbar->handler_list.prev, &handler->link);

	handler->widget = widget_add_widget(taskbar->widget, handler);
	widget_set_enter_handler(handler->widget,
				 taskbar_handler_enter_handler);
	widget_set_leave_handler(handler->widget,
				   taskbar_handler_leave_handler);
	widget_set_button_handler(handler->widget,
				    taskbar_handler_button_handler);
	widget_set_redraw_handler(handler->widget,
				  taskbar_handler_redraw_handler);

	managed_surface_add_listener(handler->surface,
	                             &managed_surface_listener,
	                             handler);
}

enum {
	BACKGROUND_SCALE,
	BACKGROUND_SCALE_CROP,
	BACKGROUND_TILE
};

static void
background_draw(struct widget *widget, void *data)
{
	struct background *background = data;
	cairo_surface_t *surface, *image;
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	cairo_t *cr;
	double im_w, im_h;
	double sx, sy, s;
	double tx, ty;
	struct rectangle allocation;
	struct display *display;
	struct wl_region *opaque;

	surface = window_get_surface(background->window);

	cr = widget_cairo_create(background->widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.2, 1.0);
	cairo_paint(cr);

	widget_get_allocation(widget, &allocation);
	image = NULL;
	if (background->image)
		image = load_cairo_surface(background->image);

	if (image && background->type != -1) {
		im_w = cairo_image_surface_get_width(image);
		im_h = cairo_image_surface_get_height(image);
		sx = im_w / allocation.width;
		sy = im_h / allocation.height;

		pattern = cairo_pattern_create_for_surface(image);

		switch (background->type) {
		case BACKGROUND_SCALE:
			cairo_matrix_init_scale(&matrix, sx, sy);
			cairo_pattern_set_matrix(pattern, &matrix);
			break;
		case BACKGROUND_SCALE_CROP:
			s = (sx < sy) ? sx : sy;
			/* align center */
			tx = (im_w - s * allocation.width) * 0.5;
			ty = (im_h - s * allocation.height) * 0.5;
			cairo_matrix_init_translate(&matrix, tx, ty);
			cairo_matrix_scale(&matrix, s, s);
			cairo_pattern_set_matrix(pattern, &matrix);
			break;
		case BACKGROUND_TILE:
			cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
			break;
		}

		cairo_set_source(cr, pattern);
		cairo_pattern_destroy (pattern);
		cairo_surface_destroy(image);
	} else {
		set_hex_color(cr, background->color);
	}

	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	display = window_get_display(background->window);
	opaque = wl_compositor_create_region(display_get_compositor(display));
	wl_region_add(opaque, allocation.x, allocation.y,
		      allocation.width, allocation.height);
	wl_surface_set_opaque_region(window_get_wl_surface(background->window), opaque);
	wl_region_destroy(opaque);

	background->painted = 1;
	check_desktop_ready(background->window);
}

static void
background_configure(void *data,
		     struct desktop_shell *desktop_shell,
		     uint32_t edges, struct window *window,
		     int32_t width, int32_t height)
{
	struct background *background =
		(struct background *) window_get_user_data(window);

	widget_schedule_resize(background->widget, width, height);
}

static void
unlock_dialog_redraw_handler(struct widget *widget, void *data)
{
	struct unlock_dialog *dialog = data;
	struct rectangle allocation;
	cairo_surface_t *surface;
	cairo_t *cr;
	cairo_pattern_t *pat;
	double cx, cy, r, f;

	cr = widget_cairo_create(widget);

	widget_get_allocation(dialog->widget, &allocation);
	cairo_rectangle(cr, allocation.x, allocation.y,
			allocation.width, allocation.height);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
	cairo_fill(cr);

	cairo_translate(cr, allocation.x, allocation.y);
	if (dialog->button_focused)
		f = 1.0;
	else
		f = 0.7;

	cx = allocation.width / 2.0;
	cy = allocation.height / 2.0;
	r = (cx < cy ? cx : cy) * 0.4;
	pat = cairo_pattern_create_radial(cx, cy, r * 0.7, cx, cy, r);
	cairo_pattern_add_color_stop_rgb(pat, 0.0, 0, 0.86 * f, 0);
	cairo_pattern_add_color_stop_rgb(pat, 0.85, 0.2 * f, f, 0.2 * f);
	cairo_pattern_add_color_stop_rgb(pat, 1.0, 0, 0.86 * f, 0);
	cairo_set_source(cr, pat);
	cairo_pattern_destroy(pat);
	cairo_arc(cr, cx, cy, r, 0.0, 2.0 * M_PI);
	cairo_fill(cr);

	widget_set_allocation(dialog->button,
			      allocation.x + cx - r,
			      allocation.y + cy - r, 2 * r, 2 * r);

	cairo_destroy(cr);

	surface = window_get_surface(dialog->window);
	cairo_surface_destroy(surface);
}

static void
unlock_dialog_button_handler(struct widget *widget,
			     struct input *input, uint32_t time,
			     uint32_t button,
			     enum wl_pointer_button_state state, void *data)
{
	struct unlock_dialog *dialog = data;
	struct desktop *desktop = dialog->desktop;

	if (button == BTN_LEFT) {
		if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
		    !dialog->closing) {
			display_defer(desktop->display, &desktop->unlock_task);
			dialog->closing = 1;
		}
	}
}

static void
unlock_dialog_touch_down_handler(struct widget *widget, struct input *input,
		   uint32_t serial, uint32_t time, int32_t id,
		   float x, float y, void *data)
{
	struct unlock_dialog *dialog = data;

	dialog->button_focused = 1;
	widget_schedule_redraw(widget);
}

static void
unlock_dialog_touch_up_handler(struct widget *widget, struct input *input,
				uint32_t serial, uint32_t time, int32_t id,
				void *data)
{
	struct unlock_dialog *dialog = data;
	struct desktop *desktop = dialog->desktop;

	dialog->button_focused = 0;
	widget_schedule_redraw(widget);
	display_defer(desktop->display, &desktop->unlock_task);
	dialog->closing = 1;
}

static void
unlock_dialog_keyboard_focus_handler(struct window *window,
				     struct input *device, void *data)
{
	window_schedule_redraw(window);
}

static int
unlock_dialog_widget_enter_handler(struct widget *widget,
				   struct input *input,
				   float x, float y, void *data)
{
	struct unlock_dialog *dialog = data;

	dialog->button_focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
unlock_dialog_widget_leave_handler(struct widget *widget,
				   struct input *input, void *data)
{
	struct unlock_dialog *dialog = data;

	dialog->button_focused = 0;
	widget_schedule_redraw(widget);
}

static struct unlock_dialog *
unlock_dialog_create(struct desktop *desktop)
{
	struct display *display = desktop->display;
	struct unlock_dialog *dialog;

	dialog = xzalloc(sizeof *dialog);

	dialog->window = window_create_custom(display);
	dialog->widget = window_frame_create(dialog->window, dialog);
	window_set_title(dialog->window, "Unlock your desktop");

	window_set_user_data(dialog->window, dialog);
	window_set_keyboard_focus_handler(dialog->window,
					  unlock_dialog_keyboard_focus_handler);
	dialog->button = widget_add_widget(dialog->widget, dialog);
	widget_set_redraw_handler(dialog->widget,
				  unlock_dialog_redraw_handler);
	widget_set_enter_handler(dialog->button,
				 unlock_dialog_widget_enter_handler);
	widget_set_leave_handler(dialog->button,
				 unlock_dialog_widget_leave_handler);
	widget_set_button_handler(dialog->button,
				  unlock_dialog_button_handler);
	widget_set_touch_down_handler(dialog->button,
				      unlock_dialog_touch_down_handler);
	widget_set_touch_up_handler(dialog->button,
				      unlock_dialog_touch_up_handler);

	desktop_shell_set_lock_surface(desktop->shell,
				       window_get_wl_surface(dialog->window));

	window_schedule_resize(dialog->window, 260, 230);

	return dialog;
}

static void
unlock_dialog_destroy(struct unlock_dialog *dialog)
{
	window_destroy(dialog->window);
	free(dialog);
}

static void
unlock_dialog_finish(struct task *task, uint32_t events)
{
	struct desktop *desktop =
		container_of(task, struct desktop, unlock_task);

	desktop_shell_unlock(desktop->shell);
	unlock_dialog_destroy(desktop->unlock_dialog);
	desktop->unlock_dialog = NULL;
}

static void
desktop_shell_configure(void *data,
			struct desktop_shell *desktop_shell,
			uint32_t edges,
			struct wl_surface *surface,
			int32_t width, int32_t height)
{
	struct window *window = wl_surface_get_user_data(surface);
	struct surface *s = window_get_user_data(window);

	s->configure(data, desktop_shell, edges, window, width, height);
}

static void
desktop_shell_prepare_lock_surface(void *data,
				   struct desktop_shell *desktop_shell)
{
	struct desktop *desktop = data;

	if (!desktop->locking) {
		desktop_shell_unlock(desktop->shell);
		return;
	}

	if (!desktop->unlock_dialog) {
		desktop->unlock_dialog = unlock_dialog_create(desktop);
		desktop->unlock_dialog->desktop = desktop;
	}
}

static void
desktop_shell_grab_cursor(void *data,
			  struct desktop_shell *desktop_shell,
			  uint32_t cursor)
{
	struct desktop *desktop = data;

	switch (cursor) {
	case DESKTOP_SHELL_CURSOR_NONE:
		desktop->grab_cursor = CURSOR_BLANK;
		break;
	case DESKTOP_SHELL_CURSOR_BUSY:
		desktop->grab_cursor = CURSOR_WATCH;
		break;
	case DESKTOP_SHELL_CURSOR_MOVE:
		desktop->grab_cursor = CURSOR_DRAGGING;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP:
		desktop->grab_cursor = CURSOR_TOP;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM:
		desktop->grab_cursor = CURSOR_BOTTOM;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_LEFT:
		desktop->grab_cursor = CURSOR_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_RIGHT:
		desktop->grab_cursor = CURSOR_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP_LEFT:
		desktop->grab_cursor = CURSOR_TOP_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP_RIGHT:
		desktop->grab_cursor = CURSOR_TOP_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM_LEFT:
		desktop->grab_cursor = CURSOR_BOTTOM_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM_RIGHT:
		desktop->grab_cursor = CURSOR_BOTTOM_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_ARROW:
	default:
		desktop->grab_cursor = CURSOR_LEFT_PTR;
	}
}

static void
desktop_shell_add_managed_surface(void *data,
				   struct desktop_shell *desktop_shell,
				   struct managed_surface *managed_surface)
{
	struct desktop *desktop = data;
	struct output *output;

	wl_list_for_each(output, &desktop->outputs, link) {
		/* add a handler with default title */
		taskbar_add_handler(output->taskbar, managed_surface, "<Default>");
		update_window(output->taskbar->window);
	}
}

static const struct desktop_shell_listener listener = {
	desktop_shell_configure,
	desktop_shell_prepare_lock_surface,
	desktop_shell_grab_cursor,
	desktop_shell_add_managed_surface
};

static void
managed_surface_state_changed(void *data,
		struct managed_surface *managed_surface,
		uint32_t state)
{
	struct taskbar_handler *handler = data;

	if (handler->surface == managed_surface) {
		/* set the handler state */
		handler->state = state;
	}
}

static void
managed_surface_title_changed(void *data,
		struct managed_surface *managed_surface,
		const char *title)
{
	struct taskbar_handler *handler = data;

	if (handler->surface == managed_surface) {
		/* change the handler title text */
		handler->title = strdup(title);
		update_window(handler->taskbar->window);
	}
}

static void
managed_surface_removed(void *data,
		struct managed_surface *managed_surface)
{
	struct taskbar_handler *handler = data;

	if (handler->surface == managed_surface) {
		/* destroy the handler */
		taskbar_destroy_handler(handler);
		update_window(handler->taskbar->window);

		managed_surface_destroy(managed_surface);
	}
}

static const struct managed_surface_listener managed_surface_listener = {
	managed_surface_state_changed,
	managed_surface_title_changed,
	managed_surface_removed
};

static void
background_destroy(struct background *background)
{
	widget_destroy(background->widget);
	window_destroy(background->window);

	free(background->image);
	free(background);
}

static struct background *
background_create(struct desktop *desktop)
{
	struct background *background;
	struct weston_config_section *s;
	char *type;

	background = xzalloc(sizeof *background);
	background->base.configure = background_configure;
	background->window = window_create_custom(desktop->display);
	background->widget = window_add_widget(background->window, background);
	window_set_user_data(background->window, background);
	widget_set_redraw_handler(background->widget, background_draw);
	window_set_preferred_format(background->window,
				    WINDOW_PREFERRED_FORMAT_RGB565);

	s = weston_config_get_section(desktop->config, "shell", NULL, NULL);
	weston_config_section_get_string(s, "background-image",
					 &background->image,
					 DATADIR "/weston/pattern.png");
	weston_config_section_get_uint(s, "background-color",
				       &background->color, 0xff002244);

	weston_config_section_get_string(s, "background-type",
					 &type, "tile");
	if (type == NULL) {
		fprintf(stderr, "%s: out of memory\n", program_invocation_short_name);
		exit(EXIT_FAILURE);
	}

	if (strcmp(type, "scale") == 0) {
		background->type = BACKGROUND_SCALE;
	} else if (strcmp(type, "scale-crop") == 0) {
		background->type = BACKGROUND_SCALE_CROP;
	} else if (strcmp(type, "tile") == 0) {
		background->type = BACKGROUND_TILE;
	} else {
		background->type = -1;
		fprintf(stderr, "invalid background-type: %s\n",
			type);
	}

	free(type);

	return background;
}

static int
grab_surface_enter_handler(struct widget *widget, struct input *input,
			   float x, float y, void *data)
{
	struct desktop *desktop = data;

	return desktop->grab_cursor;
}

static void
grab_surface_destroy(struct desktop *desktop)
{
	widget_destroy(desktop->grab_widget);
	window_destroy(desktop->grab_window);
}

static void
grab_surface_create(struct desktop *desktop)
{
	struct wl_surface *s;

	desktop->grab_window = window_create_custom(desktop->display);
	window_set_user_data(desktop->grab_window, desktop);

	s = window_get_wl_surface(desktop->grab_window);
	desktop_shell_set_grab_surface(desktop->shell, s);

	desktop->grab_widget =
		window_add_widget(desktop->grab_window, desktop);
	/* We set the allocation to 1x1 at 0,0 so the fake enter event
	 * at 0,0 will go to this widget. */
	widget_set_allocation(desktop->grab_widget, 0, 0, 1, 1);

	widget_set_enter_handler(desktop->grab_widget,
				 grab_surface_enter_handler);
}

static void
output_destroy(struct output *output)
{
	background_destroy(output->background);
	panel_destroy(output->panel);
	taskbar_destroy(output->taskbar);
	wl_output_destroy(output->output);
	wl_list_remove(&output->link);

	free(output);
}

static void
desktop_destroy_outputs(struct desktop *desktop)
{
	struct output *tmp;
	struct output *output;

	wl_list_for_each_safe(output, tmp, &desktop->outputs, link)
		output_destroy(output);
}

static void
output_handle_geometry(void *data,
                       struct wl_output *wl_output,
                       int x, int y,
                       int physical_width,
                       int physical_height,
                       int subpixel,
                       const char *make,
                       const char *model,
                       int transform)
{
	struct output *output = data;

	window_set_buffer_transform(output->panel->window, transform);
	window_set_buffer_transform(output->taskbar->window, transform);
	window_set_buffer_transform(output->background->window, transform);
}

static void
output_handle_mode(void *data,
		   struct wl_output *wl_output,
		   uint32_t flags,
		   int width,
		   int height,
		   int refresh)
{
}

static void
output_handle_done(void *data,
                   struct wl_output *wl_output)
{
}

static void
output_handle_scale(void *data,
                    struct wl_output *wl_output,
                    int32_t scale)
{
	struct output *output = data;

	window_set_buffer_scale(output->panel->window, scale);
	window_set_buffer_scale(output->taskbar->window, scale);
	window_set_buffer_scale(output->background->window, scale);
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale
};

static void
output_init(struct output *output, struct desktop *desktop)
{
	struct wl_surface *surface;

	output->panel = panel_create(desktop);
	surface = window_get_wl_surface(output->panel->window);
	desktop_shell_set_panel(desktop->shell,
				output->output, surface);

	output->taskbar = taskbar_create(desktop);
	surface = window_get_wl_surface(output->taskbar->window);
	desktop_shell_set_taskbar(desktop->shell,
				output->output, surface);

	output->background = background_create(desktop);
	surface = window_get_wl_surface(output->background->window);
	desktop_shell_set_background(desktop->shell,
				     output->output, surface);
}

static void
create_output(struct desktop *desktop, uint32_t id)
{
	struct output *output;

	output = calloc(1, sizeof *output);
	if (!output)
		return;

	output->output =
		display_bind(desktop->display, id, &wl_output_interface, 2);
	output->server_output_id = id;

	wl_output_add_listener(output->output, &output_listener, output);

	wl_list_insert(&desktop->outputs, &output->link);

	/* On start up we may process an output global before the shell global
	 * in which case we can't create the panel and background just yet */
	if (desktop->shell)
		output_init(output, desktop);
}

static void
global_handler(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct desktop *desktop = data;

	if (!strcmp(interface, "desktop_shell")) {
		desktop->interface_version = (version < 2) ? version : 2;
		desktop->shell = display_bind(desktop->display,
					      id, &desktop_shell_interface,
					      desktop->interface_version);
		desktop_shell_add_listener(desktop->shell, &listener, desktop);
	} else if (!strcmp(interface, "wl_output")) {
		create_output(desktop, id);
	}
}

static void
global_handler_remove(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct desktop *desktop = data;
	struct output *output;

	if (!strcmp(interface, "wl_output")) {
		wl_list_for_each(output, &desktop->outputs, link) {
			if (output->server_output_id == id) {
				output_destroy(output);
				break;
			}
		}
	}
}

static void
panel_add_launchers(struct panel *panel, struct desktop *desktop)
{
	struct weston_config_section *s;
	char *icon, *path;
	const char *name;
	int count;

	count = 0;
	s = NULL;
	while (weston_config_next_section(desktop->config, &s, &name)) {
		if (strcmp(name, "launcher") != 0)
			continue;

		weston_config_section_get_string(s, "icon", &icon, NULL);
		weston_config_section_get_string(s, "path", &path, NULL);

		if (icon != NULL && path != NULL) {
			panel_add_launcher(panel, icon, path);
			count++;
		} else {
			fprintf(stderr, "invalid launcher section\n");
		}

		free(icon);
		free(path);
	}

	if (count == 0) {
		/* add default launcher */
		panel_add_launcher(panel,
				   DATADIR "/weston/terminal.png",
				   BINDIR "/weston-terminal");
	}
}

int main(int argc, char *argv[])
{
	struct desktop desktop = { 0 };
	struct output *output;
	struct weston_config_section *s;

	desktop.unlock_task.run = unlock_dialog_finish;
	wl_list_init(&desktop.outputs);

	desktop.config = weston_config_parse("weston.ini");
	s = weston_config_get_section(desktop.config, "shell", NULL, NULL);
	weston_config_section_get_bool(s, "locking", &desktop.locking, 1);

	desktop.display = display_create(&argc, argv);
	if (desktop.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(desktop.display, &desktop);
	display_set_global_handler(desktop.display, global_handler);
	display_set_global_handler_remove(desktop.display, global_handler_remove);

	/* Create panel and background for outputs processed before the shell
	 * global interface was processed */
	wl_list_for_each(output, &desktop.outputs, link)
		if (!output->panel)
			output_init(output, &desktop);

	grab_surface_create(&desktop);

	signal(SIGCHLD, sigchild_handler);

	display_run(desktop.display);

	/* Cleanup */
	grab_surface_destroy(&desktop);
	desktop_destroy_outputs(&desktop);
	if (desktop.unlock_dialog)
		unlock_dialog_destroy(desktop.unlock_dialog);
	desktop_shell_destroy(desktop.shell);
	display_destroy(desktop.display);

	return 0;
}
