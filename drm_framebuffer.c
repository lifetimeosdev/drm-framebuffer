/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

extern char _picture_start[];
extern char _picture_end[];

struct framebuffer {
	int fd;
	uint32_t buffer_id;
	uint16_t res_x;
	uint16_t res_y;
	uint8_t *data;
	uint32_t size;
	struct drm_mode_create_dumb dumb_framebuffer;
	drmModeCrtcPtr crtc;
	drmModeConnectorPtr connector;
	drmModeModeInfoPtr resolution;
};

struct type_name {
	unsigned int type;
	const char *name;
};

static const struct type_name connector_type_names[] = {
	{DRM_MODE_CONNECTOR_Unknown, "unknown"},
	{DRM_MODE_CONNECTOR_VGA, "VGA"},
	{DRM_MODE_CONNECTOR_DVII, "DVI-I"},
	{DRM_MODE_CONNECTOR_DVID, "DVI-D"},
	{DRM_MODE_CONNECTOR_DVIA, "DVI-A"},
	{DRM_MODE_CONNECTOR_Composite, "composite"},
	{DRM_MODE_CONNECTOR_SVIDEO, "s-video"},
	{DRM_MODE_CONNECTOR_LVDS, "LVDS"},
	{DRM_MODE_CONNECTOR_Component, "component"},
	{DRM_MODE_CONNECTOR_9PinDIN, "9-pin DIN"},
	{DRM_MODE_CONNECTOR_DisplayPort, "DP"},
	{DRM_MODE_CONNECTOR_HDMIA, "HDMI-A"},
	{DRM_MODE_CONNECTOR_HDMIB, "HDMI-B"},
	{DRM_MODE_CONNECTOR_TV, "TV"},
	{DRM_MODE_CONNECTOR_eDP, "eDP"},
	{DRM_MODE_CONNECTOR_VIRTUAL, "Virtual"},
	{DRM_MODE_CONNECTOR_DSI, "DSI"},
	{DRM_MODE_CONNECTOR_DPI, "DPI"},
};

static const char *connector_type_name(unsigned int type)
{
	if (type < ARRAY_SIZE(connector_type_names) && type >= 0) {
		return connector_type_names[type].name;
	}

	return "INVALID";
}

static void release_framebuffer(struct framebuffer *fb)
{
	if (fb->fd) {
		/* Try to become master again, else we can't set CRTC. Then the current master needs
		 * to reset everything. */
		drmSetMaster(fb->fd);
		if (fb->crtc) {
			/* Set back to orignal frame buffer */
			drmModeSetCrtc(fb->fd, fb->crtc->crtc_id, fb->crtc->buffer_id, 0, 0,
				       &fb->connector->connector_id, 1, fb->resolution);
			drmModeFreeCrtc(fb->crtc);
		}
		if (fb->buffer_id)
			drmModeFreeFB(drmModeGetFB(fb->fd, fb->buffer_id));
		/* This will also release resolution */
		if (fb->connector) {
			drmModeFreeConnector(fb->connector);
			fb->resolution = 0;
		}
		if (fb->dumb_framebuffer.handle)
			ioctl(fb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, fb->dumb_framebuffer);
		close(fb->fd);
	}
}

static int get_framebuffer(const char *dri_device, const char *connector_name,
			   struct framebuffer *fb)
{
	int err;
	int fd;
	drmModeResPtr res;
	drmModeEncoderPtr encoder = 0;

	/* Open the dri device /dev/dri/cardX */
	fd = open(dri_device, O_RDWR);
	if (fd < 0) {
		printf("Could not open dri device %s\n", dri_device);
		return -EINVAL;
	}

	/* Get the resources of the DRM device (connectors, encoders, etc.)*/
	res = drmModeGetResources(fd);
	if (!res) {
		printf("Could not get drm resources\n");
		return -EINVAL;
	}

	/* Search the connector provided as argument */
	drmModeConnectorPtr connector = 0;
	for (int i = 0; i < res->count_connectors; i++) {
		char name[32];

		connector = drmModeGetConnectorCurrent(fd, res->connectors[i]);
		if (!connector)
			continue;

		snprintf(name, sizeof(name), "%s-%u",
			 connector_type_name(connector->connector_type),
			 connector->connector_type_id);

		if (strncmp(name, connector_name, sizeof(name)) == 0)
			break;

		drmModeFreeConnector(connector);
		connector = 0;
	}

