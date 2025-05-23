/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copied from https://github.com/swaywm/sway
 *
 * Copyright (C) 2016-2017 Drew DeVault
 */
#ifndef BUFFERS_H
#define BUFFERS_H
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct pool_buffer {
	struct wl_buffer *buffer;
	cairo_surface_t *surface;
	cairo_t *cairo;
	PangoContext *pango;
	uint32_t width, height;
	void *data;
	size_t size;
	bool busy;
};

struct pool_buffer *get_next_buffer(struct wl_shm *shm,
		struct pool_buffer pool[static 2], uint32_t width, uint32_t height);
void destroy_buffer(struct pool_buffer *buffer);

#endif
