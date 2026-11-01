/*
 * controls.c: routines to self-manage the controls in a dialog
 * box.
 */

/*
 * Possible TODO in new cross-platform config box stuff:
 *
 *  - When lining up two controls alongside each other, I wonder if
 *    we could conveniently arrange to centre them vertically?
 *    Particularly ugly in the current setup is the `Add new
 *    forwarded port:' static next to the rather taller `Remove'
 *    button.
 */

#include <assert.h>
#include <ctype.h>

#include "putty.h"
#include "misc.h"
#include "dialog.h"

#include <commctrl.h>

#define GAPBETWEEN 3
#define GAPWITHIN 1
#define GAPXBOX 7
#define GAPYBOX 4
#define DLGWIDTH 168
#define STATICHEIGHT 8
#define TITLEHEIGHT 12
#define CHECKBOXHEIGHT 8
#define RADIOHEIGHT 8
#define EDITHEIGHT 12
#define LISTHEIGHT 11
#define LISTINCREMENT 8
#define COMBOHEIGHT 12
#define PUSHBTNHEIGHT 14
#define PROGBARHEIGHT 14

/*
 * Colour used for radio-button labels that are not the current
 * choice: a mid grey, dark enough to stay readable on the dialog
 * background but clearly lighter than normal text.
 */
#define DIMMED_TEXT_COLOUR RGB(128, 128, 128)

/*
 * A dimmed drop-down list is painted from a picture of itself, which
 * has to be thrown away whenever the text it shows changes.
 */
static void combo_dim_invalidate(HWND combo);

DECL_WINDOWS_FUNCTION(static, void, InitCommonControls, (void));
DECL_WINDOWS_FUNCTION(static, BOOL, MakeDragList, (HWND));
DECL_WINDOWS_FUNCTION(static, int, LBItemFromPt, (HWND, POINT, BOOL));
DECL_WINDOWS_FUNCTION(static, void, DrawInsert, (HWND, HWND, int));

void init_common_controls(void)
{
    HMODULE comctl32_module = load_system32_dll("comctl32.dll");
    GET_WINDOWS_FUNCTION(comctl32_module, InitCommonControls);
    GET_WINDOWS_FUNCTION(comctl32_module, MakeDragList);
    GET_WINDOWS_FUNCTION(comctl32_module, LBItemFromPt);
    GET_WINDOWS_FUNCTION(comctl32_module, DrawInsert);
    p_InitCommonControls();
}

void ctlposinit(struct ctlpos *cp, HWND hwnd,
                int leftborder, int rightborder, int topborder)
{
    RECT r, r2;
    cp->hwnd = hwnd;
    cp->font = SendMessage(hwnd, WM_GETFONT, 0, 0);
    cp->ypos = topborder;
    GetClientRect(hwnd, &r);
    r2.left = r2.top = 0;
    r2.right = 4;
    r2.bottom = 8;
    MapDialogRect(hwnd, &r2);
    cp->dlu4inpix = r2.right;
    cp->width = (r.right * 4) / (r2.right) - 2 * GAPBETWEEN;
    cp->xoff = leftborder;
    cp->width -= leftborder + rightborder;
}

/*
 * Subclass for a radio button. The control keeps its ordinary
 * BS_AUTORADIOBUTTON behaviour, so the dialog manager still handles
 * the click and the mutual exclusion, but its appearance is painted
 * here instead of by the theme engine: the selected option is drawn
 * in the normal text colour and the rest are dimmed, which a themed
 * button gives no way to do.
 */
static LRESULT CALLBACK radio_button_subclass(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    switch (msg) {
      case WM_ERASEBKGND:
        /*
         * The whole client area is repainted in WM_PAINT, so there
         * is no point letting the default handler flicker the
         * background in first.
         */
        return TRUE;

      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r, rr, tr;
        char text[128];
        bool selected = SendMessage(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
        bool disabled = !IsWindowEnabled(hwnd);
        COLORREF textcol = disabled ? GetSysColor(COLOR_GRAYTEXT) :
            selected ? GetSysColor(COLOR_WINDOWTEXT) : DIMMED_TEXT_COLOUR;
        int d = 11;                    /* indicator diameter, pixels */
        HGDIOBJ oldpen, oldbrush, oldfont;
        HPEN pen;
        TEXTMETRIC tm;
        int texty;

        GetClientRect(hwnd, &r);
        GetWindowText(hwnd, text, sizeof(text));

        SetBkMode(hdc, TRANSPARENT);
        FillRect(hdc, &r, GetSysColorBrush(COLOR_BTNFACE));

        /*
         * The indicator, at the left edge and vertically centred in
         * the control.
         */
        rr.left = r.left + 1;
        rr.top = r.top + (r.bottom - r.top - d) / 2;
        rr.right = rr.left + d;
        rr.bottom = rr.top + d;

        pen = CreatePen(PS_SOLID, 1, textcol);
        oldpen = SelectObject(hdc, pen);
        oldbrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, rr.left, rr.top, rr.right, rr.bottom);
        if (selected) {
            /*
             * A smaller filled ellipse inside the ring is what a
             * classic selected radio button looks like.
             */
            HBRUSH dot = CreateSolidBrush(textcol);
            SelectObject(hdc, dot);
            Ellipse(hdc, rr.left + 3, rr.top + 3,
                    rr.right - 2, rr.bottom - 2);
            SelectObject(hdc, oldbrush);
            DeleteObject(dot);
        } else {
            SelectObject(hdc, oldbrush);
        }
        SelectObject(hdc, oldpen);
        DeleteObject(pen);

        /*
         * The label. The control is only a few pixels taller than
         * the font's line height, so DT_VCENTER would push the text
         * off the bottom; centre it on the glyph box instead.
         * DrawText is used so the '&' shortcut marker is handled.
         */
        oldfont = SelectObject(hdc, (HGDIOBJ)(ULONG_PTR)
                               SendMessage(hwnd, WM_GETFONT, 0, 0));
        GetTextMetrics(hdc, &tm);
        texty = r.top + (r.bottom - r.top - tm.tmHeight) / 2;
        tr = r;
        tr.left = rr.right + 4;
        tr.top = texty;
        tr.bottom = texty + tm.tmHeight;
        SetTextColor(hdc, textcol);
        DrawText(hdc, text, -1, &tr, DT_LEFT | DT_SINGLELINE);
        SelectObject(hdc, oldfont);

        EndPaint(hwnd, &ps);
        return 0;
      }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}


HWND doctl(struct ctlpos *cp, RECT r, const char *wclass, int wstyle,
           int exstyle, const char *wtext, int wid)
{
    HWND ctl;
    /*
     * Note nonstandard use of RECT. This is deliberate: by
     * transforming the width and height directly we arrange to
     * have all supposedly same-sized controls really same-sized.
     */

    r.left += cp->xoff;
    MapDialogRect(cp->hwnd, &r);

    /*
     * We can pass in cp->hwnd == NULL, to indicate a dry run
     * without creating any actual controls.
     */
    if (cp->hwnd) {
        ctl = CreateWindowEx(exstyle, wclass, wtext, wstyle,
                             r.left, r.top, r.right, r.bottom,
                             cp->hwnd, (HMENU)(ULONG_PTR)wid, hinst, NULL);
        SendMessage(ctl, WM_SETFONT, cp->font, MAKELPARAM(true, 0));

        if (!strcmp(wclass, "LISTBOX")) {
            /*
             * Bizarre Windows bug: the list box calculates its
             * number of lines based on the font it has at creation
             * time, but sending it WM_SETFONT doesn't cause it to
             * recalculate. So now, _after_ we've sent it
             * WM_SETFONT, we explicitly resize it (to the same
             * size it was already!) to force it to reconsider.
             */
            SetWindowPos(ctl, NULL, 0, 0, r.right, r.bottom,
                         SWP_NOACTIVATE | SWP_NOCOPYBITS |
                         SWP_NOMOVE | SWP_NOZORDER);
        }
    } else
        ctl = NULL;
    return ctl;
}

/*
 * A title bar across the top of a sub-dialog.
 */
void bartitle(struct ctlpos *cp, const char *name, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.right = cp->width;
    r.top = cp->ypos;
    r.bottom = STATICHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, name, id);
}

/*
 * Begin a grouping box, with or without a group title.
 */
void beginbox(struct ctlpos *cp, const char *name, int idbox)
{
    cp->boxystart = cp->ypos;
    if (!name)
        cp->boxystart -= STATICHEIGHT / 2;
    if (name)
        cp->ypos += STATICHEIGHT;
    cp->ypos += GAPYBOX;
    cp->width -= 2 * GAPXBOX;
    cp->xoff += GAPXBOX;
    cp->boxid = idbox;
    cp->boxtext = name;
}

/*
 * End a grouping box.
 */
void endbox(struct ctlpos *cp)
{
    RECT r;
    cp->xoff -= GAPXBOX;
    cp->width += 2 * GAPXBOX;
    cp->ypos += GAPYBOX - GAPBETWEEN;
    r.left = GAPBETWEEN;
    r.right = cp->width;
    r.top = cp->boxystart;
    r.bottom = cp->ypos - cp->boxystart;
    doctl(cp, r, "BUTTON", BS_GROUPBOX | WS_CHILD | WS_VISIBLE, 0,
          cp->boxtext ? cp->boxtext : "", cp->boxid);
    cp->ypos += GAPYBOX;
}

/*
 * A static line, followed by a full-width edit box.
 */
void editboxfw(struct ctlpos *cp, bool password, bool readonly,
               const char *text, int staticid, int editid)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.right = cp->width;

    if (text) {
        r.top = cp->ypos;
        r.bottom = STATICHEIGHT;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, text, staticid);
        cp->ypos += STATICHEIGHT + GAPWITHIN;
    }
    r.top = cp->ypos;
    r.bottom = EDITHEIGHT;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
          (password ? ES_PASSWORD : 0) |
          (readonly ? ES_READONLY : 0),
          WS_EX_CLIENTEDGE, "", editid);
    cp->ypos += EDITHEIGHT + GAPBETWEEN;
}

/*
 * A static line, followed by a full-width combo box.
 */
void combobox(struct ctlpos *cp, const char *text, int staticid, int listid)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.right = cp->width;

    if (text) {
        r.top = cp->ypos;
        r.bottom = STATICHEIGHT;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, text, staticid);
        cp->ypos += STATICHEIGHT + GAPWITHIN;
    }
    r.top = cp->ypos;
    r.bottom = COMBOHEIGHT * 10;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWN | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", listid);
    cp->ypos += COMBOHEIGHT + GAPBETWEEN;
}

struct radio { const char *text; int id; };

static void radioline_common(struct ctlpos *cp, const char *text, int id,
                             int nacross, struct radio *buttons, int nbuttons)
{
    RECT r;
    int group;
    int i;
    int j;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    if (text) {
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, text, id);
    } else {
        r.right = r.bottom = 0;
    }

    group = WS_GROUP;
    i = 0;
    for (j = 0; j < nbuttons; j++) {
        const char *btext = buttons[j].text;
        int bid = buttons[j].id;

        if (i == nacross) {
            cp->ypos += r.bottom + (nacross > 1 ? GAPBETWEEN : GAPWITHIN);
            i = 0;
        }
        r.left = GAPBETWEEN + i * (cp->width + GAPBETWEEN) / nacross;
        if (j < nbuttons-1)
            r.right =
                (i + 1) * (cp->width + GAPBETWEEN) / nacross - r.left;
        else
            r.right = cp->width - r.left;
        r.top = cp->ypos;
        r.bottom = RADIOHEIGHT;
        /*
         * The button keeps its ordinary BS_AUTORADIOBUTTON type so
         * that the dialog manager still maintains the mutual
         * exclusion and check state; the drawing is taken over by a
         * subclass instead, because a themed radio button ignores
         * WM_CTLCOLORBTN's text colour and there is no other way to
         * show the unselected options dimmed.
         */
        {
            HWND btn = doctl(cp, r, "BUTTON",
                             BS_NOTIFY | BS_AUTORADIOBUTTON | WS_CHILD |
                             WS_VISIBLE | WS_TABSTOP | group, 0, btext, bid);
            SetWindowSubclass(btn, radio_button_subclass, 1, 0);
        }
        group = 0;
        i++;
    }
    cp->ypos += r.bottom + GAPBETWEEN;
}

/*
 * A set of radio buttons on the same line, with a static above
 * them. `nacross' dictates how many parts the line is divided into
 * (you might want this not to equal the number of buttons if you
 * needed to line up some 2s and some 3s to look good in the same
 * panel).
 *
 * There's a bit of a hack in here to ensure that if nacross
 * exceeds the actual number of buttons, the rightmost button
 * really does get all the space right to the edge of the line, so
 * you can do things like
 *
 * (*) Button1  (*) Button2  (*) ButtonWithReallyLongTitle
 */
