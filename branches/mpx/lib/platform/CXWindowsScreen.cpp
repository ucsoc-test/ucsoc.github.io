/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2002 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "CXWindowsScreen.h"
#include "CXWindowsClipboard.h"
#include "CXWindowsEventQueueBuffer.h"
#include "CXWindowsKeyState.h"
#include "CXWindowsScreenSaver.h"
#include "CXWindowsUtil.h"
#include "CClipboard.h"
#include "CKeyMap.h"
#include "XScreen.h"
#include "XArch.h"
#include "CLog.h"
#include "CStopwatch.h"
#include "CStringUtil.h"
#include "IEventQueue.h"
#include "TMethodEventJob.h"
#include <cstring>
#include <cstdlib>
#include <sstream>

#if X_DISPLAY_MISSING
#	error X11 is required to build synergy
#else
#	include <X11/X.h>
#	include <X11/Xutil.h>
#	define XK_MISCELLANY
#	define XK_XKB_KEYS
#	include <X11/keysymdef.h>
#	if HAVE_X11_EXTENSIONS_DPMS_H
		extern "C" {
#		include <X11/extensions/dpms.h>
		}
#	endif
#	if HAVE_X11_EXTENSIONS_XTEST_H
#		include <X11/extensions/XTest.h>
#	else
#		error The XTest extension is required to build synergy
#	endif
#	if HAVE_X11_EXTENSIONS_XINERAMA_H
		// Xinerama.h may lack extern "C" for inclusion by C++
		extern "C" {
#		include <X11/extensions/Xinerama.h>
		}
#	endif
#	if HAVE_XKB_EXTENSION
#		include <X11/XKBlib.h>
#	endif
#endif
#include "CArch.h"


//
// CXWindowsScreen
//

// NOTE -- the X display is shared among several objects but is owned
// by the CXWindowsScreen.  Xlib is not reentrant so we must ensure
// that no two objects can simultaneously call Xlib with the display.
// this is easy since we only make X11 calls from the main thread.
// we must also ensure that these objects do not use the display in
// their destructors or, if they do, we can tell them not to.  This
// is to handle unexpected disconnection of the X display, when any
// call on the display is invalid.  In that situation we discard the
// display and the X11 event queue buffer, ignore any calls that try
// to use the display, and wait to be destroyed.

CXWindowsScreen*		CXWindowsScreen::s_screen = NULL;

CXWindowsScreen::CXWindowsScreen(const char* displayName, bool isPrimary, CString serverName, int mouseScrollDelta) :
	m_isPrimary(isPrimary),
	m_dev(CDeviceManager::getInstance()),
	m_serverName(serverName),
	m_mouseScrollDelta(mouseScrollDelta),
	m_display(NULL),
	m_root(None),
	m_window(None),
	m_isOnScreen(m_isPrimary),
	m_x(0), m_y(0),
	m_w(0), m_h(0),
	m_xCenter(0), m_yCenter(0),
	m_xCursor(0), m_yCursor(0),
	// not with MPX: m_keyState(NULL),
	m_lastFocus(None),
	m_lastFocusRevert(RevertToNone),
	m_im(NULL),
	m_ic(NULL),
	m_lastKeycode(0),
	m_sequenceNumber(0),
	m_screensaver(NULL),
	m_screensaverNotify(false),
	m_xtestIsXineramaUnaware(true),
	m_preserveFocus(false),
	m_xkb(false)
{
	assert(s_screen == NULL);

	if (mouseScrollDelta==0) m_mouseScrollDelta=120;
	s_screen = this;
	
	// initializes Xlib support for concurrent threads.
	if (XInitThreads() == 0)
	{
		throw XArch("XInitThreads() returned zero");
	}
	

	// set the X I/O error handler so we catch the display disconnecting
	XSetIOErrorHandler(&CXWindowsScreen::ioErrorHandler);

	try {
		m_display     = openDisplay(displayName);
		m_root        = DefaultRootWindow(m_display);
		saveShape();
		m_window      = openWindow();
		m_screensaver = new CXWindowsScreenSaver(m_display, m_window, getEventTarget());		
		
		LOG((CLOG_DEBUG "screen shape: %d,%d %dx%d %s", m_x, m_y, m_w, m_h, m_xinerama ? "(xinerama)" : ""));
		LOG((CLOG_DEBUG "window is 0x%08x", m_window));
	}
	catch (...) {
		if (m_display != NULL) {
			XCloseDisplay(m_display);
		}
		throw;
	}

	// primary/secondary screen only initialization
	if (m_isPrimary) {
	  	SInt32 x, y;
// 		std::list<UInt8> ptrIDs;
// 		std::list<UInt8>::iterator i;
// 		m_dev->getAllPointerIDs(ptrIDs);
// 		for(i=ptrIDs.begin(); i != ptrIDs.end(); ++i)
// 		{
// 		    getCursorPos(x, y, *i);  
// 		    LOG((CLOG_DEBUG "CXWindowsScreen constructor dev(%d) x(%d),y(%d)", *i, x, y));
// 		    m_dev->setLastCursorPos(x, y, *i);
// 		}	
		m_dev->setPrimary(true);
		initDevices();

    
		// start watching for events on other windows
		selectEvents(m_root);

		// prepare to use input methods
		openIM();
	}
	else {
		// become impervious to server grabs
		XTestGrabControl(m_display, True);
	}

	// initialize the clipboards
	for (ClipboardID cId = 0; cId < kClipboardEnd; ++cId) {
		m_clipboard[cId] = new CXWindowsClipboard(m_display, m_window, cId);
	}

	// install event handlers
	EVENTQUEUE->adoptHandler(CEvent::kSystem, IEventQueue::getSystemTarget(),
							new TMethodEventJob<CXWindowsScreen>(this,
								&CXWindowsScreen::handleSystemEvent));

	// install the platform event queue
	EVENTQUEUE->adoptBuffer(new CXWindowsEventQueueBuffer(m_display, m_window));
}

CXWindowsScreen::~CXWindowsScreen()
{
	assert(s_screen  != NULL);
	assert(m_display != NULL);

	EVENTQUEUE->adoptBuffer(NULL);
	EVENTQUEUE->removeHandler(CEvent::kSystem, IEventQueue::getSystemTarget());
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		delete m_clipboard[id];
	}
	// not with MPX: delete m_keyState;
	delete m_screensaver;
	// not with MPX: m_keyState    = NULL;
	m_screensaver = NULL;
	if (m_display != NULL) {
		// FIXME -- is it safe to clean up the IC and IM without a display?
		if (m_ic != NULL) {
			XDestroyIC(m_ic);
		}
		if (m_im != NULL) {
			XCloseIM(m_im);
		}
		XDestroyWindow(m_display, m_window);
		XCloseDisplay(m_display);
	}
	XSetIOErrorHandler(NULL);

	s_screen = NULL;
}

IPlatformScreen*
CXWindowsScreen::getInstance(const char* displayName, bool isPrimary, CString serverName, int mouseScrollDelta)
{
    if(s_screen)
      return s_screen;
    else
    {
      s_screen = new CXWindowsScreen(displayName, isPrimary, serverName, mouseScrollDelta);      
      LOG((CLOG_DEBUG1 "Returning CXWindowsScreen instance: %p", s_screen));
      return s_screen;
    }
}

void
CXWindowsScreen::enable()
{
#if HAVE_X11_EXTENSIONS_XINPUT2_H
	if (!m_isPrimary) {
		std::list<UInt8> ptrIDs;
		std::list<UInt8>::iterator i;
		m_dev->getAllPointerIDs(ptrIDs);
		bool isHandled = false;
		for(i=ptrIDs.begin(); i != ptrIDs.end(); ++i)
		{
		    CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(*i);
		    if(!m_dev->isPointer(*i))
		    {
			// get the keyboard control state
			XKeyboardState keyControl;
			XGetKeyboardControl(m_display, &keyControl);
			m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
			l_keyState->setAutoRepeat(keyControl);
		    }
		}
	}
#else
	if (!m_isPrimary) {
		// get the keyboard control state
		XKeyboardState keyControl;
		XGetKeyboardControl(m_display, &keyControl);
		m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
		m_keyState->setAutoRepeat(keyControl);

		// move hider window under the cursor center
		XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);

		// raise and show the window
		// FIXME -- take focus?
		XMapRaised(m_display, m_window);

		// warp the mouse to the cursor center
		fakeMouseMove(m_xCenter, m_yCenter);
	}
#endif
}

void
CXWindowsScreen::disable()
{
#if HAVE_X11_EXTENSIONS_XINPUT2_H
	// release input context focus
	if (m_ic != NULL) {
		XUnsetICFocus(m_ic);
	}
	// restore auto-repeat state
// 	if (!m_isPrimary && m_autoRepeat) {
// 		XAutoRepeatOn(m_display);
// 	}
#else
	// release input context focus
	if (m_ic != NULL) {
		XUnsetICFocus(m_ic);
	}

	// unmap the hider/grab window.  this also ungrabs the mouse and
	// keyboard if they're grabbed.
	XUnmapWindow(m_display, m_window);

	// restore auto-repeat state
	if (!m_isPrimary && m_autoRepeat) {
		//XAutoRepeatOn(m_display);
	}
#endif
}

void
CXWindowsScreen::enter(UInt8 kId, UInt8 pId)
{
#if HAVE_X11_EXTENSIONS_XINPUT2_H
	screensaver(false);

        LOG((CLOG_DEBUG "entering Screen on primary: dev(%d,%d)", pId, kId));
	// release input context focus
	if (m_ic != NULL) {
		XUnsetICFocus(m_ic);
	}

	// set the input focus to what it had been when we took it
	if (m_lastFocus != None) {
		// the window may not exist anymore so ignore errors
		CXWindowsUtil::CErrorLock lock(m_display);
		//XSetInputFocus(m_display, m_lastFocus, m_lastFocusRevert, CurrentTime);
	}

	#if HAVE_X11_EXTENSIONS_DPMS_H
	// Force the DPMS to turn screen back on since we don't
	// actually cause physical hardware input to trigger it
	int dummy;
	CARD16 powerlevel;
	BOOL enabled;
	if (DPMSQueryExtension(m_display, &dummy, &dummy) &&
	    DPMSCapable(m_display) &&
	    DPMSInfo(m_display, &powerlevel, &enabled))
	{
		if (enabled && powerlevel != DPMSModeOn)
			DPMSForceLevel(m_display, DPMSModeOn);
	}
	#endif

	if (!m_isPrimary) {
		createMaster(m_serverName.c_str(), kId, pId);
		// get the keyboard control state
		XKeyboardState keyControl;
		XGetKeyboardControl(m_display, &keyControl);
		m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
		CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(m_dev->getIdFromSid(kId));
		//l_keyState->setAutoRepeat(m_autoRepeat);
		l_keyState->setAutoRepeat(keyControl);

		// turn off auto-repeat.  we do this so fake key press events don't
		// cause the local server to generate their own auto-repeats of
		// those keys.
		// -- no need when using MPX
		// XAutoRepeatOff(m_display);
		l_keyState->updateKeyMap(m_dev->getIdFromSid(kId));
		l_keyState->updateKeyState(m_dev->getIdFromSid(kId));
		m_dev->setIsOnScreen(true, m_dev->getIdFromSid(pId));
	}
	
	if(m_isPrimary)
	{
	    SInt32 x,y;
	    LOG((CLOG_DEBUG "ungrabbing mouse %d", pId));
	    XIUngrabDevice(m_display, pId, CurrentTime);
	    LOG((CLOG_DEBUG "ungrabbing keyboard %d", kId));
	    XIUngrabDevice(m_display, kId, CurrentTime);
	    m_dev->setIsOnScreen(true, pId);
	    LOG((CLOG_DEBUG "done ungrabbing", kId));
	    if(m_hidden)
	    {
		XFixesShowCursor(m_display, m_root);
		m_hidden = false;
	    }
	}


#else
	screensaver(false);

	// release input context focus
	if (m_ic != NULL) {
		XUnsetICFocus(m_ic);
	}

	// set the input focus to what it had been when we took it
	if (m_lastFocus != None) {
		// the window may not exist anymore so ignore errors
		CXWindowsUtil::CErrorLock lock(m_display);
		//XSetInputFocus(m_display, m_lastFocus, m_lastFocusRevert, CurrentTime);
	}

	#if HAVE_X11_EXTENSIONS_DPMS_H
	// Force the DPMS to turn screen back on since we don't
	// actually cause physical hardware input to trigger it
	int dummy;
	CARD16 powerlevel;
	BOOL enabled;
	if (DPMSQueryExtension(m_display, &dummy, &dummy) &&
	    DPMSCapable(m_display) &&
	    DPMSInfo(m_display, &powerlevel, &enabled))
	{
		if (enabled && powerlevel != DPMSModeOn)
			DPMSForceLevel(m_display, DPMSModeOn);
	}
	#endif
	
	// unmap the hider/grab window.  this also ungrabs the mouse and
	// keyboard if they're grabbed.
	XUnmapWindow(m_display, m_window);

/* maybe call this if entering for the screensaver
	// set keyboard focus to root window.  the screensaver should then
	// pick up key events for when the user enters a password to unlock. 
	XSetInputFocus(m_display, PointerRoot, PointerRoot, CurrentTime);
*/

	if (!m_isPrimary) {
		// get the keyboard control state
		XKeyboardState keyControl;
		XGetKeyboardControl(m_display, &keyControl);
		m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
		m_keyState->setAutoRepeat(keyControl);

		// turn off auto-repeat.  we do this so fake key press events don't
		// cause the local server to generate their own auto-repeats of
		// those keys.
		//XAutoRepeatOff(m_display);
	}

	// now on screen
	m_dev->isOnScreen(true, id);
#endif
}

