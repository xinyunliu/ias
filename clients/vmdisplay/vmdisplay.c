/*
 *-----------------------------------------------------------------------------
 * Filename: vmdisplay.c
 *-----------------------------------------------------------------------------
 * Copyright 2012-2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *-----------------------------------------------------------------------------
 * Description:
 *   Library for vmdisplay which has the helper function
 *-----------------------------------------------------------------------------
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define __user
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/mman.h>
#include <assert.h>
#include <sys/time.h>
#include <errno.h>
#include <stropts.h>
#include <sys/socket.h>
#include <sys/un.h>

#define PAGE_SIZE 0x1000
#define PAGE_SHIFT 12

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <libdrm/drm.h>
#include <libdrm/i915_drm.h>
#include <intel_bufmgr.h>
#include "wayland-drm-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "vmdisplay.h"
#include "vmdisplay-parser.h"

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8            fourcc_code('R', '8', ' ', ' ')	/* [7:0] R */
#endif

#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88          fourcc_code('G', 'R', '8', '8')	/* [15:0] G:R 8:8 little endian */
#endif

#define HYPER_DMABUF_LIST_LEN 4

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;

static struct buffer_list hyper_dmabuf_list;

struct egl_manager g_eman_common;
GLuint current_textureId[2];
uint32_t current_texture_sampler_format;
struct wl_buffer *current_buffer;
extern struct zwp_linux_dmabuf_v1 *wl_dmabuf;
extern struct wl_buffer *buf;

extern int use_egl;
extern int using_mesa;
extern int use_event_poll;

static int fd = -1;

hyper_dmabuf_id_t current_hyper_dmabuf_id = { 0, {0, 0, 0} };

unsigned int pipe_id = 0;

int hyper_dmabuf_fd = -1;
static int counter = 0;

vmdisplay_socket vmsocket;

#ifndef EGLTILING
#define EGL_TILING			  0x3062
#endif

int open_drm(void)
{
	fd = open("/dev/dri/card0", O_RDWR);
	return fd;
}

int import_bo_from_hbuf(int hyper_dmabuf_fd, hyper_dmabuf_id_t hid,
                        int width, int height, int bpp, char *out_buf)
{
        struct ioctl_hyper_dmabuf_export_fd msg = {0};
        int drm_fd;
        drm_intel_bo *bo;
        ssize_t size = width * height * bpp/8;

        msg.hid = hid;

        if(ioctl(hyper_dmabuf_fd, IOCTL_HYPER_DMABUF_EXPORT_FD, &msg)) {
                printf("%s: ioctl failed\n", __func__);
                return -1;
        }

        printf("%s: ioctl successful\n", __func__);

        drm_fd = drmOpen("i915", NULL);
        if (drm_fd < 0) {
                printf("Failed to open drm device\n");
                return -1;
        }

        dri_bufmgr *bufmgr = intel_bufmgr_gem_init(drm_fd, 0x80000);

        bo = drm_intel_bo_gem_create_from_prime(bufmgr, msg.fd, size);

        if (!bo) {
                printf("failed to map bo\n");
                return -1;
        }

        drm_intel_gem_bo_map_gtt(bo);

        if (!bo->virtual) {
                printf("failed to map bo in aperture\n");
                return -1;
        }

        /* Get data from start of buffer and print them out */
        memcpy(out_buf, (char*)bo->virtual, size);
        drm_intel_gem_bo_unmap_gtt(bo);

        return 0;
}

/* dump dma_buf binary data to file for offline analysis
   fd: dma_buf file discriptor
   fname: offline file name
   size: the count of bytes
*/
static void dump_dmabuf_data_to_file(int fd, const char *fname, size_t size)
{
	FILE * pfile = NULL;
	size_t len=0;

	int drm_fd;
	drm_intel_bo *bo;

	drm_fd = drmOpen("i915", NULL);
	if (drm_fd < 0) {
		printf("Failed to open drm device\n");
		return;
	}

	dri_bufmgr *bufmgr = intel_bufmgr_gem_init(drm_fd, 0x1000*8);

	bo = drm_intel_bo_gem_create_from_prime(bufmgr, fd, size);

	if (!bo) {
		printf("failed to map bo\n");
		goto err_drm_cleanup;
	}

	drm_intel_gem_bo_map_gtt(bo);

	if (!bo->virtual) {
		printf("failed to map bo in aperture\n");
		goto err_bufmgr_cleanup;
	}

	printf("Open %s\n", fname);
	pfile = fopen(fname, "wb");
	if(pfile) {
		/* Get data from start of buffer and print them out */
		/*  memcpy(out_buf, (int*)bo->virtual, size); */
		len = fwrite(bo->virtual, 1, size, pfile);
		fclose(pfile);
		printf(" dump %ld bytes to %s successed\n", len, fname);
	} else {
		printf(" Failed: can't open %s", fname);
	}

	drm_intel_gem_bo_unmap_gtt(bo);

err_bo_cleanup:
	drm_intel_bo_unreference(bo);

err_bufmgr_cleanup:
	drm_intel_bufmgr_destroy(bufmgr);

err_drm_cleanup:
	drmClose(drm_fd);

	return;
}

