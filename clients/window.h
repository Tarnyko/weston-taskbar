/*
 * Copyright © 2008 Kristian Høgsberg
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

#ifndef _WINDOW_H_
#define _WINDOW_H_

#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include <cairo.h>
#include "../shared/config-parser.h"
#include "../shared/zalloc.h"
#include "subsurface-client-protocol.h"

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

#define container_of(ptr, type, member) ({				\
	const __typeof__( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct window;
struct widget;
struct display;
struct input;
struct output;

struct task {
	void (*run)(struct task *task, uint32_t events);
	struct wl_list link;
};

struct rectangle {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

void *
fail_on_null(void *p);
void *
xmalloc(size_t s);
void *
xzalloc(size_t s);
char *
xstrdup(const char *s);

struct display *
display_create(int *argc, char *argv[]);

void
display_destroy(struct display *display);

void
display_set_user_data(struct display *display, void *data);

void *
display_get_user_data(struct display *display);

struct wl_display *
display_get_display(struct display *display);

int
display_has_subcompositor(struct display *display);

cairo_device_t *
display_get_cairo_device(struct display *display);

struct wl_compositor *
display_get_compositor(struct display *display);

struct wl_shell *
display_get_shell(struct display *display);

struct output *
display_get_output(struct display *display);

uint32_t
display_get_serial(struct display *display);

typedef void (*display_global_handler_t)(struct display *display,
					 uint32_t name,
					 const char *interface,
					 uint32_t version, void *data);

void
display_set_global_handler(struct display *display,
			   display_global_handler_t handler);
void *
display_bind(struct display *display, uint32_t name,
	     const struct wl_interface *interface, uint32_t version);

typedef void (*display_output_handler_t)(struct output *output, void *data);

/*
 * The output configure handler is called, when a new output is connected
 * and we know its current mode, or when the current mode changes.
 * Test and set the output user data in your handler to know, if the
 * output is new. Note: 'data' in the configure handler is the display
 * user data.
 */
void
display_set_output_configure_handler(struct display *display,
				     display_output_handler_t handler);

struct wl_data_source *
display_create_data_source(struct display *display);

#ifdef EGL_NO_DISPLAY
EGLDisplay
display_get_egl_display(struct display *d);

EGLConfig
display_get_argb_egl_config(struct display *d);

int
display_acquire_window_surface(struct display *display,
			       struct window *window,
			       EGLContext ctx);
void
display_release_window_surface(struct display *display,
			       struct window *window);
#endif

#define SURFACE_OPAQUE 0x01
#define SURFACE_SHM    0x02

#define SURFACE_HINT_RESIZE 0x10

#define SURFACE_HINT_RGB565 0x100

cairo_surface_t *
display_create_surface(struct display *display,
		       struct wl_surface *surface,
		       struct rectangle *rectangle,
		       uint32_t flags);

struct wl_buffer *
display_get_buffer_for_surface(struct display *display,
			       cairo_surface_t *surface);

struct wl_cursor_image *
display_get_pointer_image(struct display *display, int pointer);

void
display_defer(struct display *display, struct task *task);

void
display_watch_fd(struct display *display,
		 int fd, uint32_t events, struct task *task);

void
display_unwatch_fd(struct display *display, int fd);

void
display_run(struct display *d);

void
display_exit(struct display *d);

enum cursor_type {
	CURSOR_BOTTOM_LEFT,
	CURSOR_BOTTOM_RIGHT,
	CURSOR_BOTTOM,
	CURSOR_DRAGGING,
	CURSOR_LEFT_PTR,
	CURSOR_LEFT,
	CURSOR_RIGHT,
	CURSOR_TOP_LEFT,
	CURSOR_TOP_RIGHT,
	CURSOR_TOP,
	CURSOR_IBEAM,
	CURSOR_HAND1,
	CURSOR_WATCH,

	CURSOR_BLANK
};

typedef void (*window_key_handler_t)(struct window *window, struct input *input,
				     uint32_t time, uint32_t key, uint32_t unicode,
				     enum wl_keyboard_key_state state, void *data);

typedef void (*window_keyboard_focus_handler_t)(struct window *window,
						struct input *device, void *data);

typedef void (*window_data_handler_t)(struct window *window,
				      struct input *input,
				      float x, float y,
				      const char **types,
				      void *data);

typedef void (*window_drop_handler_t)(struct window *window,
				      struct input *input,
				      int32_t x, int32_t y, void *data);

typedef void (*window_close_handler_t)(struct window *window, void *data);
typedef void (*window_fullscreen_handler_t)(struct window *window, void *data);

typedef void (*window_output_handler_t)(struct window *window, struct output *output,
					int enter, void *data);

typedef void (*widget_resize_handler_t)(struct widget *widget,
					int32_t width, int32_t height,
					void *data);
typedef void (*widget_redraw_handler_t)(struct widget *widget, void *data);