bool
CXWindowsScreen::leave(UInt8 id)
{
#if HAVE_X11_EXTENSIONS_XINPUT2_H
	if (!m_isPrimary) {
		// restore the previous keyboard auto-repeat state.  if the user
		// changed the auto-repeat configuration while on the client then
		// that state is lost.  that's because we can't get notified by
		// the X server when the auto-repeat configuration is changed so
		// we can't track the desired configuration.
// 		if (m_autoRepeat) {
// 			XAutoRepeatOn(m_display);
// 		}
	}

	// grab the mouse and keyboard, if primary and possible
	if (m_isPrimary && !grabMouseAndKeyboard(id)) {
		return false;
	}

	// save current focus
	//XGetInputFocus(m_display, &m_lastFocus, &m_lastFocusRevert);

	// take focus
	if (m_isPrimary || !m_preserveFocus) {
		//XSetInputFocus(m_display, m_window, RevertToPointerRoot, CurrentTime);
	}

	// now warp the mouse.  we warp after showing the window so we're
	// guaranteed to get the mouse leave event and to prevent the
	// keyboard focus from changing under point-to-focus policies.
	if (m_isPrimary) {
		//warpCursor(m_xCenter, m_yCenter, id);
		// now off screen
		m_dev->setIsOnScreen(false,id);
		// 	Status stat2 = XIUndefineCursor(m_display, id, m_window);
		// 	LOG((CLOG_DEBUG "XIUndefineCursor Status: %d",stat2));
		if(!m_hidden)
		{
		    XFixesHideCursor(m_display, m_root);
		    m_hidden = true;
		}		
	}
	else {
		//fakeMotionEvent(m_xCenter, m_yCenter, id);
	}

	// set input context focus to our window
	if (m_ic != NULL) {
	   LOG((CLOG_DEBUG "m_ic != NULL"));
		XmbResetIC(m_ic);
		XSetICFocus(m_ic);
		m_filtered.clear();
	}

	return true;
#else
	if (!m_isPrimary) {
		// restore the previous keyboard auto-repeat state.  if the user
		// changed the auto-repeat configuration while on the client then
		// that state is lost.  that's because we can't get notified by
		// the X server when the auto-repeat configuration is changed so
		// we can't track the desired configuration.
		if (m_autoRepeat) {
			//XAutoRepeatOn(m_display);
		}

		// move hider window under the cursor center
		XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);
	}

	// raise and show the window
	XMapRaised(m_display, m_window);

	// grab the mouse and keyboard, if primary and possible
	if (m_isPrimary && !grabMouseAndKeyboard()) {
		XUnmapWindow(m_display, m_window);
		return false;
	}

	// save current focus
	XGetInputFocus(m_display, &m_lastFocus, &m_lastFocusRevert);

	// take focus
	if (m_isPrimary || !m_preserveFocus) {
		XSetInputFocus(m_display, m_window, RevertToPointerRoot, CurrentTime);
	}

	// now warp the mouse.  we warp after showing the window so we're
	// guaranteed to get the mouse leave event and to prevent the
	// keyboard focus from changing under point-to-focus policies.
	if (m_isPrimary) {
		warpCursor(m_xCenter, m_yCenter);
	}
	else {
		fakeMouseMove(m_xCenter, m_yCenter);
	}

	// set input context focus to our window
	if (m_ic != NULL) {
		XmbResetIC(m_ic);
		XSetICFocus(m_ic);
		m_filtered.clear();
	}

	// now off screen
	m_isOnScreen = false;

	return true;
#endif
}

bool
CXWindowsScreen::setClipboard(ClipboardID cId, const IClipboard* clipboard)
{
	// fail if we don't have the requested clipboard
	if (m_clipboard[cId] == NULL) {
		return false;
	}

	// get the actual time.  ICCCM does not allow CurrentTime.
	Time timestamp = CXWindowsUtil::getCurrentTime(
								m_display, m_clipboard[cId]->getWindow());

	if (clipboard != NULL) {
		// save clipboard data
		return CClipboard::copy(m_clipboard[cId], clipboard, timestamp);
	}
	else {
		// assert clipboard ownership
		if (!m_clipboard[cId]->open(timestamp)) {
			return false;
		}
		m_clipboard[cId]->empty();
		m_clipboard[cId]->close();
		return true;
	}
}

void
CXWindowsScreen::checkClipboards()
{
	// do nothing, we're always up to date
}

void
CXWindowsScreen::openScreensaver(bool notify)
{
	m_screensaverNotify = notify;
	if (!m_screensaverNotify) {
		m_screensaver->disable();
	}
}

void
CXWindowsScreen::closeScreensaver()
{
	if (!m_screensaverNotify) {
		m_screensaver->enable();
	}
}

void
CXWindowsScreen::screensaver(bool activate)
{
	if (activate) {
		m_screensaver->activate();
	}
	else {
		m_screensaver->deactivate();
	}
}

void
CXWindowsScreen::resetOptions()
{
	m_xtestIsXineramaUnaware = true;
	m_preserveFocus = false;
}

void
CXWindowsScreen::setOptions(const COptionsList& options)
{
	for (UInt32 i = 0, n = options.size(); i < n; i += 2) {
		if (options[i] == kOptionXTestXineramaUnaware) {
			m_xtestIsXineramaUnaware = (options[i + 1] != 0);
			LOG((CLOG_DEBUG1 "XTest is Xinerama unaware %s", m_xtestIsXineramaUnaware ? "true" : "false"));
		}
		else if (options[i] == kOptionScreenPreserveFocus) {
			m_preserveFocus = (options[i + 1] != 0);
			LOG((CLOG_DEBUG1 "Preserve Focus = %s", m_preserveFocus ? "true" : "false"));
		}
	}
}

void
CXWindowsScreen::setSequenceNumber(UInt32 seqNum)
{
	m_sequenceNumber = seqNum;
}

bool
CXWindowsScreen::isPrimary() const
{
	return m_isPrimary;
}

void*
CXWindowsScreen::getEventTarget() const
{
	return const_cast<CXWindowsScreen*>(this);
}

bool
CXWindowsScreen::getClipboard(ClipboardID cId, IClipboard* clipboard) const
{
	assert(clipboard != NULL);
	LOG((CLOG_DEBUG "getClipboard %d",cId));	 	

	// fail if we don't have the requested clipboard
	if (m_clipboard[cId] == NULL) {
		return false;
	}

	// get the actual time.  ICCCM does not allow CurrentTime.
	Time timestamp = CXWindowsUtil::getCurrentTime(
								m_display, m_clipboard[cId]->getWindow());

	// copy the clipboard
	return CClipboard::copy(clipboard, m_clipboard[cId], timestamp);
}

void
CXWindowsScreen::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
	x = m_x;
	y = m_y;
	w = m_w;
	h = m_h;
}

#if HAVE_X11_EXTENSIONS_XINPUT2_H
void
CXWindowsScreen::getCursorPos(SInt32& x, SInt32& y, UInt8 id) const
{
        Window root,window;
        double mx, my, xWindow, yWindow;
        XIButtonState       buttons;
        XIModifierState     mods;
        XIGroupState        group;
	LOG((CLOG_DEBUG2 "XIQueryPointer(%d)", id));
        if(XIQueryPointer(m_display, id, m_root, &root, &window,
                          &mx, &my, &xWindow, &yWindow, &buttons,
                          &mods, &group)){
            x = mx;
            y = my;
            //LOG((CLOG_DEBUG "getcursorpos id(%d) x:%d y:%d", id, x, y));
        }
        else{
            x = 0; // xWindow;
            y = 0; // yWindow;
        }
}

#else
void
CXWindowsScreen::getCursorPos(SInt32& x, SInt32& y) const
{
	Window root, window;
	int mx, my, xWindow, yWindow;
	unsigned int mask;
	if (XQueryPointer(m_display, m_root, &root, &window,
								&mx, &my, &xWindow, &yWindow, &mask)) {
		x = mx;
		y = my;
	}
	else {
		x = m_xCenter;
		y = m_yCenter;
	}
}
#endif

void
CXWindowsScreen::reconfigure(UInt32)
{
	// do nothing
}

#if HAVE_X11_EXTENSIONS_XINPUT2_H
void
CXWindowsScreen::warpCursor(SInt32 x, SInt32 y, UInt8 id)
{
	// warp mouse
	warpCursorNoFlush(x, y, id);

	// remove all input events before and including warp
// 	XEvent event;
// 	while (XCheckMaskEvent(m_display, XI_MotionMask | XI_ButtonPressMask | XI_ButtonReleaseMask |
// 								XI_KeyPressMask | XI_KeyReleaseMask |
// 								KeymapStateMask, &event)) {
// 		// do nothing
// 	}
// 	while (XCheckMaskEvent(m_display, PointerMotionMask | ButtonPressMask | ButtonReleaseMask |
// 								KeyPressMask | KeyReleaseMask |
// 								KeymapStateMask,
// 								&event)) {
// 		// do nothing
// 	}

// This does not work. need a replacement for xcheckmaskevent
// 	XIEventMask mask;    
// 	/* Select for motion events */
// 	mask.deviceid = XIAllMasterDevices;
// 	mask.mask_len = XIMaskLen(XI_RawMotion);
// 	mask.mask = (unsigned char*)calloc(mask.mask_len, sizeof(char));
// 	XISetMask(mask.mask, XI_RawMotion);
// 	XISetMask(mask.mask, XI_Motion);
// 	XISetMask(mask.mask, XI_KeyPress);
// 	XISetMask(mask.mask, XI_KeyRelease);
// 	XISetMask(mask.mask, XI_ButtonPress);
// 	XISetMask(mask.mask, XI_ButtonRelease);
// 	while(XCheckMaskEvent(m_display, mask.mask))
// 	  ;
// 	free(mask.mask);
// 	
	// save position as last position
	m_dev->setLastCursorPos(x,y,id);

}
#else
void
CXWindowsScreen::warpCursor(SInt32 x, SInt32 y)
{
	// warp mouse
	warpCursorNoFlush(x, y);

	// remove all input events before and including warp
	XEvent event;
	while (XCheckMaskEvent(m_display, PointerMotionMask |
								ButtonPressMask | ButtonReleaseMask |
								KeyPressMask | KeyReleaseMask |
								KeymapStateMask,
								&event)) {
		// do nothing
	}

	// save position as last position
	m_xCursor = x;
	m_yCursor = y;
}
#endif