static void dump_dmabuf_data(int fd, const char *fname, int width, int height, int bpp, int n)
{
	static int i = 0;
	char bin_name[100];

	printf("dump_dmabuf_data: w:%d h:%d bpp:%d times:%d\n", width, height, bpp, n);

	if(i<n) {
		sprintf(bin_name,"%s_%d_%d_%d_%d.bin", fname, width, height, bpp, i);
		dump_dmabuf_data_to_file(fd, bin_name, width*height*bpp/8);
		i++;
	}
}

#define ALIGN(x, y) ((x + y - 1) & ~(y - 1))

static int find_rec(struct buffer_list *l, uint32_t id)
{
	int i, r;
	r = -1;

	for (i = 0; i < l->len; i++) {
		if (l->l[i].hyper_dmabuf_id == id) {
			r = i;
			break;
		}
	}

	return r;
}

static void age_list(struct buffer_list *l)
{
	int i;
	for (i = 0; i < l->len; i++)
		l->l[i].age++;
}

static void last_rec(struct buffer_list *l, int i)
{
	l->l[i].age = 0;
}

static int oldest_rec(struct buffer_list *l, int len)
{
	int i, a, r;
	a = 0, r = 0;

	for (i = 0; i < len; i++) {
		if (l->l[i].age >= a) {
			a = l->l[i].age;
			r = i;
		}
	}

	return r;
}

static void clear_rec(struct buffer_list *l, int i)
{
	if (g_Dbg) {
		printf("clear_rec(): hid=%x tex=%x %x\n",
			l->l[i].hyper_dmabuf_id,
			l->l[i].textureId[0],
			l->l[i].textureId[1]);
	}

	l->l[i].age = 0;

	glDeleteTextures(2, l->l[i].textureId);

	if (l->l[i].buffer)
		wl_buffer_destroy(l->l[i].buffer);

	l->l[i].textureId[0] = 0;
	l->l[i].textureId[1] = 0;
	l->l[i].width = 0;
	l->l[i].height = 0;
	l->l[i].buffer = 0;
	l->l[i].hyper_dmabuf_id = 0;
	l->l[i].gem_handle = 0;
}

void init_buffers(void)
{
	hyper_dmabuf_list.l = calloc(1, HYPER_DMABUF_LIST_LEN * sizeof(struct buffer_rec));
	hyper_dmabuf_list.len = HYPER_DMABUF_LIST_LEN;

	if(!hyper_dmabuf_list.l) {
		fprintf(stderr, "Error: allocating memory\n");
		exit(1);
	}
}

int init_hyper_dmabuf(int dom)
{
	struct ioctl_hyper_dmabuf_rx_ch_setup msg;

	hyper_dmabuf_fd = open(HYPER_DMABUF_DEV_PATH, O_RDWR);

	/* If failed, try legacy dev path of hyper dmabuf */
	if (hyper_dmabuf_fd < 0)
		hyper_dmabuf_fd = open(HYPER_DMABUF_DEV_PATH_LEGACY, O_RDWR);

	if (hyper_dmabuf_fd < 0) {
		printf("Cannot open hyper dmabuf device\n");
		return -1;
	}

	msg.source_domain = dom;

	if (ioctl(hyper_dmabuf_fd, IOCTL_HYPER_DMABUF_RX_CH_SETUP, &msg)) {
		printf("%s: ioctl failed\n", __func__);
		return -1;
	}
	return 0;
}

