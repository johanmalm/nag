#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "list.h"
#include "log.h"
#include "swaynag.h"
#include "types.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct swaynag swaynag;

static bool parse_color(const char *color, uint32_t *result) {
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

static char *read_and_trim_stdin(void) {
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

int swaynag_parse_options(int argc, char **argv, struct swaynag *swaynag,
		list_t *types, struct swaynag_type *type, char **config, bool *debug) {
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
		{"type", required_argument, NULL, 't'},
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
		"  -B, --button-no-terminal <text> <action>  Like --button, but does"
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
		"  -t, --type <type>               Set the message type.\n"
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
		int c = getopt_long(argc, argv, "b:B:z:Z:c:de:y:f:hlL:m:o:s:t:v", opts, NULL);
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
					return EXIT_FAILURE;
				}
				struct swaynag_button *button = calloc(1, sizeof(struct swaynag_button));
				if (!button) {
					perror("calloc");
					return EXIT_FAILURE;
				}
				button->text = strdup(optarg);
				button->type = SWAYNAG_ACTION_COMMAND;
				button->action = strdup(argv[optind]);
				button->terminal = c == 'b';
				button->dismiss = c == 'z' || c == 'Z';
				list_add(swaynag->buttons, button);
			}
			optind++;
			break;
		case 'c': // Config
			if (config) {
				*config = strdup(optarg);
			}
			break;
		case 'd': // Debug
			if (debug) {
				*debug = true;
			}
			break;
		case 'e': // Edge
			if (type) {
				if (strcmp(optarg, "top") == 0) {
					type->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
						| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
				} else if (strcmp(optarg, "bottom") == 0) {
					type->anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
						| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
						| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
				} else {
					fprintf(stderr, "Invalid edge: %s\n", optarg);
					return EXIT_FAILURE;
				}
			}
			break;
		case 'y': // Layer
			if (type) {
				if (strcmp(optarg, "background") == 0) {
					type->layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
				} else if (strcmp(optarg, "bottom") == 0) {
					type->layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
				} else if (strcmp(optarg, "top") == 0) {
					type->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
				} else if (strcmp(optarg, "overlay") == 0) {
					type->layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
				} else {
					fprintf(stderr, "Invalid layer: %s\n"
							"Usage: --layer overlay|top|bottom|background\n",
							optarg);
					return EXIT_FAILURE;
				}
			}
			break;
		case 'f': // Font
			if (type) {
				pango_font_description_free(type->font_description);
				type->font_description = pango_font_description_from_string(optarg);
			}
			break;
		case 'l': // Detailed Message
			if (swaynag) {
				free(swaynag->details.message);
				swaynag->details.message = read_and_trim_stdin();
				if (!swaynag->details.message) {
					return EXIT_FAILURE;
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
			if (type) {
				free(type->output);
				type->output = strdup(optarg);
			}
			break;
		case 's': // Dismiss Button Text
			if (swaynag) {
				struct swaynag_button *button_close = swaynag->buttons->items[0];
				free(button_close->text);
				button_close->text = strdup(optarg);
			}
			break;
		case 't': // Type
			if (swaynag) {
				swaynag->type = swaynag_type_get(types, optarg);
				if (!swaynag->type) {
					fprintf(stderr, "Unknown type %s\n", optarg);
					return EXIT_FAILURE;
				}
			}
			break;
		case 'v': // Version
			// TODO
			return -1;
		case TO_COLOR_BACKGROUND: // Background color
			if (type && !parse_color(optarg, &type->background)) {
				fprintf(stderr, "Invalid background color: %s", optarg);
			}
			break;
		case TO_COLOR_BORDER: // Border color
			if (type && !parse_color(optarg, &type->border)) {
				fprintf(stderr, "Invalid border color: %s", optarg);
			}
			break;
		case TO_COLOR_BORDER_BOTTOM: // Bottom border color
			if (type && !parse_color(optarg, &type->border_bottom)) {
				fprintf(stderr, "Invalid border bottom color: %s", optarg);
			}
			break;
		case TO_COLOR_BUTTON:  // Button background color
			if (type && !parse_color(optarg, &type->button_background)) {
				fprintf(stderr, "Invalid button background color: %s", optarg);
			}
			break;
		case TO_COLOR_DETAILS:  // Details background color
			if (type && !parse_color(optarg, &type->details_background)) {
				fprintf(stderr, "Invalid details background color: %s", optarg);
			}
			break;
		case TO_COLOR_TEXT:  // Text color
			if (type && !parse_color(optarg, &type->text)) {
				fprintf(stderr, "Invalid text color: %s", optarg);
			}
			break;
		case TO_COLOR_BUTTON_TEXT:  // Button text color
			if (type && !parse_color(optarg, &type->button_text)) {
				fprintf(stderr, "Invalid button text color: %s", optarg);
			}
			break;
		case TO_THICK_BAR_BORDER:  // Bottom border thickness
			if (type) {
				type->bar_border_thickness = strtol(optarg, NULL, 0);
			}
			break;
		case TO_PADDING_MESSAGE:  // Message padding
			if (type) {
				type->message_padding = strtol(optarg, NULL, 0);
			}
			break;
		case TO_THICK_DET_BORDER:  // Details border thickness
			if (type) {
				type->details_border_thickness = strtol(optarg, NULL, 0);
			}
			break;
		case TO_THICK_BTN_BORDER:  // Button border thickness
			if (type) {
				type->button_border_thickness = strtol(optarg, NULL, 0);
			}
			break;
		case TO_GAP_BTN: // Gap between buttons
			if (type) {
				type->button_gap = strtol(optarg, NULL, 0);
			}
			break;
		case TO_GAP_BTN_DISMISS:  // Gap between dismiss button
			if (type) {
				type->button_gap_close = strtol(optarg, NULL, 0);
			}
			break;
		case TO_MARGIN_BTN_RIGHT:  // Margin on the right side of button area
			if (type) {
				type->button_margin_right = strtol(optarg, NULL, 0);
			}
			break;
		case TO_PADDING_BTN:  // Padding for the button text
			if (type) {
				type->button_padding = strtol(optarg, NULL, 0);
			}
			break;
		default: // Help or unknown flag
			fprintf(c == 'h' ? stdout : stderr, "%s", usage);
			return -1;
		}
	}

	return 0;
}
void sig_handler(int signal) {
	swaynag_destroy(&swaynag);
	exit(EXIT_FAILURE);
}

