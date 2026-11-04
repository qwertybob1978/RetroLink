#ifndef MACSTUDIO_DESIGNER_FORM_H
#define MACSTUDIO_DESIGNER_FORM_H

#include <Dialogs.h>
#include <Quickdraw.h>

typedef DialogPtr RetroLinkForm;

/** Runs the generated form event loop and returns its result. */
int MacStudioRunForm(void);
/** Sends a bounded printable debug record to the running MacStudio Agent. */
OSErr MacStudioDebugLog(unsigned char severity, const char *message);
/** Called after the form and all designer widgets are initialized. */
void MacStudioFormDidLoad(RetroLinkForm form);

/** Reads Pascal text from a text-bearing control by stable ID. */
short MacStudioGetControlText(DialogPtr dialog, const char *controlId, unsigned char *text);
/** Updates Pascal text on a text-bearing control by stable ID. */
short MacStudioSetControlText(DialogPtr dialog, const char *controlId, const unsigned char *text);
/** Gets a checkbox/radio state, popup/list selection, or scrollbar value. */
short MacStudioGetControlValue(DialogPtr dialog, const char *controlId, short *value);
/** Sets a checkbox/radio state, popup/list selection, or scrollbar value. */
short MacStudioSetControlValue(DialogPtr dialog, const char *controlId, short value);
/** Gets control bounds relative to the form's content area. */
short MacStudioGetControlBounds(DialogPtr dialog, const char *controlId, Rect *bounds);
/** Sets control bounds relative to the form's content area. */
short MacStudioSetControlBounds(DialogPtr dialog, const char *controlId, const Rect *bounds);
/** Draws a packed 1-bit icon over a native push button. */
short MacStudioSetControlIcon(DialogPtr dialog, const char *controlId, short iconId);

#define MACSTUDIO_ID_CONTROLCONNECTIONSTATUS "ConnectionStatus"
/** Stable-ID accessor set for the Static Text instance 'ConnectionStatus'. */
short MacStudioGetControlConnectionStatusBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlConnectionStatusBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlConnectionStatusText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlConnectionStatusText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLROLESTATUS "RoleStatus"
/** Stable-ID accessor set for the Static Text instance 'RoleStatus'. */
short MacStudioGetControlRoleStatusBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlRoleStatusBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlRoleStatusText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlRoleStatusText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLTAKECONTROLBUTTON "TakeControlButton"
/** Stable-ID accessor set for the Push Button instance 'TakeControlButton'. */
short MacStudioGetControlTakeControlButtonBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlTakeControlButtonBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlTakeControlButtonText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlTakeControlButtonText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLSERIALPORTLABEL "SerialPortLabel"
/** Stable-ID accessor set for the Static Text instance 'SerialPortLabel'. */
short MacStudioGetControlSerialPortLabelBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlSerialPortLabelBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlSerialPortLabelText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlSerialPortLabelText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR "SerialPortSelector"
/** Stable-ID accessor set for the Popup Menu instance 'SerialPortSelector'. */
short MacStudioGetControlSerialPortSelectorBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlSerialPortSelectorBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlSerialPortSelectorText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlSerialPortSelectorText(DialogPtr dialog, const unsigned char *text);
short MacStudioGetControlSerialPortSelectorValue(DialogPtr dialog, short *value);
short MacStudioSetControlSerialPortSelectorValue(DialogPtr dialog, short value);

#define MACSTUDIO_ID_CONTROLSERIALBAUDLABEL "SerialBaudLabel"
/** Stable-ID accessor set for the Static Text instance 'SerialBaudLabel'. */
short MacStudioGetControlSerialBaudLabelBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlSerialBaudLabelBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlSerialBaudLabelText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlSerialBaudLabelText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR "SerialBaudSelector"
/** Stable-ID accessor set for the Popup Menu instance 'SerialBaudSelector'. */
short MacStudioGetControlSerialBaudSelectorBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlSerialBaudSelectorBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlSerialBaudSelectorText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlSerialBaudSelectorText(DialogPtr dialog, const unsigned char *text);
short MacStudioGetControlSerialBaudSelectorValue(DialogPtr dialog, short *value);
short MacStudioSetControlSerialBaudSelectorValue(DialogPtr dialog, short value);

#define MACSTUDIO_ID_CONTROLSERIALSETTINGSBUTTON "SerialSettingsButton"
/** Stable-ID accessor set for the Push Button instance 'SerialSettingsButton'. */
short MacStudioGetControlSerialSettingsButtonBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlSerialSettingsButtonBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlSerialSettingsButtonText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlSerialSettingsButtonText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLSERIALCLOSEBUTTON "SerialCloseButton"
/** Stable-ID accessor set for the Push Button instance 'SerialCloseButton'. */
short MacStudioGetControlSerialCloseButtonBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlSerialCloseButtonBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlSerialCloseButtonText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlSerialCloseButtonText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLMACVOLUMESLABEL "MacVolumesLabel"
/** Stable-ID accessor set for the Static Text instance 'MacVolumesLabel'. */
short MacStudioGetControlMacVolumesLabelBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlMacVolumesLabelBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlMacVolumesLabelText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlMacVolumesLabelText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLMACFILES "MacFiles"
/** Stable-ID accessor set for the List Box instance 'MacFiles'. */
short MacStudioGetControlMacFilesBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlMacFilesBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlMacFilesValue(DialogPtr dialog, short *value);
short MacStudioSetControlMacFilesValue(DialogPtr dialog, short value);