typedef int (*widget_enter_handler_t)(struct widget *widget,
				      struct input *input,
				      float x, float y, void *data);
typedef void (*widget_leave_handler_t)(struct widget *widget,
				       struct input *input, void *data);
typedef int (*widget_motion_handler_t)(struct widget *widget,
				       struct input *input, uint32_t time,
				       float x, float y, void *data);
typedef void (*widget_button_handler_t)(struct widget *widget,
					struct input *input, uint32_t time,
					uint32_t button,
					enum wl_pointer_button_state state,
					void *data);
typedef void (*widget_touch_down_handler_t)(struct widget *widget,
					    struct input *input,
					    uint32_t serial,
					    uint32_t time,
					    int32_t id,
					    float x,
					    float y,
					    void *data);
typedef void (*widget_touch_up_handler_t)(struct widget *widget,
					  struct input *input,
					  uint32_t serial,
					  uint32_t time,
					  int32_t id,
					  void *data);
typedef void (*widget_touch_motion_handler_t)(struct widget *widget,
					      struct input *input,
					      uint32_t time,
					      int32_t id,
					      float x,
					      float y,
					      void *data);
typedef void (*widget_touch_frame_handler_t)(struct widget *widget, 
					     struct input *input, void *data);
typedef void (*widget_touch_cancel_handler_t)(struct widget *widget, 
					      struct input *input, void *data);
typedef void (*widget_axis_handler_t)(struct widget *widget,
				      struct input *input, uint32_t time,
				      uint32_t axis,
				      wl_fixed_t value,
				      void *data);

struct window *
window_create(struct display *display);
struct window *
window_create_transient(struct display *display, struct window *parent,
			int32_t x, int32_t y, uint32_t flags);
struct window *
window_create_custom(struct display *display);

int
window_has_focus(struct window *window);

typedef void (*menu_func_t)(struct window *window, int index, void *data);

void
window_show_menu(struct display *display,
		 struct input *input, uint32_t time, struct window *parent,
		 int32_t x, int32_t y,
		 menu_func_t func, const char **entries, int count);

void
window_show_frame_menu(struct window *window,
		       struct input *input, uint32_t time);

int
window_get_buffer_transform(struct window *window);

void
window_set_buffer_transform(struct window *window,
			    enum wl_output_transform transform);

uint32_t
window_get_buffer_scale(struct window *window);

void
window_set_buffer_scale(struct window *window,
                        int32_t scale);

uint32_t
window_get_output_scale(struct window *window);

void
window_destroy(struct window *window);

struct widget *
window_add_widget(struct window *window, void *data);

enum subsurface_mode {
	SUBSURFACE_SYNCHRONIZED,
	SUBSURFACE_DESYNCHRONIZED
};

struct widget *
window_add_subsurface(struct window *window, void *data,
		      enum subsurface_mode default_mode);

typedef void (*data_func_t)(void *data, size_t len,
			    int32_t x, int32_t y, void *user_data);

struct display *
window_get_display(struct window *window);
void
window_move(struct window *window, struct input *input, uint32_t time);
void
window_touch_move(struct window *window, struct input *input, uint32_t time);
void
window_get_allocation(struct window *window, struct rectangle *allocation);
void
window_schedule_redraw(struct window *window);
void
window_schedule_resize(struct window *window, int width, int height);

void
window_damage(struct window *window, int32_t x, int32_t y,
	      int32_t width, int32_t height);

cairo_surface_t *
window_get_surface(struct window *window);

struct wl_surface *
window_get_wl_surface(struct window *window);

struct wl_shell_surface *
window_get_wl_shell_surface(struct window *window);

enum window_buffer_type {
	WINDOW_BUFFER_TYPE_EGL_WINDOW,
	WINDOW_BUFFER_TYPE_SHM,
};

void
display_surface_damage(struct display *display, cairo_surface_t *cairo_surface,
		       int32_t x, int32_t y, int32_t width, int32_t height);

void
window_set_buffer_type(struct window *window, enum window_buffer_type type);

int
window_is_fullscreen(struct window *window);

void
window_set_fullscreen(struct window *window, int fullscreen);

void
window_set_fullscreen_method(struct window *window,
			     enum wl_shell_surface_fullscreen_method method);

void
window_set_minimized(struct window *window, int minimized);

int
window_is_maximized(struct window *window);

void
window_set_maximized(struct window *window, int maximized);

void
window_set_user_data(struct window *window, void *data);

void *
window_get_user_data(struct window *window);

void
window_set_key_handler(struct window *window,
		       window_key_handler_t handler);

void
window_set_keyboard_focus_handler(struct window *window,
				  window_keyboard_focus_handler_t handler);

void
window_set_data_handler(struct window *window,
			window_data_handler_t handler);

void
window_set_drop_handler(struct window *window,
			window_drop_handler_t handler);

void
window_set_close_handler(struct window *window,
			 window_close_handler_t handler);