UInt32
CXWindowsScreen::registerHotKey(KeyID key, KeyModifierMask mask, UInt8 id)
{
  	CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);

	// only allow certain modifiers
	if ((mask & ~(KeyModifierShift | KeyModifierControl |
				  KeyModifierAlt   | KeyModifierSuper)) != 0) {
		LOG((CLOG_WARN "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}

	// fail if no keys
	if (key == kKeyNone && mask == 0) {
		return 0;
	}

	// convert to X
	unsigned int modifiers;
	if (!l_keyState->mapModifiersToX(mask, modifiers)) {
		// can't map all modifiers
		LOG((CLOG_WARN "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}
	CXWindowsKeyState::CKeycodeList keycodes;
	l_keyState->mapKeyToKeycodes(key, keycodes);
	if (key != kKeyNone && keycodes.empty()) {
		// can't map key
		LOG((CLOG_WARN "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}

	// choose hotkey id
	UInt32 hId;
	if (!m_oldHotKeyIDs.empty()) {
		hId = m_oldHotKeyIDs.back();
		m_oldHotKeyIDs.pop_back();
	}
	else {
		hId = m_hotKeys.size() + 1;
	}
	HotKeyList& hotKeys = m_hotKeys[hId];

	// all modifier hotkey must be treated specially.  for each modifier
	// we need to grab the modifier key in combination with all the other
	// requested modifiers.
	bool err = false;
	{
		CXWindowsUtil::CErrorLock lock(m_display, &err);
		if (key == kKeyNone) {
			static const KeyModifierMask s_hotKeyModifiers[] = {
				KeyModifierShift,
				KeyModifierControl,
				KeyModifierAlt,
				KeyModifierMeta,
				KeyModifierSuper
			};

			XModifierKeymap* modKeymap = XGetModifierMapping(m_display);
			for (size_t j = 0; j < sizeof(s_hotKeyModifiers) /
									sizeof(s_hotKeyModifiers[0]) && !err; ++j) {
				// skip modifier if not in mask
				if ((mask & s_hotKeyModifiers[j]) == 0) {
					continue;
				}

				// skip with error if we can't map remaining modifiers
				unsigned int modifiers2;
				KeyModifierMask mask2 = (mask & ~s_hotKeyModifiers[j]);
				if (!l_keyState->mapModifiersToX(mask2, modifiers2)) {
					err = true;
					continue;
				}

				// compute modifier index for modifier.  there should be
				// exactly one X modifier missing
				int index;
				switch (modifiers ^ modifiers2) {
				case ShiftMask:
					index = ShiftMapIndex;
					break;

				case LockMask:
					index = LockMapIndex;
					break;

				case ControlMask:
					index = ControlMapIndex;
					break;

				case Mod1Mask:
					index = Mod1MapIndex;
					break;

				case Mod2Mask:
					index = Mod2MapIndex;
					break;

				case Mod3Mask:
					index = Mod3MapIndex;
					break;

				case Mod4Mask:
					index = Mod4MapIndex;
					break;

				case Mod5Mask:
					index = Mod5MapIndex;
					break;

				default:
					err = true;
					continue;
				}

				// grab each key for the modifier
				const KeyCode* modifiermap =
					modKeymap->modifiermap + index * modKeymap->max_keypermod;
				for (int k = 0; k < modKeymap->max_keypermod && !err; ++k) {
					KeyCode code = modifiermap[k];
					if (modifiermap[k] != 0) {
						XGrabKey(m_display, code, modifiers2, m_root,
									False, GrabModeAsync, GrabModeAsync);
						if (!err) {
							hotKeys.push_back(std::make_pair(code, modifiers2));
							m_hotKeyToIDMap[CHotKeyItem(code, modifiers2)] = hId;
						}
					}
				}
			}
			XFreeModifiermap(modKeymap);
		}

		// a non-modifier key must be insensitive to CapsLock, NumLock and
		// ScrollLock, so we have to grab the key with every combination of
		// those.
		else {
			// collect available toggle modifiers
			unsigned int modifier;
			unsigned int toggleModifiers[3];
			size_t numToggleModifiers = 0;
			if (l_keyState->mapModifiersToX(KeyModifierCapsLock, modifier)) {
				toggleModifiers[numToggleModifiers++] = modifier;
			}
			if (l_keyState->mapModifiersToX(KeyModifierNumLock, modifier)) {
				toggleModifiers[numToggleModifiers++] = modifier;
			}
			if (l_keyState->mapModifiersToX(KeyModifierScrollLock, modifier)) {
				toggleModifiers[numToggleModifiers++] = modifier;
			}


			for (CXWindowsKeyState::CKeycodeList::iterator j = keycodes.begin();
									j != keycodes.end() && !err; ++j) {
				for (size_t i = 0; i < (1u << numToggleModifiers); ++i) {
					// add toggle modifiers for index i
					unsigned int tmpModifiers = modifiers;
					if ((i & 1) != 0) {
						tmpModifiers |= toggleModifiers[0];
					}
					if ((i & 2) != 0) {
						tmpModifiers |= toggleModifiers[1];
					}
					if ((i & 4) != 0) {
						tmpModifiers |= toggleModifiers[2];
					}

					// add grab
					XGrabKey(m_display, *j, tmpModifiers, m_root,
										False, GrabModeAsync, GrabModeAsync);
					if (!err) {
						hotKeys.push_back(std::make_pair(*j, tmpModifiers));
						m_hotKeyToIDMap[CHotKeyItem(*j, tmpModifiers)] = hId;
					}
				}
			}
		}
	}

	if (err) {
		// if any failed then unregister any we did get
		for (HotKeyList::iterator j = hotKeys.begin();
								j != hotKeys.end(); ++j) {
			XUngrabKey(m_display, j->first, j->second, m_root);
			m_hotKeyToIDMap.erase(CHotKeyItem(j->first, j->second));
		}

		m_oldHotKeyIDs.push_back(hId);
		m_hotKeys.erase(hId);
		LOG((CLOG_WARN "failed to register hotkey %s (id=%04x mask=%04x)", CKeyMap::formatKey(key, mask).c_str(), key, mask));
		return 0;
	}
	
	LOG((CLOG_DEBUG "registered hotkey %s (id=%04x mask=%04x) as id=%d", CKeyMap::formatKey(key, mask).c_str(), key, mask, hId));
	return hId;
}

void
CXWindowsScreen::unregisterHotKey(UInt32 hId)
{
	// look up hotkey
	HotKeyMap::iterator i = m_hotKeys.find(hId);
	if (i == m_hotKeys.end()) {
		return;
	}

	// unregister with OS
	bool err = false;
	{
		CXWindowsUtil::CErrorLock lock(m_display, &err);
		HotKeyList& hotKeys = i->second;
		for (HotKeyList::iterator j = hotKeys.begin();
								j != hotKeys.end(); ++j) {
			XUngrabKey(m_display, j->first, j->second, m_root);
			m_hotKeyToIDMap.erase(CHotKeyItem(j->first, j->second));
		}
	}
	if (err) {
		LOG((CLOG_WARN "failed to unregister hotkey id=%d", hId));
	}
	else {
		LOG((CLOG_DEBUG "unregistered hotkey id=%d", hId));
	}

	// discard hot key from map and record old id for reuse
	m_hotKeys.erase(i);
	m_oldHotKeyIDs.push_back(hId);
}

void
CXWindowsScreen::fakeInputBegin()
{
	// FIXME -- not implemented
}

void
CXWindowsScreen::fakeInputEnd()
{
	// FIXME -- not implemented
}

SInt32
CXWindowsScreen::getJumpZoneSize() const
{
	return 1;
}

bool
CXWindowsScreen::isAnyMouseButtonDown(UInt8 id) const
{
#if HAVE_X11_EXTENSIONS_XINPUT2_H
	// query the pointer to get the button state
	Window root, window;
	double xRoot, yRoot, xWindow, yWindow;
	XIButtonState       buttons;
        XIModifierState     mods;
        XIGroupState        group;
	unsigned int buttonsdown = 0;
	bool xireturn = XIQueryPointer(m_display, id, m_root, &root, &window,
	    &xRoot, &yRoot, &xWindow, &yWindow, &buttons, &mods, &group);
	
	LOG((CLOG_DEBUG1 "isAnyMouseButtonDown XIQueryPointer(%d): %d", id, xireturn));
	if (xireturn) {
	    for (UInt32 i = 0; i < buttons.mask_len * 8; i++)
		if (XIMaskIsSet(buttons.mask, i)) {
		    buttonsdown++;
		    LOG((CLOG_DEBUG "buttondown id=%d count=%d", id, buttonsdown));
	      }
	     //return ((buttons & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)) != 0);
	     LOG((CLOG_DEBUG1 "buttondown id=%d count=%d", id, buttonsdown));
	     return buttonsdown;
	}

	return false;
#else
	// query the pointer to get the button state
	Window root, window;
	int xRoot, yRoot, xWindow, yWindow;
	unsigned int state;
	if (XQueryPointer(m_display, m_root, &root, &window,
								&xRoot, &yRoot, &xWindow, &yWindow, &state)) {
		return ((state & (Button1Mask | Button2Mask | Button3Mask |
							Button4Mask | Button5Mask)) != 0);
	}

	return false;
#endif	
}

void
CXWindowsScreen::getCursorCenter(SInt32& x, SInt32& y) const
{
	x = m_xCenter;
	y = m_yCenter;
}

//#if HAVE_X11_EXTENSIONS_XINPUT2_H

void
CXWindowsScreen::fakeButtonEvent(ButtonID button, bool press,UInt8 id) const
{
//	UInt8 local_id = m_dev->getIdFromSid(id);
    	//const unsigned int xButton = mapButtonToX(button);
  const unsigned int xButton =  button;
  UInt8 xtest_id = m_dev->getXtestId(id);
  LOG((CLOG_DEBUG "Button! dev: %d, local dev: %d, Button: %d Press? (%d)", id, xtest_id,xButton,press));

	if (xButton != 0) {
            XDevice *my_pointer = XOpenDevice(m_display, xtest_id);
	    LOG((CLOG_DEBUG1 "Button! dev: %d, Button: %d Press? (%d)", xtest_id,xButton,press));
            SInt32 axes[2];
            SInt32 &x = axes[0];
            SInt32 &y = axes[1];
            getCursorPos(x,y,id);
            XTestFakeDeviceButtonEvent(m_display, my_pointer, xButton, press ? True : False, axes , 2, CurrentTime);            
            XCloseDevice(m_display,my_pointer);
            XFlush(m_display);
	}
}

void
CXWindowsScreen::fakeMotionEvent(SInt32 x, SInt32 y, UInt8 id) const
{
	//UInt8 local_id = m_dev->getIdFromSid(id);
	UInt8 xtest_id = m_dev->getXtestId(id);
        LOG((CLOG_DEBUG2 "fake mouse  move. lid: %d; sid: %d x: %d; y:%d", xtest_id, id,x,y));
        if (m_xinerama && m_xtestIsXineramaUnaware) {
            if (id){
            	LOG((CLOG_DEBUG2 "warping mouse id: %d", id));
                XIWarpPointer(m_display,id,None,m_root,0,0,0,0,x,y);
            }
        }
	else {
            LOG((CLOG_DEBUG2 "fake mouse move. id: %d", xtest_id));
            XDevice *my_pointer = XOpenDevice(m_display, xtest_id);
            int motion[2];
            motion[0] = x;
            motion[1] = y;
            XTestFakeDeviceMotionEvent(m_display, my_pointer, false, 0,motion,2, CurrentTime);
            XCloseDevice(m_display,my_pointer);
        }
        XFlush(m_display);
}

void
CXWindowsScreen::fakeRelativeMotionEvent(SInt32 dx, SInt32 dy, UInt8 id) const
{

	UInt8 xtest_id = m_dev->getXtestId(id);
        LOG((CLOG_DEBUG2 "fake relative mouse move. id: %d", id));
        if (m_xinerama && m_xtestIsXineramaUnaware) {
            if (id){
            	LOG((CLOG_DEBUG2 "relative warp mouse id: %d", id));
                XIWarpPointer(m_display,id,None,m_root,0,0,0,0,dx,dy);
            }
        }
	else {
                XDevice *my_pointer = XOpenDevice(m_display, xtest_id);
                int motion[2];
                motion[0] = dx;
                motion[1] = dy;

                XTestFakeDeviceMotionEvent(m_display, my_pointer, true, 0,
                                           motion, 2, CurrentTime);
                XCloseDevice(m_display,my_pointer);
        }
        XFlush(m_display);
}

void
CXWindowsScreen::fakeMouseWheelEvent(SInt32, SInt32 yDelta, UInt8 id) const
{
	// XXX -- support x-axis scrolling
	if (yDelta == 0) {
		return;
	}

	// choose button depending on rotation direction
	const unsigned int xButton = mapButtonToX(static_cast<ButtonID>((yDelta >= 0) ? -1 : -2));
	if (xButton == 0) {
		// If we get here, then the XServer does not support the scroll
		// wheel buttons, so send PageUp/PageDown keystrokes instead.
		// Patch by Tom Chadwick.
		KeyCode keycode = 0;
		if (yDelta >= 0) {
			keycode = XKeysymToKeycode(m_display, XK_Page_Up);
		}
		else {
			keycode = XKeysymToKeycode(m_display, XK_Page_Down);
		}
		if (keycode != 0) {
			XTestFakeKeyEvent(m_display, keycode, True,  CurrentTime);
			XTestFakeKeyEvent(m_display, keycode, False, CurrentTime);
		}
		return;
	}

	// now use absolute value of delta
	if (yDelta < 0) {
		yDelta = -yDelta;
	}

	// send as many clicks as necessary
	for (; yDelta >= 120; yDelta -= 120) {
	  // FIXXME: This is not device aware !
		XTestFakeButtonEvent(m_display, xButton, True, CurrentTime);
		XTestFakeButtonEvent(m_display, xButton, False, CurrentTime);
	}
	XFlush(m_display);
}

//#else
void
CXWindowsScreen::fakeMouseButton(ButtonID button, bool press) const
{
	const unsigned int xButton = mapButtonToX(button);
	if (xButton != 0) {
		XTestFakeButtonEvent(m_display, xButton,
							press ? True : False, CurrentTime);
		XFlush(m_display);
	}
}

void
CXWindowsScreen::fakeMouseMove(SInt32 x, SInt32 y) const
{
	if (m_xinerama && m_xtestIsXineramaUnaware) {
		XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
	}
	else {
		XTestFakeMotionEvent(m_display, DefaultScreen(m_display),
							x, y, CurrentTime);
	}
	XFlush(m_display);
}

void
CXWindowsScreen::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
	// FIXME -- ignore xinerama for now
	if (false && m_xinerama && m_xtestIsXineramaUnaware) {
//		XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
	}
	else {
		XTestFakeRelativeMotionEvent(m_display, dx, dy, CurrentTime);
	}
	XFlush(m_display);
}

void
CXWindowsScreen::fakeMouseWheel(SInt32, SInt32 yDelta) const
{
	// XXX -- support x-axis scrolling
	if (yDelta == 0) {
		return;
	}

	// choose button depending on rotation direction
	const unsigned int xButton = mapButtonToX(static_cast<ButtonID>(
												(yDelta >= 0) ? -1 : -2));
	if (xButton == 0) {
		// If we get here, then the XServer does not support the scroll
		// wheel buttons, so send PageUp/PageDown keystrokes instead.
		// Patch by Tom Chadwick.
		KeyCode keycode = 0;
		if (yDelta >= 0) {
			keycode = XKeysymToKeycode(m_display, XK_Page_Up);
		}
		else {
			keycode = XKeysymToKeycode(m_display, XK_Page_Down);
		}
		if (keycode != 0) {
			XTestFakeKeyEvent(m_display, keycode, True,  CurrentTime);
			XTestFakeKeyEvent(m_display, keycode, False, CurrentTime);
		}
		return;
	}

	// now use absolute value of delta
	if (yDelta < 0) {
		yDelta = -yDelta;
	}

	if (yDelta < m_mouseScrollDelta) {
		LOG((CLOG_WARN "Wheel scroll delta (%d) smaller than threshold (%d)", yDelta, m_mouseScrollDelta));
	}

	// send as many clicks as necessary
	for (; yDelta >= m_mouseScrollDelta; yDelta -= m_mouseScrollDelta) {
		XTestFakeButtonEvent(m_display, xButton, True, CurrentTime);
		XTestFakeButtonEvent(m_display, xButton, False, CurrentTime);
	}
	XFlush(m_display);
}
//#endif // all old fake device functions

Display*
CXWindowsScreen::openDisplay(const char* displayName)
{
	// get the DISPLAY
	if (displayName == NULL) {
		displayName = getenv("DISPLAY");
		if (displayName == NULL) {
			displayName = ":0.0";
		}
	}

	// open the display
	LOG((CLOG_DEBUG "XOpenDisplay(\"%s\")", displayName));
	Display* display = XOpenDisplay(displayName);
	if (display == NULL) {
		throw XScreenUnavailable(60.0);
	}

	// verify the availability of the XTest extension
	if (!m_isPrimary) {
		int majorOpcode, firstEvent, firstError;
		if (!XQueryExtension(display, XTestExtensionName,
							&majorOpcode, &firstEvent, &firstError)) {
			LOG((CLOG_ERR "XTEST extension not available"));
			XCloseDisplay(display);
			throw XScreenOpenFailure();
		}
	}

#if HAVE_XKB_EXTENSION
	{
		m_xkb = false;
		int major = XkbMajorVersion, minor = XkbMinorVersion;
		if (XkbLibraryVersion(&major, &minor)) {
			int opcode, firstError;
			if (XkbQueryExtension(display, &opcode, &m_xkbEventBase,
								&firstError, &major, &minor)) {
				m_xkb = true;
				XkbSelectEvents(display, XkbUseCoreKbd,
								XkbMapNotifyMask, XkbMapNotifyMask);
				XkbSelectEventDetails(display, XkbUseCoreKbd,
								XkbStateNotifyMask,
								XkbGroupStateMask, XkbGroupStateMask);
			}
		}
	}
#endif
#if HAVE_X11_EXTENSIONS_XINPUT2_H
        Status xi2_version;
        int xi_major_version = XI_2_Major;
        int xi_minor_version = XI_2_Minor;
        int event, error;

        LOG((CLOG_DEBUG "probing for MPX"));

        if (!XQueryExtension(display, "XInputExtension", (int*)&m_xiOpCode, &event, &error)) {
            LOG((CLOG_ERR "X Input extension not available.\n"));
            return 0;
        }

        LOG((CLOG_DEBUG "probing for XI2"));

        xi2_version = XIQueryVersion(display, &xi_major_version, &xi_minor_version);
        if(xi2_version != Success || (xi_major_version * 1000 + xi_minor_version) <
                          (XI_2_Major * 1000 + XI_2_Minor)){
            LOG((CLOG_ERR "XI2 extension not available"));
            XCloseDisplay(display);
            throw XScreenOpenFailure();
        }
        LOG((CLOG_DEBUG "Using MPX"));

#endif
	return display;
}

void
CXWindowsScreen::saveShape()
{
	// get shape of default screen
	m_x = 0;
	m_y = 0;
	m_w = WidthOfScreen(DefaultScreenOfDisplay(m_display));
	m_h = HeightOfScreen(DefaultScreenOfDisplay(m_display));

	// get center of default screen
	m_xCenter = m_x + (m_w >> 1);
	m_yCenter = m_y + (m_h >> 1);

	// check if xinerama is enabled and there is more than one screen.
	// get center of first Xinerama screen.  Xinerama appears to have
	// a bug when XWarpPointer() is used in combination with
	// XGrabPointer().  in that case, the warp is successful but the
	// next pointer motion warps the pointer again, apparently to
	// constrain it to some unknown region, possibly the region from
	// 0,0 to Wm,Hm where Wm (Hm) is the minimum width (height) over
	// all physical screens.  this warp only seems to happen if the
	// pointer wasn't in that region before the XWarpPointer().  the
	// second (unexpected) warp causes synergy to think the pointer
	// has been moved when it hasn't.  to work around the problem,
	// we warp the pointer to the center of the first physical
	// screen instead of the logical screen.
	m_xinerama = false;
#if HAVE_X11_EXTENSIONS_XINERAMA_H
	int eventBase, errorBase;
	if (XineramaQueryExtension(m_display, &eventBase, &errorBase) &&
		XineramaIsActive(m_display)) {
		int numScreens;
		XineramaScreenInfo* screens;
		screens = XineramaQueryScreens(m_display, &numScreens);
		if (screens != NULL) {
			if (numScreens > 1) {
				m_xinerama = true;
				m_xCenter  = screens[0].x_org + (screens[0].width  >> 1);
				m_yCenter  = screens[0].y_org + (screens[0].height >> 1);
			}
			XFree(screens);
		}
	}
#endif
}

Window
CXWindowsScreen::openWindow() const
{
#if HAVE_X11_EXTENSIONS_XINPUT2_H
    Window window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), 0, 0, 200,
					200, 0, 0, WhitePixel(m_display, 0));
    if (window == None) {
	throw XScreenOpenFailure();
    }
    return window;
//     Window win = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), 0, 0, 200,
//             200, 0, 0, WhitePixel(m_display, 0));
//     Window subwindow = XCreateSimpleWindow(m_display, win, 50, 50, 50, 50, 0, 0,
//             BlackPixel(m_display, 0));
// 
//     XMapWindow(m_display, subwindow);
//     //XSelectInput(m_display, win, ExposureMask);
//     XMapWindow(m_display, win);
// 
//     return win;

#else
	// default window attributes.  we don't want the window manager
	// messing with our window and we don't want the cursor to be
	// visible inside the window.
	XSetWindowAttributes attr;
	attr.do_not_propagate_mask = 0;
	attr.override_redirect     = True;
	attr.cursor                = createBlankCursor();

	// adjust attributes and get size and shape
	SInt32 x, y, w, h;
	if (m_isPrimary) {
		// grab window attributes.  this window is used to capture user
		// input when the user is focused on another client.  it covers
		// the whole screen.
		attr.event_mask = PointerMotionMask |
							 ButtonPressMask | ButtonReleaseMask |
							 KeyPressMask | KeyReleaseMask |
							 KeymapStateMask | PropertyChangeMask;
		x = m_x;
		y = m_y;
		w = m_w;
		h = m_h;
	}
	else {
		// cursor hider window attributes.  this window is used to hide the
		// cursor when it's not on the screen.  the window is hidden as soon
		// as the cursor enters the screen or the display's real mouse is
		// moved.  we'll reposition the window as necessary so its
		// position here doesn't matter.  it only needs to be 1x1 because
		// it only needs to contain the cursor's hotspot.
		attr.event_mask = LeaveWindowMask;
		x = 0;
		y = 0;
		w = 1;
		h = 1;
	}

	// create and return the window
	Window window = XCreateWindow(m_display, m_root, x, y, w, h, 0, 0,
							InputOnly, CopyFromParent,
							CWDontPropagate | CWEventMask |
							CWOverrideRedirect | CWCursor,
							&attr);
	if (window == None) {
		throw XScreenOpenFailure();
	}
	return window;
#endif
}

void
CXWindowsScreen::openIM()
{
	// open the input methods
	XIM im = XOpenIM(m_display, NULL, NULL, NULL);
	if (im == NULL) {
		LOG((CLOG_INFO "no support for IM"));
		return;
	}

	// find the appropriate style.  synergy supports XIMPreeditNothing
	// only at the moment.
	XIMStyles* styles;
	if (XGetIMValues(im, XNQueryInputStyle, &styles, NULL) != NULL ||
		styles == NULL) {
		LOG((CLOG_WARN "cannot get IM styles"));
		XCloseIM(im);
		return;
	}
	XIMStyle style = 0;
	for (unsigned short i = 0; i < styles->count_styles; ++i) {
		style = styles->supported_styles[i];
		if ((style & XIMPreeditNothing) != 0) {
			if ((style & (XIMStatusNothing | XIMStatusNone)) != 0) {
				break;
			}
		}
	}
	XFree(styles);
	if (style == 0) {
		LOG((CLOG_INFO "no supported IM styles"));
		XCloseIM(im);
		return;
	}

	// create an input context for the style and tell it about our window
	XIC ic = XCreateIC(im, XNInputStyle, style, XNClientWindow, m_window, NULL);
	if (ic == NULL) {
		LOG((CLOG_WARN "cannot create IC"));
		XCloseIM(im);
		return;
	}

	// find out the events we must select for and do so
	unsigned long mask;
	if (XGetICValues(ic, XNFilterEvents, &mask, NULL) != NULL) {
		LOG((CLOG_WARN "cannot get IC filter events"));
		XDestroyIC(ic);
		XCloseIM(im);
		return;
	}

	// we have IM
	m_im          = im;
	m_ic          = ic;
	m_lastKeycode = 0;

	// select events on our window that IM requires
	XWindowAttributes attr;
	XGetWindowAttributes(m_display, m_window, &attr);
	XSelectInput(m_display, m_window, attr.your_event_mask | mask);
}

void
CXWindowsScreen::sendEvent(CEvent::Type type, void* data)
{
	EVENTQUEUE->addEvent(CEvent(type, getEventTarget(), data));
}

void
CXWindowsScreen::sendClipboardEvent(CEvent::Type type, ClipboardID cId)
{
	CClipboardInfo* info   = (CClipboardInfo*)malloc(sizeof(CClipboardInfo));
	info->m_cId             = cId;
	info->m_sequenceNumber = m_sequenceNumber;
	sendEvent(type, info);
}

IKeyState*
CXWindowsScreen::getKeyState(UInt8 id) const
{
/*    IKeyState *my_key_state = NULL;
    if(m_isPrimary)
      my_key_state = m_dev->getKeyState(id);
    else
      my_key_state = m_dev->getKeyState(m_dev->getIdFromSid(id));
    return my_key_state;*/
    return m_dev->getKeyState(id);
}

Bool
CXWindowsScreen::findKeyEvent(Display*, XEvent* xevent, XPointer arg)
{
	CKeyEventFilter* filter = reinterpret_cast<CKeyEventFilter*>(arg);
	return (xevent->type         == filter->m_event &&
			xevent->xkey.window  == filter->m_window &&
			xevent->xkey.time    == filter->m_time &&
			xevent->xkey.keycode == filter->m_keycode) ? True : False;
}

void
CXWindowsScreen::handleSystemEvent(const CEvent& event, void*)
{
	XEvent* xevent = reinterpret_cast<XEvent*>(event.getData());
	assert(xevent != NULL);		

#if HAVE_X11_EXTENSIONS_XINPUT2_H
	CXWindowsKeyState *l_keyState = NULL;
	std::list<UInt8> kbdIDs;
	std::list<UInt8> ptrIDs;
	std::list<UInt8>::iterator i;
	UInt8 id = 0;

	// let screen saver have a go
	if (m_screensaver->handleXEvent(xevent)) {
		// screen saver handled it
		return;
	}
	

		
	// No XInput Extension 2 event
	switch (xevent->type) {
	case CreateNotify:
		LOG((CLOG_DEBUG1 "CreateNotify %d",xevent->type));	 	
		if (m_isPrimary) {
			// select events on new window
			selectEvents(xevent->xcreatewindow.window);
		}	
		break;

	case MappingNotify:
		LOG((CLOG_DEBUG "MappingNotify %d",xevent->type));	 	
		m_dev->getAllKeyboardIDs(kbdIDs);
		for(i=kbdIDs.begin(); i != kbdIDs.end(); ++i)
		{
		    LOG((CLOG_DEBUG1 "refreshing keyboard %d",*i));
		    refreshKeyboard(xevent, *i);
		}
		break;

	case SelectionClear:
		{
			LOG((CLOG_DEBUG "SelectionClear %d",xevent->type));
			// we just lost the selection.  that means someone else
			// grabbed the selection so this screen is now the
			// selection owner.  report that to the receiver.
			ClipboardID cId = getClipboardID(xevent->xselectionclear.selection);
			if (cId != kClipboardEnd) {
				LOG((CLOG_DEBUG "lost clipboard %d ownership at time %d", cId, xevent->xselectionclear.time));
				m_clipboard[cId]->lost(xevent->xselectionclear.time);
				sendClipboardEvent(getClipboardGrabbedEvent(), cId);
				return;
			}
		}
		break;

	case SelectionNotify:
	  LOG((CLOG_DEBUG "SelectionNotify %d",xevent->type));
		// notification of selection transferred.  we shouldn't
		// get this here because we handle them in the selection
		// retrieval methods.  we'll just delete the property
		// with the data (satisfying the usual ICCCM protocol).
		if (xevent->xselection.property != None) {
			XDeleteProperty(m_display,
								xevent->xselection.requestor,
								xevent->xselection.property);
		}
		break;

	case SelectionRequest:
		{
		  LOG((CLOG_DEBUG "SelectionRequest %d",xevent->type));
			// somebody is asking for clipboard data
			ClipboardID cId = getClipboardID(
								xevent->xselectionrequest.selection);
			if (cId != kClipboardEnd) {
				m_clipboard[cId]->addRequest(
								xevent->xselectionrequest.owner,
								xevent->xselectionrequest.requestor,
								xevent->xselectionrequest.target,
								xevent->xselectionrequest.time,
								xevent->xselectionrequest.property);
				return;
			}
		}
		break;

	case PropertyNotify:
	  LOG((CLOG_DEBUG1 "PropertyNotify %d",xevent->type));
		// property delete may be part of a selection conversion
		if (xevent->xproperty.state == PropertyDelete) {
			processClipboardRequest(xevent->xproperty.window,
								xevent->xproperty.time,
								xevent->xproperty.atom);
		}
		break;

	case DestroyNotify:
	  LOG((CLOG_DEBUG1 "DestroyNotify %d",xevent->type));
		// looks like one of the windows that requested a clipboard
		// transfer has gone bye-bye.
		destroyClipboardRequest(xevent->xdestroywindow.window);
		break;

	default:
#if HAVE_XKB_EXTENSION
		if (m_xkb && xevent->type == m_xkbEventBase) {
			XkbEvent* xkbEvent = reinterpret_cast<XkbEvent*>(xevent);
			switch (xkbEvent->any.xkb_type) {
			case XkbMapNotify:
				LOG((CLOG_DEBUG "XkbMapNotify %d",xevent->type));
				m_dev->getAllKeyboardIDs(kbdIDs);
				for(i=kbdIDs.begin(); i != kbdIDs.end(); ++i)   
				    refreshKeyboard(xevent, *i);
				return;

			case XkbStateNotify:
				LOG((CLOG_DEBUG "XkbStateNotify %d",xevent->type));
				LOG((CLOG_INFO "group change: %d", xkbEvent->state.group));
				m_dev->getAllKeyboardIDs(kbdIDs);
				for(i=kbdIDs.begin(); i != kbdIDs.end(); ++i)   
				{
				    l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(*i);
				    l_keyState->setActiveGroup((SInt32)xkbEvent->state.group, *i);
				}
				return;
			}
		}
#endif
		break;
	}
	
	XGenericEventCookie *cookie = (XGenericEventCookie*)&xevent->xcookie;
        if (XGetEventData(m_display, cookie))
	{
	    if(cookie->type == GenericEvent && cookie->extension == m_xiOpCode)
	    {
		if(!cookie->data)
		{
		    LOG((CLOG_DEBUG2 "No cookie->data!!!"));
                    LOG((CLOG_DEBUG2 "  cookie: %d",cookie->cookie));
                    LOG((CLOG_DEBUG2 "  evtype: %d",cookie->evtype));
                    LOG((CLOG_DEBUG2 "  extension: %d",cookie->extension));
                    LOG((CLOG_DEBUG2 "  send_event: %d",cookie->send_event));
                    LOG((CLOG_DEBUG2 "  serial: %d",cookie->serial));
                    LOG((CLOG_DEBUG2 "  type: %d",cookie->type));
		    XFreeEventData(m_display, cookie);
		    return;
		}
		
		XIDeviceEvent *xi_dev_event = static_cast<XIDeviceEvent*>(cookie->data);
		//LOG((CLOG_DEBUG2 "XI Event: %d \n",xi_dev_event->evtype));

		id = xi_dev_event->deviceid;
		m_dev->getAllDeviceIDs(ptrIDs);
		bool isHandled = false;
		for(i=ptrIDs.begin(); i != ptrIDs.end(); ++i)
		{
		    if(id == *i)
		      isHandled = true;
		}
		if(!isHandled)
		{
		  LOG((CLOG_DEBUG2 "Device ID(%d) is not on our list!",id));
		  return;
		}
		// update key state
		bool isRepeat = false;
		if (m_isPrimary) {
			if (xevent->type == XI_KeyRelease) {
				// check if this is a key repeat by getting the next
				// KeyPress event that has the same key and time as
				// this release event, if any.  first prepare the
				// filter info.
				CKeyEventFilter filter;
				filter.m_event   = KeyPress;
				filter.m_window  = xevent->xkey.window;
				filter.m_time    = xevent->xkey.time;
				filter.m_keycode = xevent->xkey.keycode;
				XEvent xevent2;
				isRepeat = (XCheckIfEvent(m_display, &xevent2,
							&CXWindowsScreen::findKeyEvent,
							(XPointer)&filter) == True);
			}

			if (xevent->type == KeyPress || xevent->type == KeyRelease) {
				if (xevent->xkey.window == m_root) {
					// this is a hot key
					onHotKey(xevent->xkey, isRepeat);
					return;
				}
				else if (!m_dev->isOnScreen(id)) {
				      // this might be a hot key
				      if (onHotKey(xevent->xkey, isRepeat)) {
						return;
					}
				}
	
				bool down             = (isRepeat || xevent->type == KeyPress);
				// FIXXME handleSystemEvent: mapModifiersFromX
				l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);
				KeyModifierMask state = l_keyState->mapModifiersFromX(xevent->xkey.state);
				l_keyState->onKey(xevent->xkey.keycode, down, state);
			}
		}

		// let input methods try to handle event first
		if (m_ic != NULL) {
			// XFilterEvent() may eat the event and generate a new KeyPress
			// event with a keycode of 0 because there isn't an actual key
			// associated with the keysym.  but the KeyRelease may pass
			// through XFilterEvent() and keep its keycode.  this means
			// there's a mismatch between KeyPress and KeyRelease keycodes.
			// since we use the keycode on the client to detect when a key
			// is released this won't do.  so we remember the keycode on
			// the most recent KeyPress (and clear it on a matching
			// KeyRelease) so we have a keycode for a synthesized KeyPress.
			if (xevent->type == KeyPress && xevent->xkey.keycode != 0) {
				m_lastKeycode = xevent->xkey.keycode;
			}
			else if (xevent->type == KeyRelease &&
				xevent->xkey.keycode == m_lastKeycode) {
				m_lastKeycode = 0;
			}
	
			// now filter the event
			if (XFilterEvent(xevent, None)) {
				if (xevent->type == KeyPress) {
					// add filtered presses to the filtered list
					m_filtered.insert(m_lastKeycode);
				}
				return;
			}

			// discard matching key releases for key presses that were
			// filtered and remove them from our filtered list.
			else if (xevent->type == KeyRelease &&
				m_filtered.count(xevent->xkey.keycode) > 0) {
				m_filtered.erase(xevent->xkey.keycode);
				return;
			}
		}

                switch (cookie->evtype) {		  
                case XI_KeyPress:
                        if (m_isPrimary) {			    
                            const XIDeviceEvent& press =
                            *(reinterpret_cast<XIDeviceEvent*>(cookie->data));
			    LOG((CLOG_DEBUG1 "XI_Keypress dev=%d", press.deviceid));
                            onKeyPress(press);
                        }
                        XFreeEventData(m_display, cookie);
                        return;			
                case XI_KeyRelease:
        		if (m_isPrimary) {
                            const XIDeviceEvent& release =
                            *(reinterpret_cast<XIDeviceEvent*>(cookie->data));
			    LOG((CLOG_DEBUG1 "XI_KeyRelease dev=%d, isRepeat: %d", release.deviceid, isRepeat));
                            onKeyRelease(release, isRepeat);
                        }
                        XFreeEventData(m_display, cookie);
                        return;
                case XI_ButtonPress:
        		if (m_isPrimary) {
                            const XIDeviceEvent& press =
                            *(reinterpret_cast<XIDeviceEvent*>(cookie->data));
                            onMousePress(press);
                        }
                        XFreeEventData(m_display, cookie);
                        return;
                case XI_ButtonRelease:
        		if (m_isPrimary) {
                            const XIDeviceEvent& release = 
                            *(reinterpret_cast<XIDeviceEvent*>(cookie->data));
                            onMouseRelease(release);
                        }
                        XFreeEventData(m_display, cookie);
                        return;

                case XI_Motion:
//                         This is for local events. If we're remote, we use
//                         raw events - no need for delta calculation :)
//        		if (m_isPrimary && m_isOnScreen) {
//                        if(cookie->data){
//                            const XIDeviceEvent *motion_ptr = reinterpret_cast<XIDeviceEvent*>(cookie->data);
//                            printf("    device: %d (%d)\n", motion_ptr->deviceid, motion_ptr->sourceid);
//                            print_deviceevent(motion_ptr);
//                            LOG((CLOG_DEBUG2 "A\n"));
//                            XIDeviceEvent *evd = (XIDeviceEvent*)cookie->data;
//                            LOG((CLOG_DEBUG "Device Motion value x: %f \n", evd->root_x));
//                            const XIDeviceEvent& motion = *(reinterpret_cast<XIDeviceEvent*>(cookie->data));
//                            LOG((CLOG_DEBUG2 "B\n"));
//                            LOG((CLOG_DEBUG2 "XI Motion Reference values x: %f y: %f\n", motion.root_x, motion.root_y));
//                            onMouseMove(motion);
//                        }
//                        else{
//                            LOG((CLOG_DEBUG2 "No cookie->data!!!"));
//                            LOG((CLOG_DEBUG2 "  cookie: %d",cookie->cookie));
//                            LOG((CLOG_DEBUG2 "  evtype: %d",cookie->evtype));
//                            LOG((CLOG_DEBUG2 "  extension: %d",cookie->extension));
//                            LOG((CLOG_DEBUG2 "  send_event: %d",cookie->send_event));
//                            LOG((CLOG_DEBUG2 "  serial: %d",cookie->serial));
//                            LOG((CLOG_DEBUG2 "  type: %d",cookie->type));
//                        }
                        LOG((CLOG_DEBUG2 "Only RawMotion is supported!"));
                        XFreeEventData(m_display, cookie);
                        return;
                case XI_RawMotion:
                        {
                            //XIRawEvent *ev = reinterpret_cast<XIRawEvent*>(cookie->data);
                            // We read the motion deltas from the raw event
                            // directly and send those to the client.
                            // If detail != 0 then the event was probably
                            // from a button or key.
                            //if(m_isPrimary && !m_isOnScreen){
                            if(cookie->data)
			    {
                                //LOG((CLOG_DEBUG2 "Raw Motion value"));
                                const XIRawEvent& raw_motion = *(reinterpret_cast<XIRawEvent*>(cookie->data));
                                onMouseMove(raw_motion);
                            }
                            else
			    {
                                LOG((CLOG_DEBUG2 "No cookie->data!!!"));
                                LOG((CLOG_DEBUG2 "  cookie: %d",cookie->cookie));
                                LOG((CLOG_DEBUG2 "  evtype: %d",cookie->evtype));
                                LOG((CLOG_DEBUG2 "  extension: %d",cookie->extension));
                                LOG((CLOG_DEBUG2 "  send_event: %d",cookie->send_event));
                                LOG((CLOG_DEBUG2 "  serial: %d",cookie->serial));
                                LOG((CLOG_DEBUG2 "  type: %d",cookie->type));
                            }
                        }                        
                        XFreeEventData(m_display, cookie);
                        return;
                }
            }
        }

	
#else   // No XInput2
	// update key state
	bool isRepeat = false;
	if (m_isPrimary) {
		if (xevent->type == KeyRelease) {
			// check if this is a key repeat by getting the next
			// KeyPress event that has the same key and time as
			// this release event, if any.  first prepare the
			// filter info.
			CKeyEventFilter filter;
			filter.m_event   = KeyPress;
			filter.m_window  = xevent->xkey.window;
			filter.m_time    = xevent->xkey.time;
			filter.m_keycode = xevent->xkey.keycode;
			XEvent xevent2;
			isRepeat = (XCheckIfEvent(m_display, &xevent2,
							&CXWindowsScreen::findKeyEvent,
							(XPointer)&filter) == True);
		}

		if (xevent->type == KeyPress || xevent->type == KeyRelease) {
			if (xevent->xkey.window == m_root) {
				// this is a hot key
				onHotKey(xevent->xkey, isRepeat);
				return;
			}
			else if (!m_isOnScreen) {
				// this might be a hot key
				if (onHotKey(xevent->xkey, isRepeat)) {
					return;
				}
			}

			bool down             = (isRepeat || xevent->type == KeyPress);
			KeyModifierMask state =
				m_keyState->mapModifiersFromX(xevent->xkey.state);
			m_keyState->onKey(xevent->xkey.keycode, down, state);
		}
	}

	// let input methods try to handle event first
	if (m_ic != NULL) {
		// XFilterEvent() may eat the event and generate a new KeyPress
		// event with a keycode of 0 because there isn't an actual key
		// associated with the keysym.  but the KeyRelease may pass
		// through XFilterEvent() and keep its keycode.  this means
		// there's a mismatch between KeyPress and KeyRelease keycodes.
		// since we use the keycode on the client to detect when a key
		// is released this won't do.  so we remember the keycode on
		// the most recent KeyPress (and clear it on a matching
		// KeyRelease) so we have a keycode for a synthesized KeyPress.
		if (xevent->type == KeyPress && xevent->xkey.keycode != 0) {
			m_lastKeycode = xevent->xkey.keycode;
		}
		else if (xevent->type == KeyRelease &&
			xevent->xkey.keycode == m_lastKeycode) {
			m_lastKeycode = 0;
		}

		// now filter the event
		if (XFilterEvent(xevent, None)) {
			if (xevent->type == KeyPress) {
				// add filtered presses to the filtered list
				m_filtered.insert(m_lastKeycode);
			}
			return;
		}

		// discard matching key releases for key presses that were
		// filtered and remove them from our filtered list.
		else if (xevent->type == KeyRelease &&
			m_filtered.count(xevent->xkey.keycode) > 0) {
			m_filtered.erase(xevent->xkey.keycode);
			return;
		}
	}

	// let screen saver have a go
	if (m_screensaver->handleXEvent(xevent)) {
		// screen saver handled it
		return;
	}

	// handle the event ourself
	switch (xevent->type) {
	case CreateNotify:
		if (m_isPrimary) {
			// select events on new window
			selectEvents(xevent->xcreatewindow.window);
		}
		break;

	case MappingNotify:
		refreshKeyboard(xevent);
		break;

	case LeaveNotify:
		if (!m_isPrimary) {
			// mouse moved out of hider window somehow.  hide the window.
			XUnmapWindow(m_display, m_window);
		}
		break;

	case SelectionClear:
		{
			// we just lost the selection.  that means someone else
			// grabbed the selection so this screen is now the
			// selection owner.  report that to the receiver.
			ClipboardID cId = getClipboardID(xevent->xselectionclear.selection);
			if (cId != kClipboardEnd) {
				LOG((CLOG_DEBUG "lost clipboard %d ownership at time %d", cId, xevent->xselectionclear.time));
				m_clipboard[cId]->lost(xevent->xselectionclear.time);
				sendClipboardEvent(getClipboardGrabbedEvent(), cId);
				return;
			}
		}
		break;

	case SelectionNotify:
		// notification of selection transferred.  we shouldn't
		// get this here because we handle them in the selection
		// retrieval methods.  we'll just delete the property
		// with the data (satisfying the usual ICCCM protocol).
		if (xevent->xselection.property != None) {
			XDeleteProperty(m_display,
								xevent->xselection.requestor,
								xevent->xselection.property);
		}
		break;

	case SelectionRequest:
		{
			// somebody is asking for clipboard data
			ClipboardID cId = getClipboardID(
								xevent->xselectionrequest.selection);
			if (cId != kClipboardEnd) {
				m_clipboard[cId]->addRequest(
								xevent->xselectionrequest.owner,
								xevent->xselectionrequest.requestor,
								xevent->xselectionrequest.target,
								xevent->xselectionrequest.time,
								xevent->xselectionrequest.property);
				return;
			}
		}
		break;

	case PropertyNotify:
		// property delete may be part of a selection conversion
		if (xevent->xproperty.state == PropertyDelete) {
			processClipboardRequest(xevent->xproperty.window,
								xevent->xproperty.time,
								xevent->xproperty.atom);
		}
		break;

	case DestroyNotify:
		// looks like one of the windows that requested a clipboard
		// transfer has gone bye-bye.
		destroyClipboardRequest(xevent->xdestroywindow.window);
		break;

	case KeyPress:
		if (m_isPrimary) {
			onKeyPress(xevent->xkey);
		}
		return;

	case KeyRelease:
		if (m_isPrimary) {
			onKeyRelease(xevent->xkey, isRepeat);
		}
		return;

	case ButtonPress:
		if (m_isPrimary) {
			onMousePress(xevent->xbutton);
		}
		return;

	case ButtonRelease:
		if (m_isPrimary) {
			onMouseRelease(xevent->xbutton);
		}
		return;

	case MotionNotify:
		if (m_isPrimary) {
			onMouseMove(xevent->xmotion);
		}
		return;

	default:
#if HAVE_XKB_EXTENSION
		if (m_xkb && xevent->type == m_xkbEventBase) {
			XkbEvent* xkbEvent = reinterpret_cast<XkbEvent*>(xevent);
			switch (xkbEvent->any.xkb_type) {
			case XkbMapNotify:
				refreshKeyboard(xevent);
				return;

			case XkbStateNotify:
				LOG((CLOG_INFO "group change: %d", xkbEvent->state.group));
				m_keyState->setActiveGroup((SInt32)xkbEvent->state.group);
				return;
			}
		}
#endif
		break;
	}
#endif // XI < 2
}

#if HAVE_X11_EXTENSIONS_XINPUT2_H

void
CXWindowsScreen::onKeyPress(const XIDeviceEvent& press)
{
	UInt8 id = press.deviceid;
	CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);
	
	LOG((CLOG_DEBUG1 "event: KeyPress code=%d, base=%d, latched=%d, locked=%d, effective=%d", 
			  press.detail, press.mods.base,press.mods.latched, press.mods.locked, press.mods.effective));
// 	const KeyModifierMask mask = l_keyState->mapModifiersFromX(xkey.state);
	const KeyModifierMask mask = l_keyState->mapModifiersFromX(press.mods.effective);
	KeyID key = mapKeyFromX(press);	
	if (key != kKeyNone) {
		// check for ctrl+alt+del emulation
		if ((key == kKeyPause || key == kKeyBreak) &&
			(mask & (KeyModifierControl | KeyModifierAlt)) ==
					(KeyModifierControl | KeyModifierAlt)) {
			// pretend it's ctrl+alt+del
			LOG((CLOG_DEBUG "emulate ctrl+alt+del"));
			key = kKeyDelete;
		}

		// get which button.  see call to XFilterEvent() in onEvent()
		// for more info.
		bool isFake = false;
		KeyButton keycode = static_cast<KeyButton>(press.detail);
		if (keycode == 0) {
			isFake  = true;
			keycode = static_cast<KeyButton>(m_lastKeycode);
			if (keycode == 0) {
				// no keycode
				return;
			}
		}

		LOG((CLOG_DEBUG1 "CXWindowsScreen::onKeyPress dev=%d, keycode=0x%08x, mask=0x%04x, keysym=0x%04x", id, keycode, mask, key));
		// handle key
		l_keyState->sendKeyEvent(getEventTarget(), true, false, key, mask, 1, keycode);

		// do fake release if this is a fake press
		if (isFake) {
		    l_keyState->sendKeyEvent(getEventTarget(), false, false, key, mask, 1, keycode);
		}
	}
}
void
CXWindowsScreen::onKeyRelease(const XIDeviceEvent& release, bool isRepeat)
{
	UInt8 id = release.deviceid;
	CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);
	
	LOG((CLOG_DEBUG1 "event: KeyRelease code=%d, base=%d, latched=%d, locked=%d, effective=%d", 
			  release.detail, release.mods.base,release.mods.latched, release.mods.locked, release.mods.effective));
	
	const KeyModifierMask mask = l_keyState->mapModifiersFromX(release.mods.effective);
	KeyID key = mapKeyFromX(release);
	if (key != kKeyNone) {
		// check for ctrl+alt+del emulation
		if ((key == kKeyPause || key == kKeyBreak) &&
			(mask & (KeyModifierControl | KeyModifierAlt)) ==
					(KeyModifierControl | KeyModifierAlt)) {
			// pretend it's ctrl+alt+del and ignore autorepeat
			LOG((CLOG_DEBUG "emulate ctrl+alt+del"));
			key      = kKeyDelete;
			isRepeat = false;
		}

		KeyButton keycode = static_cast<KeyButton>(release.detail);
		LOG((CLOG_DEBUG1 "CXWindowsScreen::onKeyRelease dev=%d, keycode=0x%08x, mask=0x%04x, keysym=0x%04x", id, keycode, mask, key));

		if (!isRepeat) {
			// no press event follows so it's a plain release
			LOG((CLOG_DEBUG1 "event: KeyRelease code=%d, state=0x%04x", keycode, release.mods.effective));
			l_keyState->sendKeyEvent(getEventTarget(), false, false, key, mask, 1, keycode);
		}
		else {
			// found a press event following so it's a repeat.
			// we could attempt to count the already queued
			// repeats but we'll just send a repeat of 1.
			// note that we discard the press event.
			LOG((CLOG_DEBUG1 "event: repeat code=%d, state=0x%04x", keycode, release.mods.effective));
			l_keyState->sendKeyEvent(getEventTarget(), false, true, key, mask, 1, keycode);
		}
	}
}
bool
CXWindowsScreen::onHotKey(XKeyEvent& xkey, bool isRepeat)
{
	// find the hot key id
	HotKeyToIDMap::const_iterator i =
		m_hotKeyToIDMap.find(CHotKeyItem(xkey.keycode, xkey.state));
	if (i == m_hotKeyToIDMap.end()) {
		return false;
	}

	// find what kind of event
	CEvent::Type type;
	if (xkey.type == KeyPress) {
		type = getHotKeyDownEvent();
	}
	else if (xkey.type == KeyRelease) {
		type = getHotKeyUpEvent();
	}
	else {
		return false;
	}

	// generate event (ignore key repeats)
	if (!isRepeat) {
		EVENTQUEUE->addEvent(CEvent(type, getEventTarget(),
								CHotKeyInfo::alloc(i->second)));
	}
	return true;
}
void
CXWindowsScreen::onMousePress(const XIDeviceEvent& press)
{
    LOG((CLOG_DEBUG1 "event: XI_ButtonPress button=%d", press.detail));
    const XIDeviceEvent *myevent = &press;
    UInt8 id = myevent->deviceid;
    CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);
    ButtonID button = mapButtonFromX(press.detail);
    KeyModifierMask mask = l_keyState->mapModifiersFromX(press.mods.effective);
    if (button != kButtonNone) {
        LOG((CLOG_DEBUG1 "sending button=%d", press.detail));
        sendEvent(getButtonDownEvent(), CButtonInfo::alloc(button, id, mask));
    }
}

void
CXWindowsScreen::onMouseRelease(const XIDeviceEvent& release)
{
	LOG((CLOG_DEBUG1 "event: XI_ButtonRelease button=%d", release.detail));
	UInt8 id = release.deviceid;
	CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);
	ButtonID button      = mapButtonFromX(release.detail);
	KeyModifierMask mask = l_keyState->mapModifiersFromX(release.mods.effective);
	if (button != kButtonNone) {
		sendEvent(getButtonUpEvent(), CButtonInfo::alloc(button, id, mask));
	}
        else if (release.detail == 4) {
		// wheel forward (away from user)
		sendEvent(getWheelEvent(), CWheelInfo::alloc(0, 120, id));
	}
        else if (release.detail == 5) {
		// wheel backward (toward user)
		sendEvent(getWheelEvent(), CWheelInfo::alloc(0, -120, id));
	}
	// XXX -- support x-axis scrolling
}

