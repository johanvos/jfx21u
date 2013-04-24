/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
 
#include "input/LensInput.h"
#include "wm/LensWindowManager.h"
#include "com_sun_glass_events_ViewEvent.h"
#include "com_sun_glass_events_WindowEvent.h"
#include "com_sun_glass_events_MouseEvent.h"
#include "com_sun_glass_events_TouchEvent.h"
#include "lensRFB/lensRFB.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t renderMutex = PTHREAD_MUTEX_INITIALIZER;

static int _mousePosX;
static int _mousePosY;

static jboolean _mousePressed = JNI_FALSE;
static jboolean _onDraggingAction = JNI_FALSE;
static NativeWindow _dragGrabbingWindow = NULL;

static jboolean isDnDStarted = JNI_FALSE;

static inline void render_lock() {
    pthread_mutex_lock(&renderMutex);
}

static inline void render_unlock() {
    pthread_mutex_unlock(&renderMutex);
}


static NativeScreen mainScreen;

static void lens_wm_rfbNotifyClearScreen();
static void lens_wm_clearScreen();
static void lens_wm_initRFB(JNIEnv *env);
static void lens_wm_rfbNotifyWindowUpdate(NativeWindow window,
                                          int width, int height);
static void lens_wm_windowCacheBounds(NativeWindow window);
static void lens_wm_windowUncacheBounds(NativeWindow window);
NativeWindow lens_wm_unsetFocusedWindow(JNIEnv *env, NativeWindow window);

//service functions to handle window state
static void lens_wm_windowMinimize(JNIEnv *env,
                                   NativeWindow window);
static void lens_wm_windowRestore(JNIEnv *env,
                                  NativeWindow window);
static void lens_wm_windowMaximize(JNIEnv *env,
                                   NativeWindow window);
static void lens_wm_windowEnterFullscreen(JNIEnv *env,
                                          NativeWindow window);

jboolean lens_wm_initialize(JNIEnv *env) {

    jboolean result;

    GLASS_LOG_FINE("Init device");
    result = glass_application_initialize(env);
    if (result) {
        GLASS_LOG_FINE("Init screen");
        mainScreen = lens_screen_initialize(env);
        if (mainScreen) {
            GLASS_LOG_FINE("Clearing screen");
            lens_wm_clearScreen();

            GLASS_LOG_FINE("Cursor init");
            fbCursorInitialize();

            lens_wm_initRFB(env);
            GLASS_LOG_FINE("Init input devices");
            result =  lens_input_initialize(env);
            if (!result) {
                GLASS_LOG_SEVERE("lens_input_initialize failed");
            }
        } else {
            GLASS_LOG_SEVERE("lens_screen_initialize() failed");
            result = JNI_FALSE;
        }
    } else {
        GLASS_LOG_SEVERE("glass_application_initialize() failed");
    }

    return result;
}

NativeScreen glass_screen_getMainScreen() {
    return mainScreen;
}

void lens_wm_getPointerPosition(int *pX, int *pY) {
    *pX = _mousePosX;
    *pY = _mousePosY;
}

void lens_wm_setPointerPosition(int x, int y) {
    _mousePosX = x;
    _mousePosY = y;
    fbCursorSetPosition(_mousePosX, _mousePosY);
}

LensResult lens_wm_notifyPlatformWindowRelease(JNIEnv *env, NativeWindow window) {

    GLASS_LOG_FINE("WM Window Relase window [%i]%p", window->id, window);

    if (window == lens_wm_getMouseWindow()) {
        // allow the next mouse motion to generate the ENTER
        lens_wm_setMouseWindow(NULL);
    }
    if (window == lens_wm_getGrabbedWindow()) {

        lens_wm_setGrabbedWindow(NULL); // don't bother with an event
    }
    if (window == glass_window_getFocusedWindow()) {
        glass_window_setFocusedWindow(NULL);
    }

    NativeWindow head = glass_window_list_getHead();
    if (head && head->view) {
        lens_wm_repaint(env, head);
    }

    return LENS_OK;
}

void lens_wm_repaint(JNIEnv *env, NativeWindow window) {
    render_lock();

    // remember clear could actually write pixels...
    lens_wm_clearScreen();

    if (window && window->view) {
        glass_application_notifyViewEvent(env,
                                          window->view,
                                          com_sun_glass_events_ViewEvent_REPAINT,
                                          window->currentBounds.x, window->currentBounds.y,
                                          window->currentBounds.width, window->currentBounds.height);
    }

    render_unlock();
}

//////////////////////////// WINDOW STATE MACHINE //////////////////////
static void lens_wm_windowMinimize(JNIEnv *env,
                                   NativeWindow window) {


    //"undo" previous state, if needed
    switch (window->state) {
        case NWS_MINIMIZED:
            GLASS_LOG_FINE("Nothing to do, skipping");
            return;
        case NWS_NORMAL:            
        case NWS_MAXIMIZED:
            //NOOP
            break;
        case NWS_FULLSCREEN:
            lens_wm_windowRestore(env, window);
            break;
        default:
            GLASS_LOG_SEVERE("Window is in unsupported NativeWindowState (%i)",
                              window->state);
    }

    //cache window bounds for restoration
    lens_wm_windowCacheBounds(window);

    //if supported let platform do the minimization 
    lens_platform_windowMinimize(env, window, JNI_TRUE);

    //update state
    window->state = NWS_MINIMIZED;

    //if window hold the focus, release it
    lens_wm_unsetFocusedWindow(env, window);


    //Stop rendering this window, because its minimized
    glass_application_RemoveWindowFromVisibleWindowList(env,window);

    //notify
    glass_application_notifyWindowEvent_resize(env,
                                               window,
                                               com_sun_glass_events_WindowEvent_MINIMIZE,
                                               window->cachedBounds.width,
                                               window->cachedBounds.height);
}


