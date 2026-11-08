/*
 * putty-rc.h - constants shared between putty-common.rc2 and the C code.
 */

#ifndef PUTTY_WIN_RES_H
#define PUTTY_WIN_RES_H

#define IDI_MAINICON     200
#define IDI_CFGICON      201
/* cn: 文件传输窗口专用图标（文件夹 + 双向箭头） */
#define IDI_SFTPXFER     202
/* cn: 文件传输列表行图标：目录 / 文件 */
#define IDI_LISTDIR      203
#define IDI_LISTFILE     204
/* cn: 自建模态弹窗用的问号图标（标题栏 + 内容区两处） */
#define IDI_QHELP     205

#define IDD_MAINBOX      102
#define IDD_LOGBOX       110
#define IDD_ABOUTBOX     111
#define IDD_RECONF       112
#define IDD_LICENCEBOX   113
#define IDD_HOSTKEY      114
#define IDD_HK_MOREINFO  116
#define IDD_CA_CONFIG    117
/* cn: 118 原为 IDD_SAVECHANGES（已删：X 与「退出」按钮改用自绘弹窗） */
#define IDD_KEYPASS      119

#define IDN_LIST        1001
#define IDN_COPY        1002

#define IDA_ICON        1001
#define IDA_TEXT        1002
#define IDA_LICENCE     1003
#define IDA_WEB         1004

#define IDC_TAB         1001
#define IDC_TABSTATIC1  1002
#define IDC_TABSTATIC2  1003
#define IDC_TABLIST     1004
#define IDC_HELPBTN     1005
#define IDC_ABOUT       1006
/* cn: 1007~1010 原属已删的 IDD_SAVECHANGES 模板（保存并退出 / 直接退出 /
 * 改动清单 / 提示语），保留空号不复用，免得与旧资源编号撞车。 */

#define IDC_HK_ICON       98
#define IDC_HK_TITLE      99
#define IDC_HK_TEXT      100
#define IDC_HK_ACCEPT   1001
#define IDC_HK_ONCE     1000
#define IDC_HK_HOST     1002
#define IDC_HK_FINGERPRINT 1003
#define IDC_HK_MOREINFO 1004

#define IDC_HKI_SHA256  1000
#define IDC_HKI_MD5     1001
#define IDC_HKI_PUBKEY  1002

#define ID_CUSTOM_CHMFILE 2000
#define TYPE_CUSTOM_CHMFILE 2000

#endif