void
CXWindowsScreen::onMouseMove(const XIRawEvent& motion)
{
    //LOG((CLOG_DEBUG2 "onMotion x: %.2f y: %.2f", motion.root_x, motion.root_y));
    // save position to compute delta of next motion

    const double *val = motion.valuators.values;
    UInt8 id = motion.deviceid;
    SInt32 xCursor, yCursor;
    getCursorPos(xCursor, yCursor, id);

    //LOG((CLOG_DEBUG "Last Cursor Position dev(%d): x: %.2f, y: %.2f", id, xCursor, yCursor));

    double x = 0;
    double y = 0;
    
    for(int i = 0; i < motion.valuators.mask_len * 8; i++) {
//        printf("MaskIsSet %d ",XIMaskIsSet(motion.valuators.mask, i));
        if (XIMaskIsSet(motion.valuators.mask, i)) {
            //LOG((CLOG_DEBUG2 "Value %.2f", *val++));
            if(i == 0){
                //LOG((CLOG_DEBUG2 "x val: %.2f",*val++));
                x = *val++;
                //LOG((CLOG_DEBUG2 "x: %.2f",x));
            }
            if(i == 1){
                //LOG((CLOG_DEBUG2 "y val: %.2f",*val++));
                y = *val++;
                //LOG((CLOG_DEBUG2 "y: %.2f",y));
            }
        }
    }
        
    if (m_dev->isOnScreen(id)) {
        // motion on primary screen
        sendEvent(getMotionOnPrimaryEvent(), CMotionInfo::alloc(xCursor, yCursor, id));
    }
    else{
        if (x != 0 || y != 0) {
            LOG((CLOG_DEBUG2 "sending raw motion event from dev(%d): x: %d, y:%d",id, int(x),int(y)));
            sendEvent(getMotionOnSecondaryEvent(), CMotionInfo::alloc(int(x), int(y), id));
        }
    }
}

