/**
 * @file glx.c
 * @brief GLX video output module for VLC media player
 */
/*****************************************************************************
 * Copyright © 2004 the VideoLAN team
 * Copyright © 2009 Rémi Denis-Courmont
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>

#include <xcb/xcb.h>
#include <X11/Xlib-xcb.h>
#include <GL/glx.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_vout_opengl.h>
#include "../opengl.h"

#include "xcb_vlc.h"

static int  Open (vlc_object_t *);
static void Close (vlc_object_t *);

/*
 * Module descriptor
 */
vlc_module_begin ()
    set_shortname (N_("GLX"))
    set_description (N_("GLX video output (XCB)"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vout display", 20)
    set_callbacks (Open, Close)

    add_shortcut ("xcb-glx")
    add_shortcut ("glx")
vlc_module_end ()

struct vout_display_sys_t
{
    Display *display; /* Xlib instance */
    vout_window_t *embed; /* VLC window (when windowed) */

    xcb_cursor_t cursor; /* blank cursor */
    xcb_window_t window; /* drawable X window */
    bool visible; /* whether to draw */

    GLXContext ctx;
    vout_opengl_t gl;
    vout_display_opengl_t vgl;
    picture_pool_t *pool; /* picture pool */
};

static picture_t *Get (vout_display_t *);
static void PictureRender (vout_display_t *, picture_t *);
static void PictureDisplay (vout_display_t *, picture_t *);
static int Control (vout_display_t *, int, va_list);
static void Manage (vout_display_t *);

static void SwapBuffers (vout_opengl_t *gl);

static vout_window_t *MakeWindow (vout_display_t *vd)
{
    vout_window_cfg_t wnd_cfg;

    memset (&wnd_cfg, 0, sizeof (wnd_cfg));
    wnd_cfg.type = VOUT_WINDOW_TYPE_XID;
    wnd_cfg.width  = vd->cfg->display.width;
    wnd_cfg.height = vd->cfg->display.height;

    vout_window_t *wnd = vout_display_NewWindow (vd, &wnd_cfg);
    if (wnd == NULL)
        msg_Err (vd, "parent window not available");
    return wnd;
}

static const xcb_screen_t *
FindWindow (vout_display_t *vd, xcb_connection_t *conn,
            unsigned *restrict pnum, uint8_t *restrict pdepth)
{
    vout_display_sys_t *sys = vd->sys;

    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply (conn,
            xcb_get_geometry (conn, sys->embed->xid), NULL);
    if (geo == NULL)
    {
        msg_Err (vd, "parent window not valid");
        return NULL;
    }

    xcb_window_t root = geo->root;
    *pdepth = geo->depth;
    free (geo);

    /* Find the selected screen */
    const xcb_setup_t *setup = xcb_get_setup (conn);
    const xcb_screen_t *screen = NULL;
    unsigned num = 0;

    for (xcb_screen_iterator_t i = xcb_setup_roots_iterator (setup);
         i.rem > 0;
         xcb_screen_next (&i))
    {
        if (i.data->root == root)
        {
            screen = i.data;
            break;
        }
        num++;
    }

    if (screen == NULL)
    {
        msg_Err (vd, "parent window screen not found");
        return NULL;
    }
    msg_Dbg (vd, "using screen 0x%"PRIx32 " (number: %u)", root, num);
    *pnum = num;
    return screen;
}

/**
 * Probe the X server.
 */
static int Open (vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = malloc (sizeof (*sys));

    if (sys == NULL)
        return VLC_ENOMEM;

    vd->sys = sys;
    sys->pool = NULL;
    sys->gl.sys = NULL;

    /* Get window */
    sys->embed = MakeWindow (vd);
    if (sys->embed == NULL)
    {
        free (sys);
        return VLC_EGENERIC;
    }

    /* Connect to X server */
    Display *dpy = XOpenDisplay (sys->embed->x11_display);
    if (dpy == NULL)
    {
        vout_display_DeleteWindow (vd, sys->embed);
        free (sys);
        return VLC_EGENERIC;
    }
    sys->display = dpy;
    sys->ctx = NULL;
    XSetEventQueueOwner (dpy, XCBOwnsEventQueue);

    xcb_connection_t *conn = XGetXCBConnection (dpy);
    assert (conn);
    RegisterMouseEvents (obj, conn, sys->embed->xid);

    /* Find window parameters */
    unsigned snum;
    uint8_t depth;
    const xcb_screen_t *scr = FindWindow (vd, conn, &snum, &depth);
    if (scr == NULL)
        goto error;

    /* Determine our pixel format */
    {
        int attr[] = {
            GLX_RGBA,
            GLX_RED_SIZE, 5,
            GLX_GREEN_SIZE, 5,
            GLX_BLUE_SIZE, 5,
            GLX_DOUBLEBUFFER,
            0 };

        XVisualInfo *vi = glXChooseVisual (dpy, snum, attr);
        if (vi == NULL)
        {
            msg_Err (vd, "cannot find GLX 1.2 visual" );
            goto error;
        }

        sys->ctx = glXCreateContext (dpy, vi, 0, True);
        XFree (vi);
        if (sys->ctx == NULL)
        {
            msg_Err (vd, "cannot create GLX context");
            goto error;
        }
    }

    /* Create window */
    unsigned width, height;
    if (GetWindowSize (sys->embed, conn, &width, &height))
        goto error;

    sys->window = xcb_generate_id (conn);
    {
        const uint32_t mask = XCB_CW_EVENT_MASK;
        const uint32_t values[] = {
            /* XCB_CW_EVENT_MASK */
            XCB_EVENT_MASK_VISIBILITY_CHANGE,
        };
        xcb_void_cookie_t c;

        c = xcb_create_window_checked (conn, depth, sys->window,
                                       sys->embed->xid, 0, 0, width, height, 0,
                                       XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                       0, mask, values);
        xcb_map_window (conn, sys->window);

        if (CheckError (vd, conn, "cannot create X11 window", c))
            goto error;
    }
    msg_Dbg (vd, "using X11 window %08"PRIx32, sys->window);

    if (glXMakeCurrent (dpy, sys->window, sys->ctx) == False)
        goto error;

    /* Initialize common OpenGL video display */
    sys->gl.lock = NULL;
    sys->gl.unlock = NULL;
    sys->gl.swap = SwapBuffers;
    sys->gl.sys = sys;

    if (vout_display_opengl_Init (&sys->vgl, &vd->fmt, &sys->gl))
    {
        sys->gl.sys = NULL;
        goto error;
    }

    sys->cursor = CreateBlankCursor (conn, scr);
    sys->visible = false;

    /* */
    vout_display_info_t info = vd->info;
    info.has_pictures_invalid = false;

    /* Setup vout_display_t once everything is fine */
    vd->info = info;

    vd->get = Get;
    vd->prepare = PictureRender;
    vd->display = PictureDisplay;
    vd->control = Control;
    vd->manage = Manage;

    /* */
    vout_display_SendEventFullscreen (vd, false);
    vout_display_SendEventDisplaySize (vd, width, height, false);

    return VLC_SUCCESS;

error:
    Close (obj);
    return VLC_EGENERIC;
}


/**
 * Disconnect from the X server.
 */
static void Close (vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;
    Display *dpy = sys->display;

    if (sys->gl.sys != NULL)
        vout_display_opengl_Clean (&sys->vgl);

    glXMakeCurrent (dpy, 0, NULL);
    if (sys->ctx != NULL)
        glXDestroyContext (dpy, sys->ctx);
    XCloseDisplay (dpy);
    vout_display_DeleteWindow (vd, sys->embed);
    free (sys);
}

static void SwapBuffers (vout_opengl_t *gl)
{
    vout_display_sys_t *sys = gl->sys;

    glXSwapBuffers (sys->display, sys->window);
}

/**
 * Return a direct buffer
 */
static picture_t *Get (vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (!sys->pool)
    {
        sys->pool = vout_display_opengl_GetPool (&sys->vgl);
        if (!sys->pool)
            return NULL;
    }
    return picture_pool_Get (sys->pool);
}

static void PictureRender (vout_display_t *vd, picture_t *pic)
{
   vout_display_sys_t *sys = vd->sys;

    vout_display_opengl_Prepare (&sys->vgl, pic);
}

static void PictureDisplay (vout_display_t *vd, picture_t *pic)
{
    vout_display_sys_t *sys = vd->sys;

    vout_display_opengl_Display (&sys->vgl, &vd->source);
    picture_Release (pic);
}

static int Control (vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query)
    {
    case VOUT_DISPLAY_CHANGE_FULLSCREEN:
    {
        const vout_display_cfg_t *c = va_arg (ap, const vout_display_cfg_t *);
        return vout_window_SetFullScreen (sys->embed, c->is_fullscreen);
    }

    case VOUT_DISPLAY_CHANGE_ON_TOP:
    {
        int b_on_top = (int)va_arg (ap, int);
        return vout_window_SetOnTop (sys->embed, b_on_top);
    }

    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM:
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        msg_Err (vd, "unimplemented control request");
        return VLC_EGENERIC;

    /* Hide the mouse. It will be send when
     * vout_display_t::info.b_hide_mouse is false */
    case VOUT_DISPLAY_HIDE_MOUSE:
        xcb_change_window_attributes (XGetXCBConnection (sys->display),
                                      sys->embed->xid,
                                    XCB_CW_CURSOR, &(uint32_t){ sys->cursor });
        return VLC_SUCCESS;
    case VOUT_DISPLAY_RESET_PICTURES:
        assert (0);
    default:
        msg_Err (vd, "Unknown request in XCB vout display");
        return VLC_EGENERIC;
    }
}

static void Manage (vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    xcb_connection_t *conn = XGetXCBConnection (sys->display);

    ManageEvent (vd, conn, &sys->visible);
}