void radioline(struct ctlpos *cp, const char *text, int id, int nacross, ...)
{
    va_list ap;
    struct radio *buttons;
    int i, nbuttons;

    va_start(ap, nacross);
    nbuttons = 0;
    while (1) {
        const char *btext = va_arg(ap, const char *);
        if (!btext)
            break;
        (void) va_arg(ap, int); /* id */
        nbuttons++;
    }
    va_end(ap);
    buttons = snewn(nbuttons, struct radio);
    va_start(ap, nacross);
    for (i = 0; i < nbuttons; i++) {
        buttons[i].text = va_arg(ap, const char *);
        buttons[i].id = va_arg(ap, int);
    }
    va_end(ap);
    radioline_common(cp, text, id, nacross, buttons, nbuttons);
    sfree(buttons);
}

/*
 * A set of radio buttons on the same line, without a static above
 * them. Otherwise just like radioline.
 */
void bareradioline(struct ctlpos *cp, int nacross, ...)
{
    va_list ap;
    struct radio *buttons;
    int i, nbuttons;

    va_start(ap, nacross);
    nbuttons = 0;
    while (1) {
        const char *btext = va_arg(ap, const char *);
        if (!btext)
            break;
        (void) va_arg(ap, int); /* id */
        nbuttons++;
    }
    va_end(ap);
    buttons = snewn(nbuttons, struct radio);
    va_start(ap, nacross);
    for (i = 0; i < nbuttons; i++) {
        buttons[i].text = va_arg(ap, const char *);
        buttons[i].id = va_arg(ap, int);
    }
    va_end(ap);
    radioline_common(cp, NULL, 0, nacross, buttons, nbuttons);
    sfree(buttons);
}

/*
 * A set of radio buttons on multiple lines, with a static above
 * them.
 */
void radiobig(struct ctlpos *cp, const char *text, int id, ...)
{
    va_list ap;
    struct radio *buttons;
    int i, nbuttons;

    va_start(ap, id);
    nbuttons = 0;
    while (1) {
        const char *btext = va_arg(ap, const char *);
        if (!btext)
            break;
        (void) va_arg(ap, int); /* id */
        nbuttons++;
    }
    va_end(ap);
    buttons = snewn(nbuttons, struct radio);
    va_start(ap, id);
    for (i = 0; i < nbuttons; i++) {
        buttons[i].text = va_arg(ap, const char *);
        buttons[i].id = va_arg(ap, int);
    }
    va_end(ap);
    radioline_common(cp, text, id, 1, buttons, nbuttons);
    sfree(buttons);
}

/*
 * A single standalone checkbox.
 */
void checkbox(struct ctlpos *cp, const char *text, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = CHECKBOXHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
          text, id);
}

static BOOL _GetTextExtentExPoint(HDC hdc, LPCSTR lpszString, int cchString, int nMaxExtent, LPINT lpnFit, LPINT lpnDx, LPSIZE lpSize)
{
    BOOL ret = GetTextExtentExPoint(hdc, lpszString, cchString, nMaxExtent, lpnFit, lpnDx, lpSize);
    if (ret && GetACP() == CP_UTF8) {
        // fix lpnFit to bytes from characters on utf-8 codepage
        int n = 0;
        for (int i = 0; i < *lpnFit; i++) {
            unsigned char c = lpszString[n];
            if (c >= 0 && c <= 127 ) {
                n += 1;
            } else if (c >= 192 && c <= 223) {
                n += 2;
            } else if (c >= 224 && c <= 239) {
                n += 3;
            } else if (c >= 240 && c <= 247) {
                n += 4;
            } else if (c >= 248 && c <= 255) {
                n += 1;
            }
            if (n >= cchString) {
                n = cchString;
                break;
            }
        }
        *lpnFit = n;
    }
    return ret;
}

/*
 * Wrap a piece of text for a static text control. Returns the
 * wrapped text (a malloc'ed string containing \ns), and also
 * returns the number of lines required.
 */
char *staticwrap(struct ctlpos *cp, HWND hwnd, const char *text, int *lines)
{
    HDC hdc = GetDC(hwnd);
    int width, nlines, j;
    INT *pwidths, nfit;
    SIZE size;
    const char *p;
    RECT r;
    HFONT oldfont, newfont;

    strbuf *sb = strbuf_new();
    p = text;
    pwidths = snewn(1+strlen(text), INT);

    /*
     * Work out the width the text will need to fit in, by doing
     * the same adjustment that the `statictext' function itself
     * will perform.
     */
    SetMapMode(hdc, MM_TEXT);          /* ensure logical units == pixels */
    r.left = r.top = r.bottom = 0;
    r.right = cp->width;
    MapDialogRect(hwnd, &r);
    width = r.right;

    nlines = 1;

    /*
     * We must select the correct font into the HDC before calling
     * GetTextExtent*, or silly things will happen.
     */
    newfont = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
    oldfont = SelectObject(hdc, newfont);

    while (*p) {
        if (!_GetTextExtentExPoint(hdc, p, strlen(p), width,
                                   &nfit, pwidths, &size) ||
            (size_t)nfit >= strlen(p)) {
            /*
             * Either GetTextExtentExPoint returned failure, or the
             * whole of the rest of the text fits on this line.
             * Either way, we stop wrapping, copy the remainder of
             * the input string unchanged to the output, and leave.
             */
            put_datapl(sb, ptrlen_from_asciz(p));
            break;
        }

        /*
         * Now we search backwards along the string from `nfit',
         * looking for a space at which to break the line. If we
         * don't find one at all, that's fine - we'll just break
         * the line at `nfit'.
         */
        for (j = nfit; j > 0; j--) {
            if (isspace((unsigned char)p[j])) {
                nfit = j;
                break;
            }
        }

        put_data(sb, p, nfit);
        put_byte(sb, '\n');

        p += nfit;
        while (*p && isspace((unsigned char)*p))
            p++;

        nlines++;
    }

    SelectObject(hdc, oldfont);
    ReleaseDC(cp->hwnd, hdc);

    if (lines) *lines = nlines;

    sfree(pwidths);

    return strbuf_to_str(sb);
}

/*
 * A single standalone static text control.
 */
void statictext(struct ctlpos *cp, const char *text, int lines, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = STATICHEIGHT * lines;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "STATIC",
          WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
          0, text, id);
}

/*
 * An owner-drawn static text control for a panel title.
 */
void paneltitle(struct ctlpos *cp, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = TITLEHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
          0, NULL, id);
}

/*
 * A button on the right hand side, with a static to its left.
 */
void staticbtn(struct ctlpos *cp, const char *stext, int sid,
               const char *btext, int bid)
{
    const int height = (PUSHBTNHEIGHT > STATICHEIGHT ?
                        PUSHBTNHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos = GAPBETWEEN + 3 * (cp->width + GAPBETWEEN) / 4;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext, bid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A simple push button.
 */
void button(struct ctlpos *cp, const char *btext, int bid, bool defbtn)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = PUSHBTNHEIGHT;

    /* Q67655: the _dialog box_ must know which button is default
     * as well as the button itself knowing */
    if (defbtn && cp->hwnd)
        SendMessage(cp->hwnd, DM_SETDEFID, bid, 0);

    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          (defbtn ? BS_DEFPUSHBUTTON : 0) | BS_PUSHBUTTON,
          0, btext, bid);

    cp->ypos += PUSHBTNHEIGHT + GAPBETWEEN;
}

/*
 * Like staticbtn, but two buttons.
 */
void static2btn(struct ctlpos *cp, const char *stext, int sid,
                const char *btext1, int bid1, const char *btext2, int bid2)
{
    const int height = (PUSHBTNHEIGHT > STATICHEIGHT ?
                        PUSHBTNHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid1, rwid2, rpos1, rpos2;

    rpos1 = GAPBETWEEN + (cp->width + GAPBETWEEN) / 2;
    rpos2 = GAPBETWEEN + 3 * (cp->width + GAPBETWEEN) / 4;
    lwid = rpos1 - 2 * GAPBETWEEN;
    rwid1 = rpos2 - rpos1 - GAPBETWEEN;
    rwid2 = cp->width + GAPBETWEEN - rpos2;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos1;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid1;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext1, bid1);

    r.left = rpos2;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid2;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext2, bid2);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * An edit control on the right hand side, with a static to its left.
 */
static void staticedit_internal(struct ctlpos *cp, const char *stext,
                                int sid, int eid, int percentedit,
                                int style)
{
    const int height = (EDITHEIGHT > STATICHEIGHT ?
                        EDITHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos =
        GAPBETWEEN + (100 - percentedit) * (cp->width + GAPBETWEEN) / 100;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = rwid;
    r.bottom = EDITHEIGHT;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
          WS_EX_CLIENTEDGE, "", eid);

    cp->ypos += height + GAPBETWEEN;
}

void staticedit(struct ctlpos *cp, const char *stext,
                int sid, int eid, int percentedit)
{
    staticedit_internal(cp, stext, sid, eid, percentedit, 0);
}

void staticpassedit(struct ctlpos *cp, const char *stext,
                    int sid, int eid, int percentedit)
{
    staticedit_internal(cp, stext, sid, eid, percentedit, ES_PASSWORD);
}

/*
 * A drop-down list box on the right hand side, with a static to
 * its left.
 */
void staticddl(struct ctlpos *cp, const char *stext,
               int sid, int lid, int percentlist)
{
    const int height = (COMBOHEIGHT > STATICHEIGHT ?
                        COMBOHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos =
        GAPBETWEEN + (100 - percentlist) * (cp->width + GAPBETWEEN) / 100;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = rwid;
    r.bottom = COMBOHEIGHT*4;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWNLIST | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A combo box on the right hand side, with a static to its left.
 */
void staticcombo(struct ctlpos *cp, const char *stext,
                 int sid, int lid, int percentlist)
{
    const int height = (COMBOHEIGHT > STATICHEIGHT ?
                        COMBOHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos =
        GAPBETWEEN + (100 - percentlist) * (cp->width + GAPBETWEEN) / 100;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = rwid;
    r.bottom = COMBOHEIGHT*10;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWN | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A static, with a full-width drop-down list box below it.
 */
void staticddlbig(struct ctlpos *cp, const char *stext,
                  int sid, int lid)
{
    RECT r;

    if (stext) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
        cp->ypos += STATICHEIGHT;
    }

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = COMBOHEIGHT*4;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWNLIST | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);
    cp->ypos += COMBOHEIGHT + GAPBETWEEN;
}

/*
 * A big multiline edit control with a static labelling it.
 */
void bigeditctrl(struct ctlpos *cp, const char *stext,
                 int sid, int eid, int lines)
{
    RECT r;

    if (stext) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
    }

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = EDITHEIGHT + (lines - 1) * STATICHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE,
          WS_EX_CLIENTEDGE, "", eid);
}

/*
 * A list box with a static labelling it.
 */
/*
 * Font used by a ruled list box. It is derived from the dialog font at
 * a larger size, so the history list is easier to read than the rest
 * of the panel. Cached across dialogs: the source font handle is
 * remembered so the derived font is rebuilt if it ever changes.
 */
#define RULEDLIST_FONT_PERCENT 130

static HFONT ruledlist_font = NULL;
static HFONT ruledlist_font_src = NULL;

static HFONT ruledlist_getfont(HFONT src)
{
    LOGFONT lf;

    if (!src || !GetObject(src, sizeof(lf), &lf))
        return NULL;
    if (ruledlist_font && ruledlist_font_src == src)
        return ruledlist_font;

    lf.lfHeight = MulDiv(lf.lfHeight, RULEDLIST_FONT_PERCENT, 100);
    if (lf.lfHeight == 0)
        lf.lfHeight = -12;

    if (ruledlist_font)
        DeleteObject(ruledlist_font);
    ruledlist_font = CreateFontIndirect(&lf);
    ruledlist_font_src = src;
    return ruledlist_font;
}

void listbox(struct ctlpos *cp, const char *stext,
             int sid, int lid, int lines, bool multi, bool ruledlines)
{
    RECT r;

    if (stext != NULL) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
    }

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = LISTHEIGHT + (lines - 1) * LISTINCREMENT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "LISTBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          LBS_NOTIFY | LBS_HASSTRINGS | LBS_USETABSTOPS |
          (ruledlines ? LBS_OWNERDRAWFIXED : 0) |
          (multi ? LBS_MULTIPLESEL : 0),
          WS_EX_CLIENTEDGE, "", lid);

    if (ruledlines && cp->hwnd) {
        /*
         * A ruled list uses a larger font than the rest of the panel,
         * so its rows are taller too. Set both explicitly: the row
         * height must be measured from the font actually in use, or
         * the text gets clipped.
         */
        HFONT bigfont = ruledlist_getfont((HFONT)cp->font);
        HDC hdc;
        HFONT oldfont;
        TEXTMETRIC tm;
        int rowheight;

        if (bigfont)
            SendDlgItemMessage(cp->hwnd, lid, WM_SETFONT,
                               (WPARAM)bigfont, MAKELPARAM(true, 0));

        hdc = GetDC(cp->hwnd);
        oldfont = SelectObject(hdc, bigfont ? bigfont
                                  : (HFONT)cp->font);
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, oldfont);
        ReleaseDC(cp->hwnd, hdc);

        /*
         * Keep the row tight: the box height comes from the dialog
         * layout and cannot grow, so any padding here costs a whole
         * extra row at the bottom. The text is centred in the row by
         * DrawText, so no padding is needed for it to look right.
         */
        rowheight = tm.tmHeight + tm.tmExternalLeading;
        if (rowheight < 1)
            rowheight = 1;
        SendDlgItemMessage(cp->hwnd, lid, LB_SETITEMHEIGHT, 0,
                           (LPARAM)rowheight);
    }
}