#define MACSTUDIO_ID_CONTROLHOSTFILESLABEL "HostFilesLabel"
/** Stable-ID accessor set for the Static Text instance 'HostFilesLabel'. */
short MacStudioGetControlHostFilesLabelBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlHostFilesLabelBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlHostFilesLabelText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlHostFilesLabelText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLHOSTFILES "HostFiles"
/** Stable-ID accessor set for the List Box instance 'HostFiles'. */
short MacStudioGetControlHostFilesBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlHostFilesBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlHostFilesValue(DialogPtr dialog, short *value);
short MacStudioSetControlHostFilesValue(DialogPtr dialog, short value);

#define MACSTUDIO_ID_CONTROLREFRESHMACBUTTON "RefreshMacButton"
/** Stable-ID accessor set for the Push Button instance 'RefreshMacButton'. */
short MacStudioGetControlRefreshMacButtonBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlRefreshMacButtonBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlRefreshMacButtonText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlRefreshMacButtonText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLREFRESHHOSTBUTTON "RefreshHostButton"
/** Stable-ID accessor set for the Push Button instance 'RefreshHostButton'. */
short MacStudioGetControlRefreshHostButtonBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlRefreshHostButtonBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlRefreshHostButtonText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlRefreshHostButtonText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLCOPYTOHOSTBUTTON "CopyToHostButton"
/** Stable-ID accessor set for the Push Button instance 'CopyToHostButton'. */
short MacStudioGetControlCopyToHostButtonBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlCopyToHostButtonBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlCopyToHostButtonText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlCopyToHostButtonText(DialogPtr dialog, const unsigned char *text);

#define MACSTUDIO_ID_CONTROLTRANSFERPROGRESSBAR "TransferProgressBar"
/** Stable-ID accessor set for the Progress Bar instance 'TransferProgressBar'. */
short MacStudioGetControlTransferProgressBarBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlTransferProgressBarBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlTransferProgressBarValue(DialogPtr dialog, short *value);
short MacStudioSetControlTransferProgressBarValue(DialogPtr dialog, short value);

#define MACSTUDIO_ID_CONTROLACTIONSTATUS "ActionStatus"
/** Stable-ID accessor set for the Static Text instance 'ActionStatus'. */
short MacStudioGetControlActionStatusBounds(DialogPtr dialog, Rect *bounds);
short MacStudioSetControlActionStatusBounds(DialogPtr dialog, const Rect *bounds);
short MacStudioGetControlActionStatusText(DialogPtr dialog, unsigned char *text);
short MacStudioSetControlActionStatusText(DialogPtr dialog, const unsigned char *text);

/** Returns the selected list row (zero-based), or -1 if none is selected. */
short MacStudioGetListBoxSelection(DialogPtr dialog, const char *controlId, short *row);
/** Replaces list-box rows with Pascal strings; itemCount is limited to 128. */
short MacStudioSetListBoxItems(DialogPtr dialog, const char *controlId, const unsigned char *const *items, short itemCount);

/** Configures the selected built-in Macintosh serial port. */
short MacStudioSerialOpen(const char *componentId);
/** Opens a serial component on port 1 (Modem)/2 (Printer) at the selected baud rate. */
short MacStudioSerialOpenConfigured(const char *componentId, short portIndex, long baudRate);
/** Stops pending serial I/O for the selected component. */
short MacStudioSerialClose(const char *componentId);
/** Returns the most recent Macintosh serial driver error. */
short MacStudioGetLastSerialError(const char *componentId);
/** Reads currently buffered serial bytes without waiting for more data. */
short MacStudioSerialRead(const char *componentId, unsigned char *data, long capacity, long *readCount);
/** Writes bytes to the selected serial output port. */
short MacStudioSerialWrite(const char *componentId, const unsigned char *data, long length);
/** Sends a memory buffer using the configured XMODEM or ZMODEM protocol. */
short MacStudioProtocolSend(const char *componentId, const unsigned char *data, long length);
/** Receives a transfer into a caller-owned buffer. */
short MacStudioProtocolReceive(const char *componentId, unsigned char *data, long capacity, long *length);
/** Receives an XMODEM transfer with an exact unpadded byte length. */
short MacStudioProtocolReceiveExact(const char *componentId, unsigned char *data, long capacity, long expectedLength, long *length);
/** Gets the last transfer progress percentage. */
short MacStudioProtocolGetProgress(const char *componentId, short *percentage);

#define MACSTUDIO_COMPONENT_SERIALPORT1 "SerialPort1"
#define MACSTUDIO_COMPONENT_PROTOCOLTRANSFER1 "ProtocolTransfer1"

void TakeControlClick(RetroLinkForm form, short item);
void CopyToDosClick(RetroLinkForm form, short item);
void SerialSettingsClick(RetroLinkForm form, short item);
void SerialCloseClick(RetroLinkForm form, short item);
void HostFilesClick(RetroLinkForm form, short item);
void CreateMacFolderClick(RetroLinkForm form, short item);
void RenameMacEntryClick(RetroLinkForm form, short item);
void DeleteMacEntryClick(RetroLinkForm form, short item);
void RefreshMacClick(RetroLinkForm form, short item);
void RefreshHostClick(RetroLinkForm form, short item);
void SerialDataAvailable(RetroLinkForm form, short item);
void SerialError(RetroLinkForm form, short item);
void TransferProgress(RetroLinkForm form, short item);

void MacStudioUserControlClicked(RetroLinkForm form, const char *controlId, short item);

#endif