static void update_oldest_rec_hyper_dmabuf(uint32_t old_id, GLuint *textureId,
				    struct wl_buffer *buf, uint32_t width,
				    uint32_t height, int age)
{
	int r = oldest_rec(&hyper_dmabuf_list, HYPER_DMABUF_LIST_LEN);

	clear_rec(&hyper_dmabuf_list, r);

	hyper_dmabuf_list.l[r].hyper_dmabuf_id = old_id;
	hyper_dmabuf_list.l[r].buffer = buf;
	hyper_dmabuf_list.l[r].textureId[0] = textureId[0];
	hyper_dmabuf_list.l[r].textureId[1] = textureId[1];
	hyper_dmabuf_list.l[r].width = width;
	hyper_dmabuf_list.l[r].height = height;
	hyper_dmabuf_list.l[r].age = age;
}

static void update_hyper_dmabuf_list(int id, int old_id)
{
	int r = find_rec(&hyper_dmabuf_list, id);

	age_list(&hyper_dmabuf_list);

	if (r >= 0) {
		if (old_id == 0 ||
		    hyper_dmabuf_list.l[r].width != surf_width ||
		    hyper_dmabuf_list.l[r].height != surf_height) {
			clear_rec(&hyper_dmabuf_list, r);
			create_new_hyper_dmabuf_buffer();
		} else {
			last_rec(&hyper_dmabuf_list, r);
			current_textureId[0] =
			    hyper_dmabuf_list.l[r].textureId[0];
			current_textureId[1] =
			    hyper_dmabuf_list.l[r].textureId[1];
			current_buffer = hyper_dmabuf_list.l[r].buffer;
		}
	} else {
		create_new_hyper_dmabuf_buffer();
	}
}

int check_for_new_buffer(void)
{
	static hyper_dmabuf_id_t old_hyper_dmabuf_id = { 0, {0, 0, 0} };
	int ret = 0;

	if (use_event_poll) {
		ret = parse_event_metadata(hyper_dmabuf_fd, &counter);
	} else {
		ret = parse_socket_metadata(&vmsocket, &counter);
	}

	if (ret) {
		printf("Buffer table parse error\n");
		clear_hyper_dmabuf_list();
		return 1;
	}

	if (hyper_dmabuf_id.id == 0) {
		old_hyper_dmabuf_id.id = 0;
		clear_hyper_dmabuf_list();
	}
	if ((hyper_dmabuf_id.id > 0)
	    && (hyper_dmabuf_id.id != old_hyper_dmabuf_id.id)) {
		update_hyper_dmabuf_list(hyper_dmabuf_id.id,
					 old_hyper_dmabuf_id.id);
	}
	old_hyper_dmabuf_id = hyper_dmabuf_id;
	return 0;
}