#else // XInput < 2
void
CXWindowsScreen::onKeyPress(XKeyEvent& xkey)
{
	LOG((CLOG_DEBUG1 "event: KeyPress code=%d, state=0x%04x", xkey.keycode, xkey.state));
	const KeyModifierMask mask = m_keyState->mapModifiersFromX(xkey.state);
	KeyID key                  = mapKeyFromX(&xkey);
	if (key != kKeyNone) {
		// check for ctrl+alt+del emulation
		if ((key == kKeyPause || key == kKeyBreak) &&
			(mask & (KeyModifierControl | KeyModifierAlt)) ==
					(KeyModifierControl | KeyModifierAlt)) {
			// pretend it's ctrl+alt+del
			LOG((CLOG_DEBUG "emulate ctrl+alt+del"));
			key = kKeyDelete;
		}

		// get which button.  see call to XFilterEvent() in onEvent()
		// for more info.
		bool isFake = false;
		KeyButton keycode = static_cast<KeyButton>(xkey.keycode);
		if (keycode == 0) {
			isFake  = true;
			keycode = static_cast<KeyButton>(m_lastKeycode);
			if (keycode == 0) {
				// no keycode
				return;
			}
		}

		// handle key
		m_keyState->sendKeyEvent(getEventTarget(),
							true, false, key, mask, 1, keycode);

		// do fake release if this is a fake press
		if (isFake) {
			m_keyState->sendKeyEvent(getEventTarget(),
							false, false, key, mask, 1, keycode);
		}
	}
}