static void lens_wm_windowRestore(JNIEnv *env,
                                  NativeWindow window){

        
    //"undo" previous state, if needed
    switch (window->state) {
        case NWS_MINIMIZED:
            GLASS_LOG_FINE("Window is minimized -notifying platform minimize(false)");

            //notify platform
            lens_platform_windowMinimize(env, window, JNI_FALSE);
            if (window->isVisible) {
                //the window is restored and visible, add it to the window list
                //to resume rendering
                glass_application_addWindowToVisibleWindowList(env,window);
            }
            break;

        case NWS_NORMAL:
            GLASS_LOG_FINE("Nothing to do, skipping");
            return;
        case NWS_MAXIMIZED:
            //NOOP
            break;
        case NWS_FULLSCREEN:
            GLASS_LOG_FINE("Window in full screen notify FULLSCREEN_EXIT"
                           " (x=%i, y=%i, w=%i, h=%i)",
                           window->cachedBounds.x,
                           window->cachedBounds.y,
                           window->cachedBounds.width,
                           window->cachedBounds.height);

            //notify view it has existed full screen
            glass_application_notifyViewEvent(env,
                                              window->view,
                                              com_sun_glass_events_ViewEvent_FULLSCREEN_EXIT,
                                              window->cachedBounds.x,    
                                              window->cachedBounds.y,    
                                              window->cachedBounds.width,
                                              window->cachedBounds.height);
            break;
        default:
            GLASS_LOG_SEVERE("Window is in unsupported NativeWindowState (%i)",
                              window->state);
    }    

    //update state
    window->state = NWS_NORMAL;        
    

    //resize and relocate window to previous bounds
    glass_window_setBoundsImpl(env,
                               window,
                               window->cachedBounds.x,    
                               window->cachedBounds.y,    
                               window->cachedBounds.width,
                               window->cachedBounds.height,
                               JNI_TRUE,
                               JNI_TRUE,
                               JNI_FALSE);

    //restore bounds
    lens_wm_windowUncacheBounds(window);

    GLASS_LOG_FINE("notify window it has been restored");
    glass_application_notifyWindowEvent_resize(env,
                                               window,
                                               com_sun_glass_events_WindowEvent_RESTORE,
                                               window->currentBounds.width,
                                               window->currentBounds.height);

    GLASS_LOG_FINE("make sure window has the focus");
    lens_wm_setFocusedWindow(env, window);
}

static void lens_wm_windowMaximize(JNIEnv *env,
                                   NativeWindow window){

    //"undo" previous state, if needed
    switch (window->state) {
        case NWS_MINIMIZED:
            lens_wm_windowRestore(env, window);
            break;
        case NWS_NORMAL:
            //NOOP
            break;
        case NWS_MAXIMIZED:
            GLASS_LOG_FINE("Nothing to do, skipping");
            return;
        case NWS_FULLSCREEN:
            lens_wm_windowRestore(env, window);
            break;
        default:
            GLASS_LOG_SEVERE("Window is in unsupported NativeWindowState (%i)",
                              window->state);
    }

    /**
    * Window's max size can be limited, so try to extend the window 
    * to the buttom right corner of the screen from the current x,y 
    * coordinates. If the window will be extended beyond the screen 
    * boundaries, push the window towards the top left corner of the
    * screen. If no limits applied to the window it will capture the 
    * entire screen. 
    */

    //cache current window bounds for restoration
    lens_wm_windowCacheBounds(window);

    //get screen size
    NativeScreen screen = glass_screen_getMainScreen();
    int width = screen->width;
    int height = screen->height;
    int x = window->currentBounds.x;
    int y = window->currentBounds.y;    

    //check if window can occupy the entire screen
    if (glass_window_check_bounds(window, &width, &height)) {
        //window can be fully maximized, so we need to move it to
        //the top left corner
        x = 0;
        y = 0;
    } else {
       //window is restricted, check if new bounds are bigger
       //from current bounds
       if (width > window->currentBounds.width || 
           height > window->currentBounds.height){
           //calculate new x,y
           x = screen->width - width -1;
           y = screen->height - height -1;

       } 
    }

    GLASS_LOG_FINE("Maximized window bounds x=%i, y=%i, width =%i, height=%i",
                   x, y, width, height);

    //notify for bounds update
    glass_window_setBoundsImpl(env, window,
                               x, y,width, height,
                               JNI_TRUE, //update location
                               JNI_TRUE, // update size
                               JNI_FALSE /* update content */);

    //update state
    window->state = NWS_MAXIMIZED;

    //notify
    glass_application_notifyWindowEvent_resize(env,
                                               window,
                                               com_sun_glass_events_WindowEvent_MAXIMIZE,
                                               width,
                                               height);
    //make sure window has the focus
    lens_wm_setFocusedWindow(env, window);

}
static void lens_wm_windowEnterFullscreen(JNIEnv *env,
                                          NativeWindow window){
    //"undo" previous state, if needed
    switch (window->state) {
        case NWS_MINIMIZED:
            GLASS_LOG_FINE("Window is minimized - restoring");
            lens_wm_windowRestore(env, window);
            break;
        case NWS_NORMAL:
        case NWS_MAXIMIZED:
            //NOOP
            break;
        case NWS_FULLSCREEN:
            GLASS_LOG_FINE("Nothing to do, skipping");
            return;
        default:
            GLASS_LOG_SEVERE("Window is in unsupported NativeWindowState (%i)",
                              window->state);
    }

    //get screen
    NativeScreen screen = glass_screen_getMainScreen();

    //cache current window bounds for restoration
    lens_wm_windowCacheBounds(window);




    //set full screen dimensions 
    glass_window_setBoundsImpl(env,window,
                               0, 0,
                               screen->width,
                               screen->height,
                               JNI_TRUE, // update position
                               JNI_TRUE, // update size
                               JNI_FALSE); // update content

    GLASS_LOG_FINE("Notifying FULLSCREEN_ENTER on view[%p] window %i[%p]"
                   " x=%i, y=%i, w=%i, h=%i",
                   window->view,
                   window->id,
                   window,
                   window->currentBounds.x,
                   window->currentBounds.y,
                   window->currentBounds.width,
                   window->currentBounds.height);

    //notify view  
    glass_application_notifyViewEvent(env,
                                      window->view,
                                      com_sun_glass_events_ViewEvent_FULLSCREEN_ENTER,
                                      window->currentBounds.x,
                                      window->currentBounds.y,
                                      window->currentBounds.width,
                                      window->currentBounds.height);

    //make sure window has the focus
    lens_wm_setFocusedWindow(env, window);

    window->state = NWS_FULLSCREEN;


}