/*
 * A tab-control substitute when a real tab control is unavailable.
 */
void ersatztab(struct ctlpos *cp, const char *stext, int sid, int lid,
               int s2id)
{
    const int height = (COMBOHEIGHT > STATICHEIGHT ?
                        COMBOHEIGHT : STATICHEIGHT);
    RECT r;
    int bigwid, lwid, rwid, rpos;
    static const int BIGGAP = 15;
    static const int MEDGAP = 3;

    bigwid = cp->width + 2 * GAPBETWEEN - 2 * BIGGAP;
    cp->ypos += MEDGAP;
    rpos = BIGGAP + (bigwid + BIGGAP) / 2;
    lwid = rpos - 2 * BIGGAP;
    rwid = bigwid + BIGGAP - rpos;

    r.left = BIGGAP;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - COMBOHEIGHT) / 2;
    r.right = rwid;
    r.bottom = COMBOHEIGHT * 10;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          CBS_DROPDOWNLIST | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);

    cp->ypos += height + MEDGAP + GAPBETWEEN;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = 2;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
          0, "", s2id);
}

/*
 * A static line, followed by an edit control on the left hand side
 * and a button on the right.
 */
void editbutton(struct ctlpos *cp, const char *stext, int sid,
                int eid, const char *btext, int bid)
{
    const int height = (EDITHEIGHT > PUSHBTNHEIGHT ?
                        EDITHEIGHT : PUSHBTNHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = STATICHEIGHT;
    cp->ypos += r.bottom + GAPWITHIN;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    rpos = GAPBETWEEN + 3 * (cp->width + GAPBETWEEN) / 4;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = lwid;
    r.bottom = EDITHEIGHT;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
          WS_EX_CLIENTEDGE, "", eid);

    r.left = rpos;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext, bid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A special control for manipulating an ordered preference list
 * (eg. for cipher selection).
 * XXX: this is a rough hack and could be improved.
 */
void prefslist(struct prefslist *hdl, struct ctlpos *cp, int lines,
               const char *stext, int sid, int listid, int upbid, int dnbid)
{
    const static int percents[] = { 5, 75, 20 };
    RECT r;
    int xpos, percent = 0, i;
    int listheight = LISTHEIGHT + (lines - 1) * LISTINCREMENT;
    const int BTNSHEIGHT = 2*PUSHBTNHEIGHT + GAPBETWEEN;
    int totalheight, buttonpos;

    /* Squirrel away IDs. */
    hdl->listid = listid;
    hdl->upbid  = upbid;
    hdl->dnbid  = dnbid;

    /* The static label. */
    if (stext != NULL) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
    }

    if (listheight > BTNSHEIGHT) {
        totalheight = listheight;
        buttonpos = (listheight - BTNSHEIGHT) / 2;
    } else {
        totalheight = BTNSHEIGHT;
        buttonpos = 0;
    }

    for (i=0; i<3; i++) {
        int left, wid;
        xpos = (cp->width + GAPBETWEEN) * percent / 100;
        left = xpos + GAPBETWEEN;
        percent += percents[i];
        xpos = (cp->width + GAPBETWEEN) * percent / 100;
        wid = xpos - left;

        switch (i) {
          case 1: {
            /* The drag list box. */
            r.left = left; r.right = wid;
            r.top = cp->ypos; r.bottom = listheight;
            HWND ctl = doctl(cp, r, "LISTBOX",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                             WS_VSCROLL | LBS_HASSTRINGS | LBS_USETABSTOPS,
                             WS_EX_CLIENTEDGE,
                             "", listid);
            p_MakeDragList(ctl);
            break;
          }

          case 2:
            /* The "Up" and "Down" buttons. */
            /* XXX worry about accelerators if we have more than one
             * prefslist on a panel */
            r.left = left; r.right = wid;
            r.top = cp->ypos + buttonpos; r.bottom = PUSHBTNHEIGHT;
            doctl(cp, r, "BUTTON",
                  BS_NOTIFY | WS_CHILD | WS_VISIBLE |
                  WS_TABSTOP | BS_PUSHBUTTON,
                  0, "上移(&U)", upbid);

            r.left = left; r.right = wid;
            r.top = cp->ypos + buttonpos + PUSHBTNHEIGHT + GAPBETWEEN;
            r.bottom = PUSHBTNHEIGHT;
            doctl(cp, r, "BUTTON",
                  BS_NOTIFY | WS_CHILD | WS_VISIBLE |
                  WS_TABSTOP | BS_PUSHBUTTON,
                  0, "下移(&D)", dnbid);

            break;

        }
    }

    cp->ypos += totalheight + GAPBETWEEN;

}

/*
 * Helper function for prefslist: move item in list box.
 */
static void pl_moveitem(HWND hwnd, int listid, int src, int dst)
{
    int tlen, val;
    char *txt;
    /* Get the item's data. */
    tlen = SendDlgItemMessage (hwnd, listid, LB_GETTEXTLEN, src, 0);
    txt = snewn(tlen+1, char);
    SendDlgItemMessage (hwnd, listid, LB_GETTEXT, src, (LPARAM) txt);
    val = SendDlgItemMessage (hwnd, listid, LB_GETITEMDATA, src, 0);
    /* Deselect old location. */
    SendDlgItemMessage (hwnd, listid, LB_SETSEL, false, src);
    /* Delete it at the old location. */
    SendDlgItemMessage (hwnd, listid, LB_DELETESTRING, src, 0);
    /* Insert it at new location. */
    SendDlgItemMessage (hwnd, listid, LB_INSERTSTRING, dst,
                        (LPARAM) txt);
    SendDlgItemMessage (hwnd, listid, LB_SETITEMDATA, dst,
                        (LPARAM) val);
    /* Set selection. */
    SendDlgItemMessage (hwnd, listid, LB_SETCURSEL, dst, 0);
    sfree (txt);
}

int pl_itemfrompt(HWND hwnd, POINT cursor, bool scroll)
{
    int ret;
    POINT uppoint, downpoint;
    int updist, downdist, upitem, downitem, i;

    /*
     * Ghastly hackery to try to figure out not which
     * _item_, but which _gap between items_, the user
     * is pointing at. We do this by first working out
     * which list item is under the cursor, and then
     * working out how far the cursor would have to
     * move up or down before the answer was different.
     * Then we put the insertion point _above_ the
     * current item if the upper edge is closer than
     * the lower edge, or _below_ it if vice versa.
     */
    ret = p_LBItemFromPt(hwnd, cursor, scroll);
    if (ret == -1)
        return ret;
    ret = p_LBItemFromPt(hwnd, cursor, false);
    updist = downdist = 0;
    for (i = 1; i < 4096 && (!updist || !downdist); i++) {
        uppoint = downpoint = cursor;
        uppoint.y -= i;
        downpoint.y += i;
        upitem = p_LBItemFromPt(hwnd, uppoint, false);
        downitem = p_LBItemFromPt(hwnd, downpoint, false);
        if (!updist && upitem != ret)
            updist = i;
        if (!downdist && downitem != ret)
            downdist = i;
    }
    if (downdist < updist)
        ret++;
    return ret;
}

/*
 * Handler for prefslist above.
 *
 * Return value has bit 0 set if the dialog box procedure needs to
 * return true from handling this message; it has bit 1 set if a
 * change may have been made in the contents of the list.
 */
int handle_prefslist(struct prefslist *hdl,
                     int *array, int maxmemb,
                     bool is_dlmsg, HWND hwnd,
                     WPARAM wParam, LPARAM lParam)
{
    int i;
    int ret = 0;

    if (is_dlmsg) {

        if ((int)wParam == hdl->listid) {
            DRAGLISTINFO *dlm = (DRAGLISTINFO *)lParam;
            int dest = 0;              /* initialise to placate gcc */
            switch (dlm->uNotification) {
              case DL_BEGINDRAG:
                /* Add a dummy item to make pl_itemfrompt() work
                 * better.
                 * FIXME: this causes scrollbar glitches if the count of
                 *        listbox contains >= its height. */
                hdl->dummyitem =
                    SendDlgItemMessage(hwnd, hdl->listid,
                                       LB_ADDSTRING, 0, (LPARAM) "");

                hdl->srcitem = p_LBItemFromPt(dlm->hWnd, dlm->ptCursor, true);
                hdl->dragging = false;
                /* XXX hack Q183115 */
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, true);
                ret |= 1; break;
              case DL_CANCELDRAG:
                p_DrawInsert(hwnd, dlm->hWnd, -1);     /* Clear arrow */
                SendDlgItemMessage(hwnd, hdl->listid,
                                   LB_DELETESTRING, hdl->dummyitem, 0);
                hdl->dragging = false;
                ret |= 1; break;
              case DL_DRAGGING:
                hdl->dragging = true;
                dest = pl_itemfrompt(dlm->hWnd, dlm->ptCursor, true);
                if (dest > hdl->dummyitem) dest = hdl->dummyitem;
                p_DrawInsert (hwnd, dlm->hWnd, dest);
                if (dest >= 0)
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DL_MOVECURSOR);
                else
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DL_STOPCURSOR);
                ret |= 1; break;
              case DL_DROPPED:
                if (hdl->dragging) {
                    dest = pl_itemfrompt(dlm->hWnd, dlm->ptCursor, true);
                    if (dest > hdl->dummyitem) dest = hdl->dummyitem;
                    p_DrawInsert (hwnd, dlm->hWnd, -1);
                }
                SendDlgItemMessage(hwnd, hdl->listid,
                                   LB_DELETESTRING, hdl->dummyitem, 0);
                if (hdl->dragging) {
                    hdl->dragging = false;
                    if (dest >= 0) {
                        /* Correct for "missing" item. */
                        if (dest > hdl->srcitem) dest--;
                        pl_moveitem(hwnd, hdl->listid, hdl->srcitem, dest);
                    }
                    ret |= 2;
                }
                ret |= 1; break;
            }
        }

    } else {

        if (((LOWORD(wParam) == hdl->upbid) ||
             (LOWORD(wParam) == hdl->dnbid)) &&
            ((HIWORD(wParam) == BN_CLICKED) ||
             (HIWORD(wParam) == BN_DOUBLECLICKED))) {
            /* Move an item up or down the list. */
            /* Get the current selection, if any. */
            int selection = SendDlgItemMessage (hwnd, hdl->listid, LB_GETCURSEL, 0, 0);
            if (selection == LB_ERR) {
                MessageBeep(0);
            } else {
                int nitems;
                /* Get the total number of items. */
                nitems = SendDlgItemMessage (hwnd, hdl->listid, LB_GETCOUNT, 0, 0);
                /* Should we do anything? */
                if (LOWORD(wParam) == hdl->upbid && (selection > 0))
                    pl_moveitem(hwnd, hdl->listid, selection, selection - 1);
                else if (LOWORD(wParam) == hdl->dnbid && (selection < nitems - 1))
                    pl_moveitem(hwnd, hdl->listid, selection, selection + 1);
                ret |= 2;
            }

        }

    }

    if (array) {
        /* Update array to match the list box. */
        for (i=0; i < maxmemb; i++)
            array[i] = SendDlgItemMessage (hwnd, hdl->listid, LB_GETITEMDATA,
                                           i, 0);
    }

    return ret;
}

/*
 * A progress bar (from Common Controls). We like our progress bars
 * to be smooth and unbroken, without those ugly divisions; some
 * older compilers may not support that, but that's life.
 */
void progressbar(struct ctlpos *cp, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = PROGBARHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;

    doctl(cp, r, PROGRESS_CLASS, WS_CHILD | WS_VISIBLE
#ifdef PBS_SMOOTH
          | PBS_SMOOTH
#endif
          , WS_EX_CLIENTEDGE, "", id);
}

/* ----------------------------------------------------------------------
 * Platform-specific side of portable dialog-box mechanism.
 */

/*
 * This function takes a string, escapes all the ampersands, and
 * places a single (unescaped) ampersand in front of the first
 * occurrence of the given shortcut character (which may be
 * NO_SHORTCUT).
 *
 * Return value is a malloc'ed copy of the processed version of the
 * string.
 */