static void create_new_buffer_common(int dmabuf_fd)
{
	GLuint textureId[2];
	struct zwp_linux_buffer_params_v1 *params;
	int i;
	int bpp;

	struct timeval start, end;
	if (g_Dbg) {
		gettimeofday( &start, NULL );
		printf("create_new_buffer_common() start:  time stamp=%ld\n", start.tv_sec*1000000+start.tv_usec);
	}

	printf("format:%x tile:%x\n", surf_format, surf_tile_format);
	switch (surf_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		bpp = 32;
		printf("XRGB 8888 and bpp: 32\n");
		break;
	case DRM_FORMAT_RGB565:
		printf("RGB 565 and bpp: 16\n");
		bpp = 16;
		break;
	case DRM_FORMAT_RGB888:
		printf("RGB 888 and bpp: 24\n");
		bpp = 24;
		break;
	case DRM_FORMAT_YUYV:
		printf("video is DRM_FORMAT_YUYV(%x)\n", surf_format);
		bpp = 16;
		break;
	case DRM_FORMAT_NV12:
		printf("video is DRM_FORMAT_NV12(%x)\n", surf_format);
		bpp = 12;
		break;
	default:
		printf("Non supported surface format 0x%x\n", surf_format);
		return;
	}

	if (use_egl) {
		create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
		destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
		image_target_texture_2d =
		    (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if (!using_mesa) {
			surf_tile_format = -1;
		}
		EGLImageKHR khr_image;
		if (surf_format == DRM_FORMAT_NV12) {
			printf("Try to create texture of DRM_FORMAT_NV12\n");
			glGenTextures(2, textureId);
			glBindTexture(GL_TEXTURE_2D, textureId[0]);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					GL_LINEAR);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					GL_LINEAR);
			glBindTexture(GL_TEXTURE_2D, textureId[1]);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					GL_LINEAR);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					GL_LINEAR);

			current_textureId[0] = textureId[0];
			current_textureId[1] = textureId[1];

			dump_dmabuf_data(dmabuf_fd, "/root/nv12", surf_width, surf_height, bpp, 5);

			if(0){
				int bpp = 12;
				char bin_name[100];
				FILE * pfile = NULL;
				size_t len = surf_width * surf_height * bpp /8;
				char *data = malloc(len);
				if ((hyper_dmabuf_id.id & 0x2) == 2) {
					sprintf(bin_name,"%s_%d_%d_%d.bin", "nv12", surf_width, surf_height, bpp);
					import_bo_from_hbuf(hyper_dmabuf_fd, hyper_dmabuf_id, surf_width, surf_height, bpp, data);
					pfile = fopen(bin_name, "wb");
					fwrite(data, 1, len, pfile);
					fclose(pfile);
					free(data);
				}
			}

			EGLint imageAttributes_tex0[] = {
				EGL_WIDTH, surf_width,
				EGL_HEIGHT, surf_height,
				EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
				EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, surf_offset[0],
				EGL_DMA_BUF_PLANE0_PITCH_EXT, surf_stride[0],
				EGL_NONE
			};
			EGLint imageAttributes_tex1[] = {
				EGL_WIDTH, surf_width / 2,
				EGL_HEIGHT, surf_height / 2,
				EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
				EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, surf_offset[1],
				EGL_DMA_BUF_PLANE0_PITCH_EXT, surf_stride[1],
				EGL_NONE
			};

			glBindTexture(GL_TEXTURE_2D, textureId[0]);
			khr_image =
			    create_image((EGLDisplay) g_eman_common.dpy,
					 EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
					 (EGLClientBuffer) NULL,
					 imageAttributes_tex0);

			if (khr_image) {
				printf("Successed: create_image with imageAttributes_tex0\n");
				image_target_texture_2d(GL_TEXTURE_2D,
							khr_image);
				destroy_image(g_eman_common.dpy, khr_image);

				glBindTexture(GL_TEXTURE_2D, textureId[1]);
				khr_image =
				    create_image((EGLDisplay) g_eman_common.dpy,
						 EGL_NO_CONTEXT,
						 EGL_LINUX_DMA_BUF_EXT,
						 (EGLClientBuffer) NULL,
						 imageAttributes_tex1);
				if(!khr_image) {
					printf("Failed: create_image with imageAttributes_tex1\n");
				}else {
					printf("Successed: create_image with imageAttributes_tex1\n");
				}

				image_target_texture_2d(GL_TEXTURE_2D,
							khr_image);
				destroy_image(g_eman_common.dpy, khr_image);
				current_texture_sampler_format =
				    DRM_FORMAT_NV12;
			} else {
				printf("Failed: create_image with imageAttributes_tex0\n");
				EGLint imageAttributes[] = {
					EGL_WIDTH, surf_width,
					EGL_HEIGHT, surf_height,
					EGL_LINUX_DRM_FOURCC_EXT,
					DRM_FORMAT_NV12,
					EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
					EGL_DMA_BUF_PLANE0_OFFSET_EXT,
					surf_offset[0],
					EGL_DMA_BUF_PLANE0_PITCH_EXT,
					surf_stride[0],
					EGL_DMA_BUF_PLANE1_FD_EXT, dmabuf_fd,
					EGL_DMA_BUF_PLANE1_OFFSET_EXT,
					surf_offset[1],
					EGL_DMA_BUF_PLANE1_PITCH_EXT,
					surf_stride[1],
					EGL_NONE
				};

				khr_image =
				    create_image((EGLDisplay) g_eman_common.dpy,
						 EGL_NO_CONTEXT,
						 EGL_LINUX_DMA_BUF_EXT,
						 (EGLClientBuffer) NULL,
						 imageAttributes);

				printf("Trying create_image with imageAttributes(NV12)\n");
				if (khr_image) {

					printf("Successed: create_image with imageAttributes(NV12)\n");
					image_target_texture_2d(GL_TEXTURE_2D,
								khr_image);
					destroy_image(g_eman_common.dpy,
						      khr_image);
					/* UFO will do color conversion on fly in sampler */
					current_texture_sampler_format =
					    DRM_FORMAT_ARGB8888;
				} else {
					printf("DRI not supporing NV12\n");
					return;
				}
			}
		} else {
			glGenTextures(1, textureId);
			glBindTexture(GL_TEXTURE_2D, textureId[0]);
			textureId[1] = 0;
			current_textureId[0] = textureId[0];
			current_textureId[1] = 0;

			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					GL_LINEAR);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					GL_LINEAR);
			if (g_Dbg) {
				printf("Generate Textures: [%x, %x] for hbuf(%x)\n",
					current_textureId[0], current_textureId[1],
					hyper_dmabuf_id.id);
			}

			//dump_dmabuf_data(dmabuf_fd, "/root/bmp", surf_width, surf_height, bpp, 5);

			if(0){
				int bpp = 32;
				char bin_name[100];
				FILE * pfile = NULL;
				size_t len = surf_width * surf_height * bpp /8;
				char *data = malloc(len);
				if ((hyper_dmabuf_id.id & 0x2) == 2) {
					sprintf(bin_name,"%s_%d_%d_%d.bin", "rgb", surf_width, surf_height, bpp);
					import_bo_from_hbuf(hyper_dmabuf_fd, hyper_dmabuf_id, surf_width, surf_height, bpp, data);
					pfile = fopen(bin_name, "wb");
					fwrite(data, 1, len, pfile);
					fclose(pfile);
					free(data);
				}
			}
			EGLint imageAttributes[] = {
				EGL_WIDTH, surf_width,
				EGL_HEIGHT, surf_height,
				EGL_LINUX_DRM_FOURCC_EXT, surf_format,
				EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
				EGL_DMA_BUF_PLANE0_PITCH_EXT, surf_stride[0],
				EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
				fourcc_mod_code(INTEL,
						surf_tile_format) & 0xFFFFFFFF,
				EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
				fourcc_mod_code(INTEL,
						surf_tile_format) >> 32,
				EGL_NONE
			};

			/* In case that DRI does not support natively YUYV import it as ARGB888 */
			EGLint imageAttributesYUYVFallback[] = {
				EGL_WIDTH, surf_width / 2,
				EGL_HEIGHT, surf_height,
				EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
				EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
				EGL_DMA_BUF_PLANE0_PITCH_EXT, surf_stride[0],
				EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
				fourcc_mod_code(INTEL,
						surf_tile_format) & 0xFFFFFFFF,
				EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
				fourcc_mod_code(INTEL,
						surf_tile_format) >> 32,
				EGL_NONE
			};

			khr_image =
			    create_image((EGLDisplay) g_eman_common.dpy,
					 EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
					 (EGLClientBuffer) NULL,
					 imageAttributes);
			/* UFO imports YUYV and on fly is doing color conversion, so in shader such texture behaves like regular RGB texture */
			current_texture_sampler_format = DRM_FORMAT_ARGB8888;
			if (!khr_image) {
				khr_image =
				    create_image((EGLDisplay) g_eman_common.dpy,
						 EGL_NO_CONTEXT,
						 EGL_LINUX_DMA_BUF_EXT,
						 (EGLClientBuffer) NULL,
						 imageAttributesYUYVFallback);
				current_texture_sampler_format =
				    DRM_FORMAT_YUYV;
			}
			image_target_texture_2d(GL_TEXTURE_2D, khr_image);
			destroy_image(g_eman_common.dpy, khr_image);
		}
	} else {
		params = zwp_linux_dmabuf_v1_create_params(wl_dmabuf);

		for (i = 0; i < (surf_format == DRM_FORMAT_NV12 ? 2 : 1); i++) {
			zwp_linux_buffer_params_v1_add(params, dmabuf_fd, i,
						       surf_offset[i],
						       surf_stride[i],
						       fourcc_mod_code(INTEL,
								       surf_tile_format)
						       >> 32,
						       fourcc_mod_code(INTEL,
								       surf_tile_format)
						       & 0xFFFFFFFF);
		}
		buf =
		    zwp_linux_buffer_params_v1_create_immed(params, surf_width,
							    surf_height,
							    surf_format, 0);
		current_buffer = buf;
	}
	update_oldest_rec_hyper_dmabuf(hyper_dmabuf_id.id, textureId, buf,
				       surf_width, surf_height, 0);
	if(g_Dbg) {
		gettimeofday( &end, NULL );
		printf("create_new_buffer_common() done:   time stamp=%ld  duration(ms):%ld\n", end.tv_sec*1000000+end.tv_usec,
				((end.tv_sec*1000000+end.tv_usec)- (start.tv_sec*1000000+start.tv_usec))/1000);
	}


}