void
CXWindowsScreen::onKeyRelease(XKeyEvent& xkey, bool isRepeat)
{
	const KeyModifierMask mask = m_keyState->mapModifiersFromX(xkey.state);
	KeyID key                  = mapKeyFromX(&xkey);
	if (key != kKeyNone) {
		// check for ctrl+alt+del emulation
		if ((key == kKeyPause || key == kKeyBreak) &&
			(mask & (KeyModifierControl | KeyModifierAlt)) ==
					(KeyModifierControl | KeyModifierAlt)) {
			// pretend it's ctrl+alt+del and ignore autorepeat
			LOG((CLOG_DEBUG "emulate ctrl+alt+del"));
			key      = kKeyDelete;
			isRepeat = false;
		}

		KeyButton keycode = static_cast<KeyButton>(xkey.keycode);
		if (!isRepeat) {
			// no press event follows so it's a plain release
			LOG((CLOG_DEBUG1 "event: KeyRelease code=%d, state=0x%04x", keycode, xkey.state));
			m_keyState->sendKeyEvent(getEventTarget(),
							false, false, key, mask, 1, keycode);
		}
		else {
			// found a press event following so it's a repeat.
			// we could attempt to count the already queued
			// repeats but we'll just send a repeat of 1.
			// note that we discard the press event.
			LOG((CLOG_DEBUG1 "event: repeat code=%d, state=0x%04x", keycode, xkey.state));
			m_keyState->sendKeyEvent(getEventTarget(),
							false, true, key, mask, 1, keycode);
		}
	}
}

bool
CXWindowsScreen::onHotKey(XKeyEvent& xkey, bool isRepeat)
{
	// find the hot key id
	HotKeyToIDMap::const_iterator i =
		m_hotKeyToIDMap.find(CHotKeyItem(xkey.keycode, xkey.state));
	if (i == m_hotKeyToIDMap.end()) {
		return false;
	}

	// find what kind of event
	CEvent::Type type;
	if (xkey.type == KeyPress) {
		type = getHotKeyDownEvent();
	}
	else if (xkey.type == KeyRelease) {
		type = getHotKeyUpEvent();
	}
	else {
		return false;
	}

	// generate event (ignore key repeats)
	if (!isRepeat) {
		EVENTQUEUE->addEvent(CEvent(type, getEventTarget(),
								CHotKeyInfo::alloc(i->second)));
	}
	return true;
}

void
CXWindowsScreen::onMousePress(const XButtonEvent& xbutton)
{
	LOG((CLOG_DEBUG1 "event: ButtonPress button=%d", xbutton.button));
	ButtonID button      = mapButtonFromX(&xbutton);
	KeyModifierMask mask = m_keyState->mapModifiersFromX(xbutton.state);
	if (button != kButtonNone) {
		sendEvent(getButtonDownEvent(), CButtonInfo::alloc(button, mask));
	}
}

void
CXWindowsScreen::onMouseRelease(const XButtonEvent& xbutton)
{
	LOG((CLOG_DEBUG1 "event: ButtonRelease button=%d", xbutton.button));
	ButtonID button      = mapButtonFromX(&xbutton);
	KeyModifierMask mask = m_keyState->mapModifiersFromX(xbutton.state);
	if (button != kButtonNone) {
		sendEvent(getButtonUpEvent(), CButtonInfo::alloc(button, mask));
	}
	else if (xbutton.button == 4) {
		// wheel forward (away from user)
		sendEvent(getWheelEvent(), CWheelInfo::alloc(0, 120));
	}
	else if (xbutton.button == 5) {
		// wheel backward (toward user)
		sendEvent(getWheelEvent(), CWheelInfo::alloc(0, -120));
	}
	// XXX -- support x-axis scrolling
}

void
CXWindowsScreen::onMouseMove(const XMotionEvent& xmotion)
{
	LOG((CLOG_DEBUG2 "event: MotionNotify %d,%d", xmotion.x_root, xmotion.y_root));

	// compute motion delta (relative to the last known
	// mouse position)
	SInt32 x = xmotion.x_root - m_xCursor;
	SInt32 y = xmotion.y_root - m_yCursor;

	// save position to compute delta of next motion
	m_xCursor = xmotion.x_root;
	m_yCursor = xmotion.y_root;

	if (xmotion.send_event) {
		// we warped the mouse.  discard events until we
		// find the matching sent event.  see
		// warpCursorNoFlush() for where the events are
		// sent.  we discard the matching sent event and
		// can be sure we've skipped the warp event.
		XEvent xevent;
		char cntr = 0;
		do {
			XMaskEvent(m_display, PointerMotionMask, &xevent);
			if (cntr++ > 10) {
				LOG((CLOG_WARN "too many discarded events! %d", cntr));
				break;
			}
		} while (!xevent.xany.send_event);
		cntr = 0;
	}
	else if (m_isOnScreen) {
		// motion on primary screen
		sendEvent(getMotionOnPrimaryEvent(),
							CMotionInfo::alloc(m_xCursor, m_yCursor));
	}
	else {
		// motion on secondary screen.  warp mouse back to
		// center.
		//
		// my lombard (powerbook g3) running linux and
		// using the adbmouse driver has two problems:
		// first, the driver only sends motions of +/-2
		// pixels and, second, it seems to discard some
		// physical input after a warp.  the former isn't a
		// big deal (we're just limited to every other
		// pixel) but the latter is a PITA.  to work around
		// it we only warp when the mouse has moved more
		// than s_size pixels from the center.
		static const SInt32 s_size = 32;
		if (xmotion.x_root - m_xCenter < -s_size ||
			xmotion.x_root - m_xCenter >  s_size ||
			xmotion.y_root - m_yCenter < -s_size ||
			xmotion.y_root - m_yCenter >  s_size) {
			warpCursorNoFlush(m_xCenter, m_yCenter);
		}

		// send event if mouse moved.  do this after warping
		// back to center in case the motion takes us onto
		// the primary screen.  if we sent the event first
		// in that case then the warp would happen after
		// warping to the primary screen's enter position,
		// effectively overriding it.
		if (x != 0 || y != 0) {
			sendEvent(getMotionOnSecondaryEvent(), CMotionInfo::alloc(x, y));
		}
	}
}