void glass_window_setBoundsImpl(JNIEnv *env,
                                NativeWindow window,
                                jint x, jint y, jint width, jint height,
                                jboolean needToUpdatePostion,
                                jboolean needToUpdateSize,
                                jboolean isContentSize) {


    jboolean windowHasBeenUpdated = JNI_FALSE;

    GLASS_LOG_FINE("setBoundsImpl on window %i[%p] x=%i y=%i w=%i h=%i"
                   " needToUpdatePostion=%s needToUpdateSize=%s isContentSize=%s"
                   " statet=%s",
                   window->id, window,
                   x, y, width, height,
                   (needToUpdatePostion)?"true":"false",
                   (needToUpdateSize)?"true":"false",
                   (isContentSize)?"true":"false",
                   lens_window_getNativeStateName(window->state));

    if (isContentSize && !needToUpdateSize) {
        GLASS_LOG_FINE("Treating content size change as window size change");
        needToUpdateSize = isContentSize;
    }

    GLASS_LOG_FINER("currentW(%i) != newW(%i) || currentH(%i)!=newH(%i)",
                    window->currentBounds.width,
                    width,
                    window->currentBounds.height,
                    height );
    //handle resize if needed
    if (needToUpdateSize &&
        (window->currentBounds.width != width || window->currentBounds.height != height)) {

        GLASS_LOG_FINE("Updatating window %i[%p] size from %iX%i to %iX%i",
                       window->id,
                       window,
                       window->currentBounds.width,
                       window->currentBounds.height,
                       width,
                       height);

        window->currentBounds.width = width;
        window->currentBounds.height = height;

        glass_application_notifyWindowEvent_resize(env,window,
                                                   com_sun_glass_events_WindowEvent_RESIZE,
                                                   width,
                                                   height);
        
        windowHasBeenUpdated = JNI_TRUE;

    }

    GLASS_LOG_FINER("curentX(%i) != newX(%i) || currentY(%i)!=newY(%i)",
                    window->currentBounds.x,
                    x,
                    window->currentBounds.y,
                    y);
    //handle move if needed
    if (needToUpdatePostion && 
        (window->currentBounds.x != x || window->currentBounds.y != y)) {
        GLASS_LOG_FINE("Updatating window %i[%p] locatoin from %iX%i to %iX%i",
                       window->id,
                       window,
                       window->currentBounds.x,
                       window->currentBounds.y,
                       x,
                       y);
        window->currentBounds.x = x;
        window->currentBounds.y = y;

        glass_application_notifyWindowEvent_move(env,
                                                 window,
                                                 x,
                                                 y);

        windowHasBeenUpdated = JNI_TRUE;
    }

    if (!windowHasBeenUpdated) {
        //if function got the same sizes as the current ones, it means that
        //window have been restored.
        //happens when swapping stages with same params, for example.
        GLASS_LOG_FINE("Notifying window restore");
        glass_application_notifyWindowEvent_resize(env,window,
                                                   com_sun_glass_events_WindowEvent_RESTORE,
                                                   window->currentBounds.width,
                                                   window->currentBounds.height);
    }

    //some time the order of the window rendering concluded by the events is wrong.
    //so force repaint.
   lens_wm_repaint(env, window);
}


jboolean glass_window_setVisible(JNIEnv *env, NativeWindow window, jboolean visible) {

    GLASS_LOG_FINE("Setting window %i[%p](owner %i[%p]) from %s, to %s",
                   window->id,
                   window,
                   window->owner? window->owner->id : -1,
                   window->owner,
                   (window->isVisible)?"visible":"invisible",
                   (visible)?"visible":"invisible");


    lens_platform_windowSetVisible(env, window, visible);

    window->isVisible = visible;

    if (!visible) {
        //lose focus and grab
        lens_wm_unsetFocusedWindow(env, window);        
    } else {
        if (!window->owner) {
            //window become visible, grant it the focus if not a pop-up
            lens_wm_setFocusedWindow(env, window);
        }

        GLASS_LOG_FINE("notify window it has been restored");
        glass_application_notifyWindowEvent_resize(env,
                                                   window,
                                                   com_sun_glass_events_WindowEvent_RESTORE,
                                                   window->currentBounds.width,
                                                   window->currentBounds.height);
    }

    return JNI_TRUE;
}

