/* SPDX-License-Identifier: GPL-3.0-or-later
 * CI-only minimal X11 WM: delay real ConfigureRequest handling. This models
 * the asynchronous WM round trip missing from bare Xvfb; not a KDE emulator.
 */
#define _POSIX_C_SOURCE 200809L
#include <xcb/xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    int screen=0;
    xcb_connection_t *xc=xcb_connect(NULL,&screen);
    if(!xc || xcb_connection_has_error(xc))return 1;
    xcb_screen_iterator_t it=xcb_setup_roots_iterator(xcb_get_setup(xc));
    while(screen--)xcb_screen_next(&it);
    const uint32_t mask=XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_generic_error_t *error=xcb_request_check(xc,
        xcb_change_window_attributes_checked(xc,it.data->root,XCB_CW_EVENT_MASK,&mask));
    if(error){free(error);xcb_disconnect(xc);return 2;}
    puts("GSF_DELAYED_WM_READY delay_ms=100");fflush(stdout);
    xcb_generic_event_t *event;
    while((event=xcb_wait_for_event(xc))) {
        const unsigned type=event->response_type & 127u;
        if(type==XCB_MAP_REQUEST) {
            xcb_map_window(xc,((xcb_map_request_event_t *)event)->window);
        } else if(type==XCB_CONFIGURE_REQUEST) {
            const xcb_configure_request_event_t *r=(const void *)event;
            uint32_t values[7];unsigned n=0;
#define ADD(bit,field) if(r->value_mask & bit)values[n++]=r->field
            ADD(XCB_CONFIG_WINDOW_X,x);ADD(XCB_CONFIG_WINDOW_Y,y);
            ADD(XCB_CONFIG_WINDOW_WIDTH,width);ADD(XCB_CONFIG_WINDOW_HEIGHT,height);
            ADD(XCB_CONFIG_WINDOW_BORDER_WIDTH,border_width);
            ADD(XCB_CONFIG_WINDOW_SIBLING,sibling);ADD(XCB_CONFIG_WINDOW_STACK_MODE,stack_mode);
#undef ADD
            const struct timespec delay={.tv_nsec=100000000};nanosleep(&delay,NULL);
            xcb_configure_window(xc,r->window,r->value_mask,values);
            printf("GSF_DELAYED_WM_CONFIGURE width=%u height=%u mask=%u\n",r->width,r->height,r->value_mask);
            fflush(stdout);
        }
        xcb_flush(xc);free(event);
    }
    xcb_disconnect(xc);return 0;
}