static char *shortcut_escape(const char *text, char shortcut)
{
    if (!text)
        return NULL;                   /* sfree won't choke on this */

    strbuf *sb = strbuf_new();
    shortcut = tolower((unsigned char)shortcut);

    const char *p = text;
    while (*p) {
        if (shortcut != NO_SHORTCUT &&
            tolower((unsigned char)*p) == shortcut) {
            put_byte(sb, '&');
            shortcut = NO_SHORTCUT;    /* stop it happening twice */
        } else if (*p == '&') {
            put_byte(sb, '&');
        }
        put_byte(sb, *p++);
    }
    return strbuf_to_str(sb);
}

void winctrl_add_shortcuts(struct dlgparam *dp, struct winctrl *c)
{
    int i;
    for (i = 0; i < lenof(c->shortcuts); i++)
        if (c->shortcuts[i] != NO_SHORTCUT) {
            unsigned char s = tolower((unsigned char)c->shortcuts[i]);
            /*
             * The dialog holds the controls of every panel at once,
             * even though only one panel is visible, and the Chinese
             * labels reuse letters across panels. Count the claims
             * instead of requiring uniqueness, so a duplicate is
             * harmless and removing one panel's control does not
             * clear the letter for another panel's.
             */
            if (dp->shortcuts[s] < 255)
                dp->shortcuts[s]++;
        }
}

void winctrl_rem_shortcuts(struct dlgparam *dp, struct winctrl *c)
{
    int i;
    for (i = 0; i < lenof(c->shortcuts); i++)
        if (c->shortcuts[i] != NO_SHORTCUT) {
            unsigned char s = tolower((unsigned char)c->shortcuts[i]);
            if (dp->shortcuts[s])
                dp->shortcuts[s]--;
        }
}