#endif // all on<Device>

Cursor
CXWindowsScreen::createBlankCursor() const
{
	// this seems just a bit more complicated than really necessary

	// get the closet cursor size to 1x1
	unsigned int w, h;
	XQueryBestCursor(m_display, m_root, 1, 1, &w, &h);

	// make bitmap data for cursor of closet size.  since the cursor
	// is blank we can use the same bitmap for shape and mask:  all
	// zeros.
	const int size = ((w + 7) >> 3) * h;
	char* data = new char[size];
	memset(data, 0, size);

	// make bitmap
	Pixmap bitmap = XCreateBitmapFromData(m_display, m_root, data, w, h);

	// need an arbitrary color for the cursor
	XColor color;
	color.pixel = 0;
	color.red   = color.green = color.blue = 0;
	color.flags = DoRed | DoGreen | DoBlue;

	// make cursor from bitmap
	Cursor cursor = XCreatePixmapCursor(m_display, bitmap, bitmap,
								&color, &color, 0, 0);

	// don't need bitmap or the data anymore
	delete[] data;
	XFreePixmap(m_display, bitmap);

	return cursor;
}

ClipboardID
CXWindowsScreen::getClipboardID(Atom selection) const
{
	for (ClipboardID cId = 0; cId < kClipboardEnd; ++cId) {
		if (m_clipboard[cId] != NULL &&
			m_clipboard[cId]->getSelection() == selection) {
			return cId;
		}
	}
	return kClipboardEnd;
}

void
CXWindowsScreen::processClipboardRequest(Window requestor,
				Time time, Atom property)
{
  	LOG((CLOG_DEBUG1 "processClipboardRequest"));	 	
	// check every clipboard until one returns success
	for (ClipboardID cId = 0; cId < kClipboardEnd; ++cId) {
		if (m_clipboard[cId] != NULL &&
			m_clipboard[cId]->processRequest(requestor, time, property)) {
			break;
		}
	}
}

void
CXWindowsScreen::destroyClipboardRequest(Window requestor)
{
	// check every clipboard until one returns success
	for (ClipboardID cId = 0; cId < kClipboardEnd; ++cId) {
		if (m_clipboard[cId] != NULL &&
			m_clipboard[cId]->destroyRequest(requestor)) {
			break;
		}
	}
}

void
CXWindowsScreen::onError()
{
	// prevent further access to the X display
	EVENTQUEUE->adoptBuffer(NULL);
	m_screensaver->destroy();
	m_screensaver = NULL;
	m_display     = NULL;

	// notify of failure
	sendEvent(getErrorEvent(), NULL);

	// FIXME -- should ensure that we ignore operations that involve
	// m_display from now on.  however, Xlib will simply exit the
	// application in response to the X I/O error so there's no
	// point in trying to really handle the error.  if we did want
	// to handle the error, it'd probably be easiest to delegate to
	// one of two objects.  one object would take the implementation
	// from this class.  the other object would be stub methods that
	// don't use X11.  on error, we'd switch to the latter.
}

int
CXWindowsScreen::ioErrorHandler(Display*)
{
	// the display has disconnected, probably because X is shutting
	// down.  X forces us to exit at this point which is annoying.
	// we'll pretend as if we won't exit so we try to make sure we
	// don't access the display anymore.
	LOG((CLOG_CRIT "X display has unexpectedly disconnected"));
	s_screen->onError();
	return 0;
}

void
CXWindowsScreen::selectEvents(Window w) const
{
	// ignore errors while we adjust event masks.  windows could be
	// destroyed at any time after the XQueryTree() in doSelectEvents()
	// so we must ignore BadWindow errors.
	CXWindowsUtil::CErrorLock lock(m_display);

	XIEventMask mask;    
	/* Select for motion events */
	mask.deviceid = XIAllMasterDevices;
	mask.mask_len = XIMaskLen(XI_RawMotion);
	mask.mask = (unsigned char*)calloc(mask.mask_len, sizeof(char));
	XISetMask(mask.mask, XI_RawMotion);
	XISelectEvents(m_display, m_root, &mask, 1);
	free(mask.mask);
	
	// adjust event masks
	doSelectEvents(w);
}

void
CXWindowsScreen::doSelectEvents(Window w) const
{
	// we want to track the mouse everywhere on the display.  to achieve
	// that we select PointerMotionMask on every window.  we also select
	// SubstructureNotifyMask in order to get CreateNotify events so we
	// select events on new windows too.
	//
	// note that this can break certain clients due a design flaw of X.
	// X will deliver a PointerMotion event to the deepest window in the
	// hierarchy that contains the pointer and has PointerMotionMask
	// selected by *any* client.  if another client doesn't select
	// motion events in a subwindow so the parent window will get them
	// then by selecting for motion events on the subwindow we break
	// that client because the parent will no longer get the events.

	// FIXME -- should provide some workaround for event selection
	// design flaw.  perhaps only select for motion events on windows
	// that already do or are top-level windows or don't propagate
	// pointer events.  or maybe an option to simply poll the mouse.

	// we don't want to adjust our grab window
	if (w == m_window) {
		return;
	}

	// select events of interest.  do this before querying the tree so
	// we'll get notifications of children created after the XQueryTree()
	// so we won't miss them.
#if HAVE_X11_EXTENSIONS_XINPUT2_H
    XIEventMask mask;
    /* Select for motion events */    
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = XIMaskLen(XI_RawMotion);
    mask.mask = (unsigned char*)calloc(mask.mask_len, sizeof(char));
    XISetMask(mask.mask, XI_RawMotion);
    //XISetMask(mask.mask, XI_Motion);
    XISelectEvents(m_display, w, &mask, 1);
    XSelectInput(m_display, w, SubstructureNotifyMask);
    free(mask.mask); 
#else
	XSelectInput(m_display, w, PointerMotionMask | SubstructureNotifyMask);
#endif
	// recurse on child windows
	Window rw, pw, *cw;
	unsigned int nc;
	if (XQueryTree(m_display, w, &rw, &pw, &cw, &nc)) {
		for (unsigned int i = 0; i < nc; ++i) {
			doSelectEvents(cw[i]);
		}
		XFree(cw);
	}
}

#if HAVE_X11_EXTENSIONS_XINPUT2_H

KeyID
CXWindowsScreen::mapKeyFromX(const XIDeviceEvent& event) const
{
// 	LOG((CLOG_DEBUG "mapKeyFromX: Opening Device %d", event.deviceid + 2));
// 	int num_codes;
	
	XKeyPressedEvent keyevent;
	keyevent.type			= KeyPress;
	keyevent.serial			= event.serial;
	keyevent.display		= m_display;
	keyevent.root   		= event.root;
	keyevent.window			= event.event;
	keyevent.subwindow  		= event.child;
	keyevent.time        		= event.time;
	keyevent.x           		= event.event_x;
	keyevent.y           		= event.event_y;
	keyevent.x_root      		= event.root_x;
	keyevent.y_root      		= event.root_y;
	keyevent.keycode		= event.detail;
	keyevent.state       		= event.mods.effective;	
	keyevent.same_screen 		= True;	
	keyevent.send_event		= False;

	XDevice *my_kbd = XOpenDevice(m_display, event.deviceid + 2);
	// convert to a keysym
	KeySym keysym;
	KeySym *xikeysym;
	if (event.type == XI_KeyPress && m_ic != NULL) {
		// do multibyte lookup.  can only call XmbLookupString with a
		// key press event and a valid XIC so we checked those above.
		char scratch[32];
		int n        = sizeof(scratch) / sizeof(scratch[0]);
		char* buffer = scratch;
		int status;
		LOG((CLOG_DEBUG "XmbLookupString"));
		n = XmbLookupString(m_ic, &keyevent, buffer, n, &keysym, &status);
		if (status == XBufferOverflow) {
			// not enough space.  grow buffer and try again.
			buffer = new char[n];
			n = XmbLookupString(m_ic, &keyevent, buffer, n, &keysym, &status);
			delete[] buffer;
		}

		// see what we got.  since we don't care about the string
		// we'll just look for a keysym.
		switch (status) {
		default:
		case XLookupNone:
		case XLookupChars:
			keysym = 0;
			break;

		case XLookupKeySym:
		case XLookupBoth:
			break;
		}
		// convert key
		return CXWindowsUtil::mapKeySymToKeyID(keysym);
	}
	else {
		// plain old lookup
		char dummy[1];
		XLookupString(&keyevent, dummy, 0, &keysym, NULL);
// 		LOG((CLOG_CRIT "XGetDeviceKeyMapping"));
// 		xikeysym = XGetDeviceKeyMapping(m_display, my_kbd, event.detail, 1, &num_codes); 
// 		LOG((CLOG_CRIT "XGetDeviceKeyMapping keysym: %d, num_codes: %d", xikeysym, num_codes));
// 		XCloseDevice(m_display, my_kbd);
 		LOG((CLOG_DEBUG "mapKeyFromX: keysym: 0x%04x keycode: %d", keysym, event.detail));
		return CXWindowsUtil::mapKeySymToKeyID(keysym);
	}

}

ButtonID
CXWindowsScreen::mapButtonFromX(const UInt8 xbutton) const
{
	unsigned int button = xbutton;

	// first three buttons map to 1, 2, 3 (kButtonLeft, Middle, Right)
	if (button >= 1 && button <= 3) {
		return static_cast<ButtonID>(button);
	}

	// buttons 4 and 5 are ignored here.  they're used for the wheel.
	// buttons 6, 7, etc and up map to 4, 5, etc.
	else if (button >= 6) {
		return static_cast<ButtonID>(button - 2);
	}

	// unknown button
	else {
		return kButtonNone;
	}

}


unsigned int
CXWindowsScreen::mapButtonToX(ButtonID bId) const
{
	// map button -1 to button 4 (+wheel)
	if (bId == static_cast<ButtonID>(-1)) {
		bId = 4;
	}

	// map button -2 to button 5 (-wheel)
	else if (bId == static_cast<ButtonID>(-2)) {
		bId = 5;
	}

	// map buttons 4, 5, etc. to 6, 7, etc. to make room for buttons
	// 4 and 5 used to simulate the mouse wheel.
	else if (bId >= 4) {
		bId += 2;
	}

	// check button is in legal range
	if (bId < 1 || bId > m_buttons.size()) {
		// out of range
		return 0;
	}

	// map button
	return static_cast<unsigned int>(bId);
}
#else
KeyID
CXWindowsScreen::mapKeyFromX(XKeyEvent* event) const
{
	// convert to a keysym
	KeySym keysym;
	if (event->type == KeyPress && m_ic != NULL) {
		// do multibyte lookup.  can only call XmbLookupString with a
		// key press event and a valid XIC so we checked those above.
		char scratch[32];
		int n        = sizeof(scratch) / sizeof(scratch[0]);
		char* buffer = scratch;
		int status;
		n = XmbLookupString(m_ic, event, buffer, n, &keysym, &status);
		if (status == XBufferOverflow) {
			// not enough space.  grow buffer and try again.
			buffer = new char[n];
			n = XmbLookupString(m_ic, event, buffer, n, &keysym, &status);
			delete[] buffer;
		}

		// see what we got.  since we don't care about the string
		// we'll just look for a keysym.
		switch (status) {
		default:
		case XLookupNone:
		case XLookupChars:
			keysym = 0;
			break;

		case XLookupKeySym:
		case XLookupBoth:
			break;
		}
	}
	else {
		// plain old lookup
		char dummy[1];
		XLookupString(event, dummy, 0, &keysym, NULL);
	}

	// convert key
	return CXWindowsUtil::mapKeySymToKeyID(keysym);
}

ButtonID
CXWindowsScreen::mapButtonFromX(const XButtonEvent* event) const
{
	unsigned int button = event->button;

	// first three buttons map to 1, 2, 3 (kButtonLeft, Middle, Right)
	if (button >= 1 && button <= 3) {
		return static_cast<ButtonID>(button);
	}

	// buttons 4 and 5 are ignored here.  they're used for the wheel.
	// buttons 6, 7, etc and up map to 4, 5, etc.
	else if (button >= 6) {
		return static_cast<ButtonID>(button - 2);
	}

	// unknown button
	else {
		return kButtonNone;
	}
}

unsigned int
CXWindowsScreen::mapButtonToX(ButtonID bId) const
{
	// map button -1 to button 4 (+wheel)
	if (bId == static_cast<ButtonID>(-1)) {
		bId = 4;
	}

	// map button -2 to button 5 (-wheel)
	else if (bId == static_cast<ButtonID>(-2)) {
		bId = 5;
	}

	// map buttons 4, 5, etc. to 6, 7, etc. to make room for buttons
	// 4 and 5 used to simulate the mouse wheel.
	else if (bId >= 4) {
		bId += 2;
	}

	// check button is in legal range
	if (bId < 1 || bId > m_buttons.size()) {
		// out of range
		return 0;
	}

	// map button
	return static_cast<unsigned int>(bId);
}

#endif // map<something>fromX