void create_new_hyper_dmabuf_buffer(void)
{
	struct ioctl_hyper_dmabuf_export_fd msg;
	msg.fd = -1;

	msg.hid = hyper_dmabuf_id;

	if (ioctl(hyper_dmabuf_fd, IOCTL_HYPER_DMABUF_EXPORT_FD, &msg)) {
		printf("Cannot import buffer %d %s\n", hyper_dmabuf_id.id,
		       strerror(errno));
		show_window = 0;
		hyper_dmabuf_id.id = 0;
		return;
	}

	create_new_buffer_common(msg.fd);

	close(msg.fd);

}

void clear_hyper_dmabuf_list(void)
{
	int i;
	for (i = 0; i < hyper_dmabuf_list.len; i++)
		clear_rec(&hyper_dmabuf_list, i);

}

void received_frames(void)
{
	static uint32_t frames = 0;
	uint32_t benchmark_interval = 5;
	static uint32_t benchmark_time = 0;
	uint32_t time;
	struct timeval tv;
	static int old_counter = 0;

	if (old_counter != counter) {
		gettimeofday(&tv, NULL);
		time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
		if (frames == 0)
			benchmark_time = time;
		if (time - benchmark_time > (benchmark_interval * 1000)) {
			fprintf(stderr, "%d frames in %d seconds: %f fps\n",
			       frames, benchmark_interval,
			       (float)frames / benchmark_interval);
			benchmark_time = time;
			frames = 0;
		}
		frames++;
		old_counter = counter;
	}
}