static int winctrl_cmp_byctrl(void *av, void *bv)
{
    struct winctrl *a = (struct winctrl *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (a->ctrl < b->ctrl)
        return -1;
    else if (a->ctrl > b->ctrl)
        return +1;
    else
        return 0;
}
static int winctrl_cmp_byid(void *av, void *bv)
{
    struct winctrl *a = (struct winctrl *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (a->base_id < b->base_id)
        return -1;
    else if (a->base_id > b->base_id)
        return +1;
    else
        return 0;
}
static int winctrl_cmp_byctrl_find(void *av, void *bv)
{
    dlgcontrol *a = (dlgcontrol *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (a < b->ctrl)
        return -1;
    else if (a > b->ctrl)
        return +1;
    else
        return 0;
}
static int winctrl_cmp_byid_find(void *av, void *bv)
{
    int *a = (int *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (*a < b->base_id)
        return -1;
    else if (*a >= b->base_id + b->num_ids)
        return +1;
    else
        return 0;
}

void winctrl_init(struct winctrls *wc)
{
    wc->byctrl = newtree234(winctrl_cmp_byctrl);
    wc->byid = newtree234(winctrl_cmp_byid);
}
void winctrl_cleanup(struct winctrls *wc)
{
    struct winctrl *c;

    while ((c = index234(wc->byid, 0)) != NULL) {
        winctrl_remove(wc, c);
        sfree(c->data);
        sfree(c);
    }

    freetree234(wc->byctrl);
    freetree234(wc->byid);
    wc->byctrl = wc->byid = NULL;
}

void winctrl_add(struct winctrls *wc, struct winctrl *c)
{
    struct winctrl *ret;
    if (c->ctrl) {
        ret = add234(wc->byctrl, c);
        assert(ret == c);
    }
    ret = add234(wc->byid, c);
    assert(ret == c);
}

void winctrl_remove(struct winctrls *wc, struct winctrl *c)
{
    struct winctrl *ret;
    ret = del234(wc->byctrl, c);
    ret = del234(wc->byid, c);
    assert(ret == c);
}

struct winctrl *winctrl_findbyctrl(struct winctrls *wc, dlgcontrol *ctrl)
{
    return find234(wc->byctrl, ctrl, winctrl_cmp_byctrl_find);
}

struct winctrl *winctrl_findbyid(struct winctrls *wc, int id)
{
    return find234(wc->byid, &id, winctrl_cmp_byid_find);
}

struct winctrl *winctrl_findbyindex(struct winctrls *wc, int index)
{
    return index234(wc->byid, index);
}

static void move_windows(HWND hwnd, int base_id, int num_ids, LONG dy)
{
    if (!dy)
        return;
    for (int i = 0; i < num_ids; i++) {
        HWND win = GetDlgItem(hwnd, base_id + i);

        RECT rect;
        if (!GetWindowRect(win, &rect))
            continue;

        POINT p;
        p.x = rect.left;
        p.y = rect.top + dy;
        if (!ScreenToClient(hwnd, &p))
            continue;

        SetWindowPos(win, NULL, p.x, p.y, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
}

void winctrl_layout(struct dlgparam *dp, struct winctrls *wc,
                    struct ctlpos *cp, struct controlset *s, int *id)
{
    struct ctlpos columns[16];
    int ncols, colstart, colspan;

    struct ctlpos tabdelays[16];
    dlgcontrol *tabdelayed[16];
    int ntabdelays;

    struct ctlpos pos;

    char shortcuts[MAX_SHORTCUTS_PER_CTRL];
    int nshortcuts;
    char *escaped;
    int i, actual_base_id, base_id, num_ids, align_id_relative;
    void *data;

    base_id = *id;

    ctrlset_normalise_aligns(s);

    /* Start a containing box, if we have a boxname. */
    if (s->boxname && *s->boxname) {
        struct winctrl *c = snew(struct winctrl);
        c->ctrl = NULL;
        c->base_id = c->align_id = base_id;
        c->num_ids = 1;
        c->data = NULL;
        memset(c->shortcuts, NO_SHORTCUT, lenof(c->shortcuts));
        winctrl_add(wc, c);
        beginbox(cp, s->boxtitle, base_id);
        base_id++;
    }

    /* Draw a title, if we have one. */
    if (!s->boxname && s->boxtitle) {
        struct winctrl *c = snew(struct winctrl);
        c->ctrl = NULL;
        c->base_id = c->align_id = base_id;
        c->num_ids = 1;
        c->data = dupstr(s->boxtitle);
        memset(c->shortcuts, NO_SHORTCUT, lenof(c->shortcuts));
        winctrl_add(wc, c);
        paneltitle(cp, base_id);
        base_id++;
    }

    /* Initially we have just one column. */
    ncols = 1;
    columns[0] = *cp;                  /* structure copy */

    /* And initially, there are no pending tab-delayed controls. */
    ntabdelays = 0;

    /* Loop over each control in the controlset. */
    for (i = 0; i < s->ncontrols; i++) {
        dlgcontrol *ctrl = s->ctrls[i];

        /*
         * Generic processing that pertains to all control types.
         * At the end of this if statement, we'll have produced
         * `ctrl' (a pointer to the control we have to create, or
         * think about creating, in this iteration of the loop),
         * `pos' (a suitable ctlpos with which to position it), and
         * `c' (a winctrl structure to receive details of the
         * dialog IDs). Or we'll have done a `continue', if it was
         * CTRL_COLUMNS and doesn't require any control creation at
         * all.
         */
        if (ctrl->type == CTRL_COLUMNS) {
            assert((ctrl->columns.ncols == 1) ^ (ncols == 1));

            if (ncols == 1) {
                /*
                 * We're splitting into multiple columns.
                 */
                int lpercent, rpercent, lx, rx, i;

                ncols = ctrl->columns.ncols;
                assert(ncols <= lenof(columns));
                for (i = 1; i < ncols; i++)
                    columns[i] = columns[0];   /* structure copy */

                lpercent = 0;
                for (i = 0; i < ncols; i++) {
                    rpercent = lpercent + ctrl->columns.percentages[i];
                    lx = columns[i].xoff + lpercent *
                        (columns[i].width + GAPBETWEEN) / 100;
                    rx = columns[i].xoff + rpercent *
                        (columns[i].width + GAPBETWEEN) / 100;
                    columns[i].xoff = lx;
                    columns[i].width = rx - lx - GAPBETWEEN;
                    lpercent = rpercent;
                }
            } else {
                /*
                 * We're recombining the various columns into one.
                 */
                int maxy = columns[0].ypos;
                int i;
                for (i = 1; i < ncols; i++)
                    if (maxy < columns[i].ypos)
                        maxy = columns[i].ypos;
                ncols = 1;
                columns[0] = *cp;      /* structure copy */
                columns[0].ypos = maxy;
            }

            continue;
        } else if (ctrl->type == CTRL_TABDELAY) {
            int i;

            assert(!ctrl->delay_taborder);
            ctrl = ctrl->tabdelay.ctrl;

            for (i = 0; i < ntabdelays; i++)
                if (tabdelayed[i] == ctrl)
                    break;
            assert(i < ntabdelays);    /* we have to have found it */

            pos = tabdelays[i];        /* structure copy */

            colstart = colspan = -1;   /* indicate this was tab-delayed */

        } else {
            /*
             * If it wasn't one of those, it's a genuine control;
             * so we'll have to compute a position for it now, by
             * checking its column span.
             */
            int col;

            colstart = COLUMN_START(ctrl->column);
            colspan = COLUMN_SPAN(ctrl->column);

            pos = columns[colstart];   /* structure copy */
            pos.width = columns[colstart+colspan-1].width +
                (columns[colstart+colspan-1].xoff - columns[colstart].xoff);

            for (col = colstart; col < colstart+colspan; col++)
                if (pos.ypos < columns[col].ypos)
                    pos.ypos = columns[col].ypos;

            /*
             * If this control is to be tabdelayed, add it to the
             * tabdelay list, and unset pos.hwnd to inhibit actual
             * control creation.
             */
            if (ctrl->delay_taborder) {
                assert(ntabdelays < lenof(tabdelays));
                tabdelays[ntabdelays] = pos;   /* structure copy */
                tabdelayed[ntabdelays] = ctrl;
                ntabdelays++;
                pos.hwnd = NULL;
            }
        }

        /* Most controls don't need anything in c->data. */
        data = NULL;

        /* And they all start off with no shortcuts registered. */
        memset(shortcuts, NO_SHORTCUT, lenof(shortcuts));
        nshortcuts = 0;

        /* Almost all controls start at base_id. */
        actual_base_id = base_id;

        /* For vertical alignment purposes, the most relevant control
         * in a group is usually the last one. But that can be
         * overridden occasionally. */
        align_id_relative = -1;

        /*
         * Now we're ready to actually create the control, by
         * switching on its type.
         */
        switch (ctrl->type) {
          case CTRL_TEXT:
            if (ctrl->text.wrap) {
                char *wrapped, *escaped;
                int lines;
                num_ids = 1;
                wrapped = staticwrap(&pos, cp->hwnd,
                                     ctrl->label, &lines);
                escaped = shortcut_escape(wrapped, NO_SHORTCUT);
                statictext(&pos, escaped, lines, base_id);
                sfree(escaped);
                sfree(wrapped);
            } else {
                num_ids = 1;
                editboxfw(&pos, false, true, NULL, 0, base_id);
                SetDlgItemText(pos.hwnd, base_id, ctrl->label);
                MakeDlgItemBorderless(pos.hwnd, base_id);
            }
            break;
          case CTRL_EDITBOX:
            num_ids = 2;               /* static, edit */
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->editbox.shortcut);
            shortcuts[nshortcuts++] = ctrl->editbox.shortcut;
            if (ctrl->editbox.percentwidth == 100) {
                if (ctrl->editbox.has_list)
                    combobox(&pos, escaped,
                             base_id, base_id+1);
                else
                    editboxfw(&pos, ctrl->editbox.password, false, escaped,
                              base_id, base_id+1);
            } else {
                if (ctrl->editbox.has_list) {
                    staticcombo(&pos, escaped, base_id, base_id+1,
                                ctrl->editbox.percentwidth);
                } else {
                    (ctrl->editbox.password ? staticpassedit : staticedit)
                        (&pos, escaped, base_id, base_id+1,
                         ctrl->editbox.percentwidth);
                }
            }
            sfree(escaped);
            break;
          case CTRL_RADIO: {
            num_ids = ctrl->radio.nbuttons + 1;   /* label as well */
            struct radio *buttons;
            int i;

            escaped = shortcut_escape(ctrl->label,
                                      ctrl->radio.shortcut);
            shortcuts[nshortcuts++] = ctrl->radio.shortcut;

            buttons = snewn(ctrl->radio.nbuttons, struct radio);

            for (i = 0; i < ctrl->radio.nbuttons; i++) {
                buttons[i].text =
                    shortcut_escape(ctrl->radio.buttons[i],
                                    (char)(ctrl->radio.shortcuts ?
                                           ctrl->radio.shortcuts[i] :
                                           NO_SHORTCUT));
                buttons[i].id = base_id + 1 + i;
                if (ctrl->radio.shortcuts) {
                    assert(nshortcuts < MAX_SHORTCUTS_PER_CTRL);
                    shortcuts[nshortcuts++] = ctrl->radio.shortcuts[i];
                }
            }

            radioline_common(&pos, escaped, base_id,
                             ctrl->radio.ncolumns,
                             buttons, ctrl->radio.nbuttons);

            for (i = 0; i < ctrl->radio.nbuttons; i++) {
                sfree((char *)buttons[i].text);
            }
            sfree(buttons);
            sfree(escaped);
            break;
          }
          case CTRL_CHECKBOX:
            num_ids = 1;
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->checkbox.shortcut);
            shortcuts[nshortcuts++] = ctrl->checkbox.shortcut;
            checkbox(&pos, escaped, base_id);
            sfree(escaped);
            break;
          case CTRL_BUTTON:
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->button.shortcut);
            shortcuts[nshortcuts++] = ctrl->button.shortcut;
            if (ctrl->button.iscancel)
                actual_base_id = IDCANCEL;
            num_ids = 1;
            button(&pos, escaped, actual_base_id, ctrl->button.isdefault);
            sfree(escaped);
            break;
          case CTRL_LISTBOX:
            num_ids = 2;
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->listbox.shortcut);
            shortcuts[nshortcuts++] = ctrl->listbox.shortcut;
            if (ctrl->listbox.draglist) {
                data = snew(struct prefslist);
                num_ids = 4;
                prefslist(data, &pos, ctrl->listbox.height, escaped,
                          base_id, base_id+1, base_id+2, base_id+3);
                shortcuts[nshortcuts++] = 'u';   /* Up */
                shortcuts[nshortcuts++] = 'd';   /* Down */
            } else if (ctrl->listbox.height == 0) {
                /* Drop-down list. */
                if (ctrl->listbox.percentwidth == 100) {
                    staticddlbig(&pos, escaped,
                                 base_id, base_id+1);
                } else {
                    staticddl(&pos, escaped, base_id,
                              base_id+1, ctrl->listbox.percentwidth);
                }
            } else {
                /* Ordinary list. */
                listbox(&pos, escaped, base_id, base_id+1,
                        ctrl->listbox.height, ctrl->listbox.multisel,
                        ctrl->listbox.ruledlines);
            }
            if (ctrl->listbox.ncols) {
                /*
                 * This method of getting the box width is a bit of
                 * a hack; we'd do better to try to retrieve the
                 * actual width in dialog units from doctl() just
                 * before MapDialogRect. But that's going to be no
                 * fun, and this should be good enough accuracy.
                 */
                int width = cp->width * ctrl->listbox.percentwidth;
                int *tabarray;
                int i, percent;

                tabarray = snewn(ctrl->listbox.ncols-1, int);
                percent = 0;
                for (i = 0; i < ctrl->listbox.ncols-1; i++) {
                    percent += ctrl->listbox.percentages[i];
                    tabarray[i] = width * percent / 10000;
                }
                SendDlgItemMessage(cp->hwnd, base_id+1, LB_SETTABSTOPS,
                                   ctrl->listbox.ncols-1, (LPARAM)tabarray);
                sfree(tabarray);
            }
            sfree(escaped);
            break;
          case CTRL_FILESELECT:
            escaped = shortcut_escape(ctrl->label, ctrl->fileselect.shortcut);
            shortcuts[nshortcuts++] = ctrl->fileselect.shortcut;
            num_ids = 3;
            if (!ctrl->fileselect.just_button) {
                editbutton(&pos, escaped, base_id, base_id+1,
                           "浏览...", base_id+2);
            } else {
                button(&pos, escaped, base_id+2, false);
            }
            sfree(escaped);
            break;
          case CTRL_FONTSELECT:
            num_ids = 3;
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->fontselect.shortcut);
            shortcuts[nshortcuts++] = ctrl->fontselect.shortcut;
            statictext(&pos, escaped, 1, base_id);
            staticbtn(&pos, "", base_id+1, "修改...", base_id+2);
            data = fontspec_new_default();
            sfree(escaped);
            break;
          default:
            unreachable("bad control type in winctrl_layout");
        }

        /* Translate the original align_id_relative of -1 into n-1 */
        if (align_id_relative < 0)
            align_id_relative += num_ids;

        /*
         * Create a `struct winctrl' for this control, and advance
         * the dialog ID counter, if it's actually been created
         * (and isn't tabdelayed).
         */
        if (pos.hwnd) {
            struct winctrl *c = snew(struct winctrl);

            c->ctrl = ctrl;
            c->base_id = actual_base_id;
            c->align_id = c->base_id + align_id_relative;
            c->num_ids = num_ids;
            c->data = data;
            memcpy(c->shortcuts, shortcuts, sizeof(shortcuts));
            winctrl_add(wc, c);
            winctrl_add_shortcuts(dp, c);
            if (actual_base_id == base_id)
                base_id += num_ids;

            if (ctrl->align_next_to) {
                /*
                 * Implement align_next_to by looking at the y extents
                 * of the two controls now that both are created, and
                 * moving one or the other downwards so that they're
                 * centred on a common horizontal line.
                 */
                LONG mid2 = 0;
                for (dlgcontrol *thisctrl = ctrl; thisctrl;
                     thisctrl = thisctrl->align_next_to) {
                    struct winctrl *thisc = winctrl_findbyctrl(wc, thisctrl);
                    assert(thisc);

                    HWND win = GetDlgItem(pos.hwnd, thisc->align_id);
                    assert(win);

                    RECT rect;
                    if (!GetWindowRect(win, &rect))
                        continue;
                    if (mid2 < rect.top + rect.bottom)
                        mid2 = rect.top + rect.bottom;
                }

                for (dlgcontrol *thisctrl = ctrl; thisctrl;
                     thisctrl = thisctrl->align_next_to) {
                    struct winctrl *thisc = winctrl_findbyctrl(wc, thisctrl);
                    assert(thisc);

                    HWND win = GetDlgItem(pos.hwnd, thisc->align_id);
                    assert(win);

                    RECT rect;
                    if (!GetWindowRect(win, &rect))
                        continue;

                    LONG dy = (mid2 - (rect.top + rect.bottom)) / 2;
                    move_windows(pos.hwnd, thisc->base_id, thisc->num_ids, dy);
                }
            }
        } else {
            sfree(data);
        }

        if (colstart >= 0) {
            /*
             * Update the ypos in all columns crossed by this
             * control.
             */
            int i;
            for (i = colstart; i < colstart+colspan; i++)
                columns[i].ypos = pos.ypos;
        }
    }

    /*
     * We've now finished laying out the controls; so now update
     * the ctlpos and control ID that were passed in, terminate
     * any containing box, and return.
     */
    for (i = 0; i < ncols; i++)
        if (cp->ypos < columns[i].ypos)
            cp->ypos = columns[i].ypos;
    *id = base_id;

    if (s->boxname && *s->boxname)
        endbox(cp);
}

static void winctrl_set_focus(dlgcontrol *ctrl, struct dlgparam *dp,
                              bool has_focus)
{
    if (has_focus) {
        if (dp->focused)
            dp->lastfocused = dp->focused;
        dp->focused = ctrl;
    } else if (!has_focus && dp->focused == ctrl) {
        dp->lastfocused = dp->focused;
        dp->focused = NULL;
    }
}

dlgcontrol *dlg_last_focused(dlgcontrol *ctrl, dlgparam *dp)
{
    return dp->focused == ctrl ? dp->lastfocused : dp->focused;
}

/*
 * The dialog-box procedure calls this function to handle Windows
 * messages on a control we manage.
 */
/*
 * Enable or disable a control, looking up its Windows handle by
 * matching the dlgcontrol pointer against the instantiated controls.
 */
static void dlg_set_ctrl_enabled(dlgparam *dp, dlgcontrol *ctrl, bool enabled)
{
    if (!ctrl)
        return;

    for (int j = 0; j < dp->nctrltrees; j++) {
        for (int i = 0; ; i++) {
            struct winctrl *c = winctrl_findbyindex(dp->controltrees[j], i);
            if (!c)
                break;
            if (c->ctrl == ctrl) {
                HWND hwnd = GetDlgItem(dp->hwnd, c->base_id);
                if (hwnd)
                    EnableWindow(hwnd, enabled);
                return;
            }
        }
    }
}

void dlg_update_apply_buttons(dlgparam *dp)
{
    if (!dp->ctrlbox || !dp->track_changes)
        return;

    /* cn: the button reflects a live comparison against the baseline
     * snapshot, so undoing a change (or re-typing the original value)
     * disables it again. */
    bool changed = dp->baseline && !conf_equal(dp->baseline, dp->data);
    dlg_set_ctrl_enabled(dp, dp->ctrlbox->apply_button, changed);
    dlg_set_ctrl_enabled(dp, dp->ctrlbox->undo_button, dp->nundo > 0);
}

bool dlg_has_unsaved_changes(dlgparam *dp)
{
    /* cn: live comparison as well, so closing after reverting every
     * change does not prompt. */
    return dp->track_changes && dp->baseline &&
        !conf_equal(dp->baseline, dp->data);
}

void dlg_mark_clean(dlgparam *dp)
{
    for (int i = 0; i < dp->nundo; i++)
        conf_free(dp->undo_stack[i]);
    dp->nundo = 0;
    dp->dirty = false;
    dp->last_change_ctrl = NULL;
    /* cn: rebase the pristine snapshot, so the next change list is
     * relative to what was just saved. */
    if (dp->baseline) {
        conf_free(dp->baseline);
        dp->baseline = conf_copy((Conf *)dp->data);
    }
    dlg_update_apply_buttons(dp);
}

bool dlg_undo_change(dlgparam *dp)
{
    if (!dp->track_changes || dp->nundo <= 0)
        return false;

    Conf *prev = dp->undo_stack[--dp->nundo];
    conf_copy_into((Conf *)dp->data, prev);
    conf_free(prev);

    dp->last_change_ctrl = NULL;
    dp->dirty = (dp->nundo > 0);

    dlg_refresh(NULL, dp);
    dlg_update_apply_buttons(dp);
    return true;
}

/*
 * Called just before an EVENT_VALCHANGE reaches a control's handler,
 * i.e. while the settings still hold the value the edit is about to
 * replace. Snapshot that value so "Undo" can restore it. Repeated
 * edits of the same control are merged into one history step.
 */
static void dlg_note_change(dlgparam *dp, dlgcontrol *ctrl)
{
    if (!dp->track_changes)
        return;

    if (dp->last_change_ctrl != ctrl && dp->nundo < 100) {
        if (dp->nundo == dp->undosize) {
            dp->undosize = dp->undosize ? dp->undosize * 2 : 16;
            dp->undo_stack = sresize(dp->undo_stack, dp->undosize, Conf *);
        }
        dp->undo_stack[dp->nundo++] = conf_copy((Conf *)dp->data);
    }

    dp->last_change_ctrl = ctrl;
    dp->dirty = true;
}

static void dlg_send_event(dlgcontrol *ctrl, dlgparam *dp, int event)
{
    if (event == EVENT_VALCHANGE)
        dlg_note_change(dp, ctrl);
    ctrl->handler(ctrl, dp, dp->data, event);
    /* cn: recompute the dirty-gated buttons AFTER the handler has
     * applied the change to the Conf, so the live comparison is not
     * one step behind. */
    dlg_update_apply_buttons(dp);
}

bool winctrl_handle_command(struct dlgparam *dp, UINT msg,
                            WPARAM wParam, LPARAM lParam)
{
    struct winctrl *c;
    dlgcontrol *ctrl;
    int i, id;
    bool ret;
    static UINT draglistmsg = WM_NULL;

    /*
     * Filter out pointless window messages. Our interest is in
     * WM_COMMAND and the drag list message, and nothing else.
     */
    if (draglistmsg == WM_NULL)
        draglistmsg = RegisterWindowMessage (DRAGLISTMSGSTRING);

    if (msg != draglistmsg && msg != WM_COMMAND && msg != WM_DRAWITEM)
        return false;

    /*
     * Look up the control ID in our data.
     */
    c = NULL;
    for (i = 0; i < dp->nctrltrees; i++) {
        c = winctrl_findbyid(dp->controltrees[i], LOWORD(wParam));
        if (c)
            break;
    }
    if (!c)
        return false;                  /* we have nothing to do */

    if (msg == WM_DRAWITEM) {
        LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT) lParam;

        if (di->CtlType == ODT_LISTBOX) {
            /*
             * Owner-drawn list box: draw the entry text ourselves and
             * rule a thin line under each row, so the list reads like
             * a table.
             */
            HDC hdc = di->hDC;
            RECT r = di->rcItem;
            COLORREF bgcol, fgcol;
            HBRUSH bg;
            char *text;
            int len;

            if (di->itemID == (UINT)-1)
                return true;           /* nothing to draw */

            len = (int)SendMessage(di->hwndItem, LB_GETTEXTLEN,
                                   di->itemID, 0);
            if (len < 0)
                return true;
            text = snewn(len + 1, char);
            SendMessage(di->hwndItem, LB_GETTEXT, di->itemID,
                        (LPARAM)text);

            if (di->itemState & ODS_SELECTED) {
                bgcol = GetSysColor(COLOR_HIGHLIGHT);
                fgcol = GetSysColor(COLOR_HIGHLIGHTTEXT);
            } else {
                bgcol = GetSysColor(COLOR_WINDOW);
                fgcol = GetSysColor(COLOR_WINDOWTEXT);
            }

            bg = CreateSolidBrush(bgcol);
            FillRect(hdc, &r, bg);
            DeleteObject(bg);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fgcol);

            /*
             * Entries may use tabs to separate columns; expand them
             * the way the list box's tab stops would.
             */
            {
                RECT tr = r;
                tr.left += 2;
                tr.right -= 2;
                DrawText(hdc, text, len, &tr,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                         DT_EXPANDTABS | DT_NOPREFIX);
            }
            sfree(text);

            /*
             * The rule itself: one pixel at the bottom of the row,
             * in a light grey that reads as a table divider without
             * competing with the text. Skip it on the selected row so
             * the highlight stays clean.
             */
            if (!(di->itemState & ODS_SELECTED)) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(208, 208, 208));
                HPEN oldpen = SelectObject(hdc, pen);
                MoveToEx(hdc, r.left, r.bottom - 1, NULL);
                LineTo(hdc, r.right, r.bottom - 1);
                SelectObject(hdc, oldpen);
                DeleteObject(pen);
            }

            return true;
        }

        /*
         * Owner-draw request for a panel title.
         */
        HDC hdc = di->hDC;
        RECT r = di->rcItem;
        SIZE s;
        HBRUSH bg;
        HFONT oldfont, boldfont = NULL;
        LOGFONT lf;

        SetMapMode(hdc, MM_TEXT);      /* ensure logical units == pixels */

        /* 灰底标题条 */
        bg = CreateSolidBrush(RGB(96, 96, 96));
        FillRect(hdc, &r, bg);
        DeleteObject(bg);

        /* 由当前字体派生加粗版本，避免硬编码字号 */
        oldfont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
        if (oldfont && GetObject(oldfont, sizeof(lf), &lf)) {
            lf.lfWeight = FW_BOLD;
            boldfont = CreateFontIndirect(&lf);
            if (boldfont)
                SelectObject(hdc, boldfont);
        }

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        GetTextExtentPoint32(hdc, (char *)c->data,
                             strlen((char *)c->data), &s);
        DrawEdge(hdc, &r, EDGE_ETCHED, BF_ADJUST | BF_RECT);
        TextOut(hdc,
                r.left + (r.right-r.left-s.cx)/2,
                r.top + (r.bottom-r.top-s.cy)/2,
                (char *)c->data, strlen((char *)c->data));

        if (boldfont) {
            SelectObject(hdc, oldfont);
            DeleteObject(boldfont);
        }

        return true;
    }

    ctrl = c->ctrl;
    id = LOWORD(wParam) - c->base_id;

    if (!ctrl || !ctrl->handler)
        return false;                  /* nothing we can do here */

    /*
     * From here on we do not issue `return' statements until the
     * very end of the dialog box: any event handler is entitled to
     * ask for a colour selector, so we _must_ always allow control
     * to reach the end of this switch statement so that the
     * subsequent code can test dp->coloursel_wanted().
     */
    ret = false;
    dp->coloursel_wanted = false;

    /*
     * Now switch on the control type and the message.
     */
    switch (ctrl->type) {
      case CTRL_EDITBOX:
        if (msg == WM_COMMAND && !ctrl->editbox.has_list &&
            (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == EN_SETFOCUS);
        if (msg == WM_COMMAND && ctrl->editbox.has_list &&
            (HIWORD(wParam)==CBN_SETFOCUS || HIWORD(wParam)==CBN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == CBN_SETFOCUS);

        if (msg == WM_COMMAND && !ctrl->editbox.has_list &&
            HIWORD(wParam) == EN_CHANGE)
            dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
        if (msg == WM_COMMAND &&
            ctrl->editbox.has_list) {
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int index, len;
                char *text;

                index = SendDlgItemMessage(dp->hwnd, c->base_id+1,
                                           CB_GETCURSEL, 0, 0);
                len = SendDlgItemMessage(dp->hwnd, c->base_id+1,
                                         CB_GETLBTEXTLEN, index, 0);
                text = snewn(len+1, char);
                SendDlgItemMessage(dp->hwnd, c->base_id+1, CB_GETLBTEXT,
                                   index, (LPARAM)text);
                SetDlgItemText(dp->hwnd, c->base_id+1, text);
                sfree(text);
                dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
            } else if (HIWORD(wParam) == CBN_EDITCHANGE) {
                dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
            } else if (HIWORD(wParam) == CBN_KILLFOCUS) {
                dlg_send_event(ctrl, dp, EVENT_REFRESH);
            }

        }
        break;
      case CTRL_RADIO:
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        /*
         * We sometimes get spurious BN_CLICKED messages for the
         * radio button that is just about to _lose_ selection, if
         * we're switching using the arrow keys. Therefore we
         * double-check that the button in wParam is actually
         * checked before generating an event.
         */
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_CLICKED ||
             HIWORD(wParam) == BN_DOUBLECLICKED) &&
            IsDlgButtonChecked(dp->hwnd, LOWORD(wParam))) {
            /*
             * The group is owner-drawn and its appearance depends on
             * which button is checked, so repaint all of it now that
             * the click has moved the selection.
             */
            for (i = 0; i < ctrl->radio.nbuttons; i++) {
                HWND button = GetDlgItem(dp->hwnd, c->base_id + 1 + i);
                if (button)
                    InvalidateRect(button, NULL, false);
            }
            dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
        }
        break;
      case CTRL_CHECKBOX:
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_CLICKED ||
             HIWORD(wParam) == BN_DOUBLECLICKED)) {
            dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
        }
        break;
      case CTRL_BUTTON:
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_CLICKED ||
             HIWORD(wParam) == BN_DOUBLECLICKED)) {
            dlg_send_event(ctrl, dp, EVENT_ACTION);
        }
        break;
      case CTRL_LISTBOX:
        if (msg == WM_COMMAND && ctrl->listbox.height != 0 &&
            (HIWORD(wParam)==LBN_SETFOCUS || HIWORD(wParam)==LBN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == LBN_SETFOCUS);
        if (msg == WM_COMMAND && ctrl->listbox.height == 0 &&
            (HIWORD(wParam)==CBN_SETFOCUS || HIWORD(wParam)==CBN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == CBN_SETFOCUS);
        if (msg == WM_COMMAND && id >= 2 &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (ctrl->listbox.draglist) {
            int pret;
            pret = handle_prefslist(c->data, NULL, 0, (msg != WM_COMMAND),
                                    dp->hwnd, wParam, lParam);
            if (pret & 2)
                dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
            ret = pret & 1;
        } else {
            if (msg == WM_COMMAND && HIWORD(wParam) == LBN_DBLCLK) {
                SetCapture(dp->hwnd);
                dlg_send_event(ctrl, dp, EVENT_ACTION);
            } else if (msg == WM_COMMAND && HIWORD(wParam) == LBN_SELCHANGE) {
                dlg_send_event(ctrl, dp, EVENT_SELCHANGE);
            }
        }
        break;
      case CTRL_FILESELECT:
        if (msg == WM_COMMAND && id == 1 &&
            (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == EN_SETFOCUS);
        if (msg == WM_COMMAND && id == 2 &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (msg == WM_COMMAND && id == 1 && HIWORD(wParam) == EN_CHANGE)
            dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
        if (id == 2 &&
            (msg == WM_COMMAND &&
             (HIWORD(wParam) == BN_CLICKED ||
              HIWORD(wParam) == BN_DOUBLECLICKED))) {
            Filename *fn_prev = NULL;
            if (!ctrl->fileselect.just_button) {
                wchar_t *text = GetDlgItemTextW_alloc(dp->hwnd, c->base_id+1);
                if (*text)
                    fn_prev = filename_from_wstr(text);
                sfree(text);
            }

            Filename *fn = request_file(
                dp->hwnd, ctrl->fileselect.title, fn_prev,
                ctrl->fileselect.for_writing, NULL, false,
                ctrl->fileselect.filter);
            if (fn_prev)
                filename_free(fn_prev);

            if (fn) {
                if (!ctrl->fileselect.just_button) {
                    SetDlgItemTextW(dp->hwnd, c->base_id + 1,
                                    filename_to_wstr(fn));
                    dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
                } else {
                    assert(!c->data);
                    c->data = fn;
                    dlg_send_event(ctrl, dp, EVENT_ACTION);
                    c->data = NULL;
                }
                filename_free(fn);
            }
        }
        break;
      case CTRL_FONTSELECT:
        if (msg == WM_COMMAND && id == 2 &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (id == 2 &&
            (msg == WM_COMMAND &&
             (HIWORD(wParam) == BN_CLICKED ||
              HIWORD(wParam) == BN_DOUBLECLICKED))) {
            CHOOSEFONT cf;
            LOGFONT lf;
            HDC hdc;
            FontSpec *fs = (FontSpec *)c->data;

            hdc = GetDC(0);
            lf.lfHeight = -MulDiv(fs->height,
                                  GetDeviceCaps(hdc, LOGPIXELSY), 72);
            ReleaseDC(0, hdc);
            lf.lfWidth = lf.lfEscapement = lf.lfOrientation = 0;
            lf.lfItalic = lf.lfUnderline = lf.lfStrikeOut = 0;
            lf.lfWeight = (fs->isbold ? FW_BOLD : 0);
            lf.lfCharSet = fs->charset;
            lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
            lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
            lf.lfQuality = DEFAULT_QUALITY;
            lf.lfPitchAndFamily = FIXED_PITCH | FF_DONTCARE;
            strncpy(lf.lfFaceName, fs->name,
                    sizeof(lf.lfFaceName) - 1);
            lf.lfFaceName[sizeof(lf.lfFaceName) - 1] = '\0';

            cf.lStructSize = sizeof(cf);
            cf.hwndOwner = dp->hwnd;
            cf.lpLogFont = &lf;
            cf.Flags = (dp->fixed_pitch_fonts ? CF_FIXEDPITCHONLY : 0) |
                CF_FORCEFONTEXIST | CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS;

            if (ChooseFont(&cf)) {
                fs = fontspec_new(lf.lfFaceName, (lf.lfWeight == FW_BOLD),
                                  cf.iPointSize / 10, lf.lfCharSet);
                dlg_fontsel_set(ctrl, dp, fs);
                fontspec_free(fs);

                dlg_send_event(ctrl, dp, EVENT_VALCHANGE);
            }
        }
        break;
    }

    /*
     * If the above event handler has asked for a colour selector,
     * now is the time to generate one.
     */
    if (dp->coloursel_wanted) {
        static CHOOSECOLOR cc;
        static DWORD custom[16] = { 0 };    /* zero initialisers */
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = dp->hwnd;
        cc.hInstance = (HWND) hinst;
        cc.lpCustColors = custom;
        cc.rgbResult = RGB(dp->coloursel_result.r,
                           dp->coloursel_result.g,
                           dp->coloursel_result.b);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColor(&cc)) {
            dp->coloursel_result.r =
                (unsigned char) (cc.rgbResult & 0xFF);
            dp->coloursel_result.g =
                (unsigned char) (cc.rgbResult >> 8) & 0xFF;
            dp->coloursel_result.b =
                (unsigned char) (cc.rgbResult >> 16) & 0xFF;
            dp->coloursel_result.ok = true;
        } else
            dp->coloursel_result.ok = false;
        dlg_send_event(ctrl, dp, EVENT_CALLBACK);
    }

    return ret;
}

/*
 * This function can be called to produce context help on a
 * control. Returns true if it has actually launched some help.
 */
bool winctrl_context_help(struct dlgparam *dp, HWND hwnd, int id)
{
    int i;
    struct winctrl *c;

    /*
     * Look up the control ID in our data.
     */
    c = NULL;
    for (i = 0; i < dp->nctrltrees; i++) {
        c = winctrl_findbyid(dp->controltrees[i], id);
        if (c)
            break;
    }
    if (!c)
        return false;                  /* we have nothing to do */

    /*
     * This is the Windows front end, so we're allowed to assume
     * `helpctx' is a context string.
     */
    if (!c->ctrl || !c->ctrl->helpctx)
        return false;            /* no help available for this ctrl */

    launch_help(hwnd, c->ctrl->helpctx);
    return true;
}

/*
 * Now the various functions that the platform-independent
 * mechanism can call to access the dialog box entries.
 */

static struct winctrl *dlg_findbyctrl(struct dlgparam *dp, dlgcontrol *ctrl)
{
    int i;

    for (i = 0; i < dp->nctrltrees; i++) {
        struct winctrl *c = winctrl_findbyctrl(dp->controltrees[i], ctrl);
        if (c)
            return c;
    }
    return NULL;
}

bool dlg_is_visible(dlgcontrol *ctrl, dlgparam *dp)
{
    /*
     * In this implementation of the dialog box, we physically
     * uncreate controls that aren't in a visible panel of the config
     * box. So we can tell if a control is visible just by checking if
     * it _exists_.
     */
    return dlg_findbyctrl(dp, ctrl) != NULL;
}

void dlg_radiobutton_set(dlgcontrol *ctrl, dlgparam *dp, int whichbutton)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int i;
    assert(c && c->ctrl->type == CTRL_RADIO);
    CheckRadioButton(dp->hwnd,
                     c->base_id + 1,
                     c->base_id + c->ctrl->radio.nbuttons,
                     c->base_id + 1 + whichbutton);
    /*
     * Owner-drawn buttons do not repaint themselves when their check
     * state changes, and the appearance depends on that state (selected
     * options are drawn in full-strength text, the rest dimmed), so
     * invalidate the whole group.
     */
    for (i = 0; i < c->ctrl->radio.nbuttons; i++) {
        HWND button = GetDlgItem(dp->hwnd, c->base_id + 1 + i);
        if (button)
            InvalidateRect(button, NULL, false);
    }
}

int dlg_radiobutton_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int i;
    assert(c && c->ctrl->type == CTRL_RADIO);
    for (i = 0; i < c->ctrl->radio.nbuttons; i++)
        if (IsDlgButtonChecked(dp->hwnd, c->base_id + 1 + i))
            return i;
    unreachable("no radio button was checked");
}

void dlg_checkbox_set(dlgcontrol *ctrl, dlgparam *dp, bool checked)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_CHECKBOX);
    CheckDlgButton(dp->hwnd, c->base_id, checked);
}