	if (!connector) {
		printf("Could not find matching connector %s\n", connector_name);
		return -EINVAL;
	}

	/* Get the preferred resolution */
	drmModeModeInfoPtr resolution = 0;
	for (int i = 0; i < connector->count_modes; i++) {
		drmModeModeInfoPtr res = 0;
		res = &connector->modes[i];
		if (res->type & DRM_MODE_TYPE_PREFERRED)
			resolution = res;
	}

	if (!resolution) {
		printf("Could not find preferred resolution\n");
		err = -EINVAL;
		goto cleanup;
	}

	fb->dumb_framebuffer.height = resolution->vdisplay;
	fb->dumb_framebuffer.width = resolution->hdisplay;
	fb->dumb_framebuffer.bpp = 32;

	err = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &fb->dumb_framebuffer);
	if (err) {
		printf("Could not create dumb framebuffer (err=%d)\n", err);
		goto cleanup;
	}

	err = drmModeAddFB(fd, resolution->hdisplay, resolution->vdisplay, 24, 32,
			   fb->dumb_framebuffer.pitch, fb->dumb_framebuffer.handle, &fb->buffer_id);
	if (err) {
		printf("Could not add framebuffer to drm (err=%d)\n", err);
		goto cleanup;
	}

	encoder = drmModeGetEncoder(fd, connector->encoder_id);
	if (!encoder) {
		printf("Could not get encoder\n");
		err = -EINVAL;
		goto cleanup;
	}

	/* Get the crtc settings */
	fb->crtc = drmModeGetCrtc(fd, encoder->crtc_id);

	struct drm_mode_map_dumb mreq;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = fb->dumb_framebuffer.handle;

	err = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (err) {
		printf("Mode map dumb framebuffer failed (err=%d)\n", err);
		goto cleanup;
	}

	fb->data = mmap(0, fb->dumb_framebuffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			mreq.offset);
	if (fb->data == MAP_FAILED) {
		err = errno;
		printf("Mode map failed (err=%d)\n", err);
		goto cleanup;
	}

	/* Make sure we are not master anymore so that other processes can add new framebuffers as
	 * well */
	drmDropMaster(fd);

	fb->fd = fd;
	fb->connector = connector;
	fb->resolution = resolution;

cleanup:
	/* We don't need the encoder and connector anymore so let's free them */
	if (encoder)
		drmModeFreeEncoder(encoder);

	if (err)
		release_framebuffer(fb);

	return err;
}

static int verbose = 0;

static void usage(void)
{
	printf("\ndrm-framebuffer [OPTIONS...]\n\n"
	       "Pipe data to a framebuffer\n\n"
	       "  -l list connectors\n"
	       "  -r get resolution dri device and connector needs to be set\n"
	       "  -v do more verbose printing\n"
	       "  -h show this message\n\n");
}

#define print_verbose(...) \
	if (verbose)       \
	printf(__VA_ARGS__)

static int list_resources(const char *dri_device)
{
	int fd;
	drmModeResPtr res;

	fd = open(dri_device, O_RDWR);
	if (fd < 0) {
		printf("Could not open dri device %s\n", dri_device);
		return -EINVAL;
	}

	res = drmModeGetResources(fd);
	if (!res) {
		printf("Could not get drm resources\n");
		return -EINVAL;
	}

	printf("connectors:");
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnectorPtr connector = 0;
		drmModeEncoderPtr encoder = 0;

		printf("\nNumber: %d ", res->connectors[i]);
		connector = drmModeGetConnectorCurrent(fd, res->connectors[i]);
		if (!connector)
			continue;

		printf("Name: %s-%u ", connector_type_name(connector->connector_type),
		       connector->connector_type_id);

		printf("Encoder: %d ", connector->encoder_id);

		encoder = drmModeGetEncoder(fd, connector->encoder_id);
		if (!encoder)
			continue;

		printf("Crtc: %d", encoder->crtc_id);

		drmModeFreeEncoder(encoder);
		drmModeFreeConnector(connector);
	}

	printf("\nFramebuffers: ");
	for (int i = 0; i < res->count_fbs; i++) {
		printf("%d ", res->fbs[i]);
	}

	printf("\nCRTCs: ");
	for (int i = 0; i < res->count_crtcs; i++) {
		printf("%d ", res->crtcs[i]);
	}

	printf("\nencoders: ");
	for (int i = 0; i < res->count_encoders; i++) {
		printf("%d ", res->encoders[i]);
	}
	printf("\n");

	drmModeFreeResources(res);

	return 0;
}

