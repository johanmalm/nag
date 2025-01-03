#include <assert.h>
#include <cairo.h>
#include <ctype.h>
#include <getopt.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wlr/util/log.h>
#include "pool-buffer.h"
#include "cursor-shape-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define SWAYNAG_MAX_HEIGHT 500
#define LAB_EXIT_FAILURE 255
#define LAB_EXIT_SUCCESS 0

struct conf {
	PangoFontDescription *font_description;
	char *output;
	uint32_t anchors;
	int32_t layer; // enum zwlr_layer_shell_v1_layer or -1 if unset

	// Colors
	uint32_t button_text;
	uint32_t button_background;
	uint32_t details_background;
	uint32_t background;
	uint32_t text;
	uint32_t border;
	uint32_t border_bottom;

	// Sizing
	ssize_t bar_border_thickness;
	ssize_t message_padding;
	ssize_t details_border_thickness;
	ssize_t button_border_thickness;
	ssize_t button_gap;
	ssize_t button_gap_close;
	ssize_t button_margin_right;
	ssize_t button_padding;
};

struct swaynag;

enum swaynag_action_type {
	SWAYNAG_ACTION_DISMISS,
	SWAYNAG_ACTION_EXPAND,
	SWAYNAG_ACTION_COMMAND,
};

struct swaynag_pointer {
	struct wl_pointer *pointer;
	uint32_t serial;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor_image;
	struct wl_surface *cursor_surface;
	int x;
	int y;
};

struct swaynag_seat {
	struct wl_seat *wl_seat;
	uint32_t wl_name;
	struct swaynag *swaynag;
	struct swaynag_pointer pointer;
	struct wl_list link;
};

struct swaynag_output {
	char *name;
	struct wl_output *wl_output;
	uint32_t wl_name;
	uint32_t scale;
	struct swaynag *swaynag;
	struct wl_list link;
};

struct swaynag_button {
	char *text;
	enum swaynag_action_type type;
	char *action;
	int x;
	int y;
	int width;
	int height;
	bool terminal;
	bool dismiss;
	struct wl_list link;
};

struct swaynag_details {
	bool visible;
	char *message;
	char *details_text;
	int close_timeout;
	bool close_timeout_cancel;
	bool use_exclusive_zone;

	int x;
	int y;
	int width;
	int height;

	int offset;
	int visible_lines;
	int total_lines;
	struct swaynag_button *button_details;
	struct swaynag_button button_up;
	struct swaynag_button button_down;
};

struct swaynag {
	bool run_display;

	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_shm *shm;
	struct wl_list outputs;  // swaynag_output::link
	struct wl_list seats;  // swaynag_seat::link
	struct swaynag_output *output;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wl_surface *surface;

	uint32_t width;
	uint32_t height;
	int32_t scale;
	struct pool_buffer buffers[2];
	struct pool_buffer *current_buffer;

	struct conf *conf;
	char *message;
	struct wl_list buttons;
	struct swaynag_details details;
};

static struct swaynag swaynag;

static int exit_status = LAB_EXIT_FAILURE;

static PangoLayout *
get_pango_layout(cairo_t *cairo, const PangoFontDescription *desc,
		const char *text, double scale, bool markup)
{
	PangoLayout *layout = pango_cairo_create_layout(cairo);
	pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);

	PangoAttrList *attrs;
	if (markup) {
		char *buf;
		GError *error = NULL;
		if (pango_parse_markup(text, -1, 0, &attrs, &buf, NULL, &error)) {
			pango_layout_set_text(layout, buf, -1);
			free(buf);
		} else {
			wlr_log(WLR_ERROR, "pango_parse_markup '%s' -> error %s", text,
					error->message);
			g_error_free(error);
			markup = false; // fallback to plain text
		}
	}
	if (!markup) {
		attrs = pango_attr_list_new();
		pango_layout_set_text(layout, text, -1);
	}

	pango_attr_list_insert(attrs, pango_attr_scale_new(scale));
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_single_paragraph_mode(layout, 1);
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);
	return layout;
}

static void
get_text_size(cairo_t *cairo, const PangoFontDescription *desc, int *width, int *height,
		int *baseline, double scale, bool markup, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}

	PangoLayout *layout = get_pango_layout(cairo, desc, buf, scale, markup);
	pango_cairo_update_layout(cairo, layout);
	pango_layout_get_pixel_size(layout, width, height);
	if (baseline) {
		*baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;
	}
	g_object_unref(layout);

	g_free(buf);
}

static void
render_text(cairo_t *cairo, const PangoFontDescription *desc,
		double scale, bool markup, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (!buf) {
		return;
	}

	PangoLayout *layout = get_pango_layout(cairo, desc, buf, scale, markup);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_get_font_options(cairo, fo);
	pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
	cairo_font_options_destroy(fo);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);

	g_free(buf);
}