bool dlg_checkbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_CHECKBOX);
    return 0 != IsDlgButtonChecked(dp->hwnd, c->base_id);
}

void dlg_editbox_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    SetDlgItemText(dp->hwnd, c->base_id+1, text);
}

void dlg_editbox_set_utf8(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    wchar_t *wtext = dup_mb_to_wc(CP_UTF8, text);
    SetDlgItemTextW(dp->hwnd, c->base_id+1, wtext);
    sfree(wtext);
}

char *dlg_editbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    return GetDlgItemText_alloc(dp->hwnd, c->base_id+1);
}

char *dlg_editbox_get_utf8(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    wchar_t *wtext = GetDlgItemTextW_alloc(dp->hwnd, c->base_id+1);
    char *text = dup_wc_to_mb(CP_UTF8, wtext, "");
    sfree(wtext);
    return text;
}

void dlg_editbox_select_range(dlgcontrol *ctrl, dlgparam *dp,
                              size_t start, size_t len)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, EM_SETSEL, start, start+len);
}

/*
 * Subclass for the EDIT child of a combo box. When the EDIT is
 * read-only, a click on the typing area would otherwise just place
 * the caret and do nothing, which makes the control feel dead
 * everywhere except the drop-down arrow. Forwarding the click to the
 * combo box as a request to open the list makes the whole control
 * behave like the arrow.
 */
