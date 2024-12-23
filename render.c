#include <cairo.h>
#include <glib.h>
#include <stdint.h>
#include <pango/pangocairo.h>
#include <wlr/util/log.h>
#include "pool-buffer.h"
#include "swaynag.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static PangoLayout *get_pango_layout(cairo_t *cairo, const PangoFontDescription *desc,
		const char *text, double scale, bool markup) {
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

static void get_text_size(cairo_t *cairo, const PangoFontDescription *desc, int *width, int *height,
		int *baseline, double scale, bool markup, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (buf == NULL) {
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

static void render_text(cairo_t *cairo, const PangoFontDescription *desc,
		double scale, bool markup, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	gchar *buf = g_strdup_vprintf(fmt, args);
	va_end(args);
	if (buf == NULL) {
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
static void cairo_set_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

static uint32_t render_message(cairo_t *cairo, struct swaynag *swaynag) {
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

static void render_details_scroll_button(cairo_t *cairo,
		struct swaynag *swaynag, struct swaynag_button *button) {
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

static int get_detailed_scroll_button_width(cairo_t *cairo,
		struct swaynag *swaynag) {
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

static uint32_t render_detailed(cairo_t *cairo, struct swaynag *swaynag,
		uint32_t y) {
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

static uint32_t render_button(cairo_t *cairo, struct swaynag *swaynag,
		struct swaynag_button *button, int *x) {
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

static uint32_t render_to_cairo(cairo_t *cairo, struct swaynag *swaynag) {
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

void render_frame(struct swaynag *swaynag) {
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
