/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "platform/OSXClipboard.h"
#include "platform/OSXPowerManager.h"
#include "synergy/PlatformScreen.h"
#include "synergy/DragInformation.h"
#include "base/EventTypes.h"
#include "common/stdmap.h"
#include "common/stdvector.h"

#include <bitset>
#include <Carbon/Carbon.h>
#include <mach/mach_port.h>
#include <mach/mach_interface.h>
#include <mach/mach_init.h>
#include <IOKit/IOMessage.h>
#include <memory>

extern "C" {
    typedef int CGSConnectionID;
    CGError CGSSetConnectionProperty(CGSConnectionID cid, CGSConnectionID targetCID, CFStringRef key, CFTypeRef value);
    int _CGSDefaultConnection();
}    


template <class T>
class CondVar;
class EventQueueTimer;
class Mutex;
class Thread;
class OSXKeyState;
class OSXScreenSaver;
class IEventQueue;
class Mutex;

//! Implementation of IPlatformScreen for OS X
class OSXScreen : public PlatformScreen {
public:
	 OSXScreen(IEventQueue* events,
				bool isPrimary,
				bool enableLangSync = false,
				lib::synergy::ClientScrollDirection scrollDirection = lib::synergy::ClientScrollDirection::SERVER);

    virtual ~OSXScreen();

    IEventQueue*        getEvents() const { return m_events; }

    // IScreen overrides
    void*       getEventTarget() const override;
    bool        getClipboard(ClipboardID id, IClipboard*) const override;
    /* TODO: DAUN
        we want to have a getShape() to accept pos_x, pos_y
    */
    void        getShape(SInt32& x, SInt32& y,
                            SInt32& width, SInt32& height, SInt32 pos_x = INT_MIN, SInt32 pos_y = INT_MIN) const override;
    void        getCursorPos(SInt32& x, SInt32& y) const override;

    // IPrimaryScreen overrides
    void        reconfigure(UInt32 activeSides) override;
    void        warpCursor(SInt32 x, SInt32 y) override;
    UInt32      registerHotKey(KeyID key, KeyModifierMask mask) override;
    void        unregisterHotKey(UInt32 id) override;
    void        fakeInputBegin() override;
    void        fakeInputEnd() override;
    SInt32      getJumpZoneSize() const override;
    bool        isAnyMouseButtonDown(UInt32& buttonID) const override;
    void        getCursorCenter(SInt32& x, SInt32& y) const override;

    // ISecondaryScreen overrides
    void        fakeMouseButton(ButtonID id, bool press) override;
    void        fakeMouseMove(SInt32 x, SInt32 y) override;
    void        fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const override;
    void        fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const override;

    // IPlatformScreen overrides
    void        enable() override;
    void        disable() override;
    void        enter() override;
    bool        leave() override;
    bool        setClipboard(ClipboardID, const IClipboard*) override;
    void        checkClipboards() override;
    void        openScreensaver(bool notify) override;
    void        closeScreensaver() override;
    void        screensaver(bool activate) override;
    void        resetOptions() override;
    void        setOptions(const OptionsList& options) override;
    void        setSequenceNumber(UInt32) override;
    bool        isPrimary() const override;
    void        fakeDraggingFiles(DragFileList fileList) override;
    String&     getDraggingFilename() override;
    String      getSecureInputApp() const override;
    
    const String&   getDropTarget() const override { return m_dropTarget; }
    void            waitForCarbonLoop() const;
    
protected:
    // IPlatformScreen overrides
    void            handleSystemEvent(const Event&, void*) override;
    void            updateButtons() override;
    IKeyState*      getKeyState() const override;

private:
    bool                updateScreenShape();
    bool                updateScreenShape(const CGDirectDisplayID, const CGDisplayChangeSummaryFlags);
    void                postMouseEvent(CGPoint&) const;
    
    // convenience function to send events
    void                sendEvent(Event::Type type, void* = NULL) const;
    void                sendClipboardEvent(Event::Type type, ClipboardID id) const;

    // message handlers
	bool				onMouseMove(CGFloat mx, CGFloat my);
    // mouse button handler.  pressed is true if this is a mousedown
    // event, false if it is a mouseup event.  macButton is the index
    // of the button pressed using the mac button mapping.
    bool                onMouseButton(bool pressed, UInt16 macButton);
    bool                onMouseWheel(SInt32 xDelta, SInt32 yDelta) const;
    
    void                constructMouseButtonEventMap();

    bool                onKey(CGEventRef event);

    void                onMediaKey(CGEventRef event);
    
    bool                onHotKey(EventRef event) const;
    
    // Added here to allow the carbon cursor hack to be called. 
    void                showCursor();
    void                hideCursor();

    // map synergy mouse button to mac buttons
    ButtonID            mapSynergyButtonToMac(UInt16) const;

    // map mac mouse button to synergy buttons
    ButtonID            mapMacButtonToSynergy(UInt16) const;

    // map mac scroll wheel value to a synergy scroll wheel value
    SInt32                mapScrollWheelToSynergy(SInt32) const;

    // map synergy scroll wheel value to a mac scroll wheel value
    SInt32                mapScrollWheelFromSynergy(SInt32) const;

    // get the current scroll wheel speed
    double                getScrollSpeed() const;

    // enable/disable drag handling for buttons 3 and up
    void                enableDragTimer(bool enable);

    // drag timer handler
    void                handleDrag(const Event&, void*);

    // clipboard check timer handler
    void                handleClipboardCheck(const Event&, void*);

    // Resolution switch callback
    static void    displayReconfigurationCallback(CGDirectDisplayID,
                            CGDisplayChangeSummaryFlags, void*);

    // fast user switch callback
    static pascal OSStatus
                        userSwitchCallback(EventHandlerCallRef nextHandler,
                           EventRef theEvent, void* inUserData);
    