static LRESULT CALLBACK readonly_combo_edit_subclass(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    LRESULT ret = DefSubclassProc(hwnd, msg, wParam, lParam);

    if (msg == WM_LBUTTONDOWN &&
        (GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_READONLY))
        SendMessageW(GetParent(hwnd), CB_SHOWDROPDOWN, TRUE, 0);

    return ret;
}

void dlg_editbox_set_readonly(dlgcontrol *ctrl, dlgparam *dp, bool readonly)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);

    HWND wnd = GetDlgItem(dp->hwnd, c->base_id+1);
    HWND target = wnd;

    if (c->ctrl->editbox.has_list) {
        /*
         * A combo box is not itself an EDIT control; the typing area
         * is a child EDIT window, and EM_SETREADONLY sent to the
         * combo box does nothing. A non-editable combo box
         * (CBS_DROPDOWNLIST) has no such child, in which case there
         * is nothing to do.
         */
        target = FindWindowEx(wnd, NULL, "EDIT", NULL);
    }
    if (target) {
        SendMessage(target, EM_SETREADONLY, readonly ? TRUE : FALSE, 0);
        if (readonly && c->ctrl->editbox.has_list)
            SetWindowSubclass(target, readonly_combo_edit_subclass, 1, 0);
    }
}

/* The `listbox' functions can also apply to combo boxes. */
void dlg_listbox_clear(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_RESETCONTENT : CB_RESETCONTENT);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, 0);
}

void dlg_listbox_del(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_DELETESTRING : CB_DELETESTRING);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, index, 0);
}

void dlg_listbox_add(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_ADDSTRING : CB_ADDSTRING);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, (LPARAM)text);
}

/*
 * Each listbox entry may have a numeric id associated with it.
 * Note that some front ends only permit a string to be stored at
 * each position, which means that _if_ you put two identical
 * strings in any listbox then you MUST not assign them different
 * IDs and expect to get meaningful results back.
 */
void dlg_listbox_addwithid(dlgcontrol *ctrl, dlgparam *dp,
                           char const *text, int id)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg, msg2, index;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_ADDSTRING : CB_ADDSTRING);
    msg2 = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
            LB_SETITEMDATA : CB_SETITEMDATA);
    index = SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, (LPARAM)text);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg2, index, (LPARAM)id);
}

int dlg_listbox_getid(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c && c->ctrl->type == CTRL_LISTBOX);
    msg = (c->ctrl->listbox.height != 0 ? LB_GETITEMDATA : CB_GETITEMDATA);
    return
        SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, index, 0);
}

/* dlg_listbox_index returns <0 if no single element is selected. */
int dlg_listbox_index(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg, ret;
    assert(c && c->ctrl->type == CTRL_LISTBOX);
    if (c->ctrl->listbox.multisel) {
        assert(c->ctrl->listbox.height != 0); /* not combo box */
        ret = SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETSELCOUNT, 0, 0);
        if (ret == LB_ERR || ret > 1)
            return -1;
    }
    msg = (c->ctrl->listbox.height != 0 ? LB_GETCURSEL : CB_GETCURSEL);
    ret = SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, 0);
    if (ret == LB_ERR)
        return -1;
    else
        return ret;
}

bool dlg_listbox_issel(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_LISTBOX &&
           c->ctrl->listbox.multisel &&
           c->ctrl->listbox.height != 0);
    return
        SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETSEL, index, 0);
}

void dlg_listbox_select(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)) &&
           !(c->ctrl->type == CTRL_LISTBOX && c->ctrl->listbox.multisel));
    msg = (c->ctrl->type == CTRL_LISTBOX && c->ctrl->listbox.height != 0 ?
           LB_SETCURSEL : CB_SETCURSEL);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, index, 0);

    if (c->ctrl->type == CTRL_LISTBOX && c->ctrl->listbox.height == 0) {
        /*
         * A dimmed list is painted from a picture of itself, and that
         * picture shows the text of whichever entry was selected when
         * it was taken, so a change of selection makes it stale.
         */
        HWND combo = GetDlgItem(dp->hwnd, c->base_id+1);
        if (combo && GetWindowLongPtr(combo, GWLP_USERDATA)) {
            combo_dim_invalidate(combo);
            InvalidateRect(combo, NULL, true);
        }
    }
}

/*
 * A drop-down list that can be asked to look inactive.
 *
 * The combo box paints its face itself and ignores the colours offered
 * through WM_CTLCOLOR*, so the muted appearance cannot be requested
 * through the normal colour messages. It does, however, have a native
 * disabled look, and that is what gets shown: while the list is marked
 * dim, its face is painted from a bitmap of itself taken while it was
 * temporarily disabled, which is the same picture the control would
 * have drawn on its own.
 *
 * The control is never actually left disabled - the disabled state is
 * only held for the instant needed to take the picture - so it stays
 * clickable and the dropped list keeps working normally.
 */
struct combo_dim {
    HWND combo;
    HBITMAP shot;               /* cached disabled appearance */
    int shotw, shoth;
    int shotsel;                /* selection index the picture shows */
    bool dim;                   /* draw the disabled picture */
    bool grabbing;              /* inside the snapshot, avoid recursion */
};

/*
 * Discard a cached picture, so that the next paint takes a fresh one.
 *
 * Needed whenever the control's contents change, since the picture
 * shows the text as it was when it was taken.
 */
static void combo_dim_invalidate(HWND combo)
{
    struct combo_dim *p = (struct combo_dim *)
        GetWindowLongPtr(combo, GWLP_USERDATA);

    if (p && p->shot) {
        DeleteObject(p->shot);
        p->shot = NULL;
        p->shotw = p->shoth = 0;
        p->shotsel = -1;
    }
}

/*
 * Take a picture of the control as it would look while disabled.
 *
 * The caller must already have disabled it. Returns false if a bitmap
 * could not be made, in which case the control simply keeps its normal
 * appearance.
 */
static bool combo_dim_snapshot(struct combo_dim *p)
{
    RECT r;
    HDC hdc, memdc;
    HBITMAP bmp, oldbmp;

    if (p->shot) {
        DeleteObject(p->shot);
        p->shot = NULL;
    }

    GetClientRect(p->combo, &r);
    if (r.right <= 0 || r.bottom <= 0)
        return false;

    hdc = GetDC(p->combo);
    if (!hdc)
        return false;
    memdc = CreateCompatibleDC(hdc);
    if (!memdc) {
        ReleaseDC(p->combo, hdc);
        return false;
    }
    bmp = CreateCompatibleBitmap(hdc, r.right, r.bottom);
    if (!bmp) {
        DeleteDC(memdc);
        ReleaseDC(p->combo, hdc);
        return false;
    }
    oldbmp = SelectObject(memdc, bmp);

    /*
     * Let the control paint itself into the bitmap. It is disabled at
     * this point, so this is exactly the picture wanted.
     */
    p->grabbing = true;
    SendMessage(p->combo, WM_PRINTCLIENT, (WPARAM)memdc,
                PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT);
    p->grabbing = false;

    SelectObject(memdc, oldbmp);
    DeleteDC(memdc);
    ReleaseDC(p->combo, hdc);

    p->shot = bmp;
    p->shotw = r.right;
    p->shoth = r.bottom;
    p->shotsel = (int)SendMessage(p->combo, CB_GETCURSEL, 0, 0);
    return true;
}

/*
 * Paint the cached disabled picture over the control's face.
 *
 * Anything the control drew for itself has already happened by the
 * time this runs, so the picture is simply painted on top; the result
 * is the disabled appearance with no change to how the control behaves.
 */
static void combo_dim_paint(struct combo_dim *p, HDC hdc)
{
    HDC memdc;
    HGDIOBJ oldbmp;

    if (!p->shot)
        return;

    memdc = CreateCompatibleDC(hdc);
    if (!memdc)
        return;
    oldbmp = SelectObject(memdc, p->shot);
    BitBlt(hdc, 0, 0, p->shotw, p->shoth, memdc, 0, 0, SRCCOPY);
    SelectObject(memdc, oldbmp);
    DeleteDC(memdc);
}

