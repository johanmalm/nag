#ifndef NAG_H
#define NAG_H
#include <stdint.h>
#include <strings.h>
#include "cursor-shape-v1-client-protocol.h"
#include "pool-buffer.h"

#define SWAYNAG_MAX_HEIGHT 500

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

void swaynag_setup(struct swaynag *swaynag);

void swaynag_run(struct swaynag *swaynag);

void swaynag_destroy(struct swaynag *swaynag);

#endif /* NAG_H */