jboolean glass_view_drawBegin(NativeView view) {
    GLASS_LOG_FINER("glass_view_drawBegin");
    render_lock();
    return JNI_TRUE;
}

void glass_view_drawEnd(NativeView view) {
    GLASS_LOG_FINER("glass_view_drawEnd");
    render_unlock();
}

jboolean glass_window_requestFocus(JNIEnv *env, NativeWindow window, jint focusType) {

    jboolean result;

    NativeWindow focusWindow;

    if (lens_wm_getGrabbedWindow()) {
        // no changing focus in a grab
        return JNI_FALSE;
    }

    focusWindow = glass_window_getFocusedWindow();

    if (!window) {
        GLASS_LOG_WARNING("null window passes the glass_window_requestFocus");
        return JNI_FALSE;
    }

    if (window == focusWindow) {
        // no change, no notification ?
        GLASS_LOG_WARNING("Focus requested on current focus window");
        return JNI_TRUE;
    }

    if (!window->isFocusable) {
        GLASS_LOG_WARNING("Focus requested on isFocusable=false");
        return JNI_FALSE;
    }

    if (!window->isEnabled) {
        GLASS_LOG_WARNING("Focus requested on isEnabled=false");
        return JNI_FALSE;
    }

    lens_wm_setFocusedWindow(env, window);

    return JNI_TRUE;
}

jboolean glass_window_setFocusable(JNIEnv *env,
                                   NativeWindow window,
                                   jboolean isFocusable) {

    NativeWindow focusWindow;

    if (window->isFocusable == isFocusable) {
        // no change, so we can punt
        return JNI_TRUE;
    }

    focusWindow = glass_window_getFocusedWindow();
    if (!isFocusable && focusWindow == window) {
        lens_wm_setFocusedWindow(env, NULL);
        GLASS_LOG_WARNING("isFocusable(false) on focus owner, cascade ?");
    }

    window->isFocusable = isFocusable;

    return JNI_TRUE;
}

jboolean glass_window_setBackground(NativeWindow window,
                                    jfloat red,
                                    jfloat green,
                                    jfloat blue) {
    GLASS_LOG_WARNING("unimplemented glass_window_setBackground\n");
    return JNI_TRUE;
}

void glass_window_toFront(JNIEnv *env, NativeWindow window) {
    if (glass_window_list_toFront(window)) {
        lens_wm_repaint(env, window);
    }
}

void glass_window_toBack(JNIEnv *env, NativeWindow window) {
    if (glass_window_list_toBack(window)) {
        lens_wm_repaint(env, window);
    }
}

jboolean glass_window_grabFocus(JNIEnv *env, NativeWindow window) {

    if (window == lens_wm_getGrabbedWindow()) {
        //this is OK per spec
        GLASS_LOG_FINE("RE-GRAB on %p root %p\n", window, window->root);
        return JNI_TRUE;
    }
    
    if (NULL == lens_wm_getGrabbedWindow() &&
            window == glass_window_getFocusedWindow()) {
        // we allow the grab, note: focus is also checked in Java.
        GLASS_LOG_FINE("GRAB on %p root %p\n", window, window->root);
        lens_wm_setGrabbedWindow(window);
        return JNI_TRUE;
    }

    // should not be able to happen
    GLASS_LOG_FINE("ERROR NO-GRAB on %p\n", window);
    return JNI_FALSE;
}

/**
 * This functions will check if the given window is grabbed and
 * ungrab it if necessary. Note: may also be called from mouse 
 * handling 
 */
void glass_window_ungrabFocus(JNIEnv *env, NativeWindow window) {

    if (window == NULL) {
        GLASS_LOG_FINER("window=NULL - Nothing to do");
        return;
    }

    if (window != lens_wm_getGrabbedWindow()) {
        GLASS_LOG_FINE("Given window is not grabbed, ignore");
        return;
    }

    GLASS_LOG_FINE("Ungrabbing window %i[%p]",
                   window?window->id : -1,
                   window);

    lens_wm_setGrabbedWindow(NULL);

    //notify the UNGRAB
    glass_application_notifyWindowEvent(env,
                                        window,
                                        com_sun_glass_events_WindowEvent_FOCUS_UNGRAB);
}

void glass_view_setParent(JNIEnv *env,
                          NativeWindow parent,
                          NativeView view) {
    NativeWindow oldParent = view->parent;

    if (oldParent && oldParent->view) {
        GLASS_LOG_FINE("Notifying old view removed");
        glass_application_notifyViewEvent(env,
                                          oldParent->view,
                                          com_sun_glass_events_ViewEvent_REMOVE,
                                          0, 0, 0, 0);
        view->parent = NULL;
    }

    GLASS_LOG_FINE("Setting new owner, window %d [%p], for view %p",
                   parent ? (signed int)parent->id : - 1,
                   parent,
                   view);
    view->parent = parent; //may be null

    if (parent && parent->view) {
        GLASS_LOG_FINE("Notifying view it has been added %p", parent->view);
        glass_application_notifyViewEvent(env,
                                          parent->view,
                                          com_sun_glass_events_ViewEvent_ADD,
                                          0, 0, 0, 0);
    }
}

void lens_wm_shutdown(JNIEnv *env) {
    lens_platform_shutdown(env);
}

jboolean glass_window_setLevel(NativeWindow window, int level) {
    GLASS_LOG_WARNING("unimplemented glass_window_setLevel\n");
    return JNI_TRUE;
}