void
window_set_fullscreen_handler(struct window *window,
			      window_fullscreen_handler_t handler);
void
window_set_output_handler(struct window *window,
			  window_output_handler_t handler);

void
window_set_title(struct window *window, const char *title);

const char *
window_get_title(struct window *window);

void
window_set_text_cursor_position(struct window *window, int32_t x, int32_t y);

enum preferred_format {
	WINDOW_PREFERRED_FORMAT_NONE,
	WINDOW_PREFERRED_FORMAT_RGB565
};

void
window_set_preferred_format(struct window *window,
			    enum preferred_format format);

int
widget_set_tooltip(struct widget *parent, char *entry, float x, float y);

void
widget_destroy_tooltip(struct widget *parent);

struct widget *
widget_add_widget(struct widget *parent, void *data);

void
widget_destroy(struct widget *widget);
void
widget_set_default_cursor(struct widget *widget, int cursor);
void
widget_get_allocation(struct widget *widget, struct rectangle *allocation);

void
widget_set_allocation(struct widget *widget,
		      int32_t x, int32_t y, int32_t width, int32_t height);
void
widget_set_size(struct widget *widget, int32_t width, int32_t height);
void
widget_set_transparent(struct widget *widget, int transparent);
void
widget_schedule_resize(struct widget *widget, int32_t width, int32_t height);

void *
widget_get_user_data(struct widget *widget);

cairo_t *
widget_cairo_create(struct widget *widget);

struct wl_surface *
widget_get_wl_surface(struct widget *widget);

uint32_t
widget_get_last_time(struct widget *widget);

void
widget_input_region_add(struct widget *widget, const struct rectangle *rect);

void
widget_set_redraw_handler(struct widget *widget,
			  widget_redraw_handler_t handler);
void
widget_set_resize_handler(struct widget *widget,
			  widget_resize_handler_t handler);
void
widget_set_enter_handler(struct widget *widget,
			 widget_enter_handler_t handler);
void
widget_set_leave_handler(struct widget *widget,
			 widget_leave_handler_t handler);
void
widget_set_motion_handler(struct widget *widget,
			  widget_motion_handler_t handler);
void
widget_set_button_handler(struct widget *widget,
			  widget_button_handler_t handler);
void
widget_set_touch_down_handler(struct widget *widget,
			      widget_touch_down_handler_t handler);
void
widget_set_touch_up_handler(struct widget *widget,
			    widget_touch_up_handler_t handler);
void
widget_set_touch_motion_handler(struct widget *widget,
				widget_touch_motion_handler_t handler);
void
widget_set_touch_frame_handler(struct widget *widget,
			       widget_touch_frame_handler_t handler);
void
widget_set_touch_cancel_handler(struct widget *widget,
				widget_touch_cancel_handler_t handler);
void
widget_set_axis_handler(struct widget *widget,
			widget_axis_handler_t handler);
void
widget_schedule_redraw(struct widget *widget);

struct widget *
frame_create(struct window *window, void *data);

void
frame_set_child_size(struct widget *widget, int child_width, int child_height);

void
input_set_pointer_image(struct input *input, int pointer);

void
input_get_position(struct input *input, int32_t *x, int32_t *y);

#define MOD_SHIFT_MASK		0x01
#define MOD_ALT_MASK		0x02
#define MOD_CONTROL_MASK	0x04

uint32_t
input_get_modifiers(struct input *input);

void
input_grab(struct input *input, struct widget *widget, uint32_t button);

void
input_ungrab(struct input *input);

struct widget *
input_get_focus_widget(struct input *input);

struct display *
input_get_display(struct input *input);

struct wl_seat *
input_get_seat(struct input *input);

struct wl_data_device *
input_get_data_device(struct input *input);

void
input_set_selection(struct input *input,
		    struct wl_data_source *source, uint32_t time);

void
input_accept(struct input *input, const char *type);


void
input_receive_drag_data(struct input *input, const char *mime_type,
			data_func_t func, void *user_data);
int
input_receive_drag_data_to_fd(struct input *input,
			      const char *mime_type, int fd);

int
input_receive_selection_data(struct input *input, const char *mime_type,
			     data_func_t func, void *data);
int
input_receive_selection_data_to_fd(struct input *input,
				   const char *mime_type, int fd);

void
output_set_user_data(struct output *output, void *data);

void *
output_get_user_data(struct output *output);

void
output_set_destroy_handler(struct output *output,
			   display_output_handler_t handler);

void
output_get_allocation(struct output *output, struct rectangle *allocation);

struct wl_output *
output_get_wl_output(struct output *output);

enum wl_output_transform
output_get_transform(struct output *output);

uint32_t
output_get_scale(struct output *output);

void
keysym_modifiers_add(struct wl_array *modifiers_map,
		     const char *name);

xkb_mod_mask_t
keysym_modifiers_get_mask(struct wl_array *modifiers_map,
			  const char *name);

#endif
