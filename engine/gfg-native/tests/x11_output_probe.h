/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional test-only verification of COMPOSITED X11 pixels after Vulkan
 * vkQueuePresentKHR. GPU readback proves image content before WSI; XCB
 * GetImage observes what the display server exposed after presentation.
 *
 * This does not observe a physical Steam Deck OLED or DRM scanout.
 */
#ifndef GFG_X11_OUTPUT_PROBE_H
#define GFG_X11_OUTPUT_PROBE_H
#include <xcb/xcb.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

static inline uint64_t gfg_x11_monotonic_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static inline int gfg_x11_sample_pixel(xcb_connection_t *c,
             xcb_drawable_t window,int16_t x,int16_t y,int *value) {
    xcb_get_image_cookie_t cookie=xcb_get_image(c,XCB_IMAGE_FORMAT_Z_PIXMAP,
                                               window,x,y,1,1,UINT32_MAX);
    xcb_generic_error_t *err=NULL;
    xcb_get_image_reply_t *reply=xcb_get_image_reply(c,cookie,&err);
    if(err)free(err);
    if(!reply)return 0;
    const int n=xcb_get_image_data_length(reply);
    const uint8_t *data=xcb_get_image_data(reply);
    /* The test paints grayscale, so B/G/R memory order does not matter. */
    int ok=n>=3;
    if(ok) {
        if(abs((int)data[0]-(int)data[1])>4 ||
           abs((int)data[1]-(int)data[2])>4)ok=0;
        *value=(int)data[0];
    }
    free(reply);
    return ok;
}
static inline int gfg_x11_wait_visible_pair(xcb_connection_t *c,
             xcb_drawable_t window,uint32_t width,uint32_t height,
             int expected_left,int expected_right,uint64_t timeout_ns,
             uint64_t *observed_after_ns) {
    const uint64_t start=gfg_x11_monotonic_ns();
    int left=-1,right=-1;
    const struct timespec delay={.tv_sec=0,.tv_nsec=2000000L};
    while(gfg_x11_monotonic_ns()-start<timeout_ns) {
        if(gfg_x11_sample_pixel(c,window,(int16_t)(width/4),
                        (int16_t)(height/2),&left) &&
           gfg_x11_sample_pixel(c,window,(int16_t)(3*width/4),
                        (int16_t)(height/2),&right) &&
           abs(left-expected_left)<=5 && abs(right-expected_right)<=5) {
            if(observed_after_ns)*observed_after_ns=
                gfg_x11_monotonic_ns()-start;
            return 1;
        }
        nanosleep(&delay,NULL);
    }
    fprintf(stderr,"GSF X11 output-pixels TIMEOUT left=%d/%d right=%d/%d\n",
        left,expected_left,right,expected_right);
    return 0;
}
#endif