jboolean glass_window_setMinimumSize(JNIEnv *env,
                                     NativeWindow window,
                                     jint width, jint height) {
    window->minWidth  = width;
    window->minHeight = height;

    width = window->currentBounds.width;
    height = window->currentBounds.height;

    glass_window_check_bounds(window, &width, &height);

    if (width != window->currentBounds.width ||
            height != window->currentBounds.height) {
        glass_window_setBoundsImpl(env,
                                   window,
                                   0, 0, width, height,
                                   JNI_FALSE,  // position
                                   JNI_TRUE,   // size
                                   JNI_FALSE); // contentSize
    }

    return JNI_TRUE;
}

jboolean glass_window_setMaximumSize(JNIEnv *env,
                                     NativeWindow window,
                                     jint width, jint height) {
    window->maxWidth  = width;
    window->maxHeight = height;

    width = window->currentBounds.width;
    height = window->currentBounds.height;

    glass_window_check_bounds(window, &width, &height);

    if (width != window->currentBounds.width ||
            height != window->currentBounds.height) {
        glass_window_setBoundsImpl(env,
                                   window,
                                   0, 0, width, height,
                                   JNI_FALSE, // position
                                   JNI_TRUE,  // size
                                   JNI_FALSE);// contentSize
    }

    return JNI_TRUE;
}

jboolean glass_view_enterFullscreen(JNIEnv *env,
                                    NativeView view,
                                    jboolean animate,
                                    jboolean keepRatio,
                                    jboolean hideCursor) {

    NativeWindow window = view->parent;

    if (window == NULL) {
        GLASS_LOG_WARNING("Full screen request on a view(%p) with no parent window, abort",
                          view);
        return JNI_FALSE;
    }

    GLASS_LOG_FINE("Enter full screen request on view %p, window %i[%p]",
                   view, window->id, window);

   /**
    * animate, keepRatio ration and hideCursor are currently stubbed
    * to false in WindowStage.java, which is the only caller for 
    * this API. 
    * Ignoring them for now 
    */
    lens_wm_windowEnterFullscreen(env, window);

    return JNI_TRUE;

}

jboolean glass_view_exitFullscreen(JNIEnv *env,
                                   NativeView view,
                                   jboolean animate) {

    NativeWindow window = view->parent;

    if (window == NULL) {
        GLASS_LOG_WARNING("Exit full screen request on a view(%p) with no parent"
                          " window, abort",
                          view);
        return JNI_FALSE;
    }

    GLASS_LOG_FINE("Exit full screen request on view %p, window %i[%p]",
                   view, window->id, window);

    /**
    * WindowStage.applyFullScreen() always sets the animate 
    * parameter to false when calling enterFullScreen on its View, 
    * in WindowStage.java, which is the only caller for this API. 
    * Ignoring it for now.
    */

    lens_wm_windowRestore(env, window);

    return JNI_TRUE;
}

jboolean glass_window_minimize(JNIEnv *env,
                               NativeWindow window,
                               jboolean toMinimize) {
    
    GLASS_LOG_FINE("Minimize window %i[%p] toMinimize=%s",
                   window->id,
                   window,
                   (toMinimize)?"true":"false");

    if (toMinimize) {
        lens_wm_windowMinimize(env, window);
    } else {
        lens_wm_windowRestore(env, window);
    }
    
    return JNI_TRUE;

}

jboolean glass_window_maximize(JNIEnv *env,
                               NativeWindow window,
                               jboolean toMaximize,
                               jboolean isMaximized) {
    GLASS_LOG_FINE("Maximize window %i[%p] toMaximize=%s isMaximized=%s",
                   window->id,
                   window,
                   (toMaximize)?"true":"false",
                   (isMaximized)?"true":"false");
    jboolean result = JNI_TRUE;

    if (toMaximize && !isMaximized) {
        lens_wm_windowMaximize(env, window);
    } else if (!toMaximize && isMaximized) {
        lens_wm_windowRestore(env, window);
    } else {
        GLASS_LOG_WARNING("Maximize request with bad arguments");
        result = JNI_FALSE;
    }
    return result;
}

NativeWindow glass_window_findWindowAtLocation(int absX, int absY,
                                               int *pRelX, int *pRelY) {

    NativeWindow w = glass_window_list_getTail();
    while (w) {
        GLASS_LOG_FINEST("Window %d[%p] isVisible=%s, state=%s",
                       w->id, w,
                       w->isVisible?"true" : "false",
                       lens_window_getNativeStateName(w->state));
        if (w->isVisible && w->state != NWS_MINIMIZED) {
            if (absX >= w->currentBounds.x &&
                    absX < w->currentBounds.x + w->currentBounds.width &&
                    absY >= w->currentBounds.y &&
                    absY < w->currentBounds.y + w->currentBounds.height &&
                    w->isEnabled) {

                *pRelX = absX - w->currentBounds.x;
                *pRelY = absY - w->currentBounds.y;
                GLASS_LOG_FINER(
                    "Absolute coordinates %i,%i are on window %i[%p] "
                    "as relative coordinates %i,%i",
                    absX, absY, w->id, w, *pRelX, *pRelY);
                return w;
            }
        } else {
            GLASS_LOG_FINER("Skipping invisible window %i[%p]",
                           w->id, w);
        }
        w = w->previousWindow;
    }
    GLASS_LOG_FINER("Absolute coordinates %i,%i are not on a window",
                    absX, absY);
    return NULL;
}

NativeWindow grabbedWindow = NULL;

NativeWindow lens_wm_getGrabbedWindow() {
    return grabbedWindow;
}

void lens_wm_setGrabbedWindow(NativeWindow window) {
    grabbedWindow = window;
}