static void
cairo_set_source_u32(cairo_t *cairo, uint32_t color)
{
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

static uint32_t
render_message(cairo_t *cairo, struct swaynag *swaynag)
{
	int text_width, text_height;
	get_text_size(cairo, swaynag->conf->font_description, &text_width, &text_height, NULL,
			1, true, "%s", swaynag->message);

	int padding = swaynag->conf->message_padding;

	uint32_t ideal_height = text_height + padding * 2;
	uint32_t ideal_surface_height = ideal_height;
	if (swaynag->height < ideal_surface_height) {
		return ideal_surface_height;
	}

	cairo_set_source_u32(cairo, swaynag->conf->text);
	cairo_move_to(cairo, padding, (int)(ideal_height - text_height) / 2);
	render_text(cairo, swaynag->conf->font_description, 1, false,
			"%s", swaynag->message);

	return ideal_surface_height;
}

static void
render_details_scroll_button(cairo_t *cairo,
		struct swaynag *swaynag, struct swaynag_button *button)
{
	int text_width, text_height;
	get_text_size(cairo, swaynag->conf->font_description, &text_width, &text_height, NULL,
			1, true, "%s", button->text);

	int border = swaynag->conf->button_border_thickness;
	int padding = swaynag->conf->button_padding;

	cairo_set_source_u32(cairo, swaynag->conf->details_background);
	cairo_rectangle(cairo, button->x, button->y,
			button->width, button->height);
	cairo_fill(cairo);

	cairo_set_source_u32(cairo, swaynag->conf->button_background);
	cairo_rectangle(cairo, button->x + border, button->y + border,
			button->width - (border * 2), button->height - (border * 2));
	cairo_fill(cairo);

	cairo_set_source_u32(cairo, swaynag->conf->button_text);
	cairo_move_to(cairo, button->x + border + padding,
			button->y + border + (button->height - text_height) / 2);
	render_text(cairo, swaynag->conf->font_description, 1, true,
			"%s", button->text);
}

static int
get_detailed_scroll_button_width(cairo_t *cairo,
		struct swaynag *swaynag)
{
	int up_width, down_width, temp_height;
	get_text_size(cairo, swaynag->conf->font_description, &up_width, &temp_height, NULL,
			1, true,
			"%s", swaynag->details.button_up.text);
	get_text_size(cairo, swaynag->conf->font_description, &down_width, &temp_height, NULL,
			1, true,
			"%s", swaynag->details.button_down.text);

	int text_width =  up_width > down_width ? up_width : down_width;
	int border = swaynag->conf->button_border_thickness;
	int padding = swaynag->conf->button_padding;

	return text_width + border * 2 + padding * 2;
}

static uint32_t
render_detailed(cairo_t *cairo, struct swaynag *swaynag,
		uint32_t y)
{
	uint32_t width = swaynag->width;

	int border = swaynag->conf->details_border_thickness;
	int padding = swaynag->conf->message_padding;
	int decor = padding + border;

	swaynag->details.x = decor;
	swaynag->details.y = y + decor;
	swaynag->details.width = width - decor * 2;

	PangoLayout *layout = get_pango_layout(cairo, swaynag->conf->font_description,
			swaynag->details.message, 1, false);
	pango_layout_set_width(layout,
			(swaynag->details.width - padding * 2) * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_single_paragraph_mode(layout, false);
	pango_cairo_update_layout(cairo, layout);
	swaynag->details.total_lines = pango_layout_get_line_count(layout);

	PangoLayoutLine *line;
	line = pango_layout_get_line_readonly(layout, swaynag->details.offset);
	gint offset = line->start_index;
	const char *text = pango_layout_get_text(layout);
	pango_layout_set_text(layout, text + offset, strlen(text) - offset);

	int text_width, text_height;
	pango_cairo_update_layout(cairo, layout);
	pango_layout_get_pixel_size(layout, &text_width, &text_height);

	bool show_buttons = swaynag->details.offset > 0;
	int button_width = get_detailed_scroll_button_width(cairo, swaynag);
	if (show_buttons) {
		swaynag->details.width -= button_width;
		pango_layout_set_width(layout,
				(swaynag->details.width - padding * 2) * PANGO_SCALE);
	}

	uint32_t ideal_height;
	do {
		ideal_height = swaynag->details.y + text_height + decor + padding * 2;
		if (ideal_height > SWAYNAG_MAX_HEIGHT) {
			ideal_height = SWAYNAG_MAX_HEIGHT;

			if (!show_buttons) {
				show_buttons = true;
				swaynag->details.width -= button_width;
				pango_layout_set_width(layout,
					(swaynag->details.width - padding * 2) * PANGO_SCALE);
			}
		}

		swaynag->details.height = ideal_height - swaynag->details.y - decor;
		pango_layout_set_height(layout,
				(swaynag->details.height - padding * 2) * PANGO_SCALE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_cairo_update_layout(cairo, layout);
		pango_layout_get_pixel_size(layout, &text_width, &text_height);
	} while (text_height != (swaynag->details.height - padding * 2));

	swaynag->details.visible_lines = pango_layout_get_line_count(layout);

	if (show_buttons) {
		swaynag->details.button_up.x =
			swaynag->details.x + swaynag->details.width;
		swaynag->details.button_up.y = swaynag->details.y;
		swaynag->details.button_up.width = button_width;
		swaynag->details.button_up.height = swaynag->details.height / 2;
		render_details_scroll_button(cairo, swaynag,
				&swaynag->details.button_up);

		swaynag->details.button_down.x =
			swaynag->details.x + swaynag->details.width;
		swaynag->details.button_down.y =
			swaynag->details.button_up.y + swaynag->details.button_up.height;
		swaynag->details.button_down.width = button_width;
		swaynag->details.button_down.height = swaynag->details.height / 2;
		render_details_scroll_button(cairo, swaynag,
				&swaynag->details.button_down);
	}

	cairo_set_source_u32(cairo, swaynag->conf->details_background);
	cairo_rectangle(cairo, swaynag->details.x, swaynag->details.y,
			swaynag->details.width, swaynag->details.height);
	cairo_fill(cairo);

	cairo_move_to(cairo, swaynag->details.x + padding,
			swaynag->details.y + padding);
	cairo_set_source_u32(cairo, swaynag->conf->text);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);

	return ideal_height;
}

static uint32_t
render_button(cairo_t *cairo, struct swaynag *swaynag,
		struct swaynag_button *button, int *x)
{
	int text_width, text_height;
	get_text_size(cairo, swaynag->conf->font_description, &text_width, &text_height, NULL,
			1, true, "%s", button->text);

	int border = swaynag->conf->button_border_thickness;
	int padding = swaynag->conf->button_padding;

	uint32_t ideal_height = text_height + padding * 2 + border * 2;
	uint32_t ideal_surface_height = ideal_height;
	if (swaynag->height < ideal_surface_height) {
		return ideal_surface_height;
	}

	button->x = *x - border - text_width - padding * 2 + 1;
	button->y = (int)(ideal_height - text_height) / 2 - padding + 1;
	button->width = text_width + padding * 2;
	button->height = text_height + padding * 2;

	cairo_set_source_u32(cairo, swaynag->conf->border);
	cairo_rectangle(cairo, button->x - border, button->y - border,
			button->width + border * 2, button->height + border * 2);
	cairo_fill(cairo);

	cairo_set_source_u32(cairo, swaynag->conf->button_background);
	cairo_rectangle(cairo, button->x, button->y,
			button->width, button->height);
	cairo_fill(cairo);

	cairo_set_source_u32(cairo, swaynag->conf->button_text);
	cairo_move_to(cairo, button->x + padding, button->y + padding);
	render_text(cairo, swaynag->conf->font_description, 1, true,
			"%s", button->text);

	*x = button->x - border;

	return ideal_surface_height;
}

static uint32_t
render_to_cairo(cairo_t *cairo, struct swaynag *swaynag)
{
	uint32_t max_height = 0;

	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, swaynag->conf->background);
	cairo_paint(cairo);

	uint32_t h = render_message(cairo, swaynag);
	max_height = h > max_height ? h : max_height;

	int x = swaynag->width - swaynag->conf->button_margin_right;
	x -= swaynag->conf->button_gap_close;

	struct swaynag_button *button;
	wl_list_for_each(button, &swaynag->buttons, link) {
		h = render_button(cairo, swaynag, button, &x);
		max_height = h > max_height ? h : max_height;
		x -= swaynag->conf->button_gap;
	}

	if (swaynag->details.visible) {
		h = render_detailed(cairo, swaynag, max_height);
		max_height = h > max_height ? h : max_height;
	}

	int border = swaynag->conf->bar_border_thickness;
	if (max_height > swaynag->height) {
		max_height += border;
	}
	cairo_set_source_u32(cairo, swaynag->conf->border_bottom);
	cairo_rectangle(cairo, 0,
			swaynag->height - border,
			swaynag->width,
			border);
	cairo_fill(cairo);

	return max_height;
}

void
render_frame(struct swaynag *swaynag)
{
	if (!swaynag->run_display) {
		return;
	}

	cairo_surface_t *recorder = cairo_recording_surface_create(
			CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_scale(cairo, swaynag->scale, swaynag->scale);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);
	uint32_t height = render_to_cairo(cairo, swaynag);
	if (height != swaynag->height) {
		zwlr_layer_surface_v1_set_size(swaynag->layer_surface, 0, height);
		if (swaynag->details.use_exclusive_zone) {
			zwlr_layer_surface_v1_set_exclusive_zone(
				swaynag->layer_surface, height);
		}
		wl_surface_commit(swaynag->surface);
		wl_display_roundtrip(swaynag->display);
	} else {
		swaynag->current_buffer = get_next_buffer(swaynag->shm,
				swaynag->buffers,
				swaynag->width * swaynag->scale,
				swaynag->height * swaynag->scale);
		if (!swaynag->current_buffer) {
			wlr_log(WLR_DEBUG, "Failed to get buffer. Skipping frame.");
			goto cleanup;
		}

		cairo_t *shm = swaynag->current_buffer->cairo;
		cairo_save(shm);
		cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
		cairo_paint(shm);
		cairo_restore(shm);
		cairo_set_source_surface(shm, recorder, 0.0, 0.0);
		cairo_paint(shm);

		wl_surface_set_buffer_scale(swaynag->surface, swaynag->scale);
		wl_surface_attach(swaynag->surface,
				swaynag->current_buffer->buffer, 0, 0);
		wl_surface_damage(swaynag->surface, 0, 0,
				swaynag->width, swaynag->height);
		wl_surface_commit(swaynag->surface);
		wl_display_roundtrip(swaynag->display);
	}

cleanup:
	cairo_surface_destroy(recorder);
	cairo_destroy(cairo);
}

static void nop() { /* Intentionally left blank */ }

static bool
terminal_execute(char *terminal, char *command)
{
	char fname[] = "/tmp/swaynagXXXXXX";
	FILE *tmp = fdopen(mkstemp(fname), "w");
	if (!tmp) {
		wlr_log(WLR_ERROR, "Failed to create temp script");
		return false;
	}
	wlr_log(WLR_DEBUG, "Created temp script: %s", fname);
	fprintf(tmp, "#!/bin/sh\nrm %s\n%s", fname, command);
	fclose(tmp);
	chmod(fname, S_IRUSR | S_IWUSR | S_IXUSR);
	size_t cmd_size = strlen(terminal) + strlen(" -e ") + strlen(fname) + 1;
	char *cmd = malloc(cmd_size);
	if (!cmd) {
		perror("malloc");
		return false;
	}
	snprintf(cmd, cmd_size, "%s -e %s", terminal, fname);
	execlp("sh", "sh", "-c", cmd, NULL);
	wlr_log_errno(WLR_ERROR, "Failed to run command, execlp() returned.");
	free(cmd);
	return false;
}

void
swaynag_seat_destroy(struct swaynag_seat *seat)
{
	if (seat->pointer.cursor_theme) {
		wl_cursor_theme_destroy(seat->pointer.cursor_theme);
	}
	if (seat->pointer.pointer) {
		wl_pointer_destroy(seat->pointer.pointer);
	}
	wl_seat_destroy(seat->wl_seat);
	wl_list_remove(&seat->link);
	free(seat);
}

static void
swaynag_destroy(struct swaynag *swaynag)
{
	swaynag->run_display = false;

	free(swaynag->message);

	struct swaynag_button *button, *next;
	wl_list_for_each_safe(button, next, &swaynag->buttons, link) {
		wl_list_remove(&button->link);
		free(button->text);
		free(button->action);
		free(button);
	}
	free(swaynag->details.message);
	free(swaynag->details.details_text);
	free(swaynag->details.button_up.text);
	free(swaynag->details.button_down.text);

	pango_font_description_free(swaynag->conf->font_description);

	if (swaynag->layer_surface) {
		zwlr_layer_surface_v1_destroy(swaynag->layer_surface);
	}

	if (swaynag->surface) {
		wl_surface_destroy(swaynag->surface);
	}

	struct swaynag_seat *seat, *tmpseat;
	wl_list_for_each_safe(seat, tmpseat, &swaynag->seats, link) {
		swaynag_seat_destroy(seat);
	}

	destroy_buffer(&swaynag->buffers[0]);
	destroy_buffer(&swaynag->buffers[1]);

	if (swaynag->outputs.prev || swaynag->outputs.next) {
		struct swaynag_output *output, *temp;
		wl_list_for_each_safe(output, temp, &swaynag->outputs, link) {
			wl_output_destroy(output->wl_output);
			free(output->name);
			wl_list_remove(&output->link);
			free(output);
		};
	}

	if (swaynag->compositor) {
		wl_compositor_destroy(swaynag->compositor);
	}

	if (swaynag->shm) {
		wl_shm_destroy(swaynag->shm);
	}

	if (swaynag->display) {
		wl_display_disconnect(swaynag->display);
	}
	pango_cairo_font_map_set_default(NULL);
}

static void
swaynag_button_execute(struct swaynag *swaynag,
		struct swaynag_button *button)
{
	wlr_log(WLR_DEBUG, "Executing [%s]: %s", button->text, button->action);
	if (button->type == SWAYNAG_ACTION_DISMISS) {
		swaynag->run_display = false;
	} else if (button->type == SWAYNAG_ACTION_EXPAND) {
		swaynag->details.visible = !swaynag->details.visible;
		render_frame(swaynag);
	} else {
		pid_t pid = fork();
		if (pid < 0) {
			wlr_log_errno(WLR_DEBUG, "Failed to fork");
			return;
		} else if (pid == 0) {
			// Child process. Will be used to prevent zombie processes
			pid = fork();
			if (pid < 0) {
				wlr_log_errno(WLR_DEBUG, "Failed to fork");
				return;
			} else if (pid == 0) {
				// Child of the child. Will be reparented to the init process
				char *terminal = getenv("TERMINAL");
				if (button->terminal && terminal && *terminal) {
					wlr_log(WLR_DEBUG, "Found $TERMINAL: %s", terminal);
					if (!terminal_execute(terminal, button->action)) {
						swaynag_destroy(swaynag);
						_exit(LAB_EXIT_FAILURE);
					}
				} else {
					if (button->terminal) {
						wlr_log(WLR_DEBUG,
								"$TERMINAL not found. Running directly");
					}
					execlp("sh", "sh", "-c", button->action, NULL);
					wlr_log_errno(WLR_DEBUG, "execlp failed");
					_exit(LAB_EXIT_FAILURE);
				}
			}
			_exit(EXIT_SUCCESS);
		}

		if (button->dismiss) {
			swaynag->run_display = false;
		}

		if (waitpid(pid, NULL, 0) < 0) {
			wlr_log_errno(WLR_DEBUG, "waitpid failed");
		}
	}
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	struct swaynag *swaynag = data;
	swaynag->width = width;
	swaynag->height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	render_frame(swaynag);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct swaynag *swaynag = data;
	swaynag_destroy(swaynag);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void
surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	struct swaynag *swaynag = data;
	struct swaynag_output *swaynag_output;
	wl_list_for_each(swaynag_output, &swaynag->outputs, link) {
		if (swaynag_output->wl_output == output) {
			wlr_log(WLR_DEBUG, "Surface enter on output %s",
					swaynag_output->name);
			swaynag->output = swaynag_output;
			swaynag->scale = swaynag->output->scale;
			render_frame(swaynag);
			break;
		}
	}
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = nop,
};

static void
update_cursor(struct swaynag_seat *seat)
{
	struct swaynag_pointer *pointer = &seat->pointer;
	struct swaynag *swaynag = seat->swaynag;
	if (pointer->cursor_theme) {
		wl_cursor_theme_destroy(pointer->cursor_theme);
	}
	const char *cursor_theme = getenv("XCURSOR_THEME");
	unsigned int cursor_size = 24;
	const char *env_cursor_size = getenv("XCURSOR_SIZE");
	if (env_cursor_size && *env_cursor_size) {
		errno = 0;
		char *end;
		unsigned int size = strtoul(env_cursor_size, &end, 10);
		if (!*end && errno == 0) {
			cursor_size = size;
		}
	}
	pointer->cursor_theme = wl_cursor_theme_load(
		cursor_theme, cursor_size * swaynag->scale, swaynag->shm);
	if (!pointer->cursor_theme) {
		wlr_log(WLR_ERROR, "Failed to load cursor theme");
		return;
	}
	struct wl_cursor *cursor = wl_cursor_theme_get_cursor(pointer->cursor_theme, "default");
	if (!cursor) {
		wlr_log(WLR_ERROR, "Failed to get default cursor from theme");
		return;
	}
	pointer->cursor_image = cursor->images[0];
	wl_surface_set_buffer_scale(pointer->cursor_surface,
			swaynag->scale);
	wl_surface_attach(pointer->cursor_surface,
			wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
	wl_pointer_set_cursor(pointer->pointer, pointer->serial,
			pointer->cursor_surface,
			pointer->cursor_image->hotspot_x / swaynag->scale,
			pointer->cursor_image->hotspot_y / swaynag->scale);
	wl_surface_damage_buffer(pointer->cursor_surface, 0, 0,
			INT32_MAX, INT32_MAX);
	wl_surface_commit(pointer->cursor_surface);
}

void
update_all_cursors(struct swaynag *swaynag)
{
	struct swaynag_seat *seat;
	wl_list_for_each(seat, &swaynag->seats, link) {
		if (seat->pointer.pointer) {
			update_cursor(seat);
		}
	}
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct swaynag_seat *seat = data;

	struct swaynag_pointer *pointer = &seat->pointer;
	pointer->x = wl_fixed_to_int(surface_x);
	pointer->y = wl_fixed_to_int(surface_y);

	if (seat->swaynag->cursor_shape_manager) {
		struct wp_cursor_shape_device_v1 *device =
			wp_cursor_shape_manager_v1_get_pointer(
				seat->swaynag->cursor_shape_manager, wl_pointer);
		wp_cursor_shape_device_v1_set_shape(device, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		wp_cursor_shape_device_v1_destroy(device);
	} else {
		pointer->serial = serial;
		update_cursor(seat);
	}
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x,
		wl_fixed_t surface_y)
{
	struct swaynag_seat *seat = data;
	seat->pointer.x = wl_fixed_to_int(surface_x);
	seat->pointer.y = wl_fixed_to_int(surface_y);
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state)
{
	struct swaynag_seat *seat = data;
	struct swaynag *swaynag = seat->swaynag;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	double x = seat->pointer.x;
	double y = seat->pointer.y;

	int index = 0;
	struct swaynag_button *nagbutton;
	wl_list_for_each(nagbutton, &swaynag->buttons, link) {
		if (x >= nagbutton->x
				&& y >= nagbutton->y
				&& x < nagbutton->x + nagbutton->width
				&& y < nagbutton->y + nagbutton->height) {
			swaynag_button_execute(swaynag, nagbutton);
			exit_status = index;
			return;
		}
		++index;
	}

	if (swaynag->details.visible &&
			swaynag->details.total_lines != swaynag->details.visible_lines) {
		struct swaynag_button button_up = swaynag->details.button_up;
		if (x >= button_up.x
				&& y >= button_up.y
				&& x < button_up.x + button_up.width
				&& y < button_up.y + button_up.height
				&& swaynag->details.offset > 0) {
			swaynag->details.offset--;
			render_frame(swaynag);
			return;
		}

		struct swaynag_button button_down = swaynag->details.button_down;
		int bot = swaynag->details.total_lines;
		bot -= swaynag->details.visible_lines;
		if (x >= button_down.x
				&& y >= button_down.y
				&& x < button_down.x + button_down.width
				&& y < button_down.y + button_down.height
				&& swaynag->details.offset < bot) {
			swaynag->details.offset++;
			render_frame(swaynag);
			return;
		}
	}
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis,
		wl_fixed_t value)
{
	struct swaynag_seat *seat = data;
	struct swaynag *swaynag = seat->swaynag;
	if (!swaynag->details.visible
			|| seat->pointer.x < swaynag->details.x
			|| seat->pointer.y < swaynag->details.y
			|| seat->pointer.x >= swaynag->details.x + swaynag->details.width
			|| seat->pointer.y >= swaynag->details.y + swaynag->details.height
			|| swaynag->details.total_lines == swaynag->details.visible_lines) {
		return;
	}

	int direction = wl_fixed_to_int(value);
	int bot = swaynag->details.total_lines - swaynag->details.visible_lines;
	if (direction < 0 && swaynag->details.offset > 0) {
		swaynag->details.offset--;
	} else if (direction > 0 && swaynag->details.offset < bot) {
		swaynag->details.offset++;
	}

	render_frame(swaynag);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = nop,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = nop,
	.axis_source = nop,
	.axis_stop = nop,
	.axis_discrete = nop,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat, enum wl_seat_capability caps)
{
	struct swaynag_seat *seat = data;
	bool cap_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	if (cap_pointer && !seat->pointer.pointer) {
		seat->pointer.pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(seat->pointer.pointer,
				&pointer_listener, seat);
	} else if (!cap_pointer && seat->pointer.pointer) {
		wl_pointer_destroy(seat->pointer.pointer);
		seat->pointer.pointer = NULL;
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = nop,
};

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
	struct swaynag_output *swaynag_output = data;
	swaynag_output->scale = factor;
	if (swaynag_output->swaynag->output == swaynag_output) {
		swaynag_output->swaynag->scale = swaynag_output->scale;
		if (!swaynag_output->swaynag->cursor_shape_manager) {
			update_all_cursors(swaynag_output->swaynag);
		}
		render_frame(swaynag_output->swaynag);
	}
}

static void
output_name(void *data, struct wl_output *output, const char *name)
{
	struct swaynag_output *swaynag_output = data;
	swaynag_output->name = strdup(name);

	const char *outname = swaynag_output->swaynag->conf->output;
	if (!swaynag_output->swaynag->output && outname &&
			strcmp(outname, name) == 0) {
		wlr_log(WLR_DEBUG, "Using output %s", name);
		swaynag_output->swaynag->output = swaynag_output;
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = nop,
	.mode = nop,
	.done = nop,
	.scale = output_scale,
	.name = output_name,
	.description = nop,
};

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
		uint32_t version)
{
	struct swaynag *swaynag = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		swaynag->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct swaynag_seat *seat =
			calloc(1, sizeof(struct swaynag_seat));
		if (!seat) {
			perror("calloc");
			return;
		}

		seat->swaynag = swaynag;
		seat->wl_name = name;
		seat->wl_seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);

		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);

		wl_list_insert(&swaynag->seats, &seat->link);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		swaynag->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (!swaynag->output) {
			struct swaynag_output *output =
				calloc(1, sizeof(struct swaynag_output));
			if (!output) {
				perror("calloc");
				return;
			}
			output->wl_output = wl_registry_bind(registry, name,
					&wl_output_interface, 4);
			output->wl_name = name;
			output->scale = 1;
			output->swaynag = swaynag;
			wl_list_insert(&swaynag->outputs, &output->link);
			wl_output_add_listener(output->wl_output,
					&output_listener, output);
		}
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		swaynag->layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		swaynag->cursor_shape_manager = wl_registry_bind(
				registry, name, &wp_cursor_shape_manager_v1_interface, 1);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct swaynag *swaynag = data;
	if (swaynag->output->wl_name == name) {
		swaynag->run_display = false;
	}

	struct swaynag_seat *seat, *tmpseat;
	wl_list_for_each_safe(seat, tmpseat, &swaynag->seats, link) {
		if (seat->wl_name == name) {
			swaynag_seat_destroy(seat);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

void
swaynag_setup_cursors(struct swaynag *swaynag)
{
	struct swaynag_seat *seat;

	wl_list_for_each(seat, &swaynag->seats, link) {
		struct swaynag_pointer *p = &seat->pointer;

		p->cursor_surface =
			wl_compositor_create_surface(swaynag->compositor);
		assert(p->cursor_surface);
	}
}

void
swaynag_setup(struct swaynag *swaynag)
{
	swaynag->display = wl_display_connect(NULL);
	if (!swaynag->display) {
		wlr_log(WLR_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		exit(LAB_EXIT_FAILURE);
	}

	swaynag->scale = 1;

	struct wl_registry *registry = wl_display_get_registry(swaynag->display);
	wl_registry_add_listener(registry, &registry_listener, swaynag);
	if (wl_display_roundtrip(swaynag->display) < 0) {
		wlr_log(WLR_ERROR, "failed to register with the wayland display");
		exit(LAB_EXIT_FAILURE);
	}

	assert(swaynag->compositor && swaynag->layer_shell && swaynag->shm);

	// Second roundtrip to get wl_output properties
	if (wl_display_roundtrip(swaynag->display) < 0) {
		wlr_log(WLR_ERROR, "Error during outputs init.");
		swaynag_destroy(swaynag);
		exit(LAB_EXIT_FAILURE);
	}

	if (!swaynag->output && swaynag->conf->output) {
		wlr_log(WLR_ERROR, "Output '%s' not found", swaynag->conf->output);
		swaynag_destroy(swaynag);
		exit(LAB_EXIT_FAILURE);
	}

	if (!swaynag->cursor_shape_manager) {
		swaynag_setup_cursors(swaynag);
	}

	swaynag->surface = wl_compositor_create_surface(swaynag->compositor);
	assert(swaynag->surface);
	wl_surface_add_listener(swaynag->surface, &surface_listener, swaynag);

	swaynag->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			swaynag->layer_shell, swaynag->surface,
			swaynag->output ? swaynag->output->wl_output : NULL,
			swaynag->conf->layer,
			"swaynag");
	assert(swaynag->layer_surface);
	zwlr_layer_surface_v1_add_listener(swaynag->layer_surface,
			&layer_surface_listener, swaynag);
	zwlr_layer_surface_v1_set_anchor(swaynag->layer_surface,
			swaynag->conf->anchors);

	wl_registry_destroy(registry);
}

void
swaynag_run(struct swaynag *swaynag)
{
	swaynag->run_display = true;
	render_frame(swaynag);
	int timer_fd = -1;
	if (swaynag->details.close_timeout != 0) {
		timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
		struct itimerspec timeout = {
			.it_value.tv_sec = swaynag->details.close_timeout,
		};
		timerfd_settime(timer_fd, 0, &timeout, NULL);
	}
	struct pollfd fds[] = {
		{
			.fd = wl_display_get_fd(swaynag->display),
			.events = POLLIN,
		}, {
			.fd = timer_fd,
			.events = POLLIN,
		}
	};
	while (swaynag->run_display) {
		while (wl_display_prepare_read(swaynag->display) != 0) {
			wl_display_dispatch_pending(swaynag->display);
		}
		poll(fds, 2, -1);
		if (fds[0].revents & POLLIN) {
			if (timer_fd >= 0 && swaynag->details.close_timeout_cancel) {
				close(timer_fd);
				timer_fd = -1;
				fds[1].fd = -1;
			}
			wl_display_read_events(swaynag->display);
		} else {
			wl_display_cancel_read(swaynag->display);
		}
		if (timer_fd >= 0 && fds[1].revents & POLLIN) {
			close(timer_fd);
			timer_fd = -1;
			fds[1].fd = -1;
			swaynag->run_display = false;
		}
	}
}

static void
conf_init(struct conf *conf)
{
	conf->font_description = pango_font_description_from_string("pango:Sans 10");
	conf->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
	conf->button_background = 0x333333FF;
	conf->details_background = 0x333333FF;
	conf->background = 0x323232FF;
	conf->text = 0xFFFFFFFF;
	conf->button_text = 0xFFFFFFFF;
	conf->border = 0x222222FF;
	conf->border_bottom = 0x444444FF;
	conf->bar_border_thickness = 2;
	conf->message_padding = 8;
	conf->details_border_thickness = 3;
	conf->button_border_thickness = 3;
	conf->button_gap = 20;
	conf->button_gap_close = 15;
	conf->button_margin_right = 2;
	conf->button_padding = 3;
	conf->button_background = 0x680A0AFF;
	conf->details_background = 0x680A0AFF;
	conf->background = 0x900000FF;
	conf->text = 0xFFFFFFFF;
	conf->button_text = 0xFFFFFFFF;
	conf->border = 0xD92424FF;
	conf->border_bottom = 0x470909FF;
}

static bool
parse_color(const char *color, uint32_t *result)
{
	if (color[0] == '#') {
		++color;
	}
	int len = strlen(color);
	if ((len != 6 && len != 8) || !isxdigit(color[0]) || !isxdigit(color[1])) {
		return false;
	}
	char *ptr;
	uint32_t parsed = strtoul(color, &ptr, 16);
	if (*ptr != '\0') {
		return false;
	}
	*result = len == 6 ? ((parsed << 8) | 0xFF) : parsed;
	return true;
}

static char *
read_and_trim_stdin(void)
{
	char *buffer = NULL, *line = NULL;
	size_t buffer_len = 0, line_size = 0;
	while (1) {
		ssize_t nread = getline(&line, &line_size, stdin);
		if (nread == -1) {
			if (feof(stdin)) {
				break;
			} else {
				perror("getline");
				goto freeline;
			}
		}
		buffer = realloc(buffer, buffer_len + nread + 1);
		if (!buffer) {
			perror("realloc");
			goto freebuf;
		}
		memcpy(&buffer[buffer_len], line, nread + 1);
		buffer_len += nread;
	}
	free(line);

	while (buffer_len && buffer[buffer_len - 1] == '\n') {
		buffer[--buffer_len] = '\0';
	}

	return buffer;

freeline:
	free(line);
freebuf:
	free(buffer);
	return NULL;
}

int
swaynag_parse_options(int argc, char **argv, struct swaynag *swaynag,
		struct conf *conf, bool *debug)
{
	enum type_options {
		TO_COLOR_BACKGROUND = 256,
		TO_COLOR_BORDER,
		TO_COLOR_BORDER_BOTTOM,
		TO_COLOR_BUTTON,
		TO_COLOR_DETAILS,
		TO_COLOR_TEXT,
		TO_COLOR_BUTTON_TEXT,
		TO_THICK_BAR_BORDER,
		TO_PADDING_MESSAGE,
		TO_THICK_DET_BORDER,
		TO_THICK_BTN_BORDER,
		TO_GAP_BTN,
		TO_GAP_BTN_DISMISS,
		TO_MARGIN_BTN_RIGHT,
		TO_PADDING_BTN,
	};

	static const struct option opts[] = {
		{"button", required_argument, NULL, 'b'},
		{"button-no-terminal", required_argument, NULL, 'B'},
		{"button-dismiss", required_argument, NULL, 'z'},
		{"button-dismiss-no-terminal", required_argument, NULL, 'Z'},
		{"debug", no_argument, NULL, 'd'},
		{"edge", required_argument, NULL, 'e'},
		{"layer", required_argument, NULL, 'y'},
		{"font", required_argument, NULL, 'f'},
		{"help", no_argument, NULL, 'h'},
		{"detailed-message", no_argument, NULL, 'l'},
		{"detailed-button", required_argument, NULL, 'L'},
		{"message", required_argument, NULL, 'm'},
		{"output", required_argument, NULL, 'o'},
		{"dismiss-button", required_argument, NULL, 's'},
		{"timeout", no_argument, NULL, 't'},
		{"version", no_argument, NULL, 'v'},

		{"background", required_argument, NULL, TO_COLOR_BACKGROUND},
		{"border", required_argument, NULL, TO_COLOR_BORDER},
		{"border-bottom", required_argument, NULL, TO_COLOR_BORDER_BOTTOM},
		{"button-background", required_argument, NULL, TO_COLOR_BUTTON},
		{"text", required_argument, NULL, TO_COLOR_TEXT},
		{"button-text", required_argument, NULL, TO_COLOR_BUTTON_TEXT},
		{"border-bottom-size", required_argument, NULL, TO_THICK_BAR_BORDER},
		{"message-padding", required_argument, NULL, TO_PADDING_MESSAGE},
		{"details-border-size", required_argument, NULL, TO_THICK_DET_BORDER},
		{"details-background", required_argument, NULL, TO_COLOR_DETAILS},
		{"button-border-size", required_argument, NULL, TO_THICK_BTN_BORDER},
		{"button-gap", required_argument, NULL, TO_GAP_BTN},
		{"button-dismiss-gap", required_argument, NULL, TO_GAP_BTN_DISMISS},
		{"button-margin-right", required_argument, NULL, TO_MARGIN_BTN_RIGHT},
		{"button-padding", required_argument, NULL, TO_PADDING_BTN},

		{0, 0, 0, 0}
	};

	const char *usage =
		"Usage: swaynag [options...]\n"
		"\n"
		"  -b, --button <text> <action>  Create a button with text that "
			"executes action in a terminal when pressed. Multiple buttons can "
			"be defined.\n"
		"  -B, --button-no-terminal <text> <action>  Like --button, but does "
			"not run the action in a terminal.\n"
		"  -z, --button-dismiss <text> <action>  Create a button with text that "
			"dismisses swaynag, and executes action in a terminal when pressed. "
			"Multiple buttons can be defined.\n"
		"  -Z, --button-dismiss-no-terminal <text> <action>  Like "
			"--button-dismiss, but does not run the action in a terminal.\n"
		"  -d, --debug                     Enable debugging.\n"
		"  -e, --edge top|bottom           Set the edge to use.\n"
		"  -y, --layer overlay|top|bottom|background\n"
		"                                  Set the layer to use.\n"
		"  -f, --font <font>               Set the font to use.\n"
		"  -h, --help                      Show help message and quit.\n"
		"  -l, --detailed-message          Read a detailed message from stdin.\n"
		"  -L, --detailed-button <text>    Set the text of the detail button.\n"
		"  -m, --message <msg>             Set the message text.\n"
		"  -o, --output <output>           Set the output to use.\n"
		"  -s, --dismiss-button <text>     Set the dismiss button text.\n"
		"  -t, --timeout <seconds>         Set duration to close dialog.\n"
		"  -x, --exclusive-zone            Use exclusive zone.\n"
		"  -v, --version                   Show the version number and quit.\n"
		"\n"
		"The following appearance options can also be given:\n"
		"  --background RRGGBB[AA]         Background color.\n"
		"  --border RRGGBB[AA]             Border color.\n"
		"  --border-bottom RRGGBB[AA]      Bottom border color.\n"
		"  --button-background RRGGBB[AA]  Button background color.\n"
		"  --text RRGGBB[AA]               Text color.\n"
		"  --button-text RRGGBB[AA]        Button text color.\n"
		"  --border-bottom-size size       Thickness of the bar border.\n"
		"  --message-padding padding       Padding for the message.\n"
		"  --details-border-size size      Thickness for the details border.\n"
		"  --details-background RRGGBB[AA] Details background color.\n"
		"  --button-border-size size       Thickness for the button border.\n"
		"  --button-gap gap                Size of the gap between buttons\n"
		"  --button-dismiss-gap gap        Size of the gap for dismiss button.\n"
		"  --button-margin-right margin    Margin from dismiss button to edge.\n"
		"  --button-padding padding        Padding for the button text.\n";

	optind = 1;
	while (1) {
		int c = getopt_long(argc, argv, "b:B:z:Z:c:de:y:f:hlL:m:o:s:t:vx", opts, NULL);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'b': // Button
		case 'B': // Button (No Terminal)
		case 'z': // Button (Dismiss)
		case 'Z': // Button (Dismiss, No Terminal)
			if (swaynag) {
				if (optind >= argc) {
					fprintf(stderr, "Missing action for button %s\n", optarg);
					return LAB_EXIT_FAILURE;
				}
				struct swaynag_button *button = calloc(1, sizeof(*button));
				if (!button) {
					perror("calloc");
					return LAB_EXIT_FAILURE;
				}
				button->text = strdup(optarg);
				button->type = SWAYNAG_ACTION_COMMAND;
				button->action = strdup(argv[optind]);
				button->terminal = c == 'b';
				button->dismiss = c == 'z' || c == 'Z';
				wl_list_insert(swaynag->buttons.prev, &button->link);
			}
			optind++;
			break;
		case 'd': // Debug
			if (debug) {
				*debug = true;
			}
			break;
		case 'e': // Edge
			if (strcmp(optarg, "top") == 0) {
				conf->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
					| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			} else if (strcmp(optarg, "bottom") == 0) {
				conf->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
					| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
					| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
			} else {
				fprintf(stderr, "Invalid edge: %s\n", optarg);
				return LAB_EXIT_FAILURE;
			}
			break;
		case 'y': // Layer
			if (strcmp(optarg, "background") == 0) {
				conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
			} else if (strcmp(optarg, "bottom") == 0) {
				conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
			} else if (strcmp(optarg, "top") == 0) {
				conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
			} else if (strcmp(optarg, "overlay") == 0) {
				conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			} else {
				fprintf(stderr, "Invalid layer: %s\n"
						"Usage: --layer overlay|top|bottom|background\n",
						optarg);
				return LAB_EXIT_FAILURE;
			}
			break;
		case 'f': // Font
			pango_font_description_free(conf->font_description);
			conf->font_description = pango_font_description_from_string(optarg);
			break;
		case 'l': // Detailed Message
			if (swaynag) {
				free(swaynag->details.message);
				swaynag->details.message = read_and_trim_stdin();
				if (!swaynag->details.message) {
					return LAB_EXIT_FAILURE;
				}
				swaynag->details.button_up.text = strdup("▲");
				swaynag->details.button_down.text = strdup("▼");
			}
			break;
		case 'L': // Detailed Button Text
			if (swaynag) {
				free(swaynag->details.details_text);
				swaynag->details.details_text = strdup(optarg);
			}
			break;
		case 'm': // Message
			if (swaynag) {
				free(swaynag->message);
				swaynag->message = strdup(optarg);
			}
			break;
		case 'o': // Output
			free(conf->output);
			conf->output = strdup(optarg);
			break;
		case 's': // Dismiss Button Text
			if (swaynag) {
				struct swaynag_button *button;
				wl_list_for_each(button, &swaynag->buttons, link) {
					free(button->text);
					button->text = strdup(optarg);
					break;
				}
			}
			break;
		case 't':
			swaynag->details.close_timeout = atoi(optarg);
			break;
		case 'x':
			swaynag->details.use_exclusive_zone = true;
			break;
		case 'v': // Version
			// TODO
			return LAB_EXIT_FAILURE;
		case TO_COLOR_BACKGROUND: // Background color
			if (!parse_color(optarg, &conf->background)) {
				fprintf(stderr, "Invalid background color: %s", optarg);
			}
			break;
		case TO_COLOR_BORDER: // Border color
			if (!parse_color(optarg, &conf->border)) {
				fprintf(stderr, "Invalid border color: %s", optarg);
			}
			break;
		case TO_COLOR_BORDER_BOTTOM: // Bottom border color
			if (!parse_color(optarg, &conf->border_bottom)) {
				fprintf(stderr, "Invalid border bottom color: %s", optarg);
			}
			break;
		case TO_COLOR_BUTTON:  // Button background color
			if (!parse_color(optarg, &conf->button_background)) {
				fprintf(stderr, "Invalid button background color: %s", optarg);
			}
			break;
		case TO_COLOR_DETAILS:  // Details background color
			if (!parse_color(optarg, &conf->details_background)) {
				fprintf(stderr, "Invalid details background color: %s", optarg);
			}
			break;
		case TO_COLOR_TEXT:  // Text color
			if (!parse_color(optarg, &conf->text)) {
				fprintf(stderr, "Invalid text color: %s", optarg);
			}
			break;
		case TO_COLOR_BUTTON_TEXT:  // Button text color
			if (!parse_color(optarg, &conf->button_text)) {
				fprintf(stderr, "Invalid button text color: %s", optarg);
			}
			break;
		case TO_THICK_BAR_BORDER:  // Bottom border thickness
			conf->bar_border_thickness = strtol(optarg, NULL, 0);
			break;
		case TO_PADDING_MESSAGE:  // Message padding
			conf->message_padding = strtol(optarg, NULL, 0);
			break;
		case TO_THICK_DET_BORDER:  // Details border thickness
			conf->details_border_thickness = strtol(optarg, NULL, 0);
			break;
		case TO_THICK_BTN_BORDER:  // Button border thickness
			conf->button_border_thickness = strtol(optarg, NULL, 0);
			break;
		case TO_GAP_BTN: // Gap between buttons
			conf->button_gap = strtol(optarg, NULL, 0);
			break;
		case TO_GAP_BTN_DISMISS:  // Gap between dismiss button
			conf->button_gap_close = strtol(optarg, NULL, 0);
			break;
		case TO_MARGIN_BTN_RIGHT:  // Margin on the right side of button area
			conf->button_margin_right = strtol(optarg, NULL, 0);
			break;
		case TO_PADDING_BTN:  // Padding for the button text
			conf->button_padding = strtol(optarg, NULL, 0);
			break;
		default: // Help or unknown flag
			fprintf(c == 'h' ? stdout : stderr, "%s", usage);
			return LAB_EXIT_FAILURE;
		}
	}

	return LAB_EXIT_SUCCESS;
}

void
sig_handler(int signal)
{
	swaynag_destroy(&swaynag);
	exit(LAB_EXIT_FAILURE);
}

void
sway_terminate(int code)
{
	swaynag_destroy(&swaynag);
	exit(code);
}

int
main(int argc, char **argv)
{
	struct conf conf = { 0 };
	conf_init(&conf);
	swaynag.conf = &conf;

	wl_list_init(&swaynag.buttons);
	wl_list_init(&swaynag.outputs);
	wl_list_init(&swaynag.seats);

	struct swaynag_button *button_close = calloc(1, sizeof(struct swaynag_button));
	button_close->text = strdup("X");
	button_close->type = SWAYNAG_ACTION_DISMISS;
	wl_list_insert(swaynag.buttons.prev, &button_close->link);

	swaynag.details.details_text = strdup("Toggle details");
	swaynag.details.close_timeout = 5;
	swaynag.details.close_timeout_cancel = true;
	swaynag.details.use_exclusive_zone = false;

	bool debug = false;
	if (argc > 1) {
		exit_status = swaynag_parse_options(argc, argv, &swaynag, &conf, &debug);
		if (exit_status == LAB_EXIT_FAILURE) {
			goto cleanup;
		}
	}
	wlr_log_init(debug ? WLR_DEBUG : WLR_ERROR, NULL);

	if (!swaynag.message) {
		wlr_log(WLR_ERROR, "No message passed. Please provide --message/-m");
		exit_status = LAB_EXIT_FAILURE;
		goto cleanup;
	}

	if (swaynag.details.message) {
		swaynag.details.button_details = calloc(1, sizeof(struct swaynag_button));
		swaynag.details.button_details->text = strdup(swaynag.details.details_text);
		swaynag.details.button_details->type = SWAYNAG_ACTION_EXPAND;
		wl_list_insert(swaynag.buttons.prev, &swaynag.details.button_details->link);
	}

	wlr_log(WLR_DEBUG, "Output: %s", swaynag.conf->output);
	wlr_log(WLR_DEBUG, "Anchors: %" PRIu32, swaynag.conf->anchors);
	wlr_log(WLR_DEBUG, "Message: %s", swaynag.message);
	char *font = pango_font_description_to_string(swaynag.conf->font_description);
	wlr_log(WLR_DEBUG, "Font: %s", font);
	free(font);
	wlr_log(WLR_DEBUG, "Buttons");

	struct swaynag_button *button;
	wl_list_for_each(button, &swaynag.buttons, link) {
		wlr_log(WLR_DEBUG, "\t[%s] `%s`", button->text, button->action);
	}

	signal(SIGTERM, sig_handler);

	swaynag_setup(&swaynag);

	swaynag_run(&swaynag);

cleanup:
	swaynag_destroy(&swaynag);
	return exit_status;
}