static LRESULT CALLBACK combo_dim_subclass(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    struct combo_dim *p = (struct combo_dim *)ref_data;

    switch (msg) {
      case WM_PAINT:
        if (p && p->dim && !p->grabbing) {
            PAINTSTRUCT ps;
            RECT r;
            HDC hdc = BeginPaint(hwnd, &ps);

            /*
             * The picture has to match the control as it is now: if
             * the control has been resized, or its selection has
             * changed since the picture was taken, it is out of date.
             */
            GetClientRect(hwnd, &r);
            if (p->shot && (p->shotw != r.right || p->shoth != r.bottom ||
                            p->shotsel != (int)SendMessage(hwnd, CB_GETCURSEL,
                                                           0, 0))) {
                DeleteObject(p->shot);
                p->shot = NULL;
                p->shotsel = -1;
            }

            if (!p->shot) {
                /*
                 * No usable picture. The control has to be disabled for
                 * the duration of the snapshot, which must not happen
                 * while it is painting, so the paint is finished
                 * normally and the snapshot taken straight after.
                 */
                EndPaint(hwnd, &ps);
                LRESULT ret = DefSubclassProc(hwnd, msg, wParam, lParam);
                EnableWindow(hwnd, false);
                combo_dim_snapshot(p);
                EnableWindow(hwnd, true);
                InvalidateRect(hwnd, NULL, false);
                return ret;
            }

            /*
             * Paint normally first, then lay the disabled picture over
             * the result.
             */
            EndPaint(hwnd, &ps);
            {
                LRESULT ret = DefSubclassProc(hwnd, msg, wParam, lParam);
                HDC screen = GetDC(hwnd);
                if (screen) {
                    combo_dim_paint(p, screen);
                    ReleaseDC(hwnd, screen);
                }
                return ret;
            }
        }
        break;

      case WM_COMMAND:
        /*
         * A selection made in the dropped list changes the text on the
         * control's face, so any cached picture of it is out of date.
         */
        if (p && HIWORD(wParam) == CBN_SELCHANGE)
            combo_dim_invalidate(hwnd);
        break;

      case WM_NCDESTROY:
        if (p) {
            if (p->shot)
                DeleteObject(p->shot);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            sfree(p);
        }
        RemoveWindowSubclass(hwnd, combo_dim_subclass, 1);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}


/*
 * Set whether a drop-down list is drawn in the muted colours.
 *
 * A combo box paints its own face and ignores the colours offered
 * through WM_CTLCOLOR*, so there is no way to ask it for a muted
 * appearance directly. What it does have is a native disabled look, so
 * that is what gets used: the control is subclassed (see
 * combo_dim_subclass above) and, while it is marked dim, the face it
 * paints is the disabled one, obtained from the control itself.
 */
void dlg_listbox_setdim(dlgcontrol *ctrl, dlgparam *dp, bool dim)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    struct combo_dim *p;
    HWND combo;

    if (!c || c->ctrl->type != CTRL_LISTBOX ||
        c->ctrl->listbox.height != 0 || c->ctrl->listbox.dimmed == dim)
        return;

    c->ctrl->listbox.dimmed = dim;

    combo = GetDlgItem(dp->hwnd, c->base_id+1);
    if (!combo)
        return;

    p = (struct combo_dim *)GetWindowLongPtr(combo, GWLP_USERDATA);
    if (!p) {
        p = snew(struct combo_dim);
        p->combo = combo;
        p->shot = NULL;
        p->shotw = p->shoth = 0;
        p->shotsel = -1;
        p->dim = false;
        p->grabbing = false;
        SetWindowLongPtr(combo, GWLP_USERDATA, (LONG_PTR)p);
        SetWindowSubclass(combo, combo_dim_subclass, 1, (DWORD_PTR)p);
    }

    p->dim = dim;
    if (!dim && p->shot) {
        DeleteObject(p->shot);
        p->shot = NULL;
        p->shotw = p->shoth = 0;
    }
    InvalidateRect(combo, NULL, true);
}

void dlg_text_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_TEXT);
    SetDlgItemText(dp->hwnd, c->base_id, text);
}

void dlg_label_change(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    char *escaped = NULL;
    int id = -1;

    assert(c);
    switch (c->ctrl->type) {
      case CTRL_EDITBOX:
        escaped = shortcut_escape(text, c->ctrl->editbox.shortcut);
        id = c->base_id;
        break;
      case CTRL_RADIO:
        escaped = shortcut_escape(text, c->ctrl->radio.shortcut);
        id = c->base_id;
        break;
      case CTRL_CHECKBOX:
        escaped = shortcut_escape(text, ctrl->checkbox.shortcut);
        id = c->base_id;
        break;
      case CTRL_BUTTON:
        escaped = shortcut_escape(text, ctrl->button.shortcut);
        id = c->base_id;
        break;
      case CTRL_LISTBOX:
        escaped = shortcut_escape(text, ctrl->listbox.shortcut);
        id = c->base_id;
        break;
      case CTRL_FILESELECT:
        escaped = shortcut_escape(text, ctrl->fileselect.shortcut);
        if (ctrl->fileselect.just_button)
            id = c->base_id + 2;       /* the button */
        else
            id = c->base_id;           /* the label */
        break;
      case CTRL_FONTSELECT:
        escaped = shortcut_escape(text, ctrl->fontselect.shortcut);
        id = c->base_id;
        break;
      default:
        unreachable("bad control type in label_change");
    }
    if (escaped) {
        SetDlgItemText(dp->hwnd, id, escaped);
        sfree(escaped);
    }
}

void dlg_filesel_set(dlgcontrol *ctrl, dlgparam *dp, Filename *fn)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c);
    assert(c->ctrl->type == CTRL_FILESELECT);
    assert(!c->ctrl->fileselect.just_button);
    SetDlgItemTextW(dp->hwnd, c->base_id+1, fn->wpath);
}

Filename *dlg_filesel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c);
    assert(c->ctrl->type == CTRL_FILESELECT);
    if (!c->ctrl->fileselect.just_button) {
        wchar_t *tmp = GetDlgItemTextW_alloc(dp->hwnd, c->base_id+1);
        Filename *ret = filename_from_wstr(tmp);
        sfree(tmp);
        return ret;
    } else {
        return filename_copy(c->data);
    }
}

void dlg_fontsel_set(dlgcontrol *ctrl, dlgparam *dp, FontSpec *fs)
{
    char *buf, *boldstr;
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_FONTSELECT);

    fontspec_free((FontSpec *)c->data);
    c->data = fontspec_copy(fs);

    boldstr = (fs->isbold ? "粗体, " : "");
    if (fs->height == 0)
        buf = dupprintf("字体: %s, %s默认高度", fs->name, boldstr);
    else
        buf = dupprintf("字体: %s, %s%d %s", fs->name, boldstr,
                        (fs->height < 0 ? -fs->height : fs->height),
                        (fs->height < 0 ? "像素" : "点"));
    SetDlgItemText(dp->hwnd, c->base_id+1, buf);
    sfree(buf);

    dlg_auto_set_fixed_pitch_flag(dp);
}

FontSpec *dlg_fontsel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_FONTSELECT);
    return fontspec_copy((FontSpec *)c->data);
}

/*
 * Bracketing a large set of updates in these two functions will
 * cause the front end (if possible) to delay updating the screen
 * until it's all complete, thus avoiding flicker.
 */
void dlg_update_start(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    if (c && c->ctrl->type == CTRL_LISTBOX) {
        SendDlgItemMessage(dp->hwnd, c->base_id+1, WM_SETREDRAW, false, 0);
    }
}

void dlg_update_done(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    if (c && c->ctrl->type == CTRL_LISTBOX) {
        HWND hw = GetDlgItem(dp->hwnd, c->base_id+1);
        SendMessage(hw, WM_SETREDRAW, true, 0);
        InvalidateRect(hw, NULL, true);
    }
}

void dlg_set_focus(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int id;
    HWND ctl;
    if (!c)
        return;
    switch (ctrl->type) {
      case CTRL_EDITBOX: id = c->base_id + 1; break;
      case CTRL_RADIO:
        for (id = c->base_id + ctrl->radio.nbuttons; id > 1; id--)
            if (IsDlgButtonChecked(dp->hwnd, id))
                break;
        /*
         * In the theoretically-unlikely case that no button was
         * selected, id should come out of this as 1, which is a
         * reasonable enough choice.
         */
        break;
      case CTRL_CHECKBOX: id = c->base_id; break;
      case CTRL_BUTTON: id = c->base_id; break;
      case CTRL_LISTBOX: id = c->base_id + 1; break;
      case CTRL_FILESELECT: id = c->base_id + 1; break;
      case CTRL_FONTSELECT: id = c->base_id + 2; break;
      default: id = c->base_id; break;
    }
    ctl = GetDlgItem(dp->hwnd, id);
    SetFocus(ctl);
}

/*
 * During event processing, you might well want to give an error
 * indication to the user. dlg_beep() is a quick and easy generic
 * error; dlg_error() puts up a message-box or equivalent.
 */
void dlg_beep(dlgparam *dp)
{
    MessageBeep(0);
}

void dlg_error_msg(dlgparam *dp, const char *msg)
{
    MessageBox(dp->hwnd, msg,
               dp->errtitle ? dp->errtitle : NULL,
               MB_OK | MB_ICONERROR);
}

void dlg_info_msg(dlgparam *dp, const char *msg)
{
    MessageBox(dp->hwnd, msg, "PuTTY", MB_OK | MB_ICONINFORMATION);
}

/*
 * This function signals to the front end that the dialog's
 * processing is completed, and passes an integer value (typically
 * a success status).
 */
void dlg_end(dlgparam *dp, int value)
{
    dp->ended = true;
    dp->endresult = value;
}

void dlg_refresh(dlgcontrol *ctrl, dlgparam *dp)
{
    int i, j;
    struct winctrl *c;
    bool track_changes = dp->track_changes;

    /*
     * Refreshing a control writes its value from the settings, which
     * can make an edit box emit EN_CHANGE. That is not a user edit, so
     * don't let it create a new undo step or dirty the dialog.
     */
    dp->track_changes = false;

    if (!ctrl) {
        /*
         * Send EVENT_REFRESH to absolutely everything.
         */
        for (j = 0; j < dp->nctrltrees; j++) {
            for (i = 0;
                 (c = winctrl_findbyindex(dp->controltrees[j], i)) != NULL;
                 i++) {
                if (c->ctrl && c->ctrl->handler != NULL)
                    c->ctrl->handler(c->ctrl, dp,
                                     dp->data, EVENT_REFRESH);
            }
        }
    } else {
        /*
         * Send EVENT_REFRESH to a specific control.
         */
        if (ctrl->handler != NULL)
            ctrl->handler(ctrl, dp, dp->data, EVENT_REFRESH);
    }

    dp->track_changes = track_changes;
}

void dlg_coloursel_start(dlgcontrol *ctrl, dlgparam *dp, int r, int g, int b)
{
    dp->coloursel_wanted = true;
    dp->coloursel_result.r = r;
    dp->coloursel_result.g = g;
    dp->coloursel_result.b = b;
}

bool dlg_coloursel_results(dlgcontrol *ctrl, dlgparam *dp,
                           int *r, int *g, int *b)
{
    if (dp->coloursel_result.ok) {
        *r = dp->coloursel_result.r;
        *g = dp->coloursel_result.g;
        *b = dp->coloursel_result.b;
        return true;
    } else
        return false;
}

void dlg_auto_set_fixed_pitch_flag(dlgparam *dp)
{
    Conf *conf = (Conf *)dp->data;
    FontSpec *fs;
    int quality;
    HFONT hfont;
    HDC hdc;
    TEXTMETRIC tm;
    bool is_var;

    /*
     * Attempt to load the current font, and see if it's
     * variable-pitch. If so, start off the fixed-pitch flag for the
     * dialog box as false.
     *
     * We assume here that any client of the dlg_* mechanism which is
     * using font selectors at all is also using a normal 'Conf *'
     * as dp->data.
     */

    quality = conf_get_int(conf, CONF_font_quality);
    fs = conf_get_fontspec(conf, CONF_font);

    hfont = CreateFont(0, 0, 0, 0, FW_DONTCARE, false, false, false,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality),
                       FIXED_PITCH | FF_DONTCARE, fs->name);
    hdc = GetDC(NULL);
    if (hdc && SelectObject(hdc, hfont) && GetTextMetrics(hdc, &tm)) {
        /* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
        is_var = (tm.tmPitchAndFamily & TMPF_FIXED_PITCH);
    } else {
        is_var = false;                /* assume it's basically normal */
    }
    if (hdc)
        ReleaseDC(NULL, hdc);
    if (hfont)
        DeleteObject(hfont);

    if (is_var)
        dp->fixed_pitch_fonts = false;
}

bool dlg_get_fixed_pitch_flag(dlgparam *dp)
{
    return dp->fixed_pitch_fonts;
}

void dlg_set_fixed_pitch_flag(dlgparam *dp, bool flag)
{
    dp->fixed_pitch_fonts = flag;
}

void dp_init(struct dlgparam *dp)
{
    dp->nctrltrees = 0;
    dp->data = NULL;
    dp->ended = false;
    dp->focused = dp->lastfocused = NULL;
    memset(dp->shortcuts, 0, sizeof(dp->shortcuts));
    dp->hwnd = NULL;
    dp->wintitle = dp->errtitle = NULL;
    dp->fixed_pitch_fonts = true;
    dp->apply_cb = NULL;
    dp->apply_ctx = NULL;
    dp->ctrlbox = NULL;
    dp->track_changes = false;
    dp->dirty = false;
    dp->last_change_ctrl = NULL;
    dp->undo_stack = NULL;
    dp->nundo = dp->undosize = 0;
}

void dp_add_tree(struct dlgparam *dp, struct winctrls *wc)
{
    assert(dp->nctrltrees < lenof(dp->controltrees));
    dp->controltrees[dp->nctrltrees++] = wc;
}

void dp_cleanup(struct dlgparam *dp)
{
    for (int i = 0; i < dp->nundo; i++)
        conf_free(dp->undo_stack[i]);
    sfree(dp->undo_stack);
    sfree(dp->wintitle);
    sfree(dp->errtitle);
}