static void handleClickOrTouchEvent(JNIEnv *env, int xabs, int yabs) {

    int relX, relY;
    NativeWindow window = glass_window_findWindowAtLocation(xabs, yabs,
                          &relX, &relY);

    // if we have a grabbed window, check to see if this breaks the grab
    if (grabbedWindow != NULL) {
        if ((window == NULL) ||
                (window->root != grabbedWindow->root)) {
            glass_window_ungrabFocus(env, grabbedWindow);
        }
    }

    if (window != NULL) {
        NativeWindow focusedWindow = glass_window_getFocusedWindow();
        // Will this cause a focus change ?
        if (focusedWindow && window->root != focusedWindow->root) {
            lens_wm_setFocusedWindow(env, window);
        }
    }
}

void lens_wm_notifyScrollEvent(JNIEnv *env, int xabs, int yabs, int step) {

    int relX, relY;
    NativeWindow window = glass_window_findWindowAtLocation(xabs, yabs,
                          &relX, &relY);
    if (window != NULL) {
        glass_application_notifyScrollEvent(env, window, relX, relY,
                                            xabs, yabs, 0.0, step);
    }

}

// check for window grab then forward event to application.
// check for focus changes and handle them.
void lens_wm_notifyButtonEvent(JNIEnv *env,
                               jboolean pressed,
                               int button,
                               int xabs, int yabs) {

    int relX, relY;
    NativeWindow window;

    //cache new coordinates
    _mousePosX = xabs;
    _mousePosY = yabs;

    window = glass_window_findWindowAtLocation(xabs, yabs,
                          &relX, &relY);

    _mousePressed = pressed;

    if (_onDraggingAction) {
        if (pressed) {
            GLASS_LOG_SEVERE("Press event while on drag !");
        }

        if (_dragGrabbingWindow != NULL) {

            relX = xabs - _dragGrabbingWindow->currentBounds.x;
            relY = yabs - _dragGrabbingWindow->currentBounds.y;
            glass_application_notifyMouseEvent(env,
                                               _dragGrabbingWindow,
                                               com_sun_glass_events_MouseEvent_UP,
                                               relX, relY, xabs, yabs,
                                               button);

        }

        _onDraggingAction = JNI_FALSE;
        _dragGrabbingWindow = NULL;

    } else {
        if (window != NULL) {
            GLASS_LOG_FINEST("glass_wm_notifyButtonEvent sending to  %p pressed=%d, button=%d  %d,%d, %d, %d ",
                             window,
                             pressed, button,
                             relX, relY, xabs, yabs);

            // pass on the event to Java.
            glass_application_notifyMouseEvent(env,
                                               window,
                                               pressed ? com_sun_glass_events_MouseEvent_DOWN :
                                               com_sun_glass_events_MouseEvent_UP,
                                               relX, relY, xabs, yabs,
                                               button);
        }
    }

    handleClickOrTouchEvent(env, xabs, yabs);

}


// check for window grab then forward event to application.
// check for focus changes and handle them.
void lens_wm_notifyTouchEvent(JNIEnv *env,
                              jint state,
                              int id,
                              int xabs, int yabs) {

    int relX, relY;
    NativeWindow window;

    //cache new coordinates
    _mousePosX = xabs;
    _mousePosY = yabs;

    window = glass_window_findWindowAtLocation(xabs, yabs,
                          &relX, &relY);

    lens_wm_setMouseWindow(window);

    if (state == com_sun_glass_events_TouchEvent_TOUCH_PRESSED) {
        _mousePressed = JNI_TRUE;
    } else if (state == com_sun_glass_events_TouchEvent_TOUCH_RELEASED) {
        _mousePressed = JNI_FALSE;
    } else {
        GLASS_LOG_SEVERE("Unexpected state %d", state);
    }

    if (_mousePressed && window) {
        // Pressed on window
        glass_application_notifyMouseEvent(env,
                                           window,
                                           com_sun_glass_events_MouseEvent_ENTER,
                                           relX, relY, xabs, yabs,
                                           com_sun_glass_events_MouseEvent_BUTTON_NONE);
        glass_application_notifyTouchEvent(env,
                                           window,
                                           com_sun_glass_events_TouchEvent_TOUCH_PRESSED,
                                           id,
                                           relX, relY, xabs, yabs);
    }


    if (!_mousePressed) {
        if (!_onDraggingAction && window) {
            //Press-release on a window without a move in between.
            glass_application_notifyTouchEvent(env,
                                               window,
                                               com_sun_glass_events_TouchEvent_TOUCH_RELEASED,
                                               id,
                                               relX, relY, xabs, yabs);

        } else if (_onDraggingAction && _dragGrabbingWindow != NULL) {
            //Finished drag that started on actual window.
            relX = xabs - _dragGrabbingWindow->currentBounds.x;
            relY = yabs - _dragGrabbingWindow->currentBounds.y;
            glass_application_notifyTouchEvent(env,
                                               _dragGrabbingWindow,
                                               com_sun_glass_events_TouchEvent_TOUCH_RELEASED,
                                               id,
                                               relX, relY, xabs, yabs);
        }

        _onDraggingAction = JNI_FALSE;
        _dragGrabbingWindow = NULL;

    }

    handleClickOrTouchEvent(env, xabs, yabs);

}