static int get_resolution(const char *dri_device, const char *connector_name)
{
	int err = 0;
	int fd;
	drmModeResPtr res;

	fd = open(dri_device, O_RDWR);
	if (fd < 0) {
		printf("Could not open dri device %s\n", dri_device);
		return -EINVAL;
	}

	res = drmModeGetResources(fd);
	if (!res) {
		printf("Could not get drm resources\n");
		return -EINVAL;
	}

	/* Search the connector provided as argument */
	drmModeConnectorPtr connector = 0;
	for (int i = 0; i < res->count_connectors; i++) {
		char name[32];

		connector = drmModeGetConnectorCurrent(fd, res->connectors[i]);
		if (!connector)
			continue;

		snprintf(name, sizeof(name), "%s-%u",
			 connector_type_name(connector->connector_type),
			 connector->connector_type_id);

		if (strncmp(name, connector_name, sizeof(name)) == 0)
			break;

		drmModeFreeConnector(connector);
		connector = 0;
	}

	if (!connector) {
		printf("Could not find matching connector %s\n", connector_name);
		return -EINVAL;
	}

	/* Get the preferred resolution */
	drmModeModeInfoPtr resolution = 0;
	for (int i = 0; i < connector->count_modes; i++) {
		resolution = &connector->modes[i];
		if (resolution->type & DRM_MODE_TYPE_PREFERRED)
			break;
	}

	if (!resolution) {
		printf("Could not find preferred resolution\n");
		err = -EINVAL;
		goto error;
	}

	printf("%ux%u\n", resolution->hdisplay, resolution->vdisplay);

error:
	drmModeFreeConnector(connector);
	drmModeFreeResources(res);
	close(fd);
	return err;
}

static int fill_framebuffer_from_stdin(struct framebuffer *fb)
{
	size_t total_read = 0;
	int ret;

	print_verbose("Loading image\n");
	memcpy(&fb->data[total_read], _picture_start, _picture_end - _picture_start);

	/* Make sure we synchronize the display with the buffer. This also works if page flips are
	 * enabled */
	ret = drmSetMaster(fb->fd);
	if (ret) {
		printf("Could not get master role for DRM.\n");
		return ret;
	}
	drmModeSetCrtc(fb->fd, fb->crtc->crtc_id, 0, 0, 0, NULL, 0, NULL);
	drmModeSetCrtc(fb->fd, fb->crtc->crtc_id, fb->buffer_id, 0, 0, &fb->connector->connector_id,
		       1, fb->resolution);
	drmDropMaster(fb->fd);

	print_verbose("Sent image to framebuffer\n");

	sigset_t wait_set;
	sigemptyset(&wait_set);
	sigaddset(&wait_set, SIGTERM);
	sigaddset(&wait_set, SIGINT);

	int sig;
	sigprocmask(SIG_BLOCK, &wait_set, NULL);
	sigwait(&wait_set, &sig);

	return 0;
}

int main(int argc, char **argv)
{
	char *dri_device = "/dev/dri/card0";
	char *connector = "HDMI-A-1";
	int c;
	int list = 0;
	int resolution = 0;
	int ret;

	opterr = 0;
	while ((c = getopt(argc, argv, "lrhv")) != -1) {
		switch (c) {
		case 'l':
			list = 1;
			break;
		case 'r':
			resolution = 1;
			break;
		case 'h':
			usage();
			return 1;
		case 'v':
			verbose = 1;
			break;
		default:
			break;
		}
	}

	if (list) {
		return list_resources(dri_device);
	}

	if (resolution) {
		return get_resolution(dri_device, connector);
	}

	struct framebuffer fb;
	memset(&fb, 0, sizeof(fb));
	ret = 1;
	if (get_framebuffer(dri_device, connector, &fb) == 0) {
		if (!fill_framebuffer_from_stdin(&fb)) {
			// successfully shown.
			ret = 0;
		}
		release_framebuffer(&fb);
	}

	return ret;
}