void sway_terminate(int code) {
	swaynag_destroy(&swaynag);
	exit(code);
}

int main(int argc, char **argv) {
	int status = EXIT_SUCCESS;

	list_t *types = create_list();
	swaynag_types_add_default(types);

	swaynag.buttons = create_list();
	wl_list_init(&swaynag.outputs);
	wl_list_init(&swaynag.seats);

	struct swaynag_button *button_close = calloc(1, sizeof(struct swaynag_button));
	button_close->text = strdup("X");
	button_close->type = SWAYNAG_ACTION_DISMISS;
	list_add(swaynag.buttons, button_close);

	swaynag.details.details_text = strdup("Toggle details");

	bool debug = false;
	if (argc > 1) {
		struct swaynag_type *type_args = swaynag_type_new("<args>");
		list_add(types, type_args);

		status = swaynag_parse_options(argc, argv, &swaynag, types,
				type_args, NULL, &debug);
		if (status != 0) {
			goto cleanup;
		}
	}
	sway_log_init(debug ? SWAY_DEBUG : SWAY_ERROR, NULL);

	if (!swaynag.message) {
		sway_log(SWAY_ERROR, "No message passed. Please provide --message/-m");
		status = EXIT_FAILURE;
		goto cleanup;
	}

	if (!swaynag.type) {
		swaynag.type = swaynag_type_get(types, "error");
	}

	// Construct a new type with the defaults as the base, the general config
	// on top of that, followed by the type config, and finally any command
	// line arguments
	struct swaynag_type *type = swaynag_type_new(swaynag.type->name);
	swaynag_type_merge(type, swaynag_type_get(types, "<defaults>"));
	swaynag_type_merge(type, swaynag_type_get(types, "<config>"));
	swaynag_type_merge(type, swaynag.type);
	swaynag_type_merge(type, swaynag_type_get(types, "<args>"));
	swaynag.type = type;

	if (swaynag.details.message) {
		swaynag.details.button_details = calloc(1, sizeof(struct swaynag_button));
		swaynag.details.button_details->text = strdup(swaynag.details.details_text);
		swaynag.details.button_details->type = SWAYNAG_ACTION_EXPAND;
		list_add(swaynag.buttons, swaynag.details.button_details);
	}

	sway_log(SWAY_DEBUG, "Output: %s", swaynag.type->output);
	sway_log(SWAY_DEBUG, "Anchors: %" PRIu32, swaynag.type->anchors);
	sway_log(SWAY_DEBUG, "Type: %s", swaynag.type->name);
	sway_log(SWAY_DEBUG, "Message: %s", swaynag.message);
	char *font = pango_font_description_to_string(swaynag.type->font_description);
	sway_log(SWAY_DEBUG, "Font: %s", font);
	free(font);
	sway_log(SWAY_DEBUG, "Buttons");
	for (int i = 0; i < swaynag.buttons->length; i++) {
		struct swaynag_button *button = swaynag.buttons->items[i];
		sway_log(SWAY_DEBUG, "\t[%s] `%s`", button->text, button->action);
	}

	signal(SIGTERM, sig_handler);

	swaynag_setup(&swaynag);

	/* FIXME: use proper config parsing */
	swaynag.details.close_timeout = 5;
	swaynag.details.close_timeout_cancel = true;
	swaynag.details.use_exclusive_zone = false;

	swaynag_run(&swaynag);

cleanup:
	swaynag_types_free(types);
	swaynag_destroy(&swaynag);
	return status;
}