void lens_wm_notifyMotionEvent(JNIEnv *env, int mousePosX, int mousePosY, int isTouch, int touchId) {

    int relX, relY;
    int reportMove = 0;
    GLASS_LOG_FINEST("Motion event: x=%03d, y=%03d", mousePosX, mousePosY);
    //cache new coordinates
    _mousePosX = mousePosX;
    _mousePosY = mousePosY;

    //update cursor if event came from pointer device
    if (!isTouch) {
        fbCursorSetPosition(mousePosX, mousePosY);
    }


    if (_mousePressed && !_onDraggingAction && !isDnDStarted) {
        GLASS_LOG_FINE("Starting native mouse drag");
        _onDraggingAction = JNI_TRUE;
        _dragGrabbingWindow = lens_wm_getMouseWindow();
    }
    
    NativeWindow window = glass_window_findWindowAtLocation(
                              _mousePosX, _mousePosY, &relX, &relY);

    GLASS_LOG_FINER("Motion event on window %i[%p] absX=%i absY=%i, relX=%i, relY=%i",
                    (window)?window->id:-1,
                    window,
                    _mousePosX, _mousePosY, relX, relY);


    NativeWindow lastMouseWindow = lens_wm_getMouseWindow();

    GLASS_LOG_FINER("lastMouseWindow = %i[%p]",
                    (lastMouseWindow)?lastMouseWindow->id:-1,
                    lastMouseWindow);

    //Send EXIT/ENTER events
    if (_onDraggingAction && _dragGrabbingWindow != NULL) {
        if (window != _dragGrabbingWindow &&
                _dragGrabbingWindow == lastMouseWindow) {
            relX = _mousePosX - _dragGrabbingWindow->currentBounds.x;
            relY = _mousePosY - _dragGrabbingWindow->currentBounds.y;
            GLASS_LOG_FINER("MouseEvent_EXIT on dragGrabbingWindow %i[%p]",
                            (_dragGrabbingWindow)?_dragGrabbingWindow->id:-1,
                            _dragGrabbingWindow);

            glass_application_notifyMouseEvent(env,
                                               _dragGrabbingWindow,
                                               com_sun_glass_events_MouseEvent_EXIT,
                                               relX, relY, _mousePosX, _mousePosY,
                                               com_sun_glass_events_MouseEvent_BUTTON_NONE);
        }

        if (window == _dragGrabbingWindow &&
                window != lastMouseWindow) {
            GLASS_LOG_FINER("MouseEvent_ENTER on dragGrabbingWindow %i[%p]",
                            (_dragGrabbingWindow)?_dragGrabbingWindow->id:-1,
                            _dragGrabbingWindow);
            glass_application_notifyMouseEvent(env,
                                               _dragGrabbingWindow,
                                               com_sun_glass_events_MouseEvent_ENTER,
                                               relX, relY, _mousePosX, _mousePosY,
                                               com_sun_glass_events_MouseEvent_BUTTON_NONE);
        }
    }

    if (!_onDraggingAction) {
        if (window != lastMouseWindow) {
            if (lastMouseWindow) {
                // Exited from lastMouseWindow
                relX = _mousePosX - lastMouseWindow->currentBounds.x;
                relY = _mousePosY - lastMouseWindow->currentBounds.y;

                GLASS_LOG_FINER("MouseEvent_EXIT on lastMouseWindow %i[%p]",
                               (lastMouseWindow)?lastMouseWindow->id:-1,
                               lastMouseWindow);

                glass_application_notifyMouseEvent(env,
                                                   lastMouseWindow,
                                                   com_sun_glass_events_MouseEvent_EXIT,
                                                   relX, relY, _mousePosX, _mousePosY,
                                                   com_sun_glass_events_MouseEvent_BUTTON_NONE);
            }
            if (window) {
                // Enter into window
                GLASS_LOG_FINER("MouseEvent_ENTER on window %i[%p]",
                               (window)?window->id:-1,
                               window);

                glass_application_notifyMouseEvent(env,
                                                   window,
                                                   com_sun_glass_events_MouseEvent_ENTER,
                                                   relX, relY, _mousePosX, _mousePosY,
                                                   com_sun_glass_events_MouseEvent_BUTTON_NONE);

            }

        }
    }

    lens_wm_setMouseWindow(window);

    //Send the move event
    if (_onDraggingAction && _dragGrabbingWindow != NULL) {

        relX = _mousePosX - _dragGrabbingWindow->currentBounds.x;
        relY = _mousePosY - _dragGrabbingWindow->currentBounds.y;

        if (isTouch) {
            glass_application_notifyTouchEvent(env,
                                               _dragGrabbingWindow,
                                               com_sun_glass_events_TouchEvent_TOUCH_MOVED,
                                               touchId , relX, relY, _mousePosX, _mousePosY);
        }

        GLASS_LOG_FINEST("MouseEvent_MOVE on window %i[%p]",
                       (_dragGrabbingWindow)?_dragGrabbingWindow->id:-1,
                       _dragGrabbingWindow);
        glass_application_notifyMouseEvent(env,
                                           _dragGrabbingWindow,
                                           com_sun_glass_events_MouseEvent_MOVE,
                                           relX, relY, _mousePosX, _mousePosY,
                                           com_sun_glass_events_MouseEvent_BUTTON_NONE);


    } else if (!_onDraggingAction && window != NULL) {

        if (isTouch) {
            glass_application_notifyTouchEvent(env,
                                               window,
                                               com_sun_glass_events_TouchEvent_TOUCH_MOVED,
                                               touchId , relX, relY, _mousePosX, _mousePosY);
        }

         GLASS_LOG_FINEST("MouseEvent_MOVE on window %i[%p]",
                       (window)?window->id:-1,
                       window);
        glass_application_notifyMouseEvent(env,
                                           window,
                                           com_sun_glass_events_MouseEvent_MOVE,
                                           relX, relY, _mousePosX, _mousePosY,
                                           com_sun_glass_events_MouseEvent_BUTTON_NONE);

    }

}