    // sleep / wakeup support
    void                watchSystemPowerThread(void*);
    static void            testCanceled(CFRunLoopTimerRef timer, void*info);
    static void            powerChangeCallback(void* refcon, io_service_t service,
                            natural_t messageType, void* messageArgument);
    void                handlePowerChangeRequest(natural_t messageType,
                             void* messageArgument);

    void                handleConfirmSleep(const Event& event, void*);
    
    // global hotkey operating mode
    static bool            isGlobalHotKeyOperatingModeAvailable();
    static void            setGlobalHotKeysEnabled(bool enabled);
    static bool            getGlobalHotKeysEnabled();
    
    // Quartz event tap support
    static CGEventRef    handleCGInputEvent(CGEventTapProxy proxy,
                                           CGEventType type,
                                           CGEventRef event,
                                           void* refcon);
    static CGEventRef    handleCGInputEventSecondary(CGEventTapProxy proxy,
                                                    CGEventType type,
                                                    CGEventRef event,
                                                    void* refcon);
    
    // convert CFString to char*
    static char*        CFStringRefToUTF8String(CFStringRef aString);
    
    void                getDropTargetThread(void*);
    
private:
    struct HotKeyItem {
    public:
        HotKeyItem(UInt32, UInt32);
        HotKeyItem(EventHotKeyRef, UInt32, UInt32);

        EventHotKeyRef    getRef() const;

        bool            operator<(const HotKeyItem&) const;

    private:
        EventHotKeyRef    m_ref;
        UInt32            m_keycode;
        UInt32            m_mask;
    };

    enum EMouseButtonState {
        kMouseButtonUp = 0,
        kMouseButtonDragged,
        kMouseButtonDown,
        kMouseButtonStateMax
    };
    

    class MouseButtonState {
    public:
        void set(UInt32 button, EMouseButtonState state);
        bool any();
        void reset(); 
        void overwrite(UInt32 buttons);

        bool test(UInt32 button) const;
        SInt8 getFirstButtonDown() const;
    private:
        std::bitset<NumButtonIDs>      m_buttons;
    };

    typedef std::map<UInt32, HotKeyItem> HotKeyMap;
    typedef std::vector<UInt32> HotKeyIDList;
    typedef std::map<KeyModifierMask, UInt32> ModifierHotKeyMap;
    typedef std::map<HotKeyItem, UInt32> HotKeyToIDMap;

    // true if screen is being used as a primary screen, false otherwise
    bool                m_isPrimary;

    // true if mouse has entered the screen
    bool                m_isOnScreen;

    // the display
    CGDirectDisplayID    m_displayID;

    // screen shape stuff
    SInt32                m_x, m_y;
    SInt32                m_w, m_h;
    SInt32                m_xCenter, m_yCenter;

    // mouse state
    mutable SInt32        m_xCursor, m_yCursor;
    mutable bool        m_cursorPosValid;
    
    /* FIXME: this data structure is explicitly marked mutable due
       to a need to track the state of buttons since the remote
       side only lets us know of change events, and because the
       fakeMouseButton button method is marked 'const'. This is
       Evil, and this should be moved to a place where it need not
       be mutable as soon as possible. */
    mutable MouseButtonState m_buttonState;
    typedef std::map<UInt16, CGEventType> MouseButtonEventMapType;
    std::vector<MouseButtonEventMapType> MouseButtonEventMap;

    bool                m_cursorHidden;
    SInt32                m_dragNumButtonsDown;
    Point                m_dragLastPoint;
    EventQueueTimer*    m_dragTimer;

    // keyboard stuff
    OSXKeyState*        m_keyState;

    // clipboards
    OSXClipboard       m_pasteboard;
    UInt32                m_sequenceNumber;

    // screen saver stuff
    OSXScreenSaver*    m_screensaver;
    bool                m_screensaverNotify;

    // clipboard stuff
    bool                m_ownClipboard;
    EventQueueTimer*    m_clipboardTimer;

    // window object that gets user input events when the server
    // has focus.
    WindowRef            m_hiddenWindow;
    // window object that gets user input events when the server
    // does not have focus.
    WindowRef            m_userInputWindow;

    // fast user switching
    EventHandlerRef            m_switchEventHandlerRef;

    // sleep / wakeup
    Mutex*                    m_pmMutex;
    Thread*                m_pmWatchThread;
    CondVar<bool>*            m_pmThreadReady;
    CFRunLoopRef            m_pmRunloop;
    io_connect_t            m_pmRootPort;

    // hot key stuff
    HotKeyMap                m_hotKeys;
    HotKeyIDList            m_oldHotKeyIDs;
    ModifierHotKeyMap        m_modifierHotKeys;
    UInt32                    m_activeModifierHotKey;
    KeyModifierMask            m_activeModifierHotKeyMask;
    HotKeyToIDMap            m_hotKeyToIDMap;

    // global hotkey operating mode
    static bool                s_testedForGHOM;
    static bool                s_hasGHOM;
    
    // Quartz input event support
    CFMachPortRef            m_eventTapPort;
    CFRunLoopSourceRef        m_eventTapRLSR;

    // for double click coalescing.
    double                    m_lastClickTime;
    int                     m_clickState;
    SInt32                    m_lastSingleClickXCursor;
    SInt32                    m_lastSingleClickYCursor;

    IEventQueue*            m_events;
    
    std::unique_ptr<Thread>   m_getDropTargetThread;
    String                    m_dropTarget;

#if defined(MAC_OS_X_VERSION_10_7)
    Mutex*                    m_carbonLoopMutex;
    CondVar<bool>*            m_carbonLoopReady;
#endif

    OSXPowerManager m_powerManager;

    class OSXScreenImpl*    m_impl;
};