static int recvfd(int socket)
{
	int len;
	int fd;
	char tmp_buf[1];
	struct iovec iovec;
	struct msghdr hdr;
	struct cmsghdr *cmsg_hdr;
	char msg_buf[CMSG_SPACE(sizeof(int))];

	iovec.iov_base = tmp_buf;
	iovec.iov_len = sizeof(tmp_buf);

	memset(&hdr, 0, sizeof(hdr));

	hdr.msg_iov = &iovec;
	hdr.msg_iovlen = 1;
	hdr.msg_control = (caddr_t) msg_buf;
	hdr.msg_controllen = CMSG_SPACE(sizeof(int));

	len = recvmsg(socket, &hdr, 0);

	if (len < 0) {
		printf("Failed to receive fd\n");
		return -1;
	}

	if (len == 0) {
		printf("No valid fd\n");
		return -1;
	}

	cmsg_hdr = CMSG_FIRSTHDR(&hdr);
	memmove(&fd, CMSG_DATA(cmsg_hdr), sizeof(int));
	return fd;
}

int send_input_event(vmdisplay_socket * socket,
		     struct vmdisplay_input_event_header *header, void *data)
{
	send(socket->socket_fd, header, sizeof(*header), 0);
	return send(socket->socket_fd, data, header->size, 0);
}

int vmdisplay_socket_init(vmdisplay_socket * vmsocket, int domid)
{
	struct sockaddr_un addr;
	struct vmdisplay_msg msg;
	const char *runtime_dir;
	char socket_path[255];
	uint32_t i;

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		printf("XDG_RUNTIME_DIR not set !\n");
		return -1;
	}

	if (!vmsocket)
		return -1;

	if ((vmsocket->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("Cannot create socket\n");
		return -1;
	}

	addr.sun_family = AF_UNIX;

	snprintf(socket_path, 255, "%s/vmdisplay-%d", runtime_dir, domid);
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	if (connect(vmsocket->socket_fd, (struct sockaddr *)&addr, sizeof(addr))
	    == -1) {
		perror("Connect error\n");
		return -1;
	}

	recv(vmsocket->socket_fd, &msg, sizeof(msg), 0);
	for (i = 0; i < msg.display_num; i++) {
		vmsocket->outputs[i].mem_fd = recvfd(vmsocket->socket_fd);
		vmsocket->outputs[i].mem_addr =
		    mmap(NULL, METADATA_BUFFER_SIZE, PROT_READ, MAP_SHARED,
			 vmsocket->outputs[i].mem_fd, 0);
	}

	return 0;
}

void vmdisplay_socket_cleanup(vmdisplay_socket * socket)
{
	int i;
	if (socket) {
		if (socket->socket_fd) {
			close(socket->socket_fd);
			socket->socket_fd = -1;
		}

		for (i = 0; i < VM_MAX_OUTPUTS; i++) {
			if (socket->outputs[i].mem_addr) {
				munmap(socket->outputs[i].mem_addr,
				       METADATA_BUFFER_SIZE);
				socket->outputs[i].mem_addr = NULL;
			}

			if (socket->outputs[i].mem_fd) {
				close(socket->outputs[i].mem_fd);
				socket->outputs[i].mem_fd = -1;
			}
		}
	}
}