/*
 * set focus to the specified window,
 * providing FOCUS_LOST as needed to previous
 */
void lens_wm_setFocusedWindow(JNIEnv *env, NativeWindow window) {

    NativeWindow _focusedWindow = glass_window_getFocusedWindow();    
 
    if (window != _focusedWindow) {

      GLASS_LOG_FINE("Window %i[%p] is focused. Window %i[%p] requesting focus",
             (_focusedWindow)?_focusedWindow->id:-1,
             _focusedWindow,
             (window)?window->id:-1,
             window);

        if (_focusedWindow) {

            //Release the grab if window holds it
            glass_window_ungrabFocus(env, window); /* function will print the result*/

            GLASS_LOG_FINE("Notifying window %i[%p] focus lost ",
                           _focusedWindow->id,
                           _focusedWindow);

            glass_application_notifyWindowEvent(env,
                                                _focusedWindow,
                                                com_sun_glass_events_WindowEvent_FOCUS_LOST);
        }

        glass_window_setFocusedWindow(window);

        if (window != NULL) {
            GLASS_LOG_FINE("Notifying window %i[%p] focus gained ",
                           window->id,
                           window);

            glass_application_notifyWindowEvent(env,
                                                window,
                                                com_sun_glass_events_WindowEvent_FOCUS_GAINED);
        }
    } else {
        GLASS_LOG_FINE("Window %i[%p] is already focused - ignore",
                       (window)?window->id:-1,
                       window);
    }

}

/**
 * Check if this window hold the focus or the grab. Loose them 
 * if required and give focus to the next focusable and visible
 * window 
 * 
 * @param env 
 * @param window the window to unset 
 * @return the new focused window (may be NULL) 
 */
NativeWindow lens_wm_unsetFocusedWindow(JNIEnv *env, NativeWindow window){

    GLASS_LOG_FINE("unsetting focus for window %i[%p]",
                   window->id, window);
    

    NativeWindow _focusedWindow = glass_window_getFocusedWindow();

    if (window == _focusedWindow) {
        //if this window hold the grab, release it
        GLASS_LOG_FINE("Check if this window holds the grab");
        glass_window_ungrabFocus(env, window); /* function will print the result*/

        GLASS_LOG_FINE("Releasing the focus");
        lens_wm_setFocusedWindow(env, NULL);

        _focusedWindow = NULL;

        //search for the next focusable window
        NativeWindow w = glass_window_list_getTail();

        while (w) {
            if (w->isVisible && w->state != NWS_MINIMIZED) {
                if (!w->owner && w->isFocusable) {
                    GLASS_LOG_FINE("Granting window %i[%p] the focus",
                                   w->id, w);

                    lens_wm_setFocusedWindow(env, w);
                    _focusedWindow = w;
                    break;
                }
            }
            w = w->previousWindow;
        }
    }else {
        GLASS_LOG_FINE("Window %i[%p] doesn't have the focus",
                       window?window->id : -1,
                       window);
    }

    return _focusedWindow;
}

/*
 * MouseWindow
 * The window that currently has the mouse in it.
 * Note, this may be NULL.
 */
static NativeWindow mouseWindow = NULL;

NativeWindow lens_wm_getMouseWindow() {
    return mouseWindow;
}

LensResult lens_wm_setMouseWindow(NativeWindow window) {
    mouseWindow = window;
    return LENS_OK;
}


static void lens_wm_clearScreen() {
    glass_screen_clear();
    lens_wm_rfbNotifyClearScreen();
}

void lens_wm_notifyWindowUpdate(NativeWindow window, int width, int height) {
    lens_wm_rfbNotifyWindowUpdate(window, width, height);
}


static void lens_wm_windowCacheBounds(NativeWindow window) {
    window->cachedBounds.x = window->currentBounds.x;
    window->cachedBounds.y = window->currentBounds.y;
    window->cachedBounds.width = window->currentBounds.width;
    window->cachedBounds.height = window->currentBounds.height;
}

static void lens_wm_windowUncacheBounds(NativeWindow window) {
    window->currentBounds.x = window->cachedBounds.x;
    window->currentBounds.y = window->cachedBounds.y;
    window->currentBounds.width = window->cachedBounds.width;
    window->currentBounds.height = window->cachedBounds.height;

}


void notify_lens_wm_DnDStarted() {
    isDnDStarted = JNI_TRUE;
    GLASS_LOG_FINE("DnD is active");

    //reset mouse drag as DnD events has higher priority
    _onDraggingAction = JNI_FALSE;
    _dragGrabbingWindow = NULL;
}

void notify_lens_wm_DnDEnded() {
    isDnDStarted = JNI_FALSE;
    GLASS_LOG_FINE("DnD has ended");
}

//// RFB support
static void lens_wm_initRFB(JNIEnv *env) {
#ifdef USE_RFB
    lens_rfb_init(env);
#endif
}
static void lens_wm_rfbNotifyClearScreen() {
#ifdef USE_RFB
    NativeScreen screen = glass_screen_getMainScreen();
    lens_rfb_notifyDirtyRegion(0, 0, screen->width, screen->height);
#endif
}
static void lens_wm_rfbNotifyWindowUpdate(NativeWindow window,
                                          int width, int height) {
#ifdef USE_RFB
    NativeScreen screen = glass_screen_getMainScreen();
    int x = window->currentBounds.x;
    int y = window->currentBounds.y;

    width = x + width > screen->width ? screen->width - x : width;
    height = y + height > screen->height ? screen->height - y : height;

    lens_rfb_notifyDirtyRegion(x, y, width, height);
#endif
}