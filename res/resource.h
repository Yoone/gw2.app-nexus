///----------------------------------------------------------------------------------------------------
/// resource.h
/// Resource IDs for embedded assets.
///
/// These are embedded as a custom resource type named "PNG" (see gw2app.rc), because that is
/// exactly what Nexus looks for: TxLoader.cpp calls FindResourceA(module, MAKEINTRESOURCEA(id), "PNG").
/// Using RCDATA here would compile fine and then silently fail to load at runtime.
///----------------------------------------------------------------------------------------------------

#ifndef RESOURCE_H
#define RESOURCE_H

#define IDB_GW2APP_ICON        101
#define IDB_GW2APP_LOGO        102
#define IDB_GW2APP_ICON_HIDDEN 103

#endif