void
CXWindowsScreen::warpCursorNoFlush(SInt32 x, SInt32 y, UInt8 id)
{
	assert(m_window != None);

	// send an event that we can recognize before the mouse warp
// 	XEvent eventBefore;
// 	eventBefore.type                = MotionNotify;
// 	eventBefore.xmotion.display     = m_display;
// 	eventBefore.xmotion.window      = m_window;
// 	eventBefore.xmotion.root        = m_root;
// 	eventBefore.xmotion.subwindow   = m_window;
// 	eventBefore.xmotion.time        = CurrentTime;
// 	eventBefore.xmotion.x           = x;
// 	eventBefore.xmotion.y           = y;
// 	eventBefore.xmotion.x_root      = x;
// 	eventBefore.xmotion.y_root      = y;
// 	eventBefore.xmotion.state       = 0;
// 	eventBefore.xmotion.is_hint     = NotifyNormal;
// 	eventBefore.xmotion.same_screen = True;
// 	XEvent eventAfter               = eventBefore;
// 	XSendEvent(m_display, m_window, False, 0, &eventBefore);

#if HAVE_X11_EXTENSIONS_XINPUT2_H
        LOG((CLOG_DEBUG2 "warping id(%d) to %d,%d", id, x, y));
        bool status = XIWarpPointer(m_display, id, None, m_root,0,0,0,0,x,y);
	LOG((CLOG_DEBUG2 "xiwarppointer: %d",status));
#else
	// warp mouse
	XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
#endif
	// send an event that we can recognize after the mouse warp
// 	XSendEvent(m_display, m_window, False, 0, &eventAfter);
// 	XSync(m_display, False);

	LOG((CLOG_DEBUG2 "warped to %d,%d", x, y));
}

void
CXWindowsScreen::updateButtons()
{
	// query the button mapping
	UInt32 numButtons = XGetPointerMapping(m_display, NULL, 0);
	unsigned char* tmpButtons = new unsigned char[numButtons];
	XGetPointerMapping(m_display, tmpButtons, numButtons);

	// find the largest logical button id
	unsigned char maxButton = 0;
	for (UInt32 i = 0; i < numButtons; ++i) {
		if (tmpButtons[i] > maxButton) {
			maxButton = tmpButtons[i];
		}
	}

	// allocate button array
	m_buttons.resize(maxButton);

	// fill in button array values.  m_buttons[i] is the physical
	// button number for logical button i+1.
	for (UInt32 i = 0; i < numButtons; ++i) {
		m_buttons[i] = 0;
	}
	for (UInt32 i = 0; i < numButtons; ++i) {
		m_buttons[tmpButtons[i] - 1] = i + 1;
	}

	// clean up
	delete[] tmpButtons;
}

#if HAVE_X11_EXTENSIONS_XINPUT2_H
bool
CXWindowsScreen::grabMouseAndKeyboard(UInt8 id)
{
    	// grab the mouse and keyboard.  keep trying until we get them.
	// if we can't grab one after grabbing the other then ungrab
	// and wait before retrying.  give up after s_timeout seconds.
	static const double s_timeout = 1.0;
	int result;
	
	UInt8 kId = m_dev->getAttachment(id);
	
        XIEventMask mask;
        mask.mask_len = XIMaskLen(XI_KeyPress);
        mask.mask = (unsigned char*)calloc(mask.mask_len, sizeof(char));
        memset(mask.mask, 0, mask.mask_len);

        mask.deviceid = XIAllMasterDevices;
        XISetMask(mask.mask, XI_KeyPress);
        XISetMask(mask.mask, XI_KeyRelease);

        LOG((CLOG_DEBUG "grabbing..."));
	CStopwatch timer;
	do {
		// keyboard first
		do {
                        LOG((CLOG_DEBUG "mask.mask_len: %d",mask.mask_len));	
                        result = XIGrabDevice(m_display, kId, m_root,
                                              CurrentTime,None, GrabModeAsync,
                                              GrabModeAsync, False, &mask);
			LOG((CLOG_DEBUG "grab result: %d", result));
			assert(result != GrabNotViewable);
			if (result != GrabSuccess) {
				LOG((CLOG_DEBUG "waiting to grab keyboard"));
				ARCH->sleep(0.05);
				if (timer.getTime() >= s_timeout) {
					LOG((CLOG_DEBUG "grab keyboard timed out"));
					return false;
				}
			}
		} while (result != GrabSuccess);
		LOG((CLOG_DEBUG "grabbed keyboard"));
                free(mask.mask);

		// now the mouse
                mask.mask_len = XIMaskLen(XI_Motion);
                mask.mask = (unsigned char*)calloc(mask.mask_len, sizeof(char));
                memset(mask.mask, 0, mask.mask_len);

                mask.deviceid = XIAllMasterDevices;
                XISetMask(mask.mask, XI_RawMotion);
                XISetMask(mask.mask, XI_ButtonPress);
                XISetMask(mask.mask, XI_ButtonRelease);
                
                LOG((CLOG_DEBUG "mask.mask_len: %d",mask.mask_len));
                result = XIGrabDevice(m_display, id, m_root,
                                              CurrentTime,None, GrabModeAsync,
                                              GrabModeAsync, True, &mask);
		assert(result != GrabNotViewable);
		if (result != GrabSuccess) {
                        XIUngrabDevice(m_display, kId, CurrentTime);
			LOG((CLOG_DEBUG "ungrabbed keyboard, waiting to grab pointer"));
			ARCH->sleep(0.05);
			if (timer.getTime() >= s_timeout) {
				LOG((CLOG_DEBUG "grab pointer timed out"));
				return false;
			}
		}
	} while (result != GrabSuccess);

	LOG((CLOG_DEBUG "grabbed pointer and keyboard"));
        free(mask.mask);
	return true;
}
#else
bool
CXWindowsScreen::grabMouseAndKeyboard()
{
	// grab the mouse and keyboard.  keep trying until we get them.
	// if we can't grab one after grabbing the other then ungrab
	// and wait before retrying.  give up after s_timeout seconds.
	static const double s_timeout = 1.0;
	int result;
	CStopwatch timer;
	do {
		// keyboard first
		do {
			result = XGrabKeyboard(m_display, m_window, True,
								GrabModeAsync, GrabModeAsync, CurrentTime);
			assert(result != GrabNotViewable);
			if (result != GrabSuccess) {
				LOG((CLOG_DEBUG2 "waiting to grab keyboard"));
				ARCH->sleep(0.05);
				if (timer.getTime() >= s_timeout) {
					LOG((CLOG_DEBUG2 "grab keyboard timed out"));
					return false;
				}
			}
		} while (result != GrabSuccess);
		LOG((CLOG_DEBUG2 "grabbed keyboard"));

		// now the mouse
		result = XGrabPointer(m_display, m_window, True, 0,
								GrabModeAsync, GrabModeAsync,
								m_window, None, CurrentTime);
		assert(result != GrabNotViewable);
		if (result != GrabSuccess) {
			// back off to avoid grab deadlock
			XUngrabKeyboard(m_display, CurrentTime);
			LOG((CLOG_DEBUG2 "ungrabbed keyboard, waiting to grab pointer"));
			ARCH->sleep(0.05);
			if (timer.getTime() >= s_timeout) {
				LOG((CLOG_DEBUG2 "grab pointer timed out"));
				return false;
			}
		}
	} while (result != GrabSuccess);

	LOG((CLOG_DEBUG1 "grabbed pointer and keyboard"));
	return true;
}
#endif

void
CXWindowsScreen::refreshKeyboard(XEvent* event, UInt8 id)
{
	CXWindowsKeyState *l_keyState = (CXWindowsKeyState*)m_dev->getKeyState(id);
	if (XPending(m_display) > 0) {
		XEvent tmpEvent;
		XPeekEvent(m_display, &tmpEvent);
		if (tmpEvent.type == MappingNotify) {
			// discard this event since another follows.
			// we tend to get a bunch of these in a row.
			return;
		}
	}

	// keyboard mapping changed
#if HAVE_XKB_EXTENSION
	if (m_xkb && event->type == m_xkbEventBase) {
		LOG((CLOG_DEBUG2 "refreshKeyboard id: %d, m_xkb: %d", id, m_xkb));
		XkbRefreshKeyboardMapping((XkbMapNotifyEvent*)event);
	}
	else
#else
	{
		LOG((CLOG_DEBUG2 "refreshKeyboard id: %d, m_xkb: %d", id, m_xkb));
		XRefreshKeyboardMapping(&event->xmapping);
	}
#endif
	l_keyState->updateKeyMap(id);
	l_keyState->updateKeyState(id);
}


//
// CXWindowsScreen::CHotKeyItem
//

CXWindowsScreen::CHotKeyItem::CHotKeyItem(int keycode, unsigned int mask) :
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

bool
CXWindowsScreen::CHotKeyItem::operator<(const CHotKeyItem& x) const
{
	return (m_keycode < x.m_keycode ||
			(m_keycode == x.m_keycode && m_mask < x.m_mask));
}

void CXWindowsScreen::initDevices()
{
    LOG((CLOG_DEBUG "reading device hierarchy"));

    int num_devices;
    XIDeviceInfo *devices, *device;
//    CDeviceInfo *info;
    
    devices = XIQueryDevice(m_display, XIAllDevices, &num_devices);

    for (int i = 0; i < num_devices; i++) {
	device = &devices[i];
	LOG((CLOG_DEBUG "Device %s (id: %d) is a ", device->name, device->deviceid));

	switch(device->use) {
	    case XIMasterPointer:
		LOG((CLOG_DEBUG "master pointer"));
// 		info = new CDeviceInfo(device->deviceid, device->attachment);
// 		if(m_isPrimary)
// 		{
// 		    info->m_onScreen = true;
// 		    info->m_screenName = m_screenName;
// 		}
// 		m_devices->push_front(info);
		m_dev->addDevice(true, device->attachment, device->deviceid);
		break;
	    case XIMasterKeyboard:
		LOG((CLOG_DEBUG "master keyboard"));
		m_dev->addDevice(false, device->attachment, device->deviceid);
		m_dev->setKeyState(new CXWindowsKeyState(m_display, m_xkb, device->deviceid), device->deviceid);
		break;
	    case XISlavePointer: 
		LOG((CLOG_DEBUG "slave pointer"));
		break;
	    case XISlaveKeyboard: 
		LOG((CLOG_DEBUG "slave keyboard"));
		break;
	    case XIFloatingSlave:
		LOG((CLOG_DEBUG "floating slave"));
		// do nothing - we ignore floating devices for now
		break;
	}
	LOG((CLOG_DEBUG "Device is attached to/paired with %d", device->attachment));
    }
    XIFreeDeviceInfo(devices);
}

void
CXWindowsScreen::createMaster(CString name, UInt8 sKbdId, UInt8 sPtrId)
{    
    XIAddMasterInfo info;
    Status status;
    std::stringstream ss;
    LOG((CLOG_DEBUG "createMaster called with %s, %d, %d", name.c_str(), sKbdId, sPtrId));
    ss << sPtrId;
    CString pointer_name = name + " " + ss.str() + " synergy pointer";
    CString pointer_name_xtest = name + " " + ss.str() + " synergy XTEST pointer";
    CString keyboard_name = name + " " + ss.str() + " synergy keyboard";
    CString keyboard_name_xtest = name + " " + ss.str() + " synergy XTEST keyboard";

    LOG((CLOG_DEBUG "creating master pointer %s", pointer_name.c_str()));
    info.type = XIAddMaster;
    info.name = (char*)(name + " " + ss.str() + " synergy").c_str();
    info.send_core = 1;
    info.enable = 1;

    status = XIChangeHierarchy(m_display, (XIAnyHierarchyChangeInfo*)&info, 1);
    if(status == Success){
        UInt8 kId = findDeviceId(keyboard_name);
	UInt8 kId_xtest = findDeviceId(keyboard_name_xtest);
	UInt8 pId = findDeviceId(pointer_name);
	UInt8 pId_xtest = findDeviceId(pointer_name_xtest);

	LOG((CLOG_DEBUG "local id of pointer %d is %d", sPtrId, pId));
	LOG((CLOG_DEBUG "local id of keyboard %d is %d", sKbdId, kId));
        m_dev->addDevice(true,kId,pId);
	m_dev->addDevice(false,pId,kId);
	m_dev->setKeyState(new CXWindowsKeyState(m_display, m_xkb, kId), kId);
	m_dev->setServerId(sKbdId, kId);
	m_dev->setServerId(sPtrId, pId);
	m_dev->setXtestId(kId_xtest, kId);
        m_dev->setXtestId(pId_xtest, pId);
    }

}

UInt8
CXWindowsScreen::findDeviceId(CString name) const
{
  XIDeviceInfo *info;
  int num_devices;
  int i = 0, id = -1;
  LOG((CLOG_DEBUG "searching for %s in ", name.c_str()));

  
  info = XIQueryDevice(m_display, XIAllDevices, &num_devices);
  for(i = 0; i < num_devices; i++){
    LOG((CLOG_DEBUG "  %s", info[i].name));
    if(strcmp(info[i].name, name.c_str()) == 0){
      id = info[i].deviceid;
      break;
    }
  }
  XIFreeDeviceInfo(info);
  LOG((CLOG_DEBUG "local device id is %d in ", id));
  return id;
}

void
CXWindowsScreen::removeMaster(UInt8 id)
{
    LOG((CLOG_DEBUG "Removing Master Device: %d", id));
    XIRemoveMasterInfo info;

    info.type = XIRemoveMaster;
    info.deviceid = id;
    info.return_mode = XIFloating;
    if(XIChangeHierarchy(m_display, (XIAnyHierarchyChangeInfo*)&info, 1))
	LOG((CLOG_DEBUG "Device %d removed", id));
}

void
CXWindowsScreen::cleanUp(UInt8 id)
{
    removeMaster(id);
}

void 
CXWindowsScreen::hideCursor(Window w, Cursor c, UInt8 id) const
{
    CXWindowsUtil::CErrorLock lock(m_display);
    doHide(w, c, id);
}

void
CXWindowsScreen::doHide(Window w, Cursor c, UInt8 id) const
{
  
// 	if (w == m_window) {
// 		return;
// 	}
// 
	Status stat1 = XIDefineCursor(m_display, id, m_window, c);
	LOG((CLOG_DEBUG "XIDefineCursor Status: %d, DeviceID: %d, Window: %d",stat1, id, w));
// 
// 	// recurse on child windows
// 	Window rw, pw, *cw;
// 	unsigned int nc;
// 	if (XQueryTree(m_display, w, &rw, &pw, &cw, &nc)) {
// 		for (unsigned int i = 0; i < nc; ++i) {
// 			doHide(cw[i], c, id);
// 		}
// 		XFree(cw);
// 	}


}