#ifndef RETROLINK_MAC_ICONS_H
#define RETROLINK_MAC_ICONS_H

enum
{
    RETROLINK_ICON_ACCOUNT_SWITCH = 1,
    RETROLINK_ICON_REFRESH = 2,
    RETROLINK_ICON_UPLOAD = 3,
    RETROLINK_ICON_FOLDER_PLUS = 4,
    RETROLINK_ICON_PENCIL = 5,
    RETROLINK_ICON_DELETE = 6
};

#define RETROLINK_ICON_WIDTH 16
#define RETROLINK_ICON_HEIGHT 16

static const unsigned short gRetroIconAccountSwitch[RETROLINK_ICON_HEIGHT] = {
    0x0000, 0x0E70, 0x0E78, 0x0E70, 0x0420, 0x0000, 0x1F38, 0x3FBC,
    0x7F9E, 0x7F9E, 0x0000, 0x0000, 0x0420, 0x0FF0, 0x0420, 0x0000
};

static const unsigned short gRetroIconRefresh[RETROLINK_ICON_HEIGHT] = {
    0x0000, 0x0000, 0x0000, 0x07E0, 0x0C38, 0x1838, 0x1038, 0x1000,
    0x1000, 0x1008, 0x1818, 0x0C30, 0x07E0, 0x0000, 0x0000, 0x0000
};

static const unsigned short gRetroIconUpload[RETROLINK_ICON_HEIGHT] = {
    0x0000, 0x0000, 0x0000, 0x0180, 0x03C0, 0x07E0, 0x0FF0, 0x03C0,
    0x03C0, 0x03C0, 0x03C0, 0x0000, 0x1FF8, 0x0000, 0x0000, 0x0000
};

static const unsigned short gRetroIconFolderPlus[RETROLINK_ICON_HEIGHT] = {
    0x0000, 0x0000, 0x0000, 0x3E00, 0x7FFC, 0x7FFE, 0x7FFE, 0x7FFE,
    0x7FFE, 0x7FE0, 0x7FC8, 0x7F88, 0x3FBE, 0x000C, 0x0008, 0x0000
};

static const unsigned short gRetroIconPencil[RETROLINK_ICON_HEIGHT] = {
    0x0000, 0x0000, 0x0018, 0x003C, 0x005C, 0x00E8, 0x01F0, 0x03E0,
    0x07C0, 0x0F80, 0x1F00, 0x3E00, 0x3C00, 0x3800, 0x0000, 0x0000
};

static const unsigned short gRetroIconDelete[RETROLINK_ICON_HEIGHT] = {
    0x0000, 0x0000, 0x03C0, 0x1FF8, 0x0000, 0x0FF0, 0x0FF0, 0x0FF0,
    0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0FF0, 0x0000, 0x0000
};

static const unsigned short *RetroLinkGetIconRows(short iconId)
{
    switch (iconId)
    {
        case RETROLINK_ICON_ACCOUNT_SWITCH:
            return gRetroIconAccountSwitch;
        case RETROLINK_ICON_REFRESH:
            return gRetroIconRefresh;
        case RETROLINK_ICON_UPLOAD:
            return gRetroIconUpload;
        case RETROLINK_ICON_FOLDER_PLUS:
            return gRetroIconFolderPlus;
        case RETROLINK_ICON_PENCIL:
            return gRetroIconPencil;
        case RETROLINK_ICON_DELETE:
            return gRetroIconDelete;
        default:
            return NULL;
    }
}

#endif
