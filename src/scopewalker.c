// ScopeWalker - Win32/GDI
// Vectorscope + Waveform + Parade + Histogramme RGB d'une zone de l'ecran.
//
// Selection : maintenir ALT (par defaut) + clic gauche, puis glisser.
// Barre de menu : ON/OFF, Scopes, Preferences, Reset.
// Les scopes s'affichent par deux maximum sur une ligne ; au-dela ils passent
// a la ligne suivante. Ils peuvent aussi etre detaches ("Split scopes").

#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- Windows.Graphics.Capture : seul chemin qui rende le HDR intact ----
   BitBlt et la duplication DXGI s'arretent a 8 bits et recuperent le desktop
   APRES la compression HDR->SDR de Windows : tout ce qui depasse le blanc SDR
   tombe a 255 et devient irreparable. WGC rend une texture FP16 en lumiere
   LINEAIRE (verifie : le rapport mesure est constant a 3,5000 sur toute une
   rampe de gris), donc avec de la marge au-dessus du blanc.
   Ces en-tetes exigent COBJMACROS + les macros WIDL_using_* pour que les noms
   courts existent en C. Seul le C de MinGW les fournit : les en-tetes du SDK
   Microsoft ne les exposent qu'en C++, donc sous MSVC cette voie est compilee
   vide et BitBlt prend le relais. */
#if defined(__GNUC__)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Graphics
#define WIDL_using_Windows_Graphics_DirectX
#define WIDL_using_Windows_Graphics_DirectX_Direct3D11
#define WIDL_using_Windows_Graphics_Capture
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.foundation.h>
#include <windows.graphics.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

/* Les deux que l'en-tete interop de MinGW ne fournit pas. En C, la structure
   IDirect3DDxgiInterfaceAccess n'est declaree nulle part : a faire ici. */
struct IDirect3DDxgiInterfaceAccess;
typedef struct IDirect3DDxgiInterfaceAccessVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(struct IDirect3DDxgiInterfaceAccess*,REFIID,void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(struct IDirect3DDxgiInterfaceAccess*);
    ULONG (STDMETHODCALLTYPE *Release)(struct IDirect3DDxgiInterfaceAccess*);
    HRESULT (STDMETHODCALLTYPE *GetInterface)(struct IDirect3DDxgiInterfaceAccess*,REFIID,void**);
} IDirect3DDxgiInterfaceAccessVtbl;

typedef struct IDirect3DDxgiInterfaceAccess {
    CONST_VTBL IDirect3DDxgiInterfaceAccessVtbl *lpVtbl;
} IDirect3DDxgiInterfaceAccess;

static const GUID IID_DxgiAccess =
    {0xa9b3d012,0x3df2,0x4ee3,{0xb8,0xd1,0x86,0x95,0xf4,0x57,0xd3,0xc1}};

extern HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*,IInspectable**);
#endif /* __GNUC__ : en-tetes WinRT */

/* Echelle de l'interface en % (100 = 96 dpi / 100%), genree dans WinMain une
   fois le process rendu "DPI aware". Toutes les dimensions ci-dessous sont
   exprimees en pixels "a 100%" et converties a l'usage. */
static int g_uiScale=100;
#define UI(px)  ( ((px)*g_uiScale + 50) / 100 )

#define MIN_CANVAS   UI(180)
#define MAX_CANVAS   UI(1200)
#define MARGIN       UI(14)
#define MENUBAR_H    UI(32)
#define STATUS_H     UI(26)
#define SPLIT_W      UI(8)
#define MAX_SAMPLES  90000
#define RESIZE_EDGE  UI(6)

#define UI_BG_R 32
#define UI_BG_G 32
#define UI_BG_B 35
#define UI_BG        RGB(UI_BG_R,UI_BG_G,UI_BG_B)
#define UI_BG_DWORD  (((DWORD)UI_BG_R<<16)|((DWORD)UI_BG_G<<8)|UI_BG_B)

/* Chroma maximale d'une primaire saturee (le rouge et le cyan, |UV|=161.19).
   C'est elle qui definit le cercle 100% : cibles et nuage partagent ainsi la
   MEME echelle, donc une couleur pure tombe exactement sur son repere. */
static const double CHROMA_MAX = 161.19;

static const char *MAIN_CLASS   ="SWMainWnd";
static const char *OVERLAY_CLASS="SWOverlayWnd";
static const char *PREFS_CLASS  ="SWPrefsWnd";
static const char *SCOPES_CLASS ="SWScopesWnd";
static const char *SCOPEW_CLASS ="SWScopeWnd";

/* ================= langues =================
   Tout le texte passe par Tr(id). Les chaines sont en UTF-16 et rendues avec
   les API W : le francais passait par des TextOutA et des longueurs codees en
   dur, ce qui ne peut pas porter les ideogrammes. Les positions qui dependent
   de la largeur d'un texte sont MESUREES (DrawTxtW / TextW), plus estimees. */
enum { L_EN=0, L_ZH=1 };
enum {
  S_ON,S_OFF,S_SCOPES,S_PREFS,S_RESET,
  S_HINT,S_PAUSED,S_RESETZONE,S_TOOSMALL,S_ZONEBORDS,S_ZONEACTIVE,
  S_HDRSTRETCH,S_HDRWHITE,S_HDRGAIN,S_HDRSAT,S_HDR,
  S_ZOOM2,S_RGBAT,S_PCT100,S_SKIN,S_WAVEFORM,S_PARADE,S_RGB,S_255,
  S_HSEL,S_HPAUSE,S_HWAVE,S_HCOLOR,S_HBRIGHT,
  S_ALT,S_CTRL,S_SHIFT,S_WIN,S_ESC,
  S_LUMA,S_PARADEM,
  S_CKSKIN,S_CKRGB,S_CKCLIP,S_CKBARS,S_CKHDR,
  S_VEC,S_WAVE,S_PAR,S_HIST,S_SPLIT,
  S_HLANG,S_EN_LBL,S_ZH_LBL,S_COUNT_STR
};

/* Une ligne par identifiant, { anglais, chinois }. L'ordre suit l'enum au
   element pres : un decalage d'une ligne decalerait toute l'interface. */
static const wchar_t*STR[S_COUNT_STR][2]={
  {L"ON",                 L"开"},                       /* S_ON */
  {L"OFF",                L"关"},                        /* S_OFF */
  {L"Scopes",             L"示波器"},                    /* S_SCOPES */
  {L"Preferences",        L"首选项"},                    /* S_PREFS */
  {L"Reset",              L"重置"},                      /* S_RESET */

  {L"Hold Alt, then left-drag to pick an area.",
                          L"按住 Alt 并左键拖拽，框选要分析的区域。"},   /* S_HINT */
  {L"Tracking paused - press ON to resume.",
                          L"跟踪已暂停 - 点“开”恢复。"},                /* S_PAUSED */
  {L"Area cleared. Alt + left-drag to pick a new one.",
                          L"区域已清除。按住 Alt 左键拖拽可重新框选。"},  /* S_RESETZONE */
  {L"Selection too small - try again.",
                          L"框选太小 - 请重试。"},                       /* S_TOOSMALL */
  {L"Area : %d x %d px  -  edges skipped  t:%d b:%d l:%d r:%d%s",
                          L"区域：%d x %d 像素 - 已跳过边缘 上:%d 下:%d 左:%d 右:%d%s"}, /* S_ZONEBORDS */
  {L"Area : %d x %d px  -  tracking%s",
                          L"区域：%d x %d 像素 - 跟踪中%s"},             /* S_ZONEACTIVE */

  {L"  -  HDR x%.2f (white %.2f)",  L"  -  HDR x%.2f（白点 %.2f）"},     /* S_HDRSTRETCH */
  {L"  -  HDR (white %.2f)",        L"  -  HDR（白点 %.2f）"},           /* S_HDRWHITE */
  {L"  -  HDR x%.2f",               L"  -  HDR x%.2f"},                  /* S_HDRGAIN */
  {L"  -  HDR %d%% at 255",         L"  -  HDR %d%% 顶到 255"},          /* S_HDRSAT */
  {L"  -  HDR",                     L"  -  HDR"},                        /* S_HDR */

  {L"Zoom x2",              L"2 倍缩放"},                 /* S_ZOOM2 */
  {L"R:%d, G:%d, B:%d",     L"R:%d, G:%d, B:%d"},         /* S_RGBAT */
  {L"100%",                 L"100%"},                     /* S_PCT100 */
  {L"skin",                 L"肤色"},                     /* S_SKIN */
  {L"waveform",             L"波形"},                     /* S_WAVEFORM */
  {L"parade",               L"三色"},                     /* S_PARADE */
  {L"RGB",                  L"RGB"},                      /* S_RGB */
  {L"255",                  L"255"},                      /* S_255 */

  {L"Selection (hold + drag)",  L"框选（按住 + 拖拽）"},   /* S_HSEL */
  {L"Pause / resume key",       L"暂停 / 恢复按键"},       /* S_HPAUSE */
  {L"Waveform mode",            L"波形模式"},              /* S_HWAVE */
  {L"Frame colour",             L"边框颜色"},              /* S_HCOLOR */
  {L"Cloud brightness : %.2fx", L"点云亮度：%.2fx"},       /* S_HBRIGHT */

  {L"Alt",   L"Alt"},      /* S_ALT */
  {L"Ctrl",  L"Ctrl"},     /* S_CTRL */
  {L"Shift", L"Shift"},    /* S_SHIFT */
  {L"Win",   L"Win"},      /* S_WIN */
  {L"Esc",   L"Esc"},      /* S_ESC */

  {L"White (luma)",   L"亮度（黑白）"},     /* S_LUMA */
  {L"RGB (parade)",   L"RGB（三色）"},      /* S_PARADEM */

  {L"Show skin tone line",       L"显示肤色线"},            /* S_CKSKIN */
  {L"RGB at cursor",             L"光标处 RGB"},            /* S_CKRGB */
  {L"Clipping",                  L"溢出提示"},              /* S_CKCLIP */
  {L"Ignore background bars",    L"忽略背景色带"},          /* S_CKBARS */
  {L"Calibrate levels under HDR",L"HDR 下自动校准电平"},    /* S_CKHDR */

  {L"Vectorscope",    L"矢量示波器"},        /* S_VEC */
  {L"Waveform",       L"波形示波器"},        /* S_WAVE */
  {L"Parade",         L"三色示波器"},        /* S_PAR */
  {L"RGB Histogram",  L"RGB 直方图"},        /* S_HIST */
  {L"Split scopes",   L"拆分示波器"},        /* S_SPLIT */

  {L"Language",  L"语言"},                   /* S_HLANG */
  {L"English",   L"English"},                /* S_EN_LBL */
  {L"中文",      L"中文"}                     /* S_ZH_LBL */
};

static int g_lang=L_EN;

/* Garde-fou : si une ligne manque ou en est de trop dans STR[], l'ordre
   n'est plus celui de l'enum et toute l'interface se decale. Ca doit casser a
   la compilation, pas a l'execution. */
typedef char STR_TABLE_SIZE_CHECK[
    (sizeof(STR)/sizeof(STR[0])) == S_COUNT_STR ? 1 : -1 ];
static const wchar_t*Tr(int id){
    const wchar_t*s=STR[id][g_lang];
    return s?s:STR[id][0];
}

/* Dessine du texte en W et renvoie sa taille : c'est la seule facon de placer
   correctement les libelles qui changent de largeur d'une langue a l'autre. */
static SIZE DrawTxtW(HDC dc,int x,int y,const wchar_t*s){
    SIZE sz; sz.cx=0; sz.cy=0;
    if(!s||!*s) return sz;
    GetTextExtentPoint32W(dc,s,(int)wcslen(s),&sz);
    TextOutW(dc,x,y,s,(int)wcslen(s));
    return sz;
}
static int WideW(HDC dc,const wchar_t*s){
    SIZE sz; if(!s||!*s) return 0;
    if(!GetTextExtentPoint32W(dc,s,(int)wcslen(s),&sz)) return 0;
    return sz.cx;
}

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* ================= scopes ================= */
enum { SC_VEC=0, SC_WAVE, SC_PARADE, SC_HIST, SC_COUNT };

typedef struct {
    HDC     dc;
    HBITMAP bmp;
    DWORD  *bits;
    DWORD  *tpl;
    int     w,h;
    RECT    rc;
    HWND    wnd;
    BOOL    on;
    double  prop;
    RECT    wr;        /* cadre de la fenetre detachee, meme apres sa fermeture */
    BOOL    wrValid;
} Scope;

static Scope g_sc[SC_COUNT];
/* noms affiches : un identifiant de chaine par scope, plus de texte en dur */
static const int SC_STR[SC_COUNT]={S_VEC,S_WAVE,S_PAR,S_HIST};

static HWND  g_hMain=NULL,g_hOverlay=NULL,g_hPrefs=NULL,g_hScopes=NULL;
static POINT g_overlayOrigin={0,0};
static HICON g_icon=NULL;
static HINSTANCE g_inst=NULL;

static BOOL  g_dragging=FALSE;
static POINT g_dragStart;
static RECT  g_selRectScreen={0,0,0,0};

static HFONT g_hFont=NULL,g_hFontBold=NULL;

static wchar_t g_status[256];
static BOOL g_hasSelection=FALSE,g_paused=FALSE;
static int  g_refreshCounter=0,g_topmostCounter=0,g_cursorCounter=0;
#define REFRESH_TICKS 6

static BOOL     g_zoom2=FALSE;
static double   g_brightness=1.0;
static int      g_modVK=VK_MENU;
static int      g_pauseVK=VK_ESCAPE;
static COLORREF g_frameColor=RGB(255,255,255);
static BOOL     g_showSkin=TRUE;
static BOOL     g_rgbAtCursor=TRUE;
static BOOL     g_clipping=TRUE;
static BOOL     g_ignoreBars=TRUE;   /* exclure les bandes de fond (letterbox) */
static BOOL     g_hdrNorm=TRUE;      /* recalibrer l'echelle quand la sortie est HDR */
static BOOL     g_wfRGB=FALSE;
static BOOL     g_split=FALSE;
static BYTE     g_curR=0,g_curG=0,g_curB=0;

static DWORD g_lastHash=0;

enum { MI_ONOFF=0, MI_SCOPES, MI_PREFS, MI_RESET, MI_COUNT };
static RECT g_miRect[MI_COUNT];
static int  g_hoverMI=-1;
static BOOL g_tracking=FALSE;

static RECT g_zoomChkRc={0,0,0,0};
static BOOL g_zoomHover=FALSE;

/* separateurs : un par ligne, entre les deux colonnes */
#define MAX_ROWS 2
static RECT g_rowSplit[MAX_ROWS];
static int  g_rowSplitA[MAX_ROWS],g_rowSplitB[MAX_ROWS];
static int  g_splitDragRow=-1;
static int  g_splitDragX=0;
static double g_dragPropA=0,g_dragPropB=0;

static BYTE *g_sR=NULL,*g_sG=NULL,*g_sB=NULL;
static WORD *g_sCol=NULL;
static int   g_sCount=0,g_sCap=0,g_sCols=0;

static COLORREF g_swatch[6]={
    RGB(255,255,255),RGB(60,220,100),RGB(255,80,80),
    RGB(255,210,60),RGB(80,200,255),RGB(255,90,220)
};
static RECT g_swatchRc[6],g_modRc[4],g_pauseRc[3],g_wfModeRc[2];
static RECT g_skinRc,g_rgbCurRc,g_clipRc,g_barsRc,g_hdrRc,g_sliderRc,g_langRc[2];
static BOOL g_sliderDrag=FALSE;
static RECT g_scChkRc[SC_COUNT],g_scSplitRc;

static HDC     g_pfDC=NULL;   static HBITMAP g_pfBmp=NULL;
static DWORD  *g_pfBits=NULL; static int g_pfW=0,g_pfH=0;

/* ================= reglages : scopewalker.ini =================
   TOUS les reglages sont persistes a cote de l'executable : langue, touches,
   couleur du cadre, options, eclatement des scopes, proportions de chaque
   colonne et cadre de chaque fenetre (principale + detachees). Quatre sections :
     [prefs] [window] [scopes] [detached]
   Tout est ecrit en ASCII (la langue est un numero), donc le fichier se lit et
   se modifie a la main sans outillage. Les valeurs absentes laissent la valeur
   par defaut du programme. */
static char g_iniPath[MAX_PATH]="scopewalker.ini";

static const char *SC_KEY[SC_COUNT]={"vec","wave","par","hist"};
#define OFFSCREEN (-32000)

/* Un cadre enregistre sur un ecran depuis debranche designerait du vide :
   MONITOR_DEFAULTTONULL repond NULL des qu'aucun moniteur ne touche le rect. */
static BOOL RectOnScreen(const RECT*r)
{ return MonitorFromRect(r,MONITOR_DEFAULTTONULL)!=NULL; }

static void IniInit(void)
{
    static const char leaf[]="\\scopewalker.ini";
    char dir[MAX_PATH];
    DWORD n=GetModuleFileNameA(NULL,dir,(DWORD)sizeof(dir));
    if(n==0||n>=sizeof(dir)) return;
    char*s=strrchr(dir,'\\');
    if(!s) s=strrchr(dir,'/');
    if(!s) return;
    *s=0;
    size_t dl=strlen(dir), ll=sizeof(leaf)-1;
    /* longueur verifiee avant copie : pas de troncature silencieuse du chemin */
    if(dl+ll<sizeof(g_iniPath)){
        memcpy(g_iniPath,dir,dl);
        memcpy(g_iniPath+dl,leaf,ll+1);
    }
}

static int IniGetInt(const char*sec,const char*key,int def)
{ return GetPrivateProfileIntA(sec,key,def,g_iniPath); }

static double IniGetDbl(const char*sec,const char*key,double def)
{
    char b[64];
    if(GetPrivateProfileStringA(sec,key,"",b,(DWORD)sizeof(b),g_iniPath)==0) return def;
    char*e=NULL; double v=strtod(b,&e);
    return (e==b)?def:v;
}

static BOOL IniGetBool(const char*sec,const char*key,BOOL def)
{
    char b[32];
    if(GetPrivateProfileStringA(sec,key,"",b,(DWORD)sizeof(b),g_iniPath)==0||!b[0]) return def;
    switch(b[0]){
    case '1': case 'y': case 'Y': case 't': case 'T': return TRUE;
    case '0': case 'n': case 'N': case 'f': case 'F': return FALSE;
    default: return def;
    }
}

static void IniPutInt(const char*sec,const char*key,int v)
{ char b[32]; snprintf(b,sizeof(b),"%d",v); WritePrivateProfileStringA(sec,key,b,g_iniPath); }

static void IniPutBool(const char*sec,const char*key,BOOL v)
{ IniPutInt(sec,key,v?1:0); }

static void IniPutDbl(const char*sec,const char*key,double v)
{ char b[32]; snprintf(b,sizeof(b),"%.4f",v); WritePrivateProfileStringA(sec,key,b,g_iniPath); }

/* Couleur ecrite en RRGGBB : c'est le seul reglage qu'on edite a l'oeil, et le
   COLORREF de Windows est en BBGGRR, donc on retourne les octets en entree et
   en sortie pour que le fichier reste lisible. */
static void IniPutColor(const char*sec,const char*key,COLORREF c)
{
    char b[16];
    snprintf(b,sizeof(b),"%06X",
        (unsigned)(((DWORD)GetRValue(c)<<16)|((DWORD)GetGValue(c)<<8)|GetBValue(c)));
    WritePrivateProfileStringA(sec,key,b,g_iniPath);
}
static COLORREF IniGetColor(const char*sec,const char*key,COLORREF def)
{
    char b[16];
    if(GetPrivateProfileStringA(sec,key,"",b,(DWORD)sizeof(b),g_iniPath)==0||!b[0]) return def;
    char*e=NULL; long v=strtol(b,&e,16);
    if(e==b) return def;
    return RGB((BYTE)((v>>16)&0xFF),(BYTE)((v>>8)&0xFF),(BYTE)(v&0xFF));
}

/* un cadre = 4 cles <key>_x/_y/_w/_h ; _ok dit si la section etait renseignee */
static void IniGetRect(const char*sec,const char*key,RECT*r,BOOL*ok)
{
    char k[40];
    snprintf(k,sizeof(k),"%s_x",key); int x=IniGetInt(sec,k,OFFSCREEN);
    snprintf(k,sizeof(k),"%s_y",key); int y=IniGetInt(sec,k,OFFSCREEN);
    snprintf(k,sizeof(k),"%s_w",key); int w=IniGetInt(sec,k,0);
    snprintf(k,sizeof(k),"%s_h",key); int h=IniGetInt(sec,k,0);
    SetRect(r,x,y,x+w,y+h);
    if(ok) *ok=(x!=OFFSCREEN&&y!=OFFSCREEN&&w>8&&h>8&&RectOnScreen(r));
}
static void IniPutRect(const char*sec,const char*key,const RECT*r)
{
    if(!r||r->right<=r->left||r->bottom<=r->top) return;
    char k[40];
    snprintf(k,sizeof(k),"%s_x",key); IniPutInt(sec,k,r->left);
    snprintf(k,sizeof(k),"%s_y",key); IniPutInt(sec,k,r->top);
    snprintf(k,sizeof(k),"%s_w",key); IniPutInt(sec,k,r->right-r->left);
    snprintf(k,sizeof(k),"%s_h",key); IniPutInt(sec,k,r->bottom-r->top);
}

static RECT g_mainRect={0,0,0,0};
static BOOL g_mainRectValid=FALSE;

static void LoadSettings(void)
{
    IniInit();
    g_lang=IniGetInt("prefs","lang",L_EN);
    if(g_lang!=L_ZH) g_lang=L_EN;
    g_modVK=(int)IniGetInt("prefs","mod",g_modVK);
    g_pauseVK=(int)IniGetInt("prefs","pause",g_pauseVK);
    g_frameColor=IniGetColor("prefs","framecolor",g_frameColor);
    g_showSkin   =IniGetBool("prefs","skin",TRUE);
    g_rgbAtCursor=IniGetBool("prefs","rgbatcursor",TRUE);
    g_clipping   =IniGetBool("prefs","clipping",TRUE);
    g_ignoreBars =IniGetBool("prefs","ignorebars",TRUE);
    g_hdrNorm    =IniGetBool("prefs","hdrnorm",TRUE);
    g_wfRGB      =IniGetBool("prefs","wfrgb",FALSE);
    g_split      =IniGetBool("prefs","split",FALSE);
    g_zoom2      =IniGetBool("prefs","zoom2",FALSE);
    g_brightness =IniGetDbl("prefs","brightness",1.0);
    if(g_brightness<0.25) g_brightness=0.25;
    if(g_brightness>3.0)  g_brightness=3.0;

    for(int i=0;i<SC_COUNT;i++){
        char k[40];
        if(i!=SC_VEC){
            snprintf(k,sizeof(k),"%s_on",SC_KEY[i]);
            g_sc[i].on=IniGetBool("scopes",k,FALSE);
        }
        snprintf(k,sizeof(k),"%s_prop",SC_KEY[i]);
        double p=IniGetDbl("scopes",k,1.0);
        g_sc[i].prop=(p>0.3)?p:1.0;   /* 0.3 = le minimum que le glisseur produit */
        if(i!=SC_VEC){
            IniGetRect("detached",SC_KEY[i],&g_sc[i].wr,&g_sc[i].wrValid);
        }
    }
    IniGetRect("window","main",&g_mainRect,&g_mainRectValid);
}

static void SaveSettings(void)
{
    if(!g_iniPath[0]) return;
    IniPutInt("prefs","lang",g_lang);
    IniPutInt("prefs","mod",g_modVK);
    IniPutInt("prefs","pause",g_pauseVK);
    IniPutColor("prefs","framecolor",g_frameColor);
    IniPutBool("prefs","skin",g_showSkin);
    IniPutBool("prefs","rgbatcursor",g_rgbAtCursor);
    IniPutBool("prefs","clipping",g_clipping);
    IniPutBool("prefs","ignorebars",g_ignoreBars);
    IniPutBool("prefs","hdrnorm",g_hdrNorm);
    IniPutBool("prefs","wfrgb",g_wfRGB);
    IniPutBool("prefs","split",g_split);
    IniPutBool("prefs","zoom2",g_zoom2);
    IniPutDbl("prefs","brightness",g_brightness);

    char k[40];
    for(int i=0;i<SC_COUNT;i++){
        if(i!=SC_VEC){
            snprintf(k,sizeof(k),"%s_on",SC_KEY[i]);
            IniPutBool("scopes",k,g_sc[i].on);
        }
        snprintf(k,sizeof(k),"%s_prop",SC_KEY[i]);
        IniPutDbl("scopes",k,g_sc[i].prop);
    }

    /* fenetre principale : ignore l'etat minimise, qui n'est pas une place */
    if(g_hMain&&!IsIconic(g_hMain)&&IsWindowVisible(g_hMain)){
        RECT r;
        if(GetWindowRect(g_hMain,&r)){ g_mainRect=r; g_mainRectValid=TRUE; }
    }
    if(g_mainRectValid) IniPutRect("window","main",&g_mainRect);

    for(int i=SC_WAVE;i<SC_COUNT;i++){
        if(g_sc[i].wnd&&!IsIconic(g_sc[i].wnd)){
            RECT r;
            if(GetWindowRect(g_sc[i].wnd,&r)){ g_sc[i].wr=r; g_sc[i].wrValid=TRUE; }
        }
        if(g_sc[i].wrValid) IniPutRect("detached",SC_KEY[i],&g_sc[i].wr);
    }
}

static void RenderAll(void);
static void RecomputeLayout(HWND);
static void RebuildTemplate(int idx);
static void ApplySplitMode(void);
static void FitWindowToContent(HWND);
static void EnsureScopeBmp(int idx,int w,int h);
static void RenderScope(int idx);
static void ApplyLanguage(void);
static void CreateFonts(void);
static void DestroyFonts(void);


/* ================= PRNG (dithering anti-moire) ================= */
static unsigned int g_rng=2463534242u;
static inline void SeedRng(unsigned int s){ g_rng=s?s:2463534242u; }
static inline double Jitter(void)
{
    g_rng^=g_rng<<13; g_rng^=g_rng>>17; g_rng^=g_rng<<5;
    return (double)(g_rng>>8)*(1.0/16777216.0)-0.5;
}

static void ComputeUVf(double R,double G,double B,double*u,double*v)
{
    double Y=0.299*R+0.587*G+0.114*B;
    *u=(B-Y)*0.492;
    *v=(R-Y)*0.877;
}

/* ================= primitives AA ================= */
static void BlendPx(DWORD*buf,int W,int H,int x,int y,BYTE r,BYTE g,BYTE b,double a)
{
    if(x<0||x>=W||y<0||y>=H||a<=0.0) return;
    if(a>1.0)a=1.0;
    DWORD c=buf[y*W+x];
    BYTE cr=(BYTE)((c>>16)&0xFF),cg=(BYTE)((c>>8)&0xFF),cb=(BYTE)(c&0xFF);
    buf[y*W+x]=((DWORD)(BYTE)(r*a+cr*(1-a))<<16)
              |((DWORD)(BYTE)(g*a+cg*(1-a))<<8)
              | (DWORD)(BYTE)(b*a+cb*(1-a));
}

static void AACircle(DWORD*buf,int W,int H,int cx,int cy,int R,BYTE r,BYTE g,BYTE b,double a)
{
    int x0=cx-R-2,x1=cx+R+2,y0=cy-R-2,y1=cy+R+2;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        double d=sqrt((double)(x-cx)*(x-cx)+(double)(y-cy)*(y-cy));
        double cov=1.0-fabs(d-R);
        if(cov>0) BlendPx(buf,W,H,x,y,r,g,b,cov*a);
    }
}

static double SegDist(double px,double py,double ax,double ay,double bx,double by)
{
    double vx=bx-ax,vy=by-ay,wx=px-ax,wy=py-ay;
    double l2=vx*vx+vy*vy;
    double t=(l2>1e-9)?(vx*wx+vy*wy)/l2:0.0;
    if(t<0)t=0;
    if(t>1)t=1;
    double dx=px-(ax+t*vx),dy=py-(ay+t*vy);
    return sqrt(dx*dx+dy*dy);
}

static void AALine(DWORD*buf,int W,int H,double ax,double ay,double bx,double by,
                   double wid,BYTE r,BYTE g,BYTE b,double a)
{
    int x0=(int)floor(fmin(ax,bx)-wid-2),x1=(int)ceil(fmax(ax,bx)+wid+2);
    int y0=(int)floor(fmin(ay,by)-wid-2),y1=(int)ceil(fmax(ay,by)+wid+2);
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        double cov=(wid/2.0+0.5)-SegDist(x+0.5,y+0.5,ax,ay,bx,by);
        if(cov>0) BlendPx(buf,W,H,x,y,r,g,b,cov*a);
    }
}

static void AASquare(DWORD*buf,int W,int H,int cx,int cy,int half,BYTE r,BYTE g,BYTE b,double a)
{
    int x0=cx-half-2,x1=cx+half+2,y0=cy-half-2,y1=cy+half+2;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        double dx=fmax(fabs(x-cx)-half,0.0),dy=fmax(fabs(y-cy)-half,0.0);
        double cov=1.0-sqrt(dx*dx+dy*dy);
        if(cov>0) BlendPx(buf,W,H,x,y,r,g,b,cov*a);
    }
}

static void AADisc(DWORD*buf,int W,int H,double cx,double cy,double R,BYTE r,BYTE g,BYTE b,double a)
{
    int x0=(int)floor(cx-R-1),x1=(int)ceil(cx+R+1);
    int y0=(int)floor(cy-R-1),y1=(int)ceil(cy+R+1);
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        double dx=(x+0.5)-cx,dy=(y+0.5)-cy;
        double cov=R+0.5-sqrt(dx*dx+dy*dy);
        if(cov>1.0)cov=1.0;
        if(cov>0) BlendPx(buf,W,H,x,y,r,g,b,cov*a);
    }
}

static void AARing(DWORD*buf,int W,int H,double cx,double cy,double R,double wid,
                   BYTE r,BYTE g,BYTE b,double a)
{
    int x0=(int)floor(cx-R-2),x1=(int)ceil(cx+R+2);
    int y0=(int)floor(cy-R-2),y1=(int)ceil(cy+R+2);
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=W)x1=W-1;
    if(y1>=H)y1=H-1;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
        double dx=(x+0.5)-cx,dy=(y+0.5)-cy;
        double d=sqrt(dx*dx+dy*dy);
        double cov=(wid/2.0+0.5)-fabs(d-R);
        if(cov>1.0)cov=1.0;
        if(cov>0) BlendPx(buf,W,H,x,y,r,g,b,cov*a);
    }
}

static inline void AddPx(DWORD*buf,int W,int x,int y,BYTE R,BYTE G,BYTE B,double gain)
{
    DWORD c=buf[y*W+x];
    int r=(int)((c>>16)&0xFF)+(int)(R*gain);
    int g=(int)((c>>8)&0xFF)+(int)(G*gain);
    int b=(int)(c&0xFF)+(int)(B*gain);
    if(r>255)r=255;
    if(g>255)g=255;
    if(b>255)b=255;
    buf[y*W+x]=((DWORD)r<<16)|((DWORD)g<<8)|(DWORD)b;
}

/* ================= graticules ================= */
static void TplVec(void)
{
    Scope*s=&g_sc[SC_VEC];
    int W=s->w,H=s->h;
    int D=(W<H)?W:H;
    int cx=W/2,cy=H/2;
    DWORD*buf=s->bits;
    for(int i=0;i<W*H;i++) buf[i]=UI_BG_DWORD;

    double R1=D*0.44;                 /* cercle 100% */
    double k=R1/CHROMA_MAX;           /* echelle unique cibles <-> nuage */

    AACircle(buf,W,H,cx,cy,(int)R1,100,100,106,0.6);
    AACircle(buf,W,H,cx,cy,(int)(R1*0.5),100,100,106,0.35);
    AALine(buf,W,H,0,cy,W,cy,1.0,88,88,94,0.35);
    AALine(buf,W,H,cx,0,cx,H,1.0,88,88,94,0.35);

    double sx2=0,sy2=0;
    if(g_showSkin){
        /* ligne des tons chair : direction du I-axis (~123 deg), longueur = R1 */
        double u,v; ComputeUVf(255,200,160,&u,&v);
        double len=sqrt(u*u+v*v);
        if(len>1e-4){
            double dx=u/len,dy=-v/len;
            sx2=cx+dx*R1; sy2=cy+dy*R1;
            AALine(buf,W,H,cx,cy,sx2,sy2,1.2,235,150,100,0.8);
        }
    }

    typedef struct{const wchar_t*n;BYTE r,g,b;}T;
    T t[6]={{L"R",255,0,0},{L"J",255,255,0},{L"V",0,255,0},
            {L"C",0,255,255},{L"B",0,0,255},{L"M",255,0,255}};
    int tx[6],ty[6];
    int half=(int)(D*0.011); if(half<3) half=3;
    for(int i=0;i<6;i++){
        /* position REELLE de la primaire : chaque cible a son propre rayon
           (R et C sur le cercle, V et M plus pres, J et B encore plus pres) */
        double u,v; ComputeUVf(t[i].r,t[i].g,t[i].b,&u,&v);
        tx[i]=(int)(cx+u*k);
        ty[i]=(int)(cy-v*k);
        AASquare(buf,W,H,tx[i],ty[i],half,t[i].r,t[i].g,t[i].b,0.95);
    }

    HGDIOBJ of=SelectObject(s->dc,g_hFont);
    SetBkMode(s->dc,TRANSPARENT);
    TEXTMETRICW tm; GetTextMetricsW(s->dc,&tm);
    int th=tm.tmHeight;
    for(int i=0;i<6;i++){
        SetTextColor(s->dc,RGB(150,150,155));
        int lw=WideW(s->dc,t[i].n);
        int lx=tx[i]+half+UI(4);
        if(lx+lw>W-2) lx=tx[i]-half-UI(4)-lw;   /* la lettre passe a gauche */
        DrawTxtW(s->dc,lx,ty[i]-th/2,t[i].n);
    }
    SetTextColor(s->dc,RGB(120,120,124));
    DrawTxtW(s->dc,cx+UI(4),UI(2),Tr(S_PCT100));
    if(g_showSkin){
        SetTextColor(s->dc,RGB(225,160,120));
        const wchar_t*sk=Tr(S_SKIN);
        int sw=WideW(s->dc,sk);
        int skx=((int)sx2>=cx)?((int)sx2+UI(5)):((int)sx2-UI(5)-sw);
        int sky=((int)sy2>=cy)?((int)sy2+UI(2)):((int)sy2-th-UI(2));
        DrawTxtW(s->dc,skx,sky,sk);
    }
    SelectObject(s->dc,of);
    memcpy(s->tpl,s->bits,(size_t)W*H*sizeof(DWORD));
}

static void TplWaveLike(int idx,BOOL parade)
{
    Scope*s=&g_sc[idx];
    int W=s->w,H=s->h;
    DWORD*buf=s->bits;
    for(int i=0;i<W*H;i++) buf[i]=UI_BG_DWORD;

    for(int p=0;p<=4;p++){
        int y=(int)((1.0-p/4.0)*(H-1));
        AALine(buf,W,H,0,y,W,y,1.0,88,88,94,0.30);
    }
    if(parade||(idx==SC_WAVE&&g_wfRGB))
        for(int k=1;k<3;k++){
            int x=k*W/3;
            AALine(buf,W,H,x,0,x,H,1.0,88,88,94,0.35);
        }

    HGDIOBJ of=SelectObject(s->dc,g_hFont);
    SetBkMode(s->dc,TRANSPARENT);
    SetTextColor(s->dc,RGB(120,120,124));
    const wchar_t*lbl[5]={L"0",L"25",L"50",L"75",L"100"};
    for(int p=0;p<=4;p++){
        int y=(int)((1.0-p/4.0)*(H-1));
        int ty=y-14; if(ty<1) ty=1;
        DrawTxtW(s->dc,3,ty,lbl[p]);
    }
    SetTextColor(s->dc,RGB(110,110,116));
    {   /* repere colle au bord droit, quelle que soit la langue */
        const wchar_t*m=Tr(parade?S_PARADE:S_WAVEFORM);
        DrawTxtW(s->dc,W-UI(3)-WideW(s->dc,m),UI(2),m);
    }
    SelectObject(s->dc,of);
    memcpy(s->tpl,s->bits,(size_t)W*H*sizeof(DWORD));
}

static void TplHist(void)
{
    Scope*s=&g_sc[SC_HIST];
    int W=s->w,H=s->h;
    DWORD*buf=s->bits;
    for(int i=0;i<W*H;i++) buf[i]=UI_BG_DWORD;

    for(int p=0;p<=4;p++){
        int x=(int)((p/4.0)*(W-1));
        AALine(buf,W,H,x,0,x,H,1.0,88,88,94,0.28);
    }
    for(int p=1;p<4;p++){
        int y=(int)((p/4.0)*(H-1));
        AALine(buf,W,H,0,y,W,y,1.0,88,88,94,0.16);
    }
    AALine(buf,W,H,0,H-1,W,H-1,1.4,110,110,116,0.5);

    HGDIOBJ of=SelectObject(s->dc,g_hFont);
    SetBkMode(s->dc,TRANSPARENT);
    SetTextColor(s->dc,RGB(120,120,124));
    DrawTxtW(s->dc,4,H-18,L"0");
    DrawTxtW(s->dc,W-UI(4)-WideW(s->dc,Tr(S_255)),H-18,Tr(S_255));
    SetTextColor(s->dc,RGB(110,110,116));
    DrawTxtW(s->dc,4,3,Tr(S_RGB));
    SelectObject(s->dc,of);
    memcpy(s->tpl,s->bits,(size_t)W*H*sizeof(DWORD));
}

static void RebuildTemplate(int idx)
{
    Scope*s=&g_sc[idx];
    if(!s->bits||!s->tpl||s->w<=0||s->h<=0) return;
    if(idx==SC_VEC) TplVec();
    else if(idx==SC_WAVE) TplWaveLike(SC_WAVE,FALSE);
    else if(idx==SC_PARADE) TplWaveLike(SC_PARADE,TRUE);
    else TplHist();
}

/* ================= rendu vectorscope ================= */
static void RenderVec(void)
{
    Scope*s=&g_sc[SC_VEC];
    if(!s->bits||!s->tpl||s->w<=0||s->h<=0) return;
    int W=s->w,H=s->h;
    /* W*H (et non W*W) : un bitmap non carre ne doit jamais deborder */
    memcpy(s->bits,s->tpl,(size_t)W*H*sizeof(DWORD));
    if(g_sCount<=0) return;

    int D=(W<H)?W:H;
    double cx0=W/2.0,cy0=H/2.0;
    double R1=D*0.44;
    /* MEME echelle que les cibles : une couleur pure tombe sur son repere */
    double k=(R1/CHROMA_MAX)*(g_zoom2?2.0:1.0);
    double gain=8000.0/(double)g_sCount;
    if(gain<0.05)gain=0.05;
    if(gain>0.9)gain=0.9;
    gain*=g_brightness;
    int dot=(D>=560)?2:1;

    SeedRng(0x1234567u);
    for(int i=0;i<g_sCount;i++){
        BYTE R=g_sR[i],G=g_sG[i],B=g_sB[i];
        double u,v;
        ComputeUVf(R+Jitter(),G+Jitter(),B+Jitter(),&u,&v);
        double dx=u*k,dy=-v*k;
        if(dx*dx+dy*dy>R1*R1) continue;   /* ecrete au cercle 100% */
        int px=(int)(cx0+dx),py=(int)(cy0+dy);
        if(px<0||py<0||px>=W-dot||py>=H-dot) continue;
        for(int oy=0;oy<dot;oy++)
            for(int ox=0;ox<dot;ox++)
                AddPx(s->bits,W,px+ox,py+oy,R,G,B,gain);
    }
}

/* ================= rendu waveform / parade ================= */
#define CLIP_MAX 2048
static void RenderWaveLike(int idx,BOOL parade)
{
    Scope*s=&g_sc[idx];
    if(!s->on||!s->bits||!s->tpl||s->w<=0) return;
    int W=s->w,H=s->h;
    memcpy(s->bits,s->tpl,(size_t)W*H*sizeof(DWORD));
    if(g_sCount<=0||g_sCols<=0) return;

    BOOL rgb = parade || (idx==SC_WAVE && g_wfRGB);

    double gain=6000.0/(double)g_sCount;
    if(gain<0.05)gain=0.05;
    if(gain>0.9)gain=0.9;
    gain*=g_brightness;

    int panelW=rgb?(W/3):W;
    if(panelW<2) return;

    double xs=(g_sCols>1)?((double)(panelW-1)/(double)(g_sCols-1)):0.0;
    double ys=(double)(H-1)/255.0;

    static BYTE clipLo[3*CLIP_MAX],clipHi[3*CLIP_MAX];
    int nPanels=rgb?3:1;
    BOOL doClip=(g_clipping && panelW<=CLIP_MAX);
    if(doClip){
        memset(clipLo,0,(size_t)nPanels*panelW);
        memset(clipHi,0,(size_t)nPanels*panelW);
    }

    SeedRng(0x7654321u);
    for(int i=0;i<g_sCount;i++){
        BYTE R=g_sR[i],G=g_sG[i],B=g_sB[i];
        double fx=(double)g_sCol[i]*xs+Jitter()*(xs>1.0?xs:1.0);
        int bx=(int)(fx+0.5);
        if(bx<0)bx=0;
        if(bx>=panelW)bx=panelW-1;

        if(!rgb){
            double lum=0.299*R+0.587*G+0.114*B+Jitter();
            int y=(int)((H-1)-lum*ys+0.5);
            if(y>=0&&y<H) AddPx(s->bits,W,bx,y,255,255,255,gain);
            if(doClip){
                if(R==0&&G==0&&B==0) clipLo[bx]=1;
                if(R==255&&G==255&&B==255) clipHi[bx]=1;
            }
        } else {
            int y;
            y=(int)((H-1)-(R+Jitter())*ys+0.5);
            if(y>=0&&y<H) AddPx(s->bits,W,bx,y,255,60,60,gain);
            y=(int)((H-1)-(G+Jitter())*ys+0.5);
            if(y>=0&&y<H) AddPx(s->bits,W,panelW+bx,y,60,255,60,gain);
            y=(int)((H-1)-(B+Jitter())*ys+0.5);
            if(y>=0&&y<H) AddPx(s->bits,W,2*panelW+bx,y,80,110,255,gain);
            if(doClip){
                if(R==0)   clipLo[bx]=1;
                if(R==255) clipHi[bx]=1;
                if(G==0)   clipLo[panelW+bx]=1;
                if(G==255) clipHi[panelW+bx]=1;
                if(B==0)   clipLo[2*panelW+bx]=1;
                if(B==255) clipHi[2*panelW+bx]=1;
            }
        }
    }

    /* clipping : blanc pur -> barre ROUGE en haut, noir pur -> barre BLEUE en bas */
    if(doClip){
        for(int p=0;p<nPanels;p++){
            for(int bx=0;bx<panelW;bx++){
                int i2=p*panelW+bx;
                int x=p*panelW+bx;
                if(x>=W) continue;
                if(clipHi[i2])
                    for(int k=0;k<3;k++) BlendPx(s->bits,W,H,x,k,255,50,50,0.95);
                if(clipLo[i2])
                    for(int k=0;k<3;k++) BlendPx(s->bits,W,H,x,H-1-k,70,130,255,0.95);
            }
        }
    }
}

/* ================= rendu histogramme (lisse + courbes de crete) ================= */
static void RenderHist(void)
{
    Scope*s=&g_sc[SC_HIST];
    if(!s->on||!s->bits||!s->tpl||s->w<=0) return;
    int W=s->w,H=s->h;
    memcpy(s->bits,s->tpl,(size_t)W*H*sizeof(DWORD));
    if(g_sCount<=0) return;

    static double bins[3][256],sm[3][256];
    memset(bins,0,sizeof(bins));
    for(int i=0;i<g_sCount;i++){
        bins[0][g_sR[i]]+=1.0;
        bins[1][g_sG[i]]+=1.0;
        bins[2][g_sB[i]]+=1.0;
    }

    /* lissage : noyau gaussien (rayon 3) -> courbe lisible, sans dents de scie */
    static const double kern[7]={0.06,0.12,0.20,0.24,0.20,0.12,0.06};
    for(int c=0;c<3;c++){
        for(int b=0;b<256;b++){
            double acc=0,wsum=0;
            for(int j=-3;j<=3;j++){
                int bb=b+j;
                if(bb<0||bb>255) continue;
                acc+=bins[c][bb]*kern[j+3];
                wsum+=kern[j+3];
            }
            sm[c][b]=(wsum>0)?acc/wsum:0;
        }
    }

    double maxB=1;
    for(int c=0;c<3;c++)
        for(int b=0;b<256;b++)
            if(sm[c][b]>maxB) maxB=sm[c][b];

    /* Echelle en puissance (gamma ~0.45), comme Lightroom / DaVinci.
       Une echelle logarithmique etait utilisee auparavant : elle exagerait
       enormement les tres faibles comptages (1 pixel noir sur 90000 donnait
       deja une barre a 9% de hauteur), ce qui simulait un noir bouche ou des
       blancs brules inexistants. Ici, un comptage negligeable reste negligeable. */
    static const double HIST_GAMMA=0.45;

    /* Table de la courbe gamma (0.45), calculee une seule fois. On interpole
       lineairement dedans au lieu d'appeler pow() par colonne de pixels :
       le rendu reste identique, mais on economise des milliers de pow(). */
    #define HIST_LUT 1024
    static double powlut[HIST_LUT+1];
    static int lutReady=0;
    if(!lutReady){
        for(int i=0;i<=HIST_LUT;i++) powlut[i]=pow((double)i/HIST_LUT,HIST_GAMMA);
        lutReady=1;
    }

    static const BYTE col[3][3]={{255,70,70},{70,235,110},{90,130,255}};
    double alpha=0.5*g_brightness;
    if(alpha>1.0) alpha=1.0;

    /* hauteur de la courbe pour chaque colonne de pixels */
    static double hgt[3][4096];
    int WW=(W>4096)?4096:W;
    double invMax=(maxB>0)?1.0/maxB:0.0;
    for(int c=0;c<3;c++){
        for(int x=0;x<WW;x++){
            double fb=(WW>1)?((double)x*255.0/(double)(WW-1)):0.0;
            int b0=(int)fb; if(b0>255)b0=255;
            int b1=(b0<255)?b0+1:255;
            double f=fb-b0;
            double v=(1.0-f)*sm[c][b0]+f*sm[c][b1];
            double nv=v*invMax;
            if(nv<0)nv=0;
            if(nv>1)nv=1;
            double fi=nv*HIST_LUT;
            int li=(int)fi;
            double t=(li<HIST_LUT)?(powlut[li]+(powlut[li+1]-powlut[li])*(fi-li))
                                  :powlut[HIST_LUT];
            hgt[c][x]=t*(H-3);
        }
    }

    /* remplissage doux sous la courbe */
    for(int c=0;c<3;c++){
        for(int x=0;x<WW;x++){
            int top=(int)(H-2-hgt[c][x]);
            for(int y=H-2;y>top;y--)
                AddPx(s->bits,W,x,y,col[c][0],col[c][1],col[c][2],alpha*0.22);
        }
    }

    /* courbe de crete, de la couleur du canal, anti-aliasee (facon Lightroom) */
    for(int c=0;c<3;c++){
        for(int x=0;x<WW-1;x++){
            double y0=H-2-hgt[c][x];
            double y1=H-2-hgt[c][x+1];
            AALine(s->bits,W,H,x,y0,x+1,y1,1.6,col[c][0],col[c][1],col[c][2],0.95);
        }
    }
}

static void RenderScope(int idx)
{
    if(idx==SC_VEC) RenderVec();
    else if(idx==SC_WAVE) RenderWaveLike(SC_WAVE,FALSE);
    else if(idx==SC_PARADE) RenderWaveLike(SC_PARADE,TRUE);
    else RenderHist();
}

static void RenderAll(void)
{
    for(int i=0;i<SC_COUNT;i++) RenderScope(i);
    InvalidateRect(g_hMain,NULL,FALSE);
    for(int i=SC_WAVE;i<SC_COUNT;i++)
        if(g_sc[i].wnd) InvalidateRect(g_sc[i].wnd,NULL,FALSE);
}

/* ================= capture ================= */
/* Le contexte + le DIB de capture sont conserves d'une frame a l'autre :
   on ne les recree que lorsque la taille de la zone change. Cela evite une
   allocation/liberation GDI a chaque rafraichissement (~7 fois/seconde). */
static HDC     g_capDC=NULL;
static HBITMAP g_capBmp=NULL;
static void   *g_capBits=NULL;
static int     g_capW=0,g_capH=0;

/* bandes de fond de la derniere capture (haut, bas, gauche, droite), en px */
static int     g_barT=0,g_barB=0,g_barL=0,g_barR=0;
static int     g_prevCols=-1;   /* colonnes echantillonnees : repere un changement de cadrage */

/* ================= exclusion des bandes de fond =================
   Un lecteur d'images qui affiche une photo d'un format different de sa
   fenetre remplit le reste avec une couleur unie : bandes noires/grises/blanches
   en haut et en bas (letterbox) ou a gauche et a droite (pillarbox). Ces bandes
   sont collees aux bords, occupent une grande surface et fausseraient completement
   l'histogramme, le waveform et le vectorscope.

   Une bande est ici une suite de lignes (ou colonnes) consecutives partant du
   bord dont chacune est quasi uniforme (aucune texture) et dont la couleur reste
   egale a celle de la premiere ligne. Trois garde-fous evitent de confondre une
   zone unie de l'image avec une bande :
     - la couleur doit etre neutre (faible ecart entre canaux) : un fond de
       lecteur est noir, gris ou blanc, un ciel bleu ou un mur colore ne l'est pas;
     - les deux cotes opposes, quand ils existent tous les deux, doivent avoir
       la meme couleur (un ciel clair en haut de photo ne passe donc pas);
     - les 5% extremes de chaque ligne sont ignores, sinon les coins arrondis de
       certaines fenetres bloqueraient la detection.                              */
/* Une bande de fond est une couleur unie exacte : une capture d'ecran n'a ni
   bruit ni compression. Les tolerances restent donc TRES serrees. Trop larges,
   elles laissent la detection "deborder" dans une zone sombre et lisse de
   l'image (coin de ciel nocturne, degrade de vignettage) et le repere de la
   zone mesuree se decale vers l'interieur de la photo : la bande doit s'arreter
   au premier pixel qui n'est pas identique au fond. */
#define BAR_FLAT_TOL    3     /* ecart max entre pixels d'une meme ligne   */
#define BAR_COLOR_TOL   4     /* ecart max entre la ligne et la reference  */
#define BAR_NEUTRAL_TOL 20    /* ecart max entre canaux d'un fond neutre   */

static BOOL BarLineColor(BYTE*px,int w,int h,int step,int line,BOOL horiz,BYTE out[3])
{
    int dim=horiz?w:h;
    int m=dim/20;
    int ldim=horiz?h:w;
    if(line<0||line>=ldim||m<=0) return FALSE;

    int mn[3]={255,255,255},mx[3]={0,0,0};
    long s[3]={0,0,0};
    int n=0;
    for(int i=m;i<dim-m;i+=step){
        BYTE*p=horiz?(px+((size_t)line*w+i)*4):(px+((size_t)i*w+line)*4);
        for(int k=0;k<3;k++){
            BYTE c=p[2-k];              /* p[2]=R, p[1]=G, p[0]=B */
            if(c<mn[k])mn[k]=c;
            if(c>mx[k])mx[k]=c;
            s[k]+=c;
        }
        n++;
    }
    if(n<=0) return FALSE;
    for(int k=0;k<3;k++)
        if(mx[k]-mn[k]>BAR_FLAT_TOL) return FALSE;
    for(int k=0;k<3;k++) out[k]=(BYTE)(s[k]/n);
    return TRUE;
}

static int BarBand(BYTE*px,int w,int h,int step,BOOL horiz,int start,int delta,int maxScan,BYTE ref[3])
{
    int thick=0;
    for(int i=0;i<maxScan;i++){
        BYTE c[3];
        if(!BarLineColor(px,w,h,step,start+delta*i,horiz,c)) break;
        if(thick==0){
            int mx=c[0],mn=c[0];
            for(int k=1;k<3;k++){
                if(c[k]>mx)mx=c[k];
                if(c[k]<mn)mn=c[k];
            }
            if(mx-mn>BAR_NEUTRAL_TOL) break;     /* fond colore : pas une bande */
            ref[0]=c[0]; ref[1]=c[1]; ref[2]=c[2];
        } else {
            int d=0;
            for(int k=0;k<3;k++){
                int dd=(int)c[k]-(int)ref[k];
                if(dd<0) dd=-dd;
                if(dd>d) d=dd;
            }
            if(d>BAR_COLOR_TOL) break;          /* couleur qui derive : on s'arrete */
        }
        thick++;
    }
    return thick;
}

static BOOL BarSameColor(const BYTE a[3],const BYTE b[3])
{
    for(int k=0;k<3;k++){
        int d=(int)a[k]-(int)b[k];
        if(d<0) d=-d;
        if(d>BAR_COLOR_TOL) return FALSE;
    }
    return TRUE;
}

static void DetectBars(BYTE*px,int w,int h,int*t,int*b,int*l,int*r)
{
    *t=*b=*l=*r=0;
    if(w<32||h<32) return;

    int sh=(w>600)?(w/600):1;      /* ~600 echantillons par ligne   */
    int sv=(h>600)?(h/600):1;      /* ~600 echantillons par colonne */
    int maxV=h/2,maxH=w/2;

    BYTE rt[3]={0,0,0},rb[3]={0,0,0},rl[3]={0,0,0},rr[3]={0,0,0};
    int bt=BarBand(px,w,h,sh,TRUE,0,+1,maxV,rt);
    int bb=BarBand(px,w,h,sh,TRUE,h-1,-1,maxV,rb);
    int bl=BarBand(px,w,h,sv,FALSE,0,+1,maxH,rl);
    int br=BarBand(px,w,h,sv,FALSE,w-1,-1,maxH,rr);

    /* epaisseur minimale : une bande fine isolee est du bruit, pas un cadrage */
    int minV=(h/150>4)?(h/150):4;
    int minH=(w/150>4)?(w/150):4;
    if(bt<minV) bt=0;
    if(bb<minV) bb=0;
    if(bl<minH) bl=0;
    if(br<minH) br=0;

    if(bt&&bb&&!BarSameColor(rt,rb)) bt=bb=0;
    if(bl&&br&&!BarSameColor(rl,rr)) bl=br=0;

    /* il doit rester de l'image : pas plus de 80% retire sur un axe
       (une photo panoramique tres letterboxee reste dans les clous) */
    if(bt+bb>(h*8)/10) bt=bb=0;
    if(bl+br>(w*8)/10) bl=br=0;

    *t=bt; *b=bb; *l=bl; *r=br;
}

/* ================= sortie HDR =================
   BitBlt sur le DC d'ecran ne rend JAMAIS que 8 bits par canal. Sur une sortie
   HDR, ce qu'il recupere est deja le rendu que Windows a bien voulu redescendre
   en SDR. Deux comportements opposes ont ete observes selon l'application :

     - la plage est ecrasee : tout ce qui depasse le blanc SDR revient a 255.
       La, l'information est PERDUE avant nous. Tous ces pixels ont la meme
       valeur : aucune echelle, aucune courbe, aucun etirement ne peut les
       re-separer. On ne peut que le mesurer et le dire.
     - la plage est comprimee vers le bas : le lecteur rend tout sous 255 (par
       exemple blanc max mesure = 180). La, l'information est intacte mais
       mal etalee, et une mise a l'echelle lineaire est juste : elle rend a
       l'histogramme les 0..255 qu'il aurait du avoir.

   On distingue les deux a chaque capture (max mesure + % d'echantillons a 255),
   on etale quand c'est recuperable, et on l'affiche dans les deux cas. */
static BOOL     g_hdrActive=FALSE;
static DWORD    g_hdrTick=0;
static int      g_hdrSatPct=0;    /* % d'echantillons a 255 sur un canal */
static double   g_hdrGain=1.0;    /* correction appliquee, 1 = rien */

static BOOL HdrActiveAt(POINT phys)
{
    HMONITOR hm=MonitorFromPoint(phys,MONITOR_DEFAULTTONEAREST);
    if(!hm) return FALSE;
    MONITORINFOEXW mi; memset(&mi,0,sizeof mi);
    mi.cbSize=sizeof mi;
    GetMonitorInfoW(hm,(LPMONITORINFO)&mi);

    UINT32 np=0,nm=0;
    if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&np,&nm)!=ERROR_SUCCESS) return FALSE;
    DISPLAYCONFIG_PATH_INFO *pa=(DISPLAYCONFIG_PATH_INFO*)calloc(np,sizeof *pa);
    DISPLAYCONFIG_MODE_INFO *mo=(DISPLAYCONFIG_MODE_INFO*)calloc(nm,sizeof *mo);
    BOOL hdr=FALSE,any=FALSE,matched=FALSE;

    if(pa&&mo&&QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&np,pa,&nm,mo,NULL)==ERROR_SUCCESS){
        for(UINT32 i=0;i<np;i++){
            if(!(pa[i].flags&DISPLAYCONFIG_PATH_ACTIVE)) continue;

            /* le chemin correspond-il au moniteur sous la zone ? */
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn; memset(&sn,0,sizeof sn);
            sn.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size=sizeof sn;
            sn.header.adapterId=pa[i].sourceInfo.adapterId;
            sn.header.id=pa[i].sourceInfo.id;
            BOOL same=(DisplayConfigGetDeviceInfo(&sn.header)==ERROR_SUCCESS)
                   && (lstrcmpiW(sn.viewGdiDeviceName,mi.szDevice)==0);

            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO g; memset(&g,0,sizeof g);
            g.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            g.header.size=sizeof g;
            g.header.adapterId=pa[i].targetInfo.adapterId;
            g.header.id=pa[i].targetInfo.id;
            if(DisplayConfigGetDeviceInfo(&g.header)!=ERROR_SUCCESS) continue;

            if(g.advancedColorEnabled) any=TRUE;
            if(same){ hdr=g.advancedColorEnabled?TRUE:FALSE; matched=TRUE; break; }
        }
    }
    free(pa); free(mo);
    return matched?hdr:any;   /* moniteur non resolu : on prend le pire cas */
}

/* ================= capture WGC (HDR, FP16 lineaire) =================
   Seulement sous un C qui expose les en-tetes WinRT (MinGW). Ailleurs, les
   stubs en bas laissent la voie fermee et BitBlt travaille seul. */
#if defined(__GNUC__)
/*
   Tant que la sortie est en PQ, BitBlt nous rend une image deja ecrasee : les
   pixels a 255 sont differents a l'ecran mais identiques chez nous. WGC rend la
   texture du desktop AVANT cette compression, en FP16 lineaire.

   Deux choses a connaitre, et aucune ne se devine :
     - le blanc SDR dans cette echelle (mesure ici : 3,5000 sur ce poste). Il
       depend du reglage "SDR content display brightness" de Windows, donc il
       faut le mesurer sur chaque machine, et le remesurer si l'ecran change ;
     - la crete de l'image. L'axe des scopes est etire jusque-la, ce qui
       remonte le blanc SDR tout en laissant les hautes lumieres respirer au
       lieu de les coller a 255.
   Tout ceci est optionnel : si WGC n'est pas disponible, on reste sur BitBlt. */

static ID3D11Device                *g_wDev=NULL;
static ID3D11DeviceContext         *g_wCtx=NULL;
static IGraphicsCaptureItem        *g_wItem=NULL;
static IDirect3D11CaptureFramePool *g_wPool=NULL;
static IGraphicsCaptureSession     *g_wSess=NULL;
static ID3D11Texture2D             *g_wStage=NULL;
static int   g_wW=0,g_wH=0,g_wReady=0,g_wTried=0;
static double g_sdrWhite=0.0;      /* FP16 du blanc SDR ; 0 = pas encore mesure */
static DWORD  g_calTick=0;         /* cooldown des tentatives de calibrage */
static double g_hdrStretch=1.0;    /* etirement d'axe applique a la frame */

static BYTE* g_wMap=NULL; static LONG g_wPitch=0;

static HSTRING WStr(const wchar_t*s){ HSTRING h=NULL;
    WindowsCreateString(s,(UINT32)wcslen(s),&h); return h; }

static void WgcClose(void){
    if(g_wSess){ IGraphicsCaptureSession_Release(g_wSess); g_wSess=NULL; }
    if(g_wPool){ IDirect3D11CaptureFramePool_Release(g_wPool); g_wPool=NULL; }
    if(g_wItem){ IGraphicsCaptureItem_Release(g_wItem); g_wItem=NULL; }
    if(g_wStage){ ID3D11Texture2D_Release(g_wStage); g_wStage=NULL; }
    if(g_wCtx){ ID3D11DeviceContext_Release(g_wCtx); g_wCtx=NULL; }
    if(g_wDev){ ID3D11Device_Release(g_wDev); g_wDev=NULL; }
    g_wReady=0; g_wW=g_wH=0;
}

static BOOL WgcOpen(HMONITOR hm)
{
    if(g_wTried) return g_wReady;
    g_wTried=1;
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
    if(FAILED(hr)&&hr!=RPC_E_CHANGED_MODE) return FALSE;

    hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                         NULL,0,D3D11_SDK_VERSION,&g_wDev,NULL,&g_wCtx);
    if(FAILED(hr)){ WgcClose(); return FALSE; }

    IGraphicsCaptureItemInterop *io=NULL;
    hr=RoGetActivationFactory(WStr(L"Windows.Graphics.Capture.GraphicsCaptureItem"),
        &IID_IGraphicsCaptureItemInterop,(void**)&io);
    if(FAILED(hr)){ WgcClose(); return FALSE; }
    hr=IGraphicsCaptureItemInterop_CreateForMonitor(io,hm,&IID_IGraphicsCaptureItem,(void**)&g_wItem);
    IGraphicsCaptureItemInterop_Release(io);
    if(FAILED(hr)){ WgcClose(); return FALSE; }

    SizeInt32 sz; memset(&sz,0,sizeof sz);
    if(FAILED(IGraphicsCaptureItem_get_Size(g_wItem,&sz))||sz.Width<2||sz.Height<2){
        WgcClose(); return FALSE; }
    g_wW=sz.Width; g_wH=sz.Height;

    IDXGIDevice *dxgi=NULL;
    hr=ID3D11Device_QueryInterface(g_wDev,&IID_IDXGIDevice,(void**)&dxgi);
    if(FAILED(hr)){ WgcClose(); return FALSE; }
    IInspectable *wrap=NULL;
    hr=CreateDirect3D11DeviceFromDXGIDevice(dxgi,&wrap);
    IDXGIDevice_Release(dxgi);
    if(FAILED(hr)||!wrap){ WgcClose(); return FALSE; }

    IDirect3D11CaptureFramePoolStatics2 *st2=NULL;
    hr=RoGetActivationFactory(WStr(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool"),
        &IID_IDirect3D11CaptureFramePoolStatics2,(void**)&st2);
    if(FAILED(hr)){ IInspectable_Release(wrap); WgcClose(); return FALSE; }
    hr=IDirect3D11CaptureFramePoolStatics2_CreateFreeThreaded(st2,
        (IDirect3DDevice*)wrap,(DirectXPixelFormat)DXGI_FORMAT_R16G16B16A16_FLOAT,2,sz,&g_wPool);
    IInspectable_Release(wrap);
    IDirect3D11CaptureFramePoolStatics2_Release(st2);
    if(FAILED(hr)){ WgcClose(); return FALSE; }

    hr=IDirect3D11CaptureFramePool_CreateCaptureSession(g_wPool,g_wItem,&g_wSess);
    if(FAILED(hr)){ WgcClose(); return FALSE; }
    IGraphicsCaptureSession3 *s3=NULL;
    if(SUCCEEDED(IGraphicsCaptureSession_QueryInterface(g_wSess,
            &IID_IGraphicsCaptureSession3,(void**)&s3))){
        IGraphicsCaptureSession3_put_IsBorderRequired(s3,false);
        IGraphicsCaptureSession3_Release(s3);
    }
    hr=IGraphicsCaptureSession_StartCapture(g_wSess);
    if(FAILED(hr)){ WgcClose(); return FALSE; }

    g_wReady=1;
    return TRUE;
}

/* Une frame du desktop entier. Le contenu ne bouge pas tout seul : on decale le
   curseur de quelques pixels pour forcer WGC a en produire une. */
static BOOL WgcGrab(void)
{
    if(!g_wReady) return FALSE;
    IDirect3D11CaptureFrame *fr=NULL;
    POINT c0; GetCursorPos(&c0);
    for(int i=0;i<40&&!fr;i++){
        SetCursorPos(c0.x+((i&1)?6:0),c0.y+((i&2)?6:0));
        IDirect3D11CaptureFramePool_TryGetNextFrame(g_wPool,&fr);
        if(!fr) Sleep(20);
    }
    SetCursorPos(c0.x,c0.y);
    if(!fr) return FALSE;

    BOOL ok=FALSE;
    IDirect3DSurface *sf=NULL;
    IDirect3DDxgiInterfaceAccess *acc=NULL;
    ID3D11Texture2D *tex=NULL;
    if(SUCCEEDED(IDirect3D11CaptureFrame_get_Surface(fr,&sf))&&
       SUCCEEDED(IDirect3DSurface_QueryInterface(sf,&IID_DxgiAccess,(void**)&acc))&&
       SUCCEEDED(acc->lpVtbl->GetInterface(acc,&IID_ID3D11Texture2D,(void**)&tex))){
        D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(tex,&td);
        if(td.Format==DXGI_FORMAT_R16G16B16A16_FLOAT){
            if(!g_wStage||g_wW!=(int)td.Width||g_wH!=(int)td.Height){
                if(g_wStage) ID3D11Texture2D_Release(g_wStage);
                D3D11_TEXTURE2D_DESC sd=td;
                sd.Usage=D3D11_USAGE_STAGING; sd.BindFlags=0;
                sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ; sd.MiscFlags=0;
                g_wStage=NULL;
                if(SUCCEEDED(ID3D11Device_CreateTexture2D(g_wDev,&sd,NULL,&g_wStage))){
                    g_wW=(int)td.Width; g_wH=(int)td.Height;
                }
            }
            if(g_wStage){
                ID3D11DeviceContext_CopyResource(g_wCtx,(ID3D11Resource*)g_wStage,(ID3D11Resource*)tex);
                D3D11_MAPPED_SUBRESOURCE mp;
                if(SUCCEEDED(ID3D11DeviceContext_Map(g_wCtx,(ID3D11Resource*)g_wStage,
                        0,D3D11_MAP_READ,0,&mp))){
                    g_wMap=(BYTE*)mp.pData; g_wPitch=(LONG)mp.RowPitch; ok=TRUE;
                }
            }
        }
    }
    if(tex) ID3D11Texture2D_Release(tex);
    if(acc) acc->lpVtbl->Release(acc);
    if(sf)  IDirect3DSurface_Release(sf);
    IDirect3D11CaptureFrame_Release(fr);
    return ok;
}

static void WgcUngrab(void){
    if(g_wMap&&g_wStage){ ID3D11DeviceContext_Unmap(g_wCtx,(ID3D11Resource*)g_wStage,0); }
    g_wMap=NULL; g_wPitch=0;
}

static inline float F16(unsigned short h){
    unsigned sign=(unsigned)(h&0x8000u)<<16, e=(h>>10)&0x1Fu, m=h&0x3FFu;
    union{unsigned u;float f;}r;
    if(e==0)         r.f=(float)m*5.9604644775390625e-8f;   /* 2^-24 */
    else if(e==0x1F) r.u=sign|0x7F800000u|(m<<13);
    else             r.u=sign|((e+112u)<<23)|(m<<13);
    return r.f;
}
/* OETF sRGB ETENDU au-dela de 1 : la loi en pow() se prolonge toute seule, ce
   qui laisse les super-blancs (n > 1) au-dessus de l'encodage du blanc au lieu
   de les ecraser. Renvoie 1.0 pour n = 1.0 (le blanc SDR). */
static inline double OetfExt(double n){
    if(n<=0.0) return 0.0;
    if(n<=0.0031308) return n*12.92;
    return 1.055*pow(n,1.0/2.4)-0.055;
}

/* crete de la zone, exprimee en multiples du blanc SDR */
static double WgcPeak(int x0,int y0,int w,int h)
{
    double mx=0.0;
    for(int y=0;y<h;y+=2){
        unsigned short*row=(unsigned short*)(g_wMap+(size_t)(y0+y)*g_wPitch);
        for(int x=0;x<w;x+=2){
            int i=(x0+x)*4;
            double v=F16(row[i]); if(F16(row[i+1])>v)v=F16(row[i+1]); if(F16(row[i+2])>v)v=F16(row[i+2]);
            if(v>mx) mx=v;
        }
    }
    return mx/(g_sdrWhite>0?g_sdrWhite:1.0);
}

/* zone -> DIB 32bpp.

   'scale' est la crete de la zone exprimee en multiples du blanc SDR (>= 1).
   La regle qui compte : l'etirement se fait APRES l'encodage, pas avant.
   Diviser les trois canaux par un meme facteur EN LUMIERE LINEAIRE puis coder
   ne preserve PAS la teinte - l'OETF est non lineaire, donc les rapports entre
   canaux encodes changent, et tout le nuage du vectorscope se deplace et se
   contracte : c'est exactement ce qu'on voyait entre HDR et SDR.
   Diviser les valeurs ENCODEES par OetfExt(scale) est une homothetie sur
   l'axe 0..255 : rapports entre canaux conserves, teinte et saturation
   inchangees, seule la luminosite est recomprimee.
   Et quand scale vaut 1 (aucun super-blanc) OetfExt(1)=1 : on retombe
   exactement sur le codage SDR, donc les deux modes sont comparables. */
static void WgcBlit(int x0,int y0,int w,int h,BYTE*dst,double scale)
{
    double denom=OetfExt(scale);
    if(denom<1.0) denom=1.0;
    double inv=1.0/denom;
    for(int y=0;y<h;y++){
        unsigned short*row=(unsigned short*)(g_wMap+(size_t)(y0+y)*g_wPitch);
        BYTE*d=dst+(size_t)y*w*4;
        for(int x=0;x<w;x++){
            int i=(x0+x)*4;
            for(int c=0;c<3;c++){
                double n=(double)F16(row[i+c])/g_sdrWhite;   /* 1.0 = blanc SDR */
                int q=(int)(OetfExt(n)*inv*255.0+0.5);
                d[x*4+c]=(BYTE)(q<0?0:(q>255?255:q));
            }
            d[x*4+3]=0xFF;
        }
    }
}

/* Mesure le blanc SDR : un aplat 255.0.0 a l'ecran, relu en FP16. On ne peut
   pas le deviner - c'est le reglage Windows "SDR content display brightness"
   qui le fixe, different sur chaque machine. */
static LRESULT CALLBACK CalProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_PAINT){ PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc); FillRect(dc,&rc,(HBRUSH)GetStockObject(WHITE_BRUSH));
        EndPaint(h,&ps); return 0; }
    if(m==WM_ERASEBKGND) return 1;
    return DefWindowProc(h,m,w,l);
}

static void CalibrateSdrWhite(void)
{
    if(!g_wReady) return;
    static const char cls[]="SWCalibSwatch";
    static BOOL reg=FALSE;
    if(!reg){
        WNDCLASSA wc; memset(&wc,0,sizeof wc);
        wc.lpfnWndProc=CalProc; wc.hInstance=g_inst; wc.lpszClassName=cls;
        RegisterClassA(&wc); reg=TRUE;
    }
    int S=40;
    /* quelques positions candidates : il faut que l'aplat soit vraiment visible */
    int cx[4]={8,8,g_wW-S-8,g_wW/2-S/2}, cy[4]={8,g_wH-S-8,g_wH-S-8,8};
    for(int t=0;t<4;t++){
        RECT r={cx[t],cy[t],cx[t]+S,cy[t]+S};
        HWND hs=CreateWindowExA(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
            cls,"",WS_POPUP,r.left,r.top,S,S,NULL,NULL,g_inst,NULL);
        if(!hs) continue;
        ShowWindow(hs,SW_SHOWNOACTIVATE); UpdateWindow(hs);
        Sleep(120);
        SetWindowPos(hs,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        Sleep(80);
        POINT c={r.left+S/2,r.top+S/2};
        if(WindowFromPoint(c)==hs && WgcGrab()){
            unsigned short*row=(unsigned short*)(g_wMap+(size_t)c.y*g_wPitch);
            double v=F16(row[c.x*4]);
            WgcUngrab();
            if(v>0.2&&v<64.0){ g_sdrWhite=v; DestroyWindow(hs); return; }
        }
        WgcUngrab();
        DestroyWindow(hs);
    }
    /* echec : on garde la derniere valeur connue (1.0 = SDR pur) */
}

#else  /* pas de WinRT en C sous MSVC : voie HDR fermee */
static int   g_wTried=0;
static int   g_wW=0,g_wH=0;
static DWORD g_calTick=0;
static double g_sdrWhite=0.0, g_hdrStretch=1.0;
static BOOL  WgcOpen(HMONITOR h){ (void)h; return FALSE; }
static BOOL  WgcGrab(void){ return FALSE; }
static void  WgcUngrab(void){}
static void  WgcClose(void){}
static void  CalibrateSdrWhite(void){}
static double WgcPeak(int a,int b,int c,int d){ (void)a;(void)b;(void)c;(void)d; return 1.0; }
static void  WgcBlit(int a,int b,int c,int d,BYTE*e,double f){
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
}
#endif

/* ---- barre de statut ----
   Le statut est une chaine FORMATEE, donc figee dans la langue du moment : on
   retient son identifiant et ses parametres pour pouvoir la refaire quand la
   langue change, sinon la barre du bas resterait dans l'autre langue. */
static int  g_statusId=-1;
static int  g_statusW=0,g_statusH=0;
static BOOL g_statusWgc=FALSE;

static void HdrFrag(wchar_t*out,int cap,BOOL viaWgc)
{
    out[0]=0;
    if(!g_hdrActive) return;
    if(viaWgc){
        if(g_hdrStretch>1.0001)
            swprintf(out,cap,Tr(S_HDRSTRETCH),g_hdrStretch,g_sdrWhite);
        else swprintf(out,cap,Tr(S_HDRWHITE),g_sdrWhite);
    }
    else if(g_hdrGain>1.001)
        swprintf(out,cap,Tr(S_HDRGAIN),g_hdrGain);
    else if(g_hdrSatPct>=5)
        swprintf(out,cap,Tr(S_HDRSAT),g_hdrSatPct);
    else lstrcpynW(out,Tr(S_HDR),cap);
}
static void SetStatusZone(BOOL bords,int w,int h,BOOL viaWgc)
{
    wchar_t hdr[64];
    int id=bords?S_ZONEBORDS:S_ZONEACTIVE;
    HdrFrag(hdr,64,viaWgc);
    g_statusId=id; g_statusW=w; g_statusH=h; g_statusWgc=viaWgc;
    if(bords) swprintf(g_status,256,Tr(id),w,h,g_barT,g_barB,g_barL,g_barR,hdr);
    else      swprintf(g_status,256,Tr(id),w,h,hdr);
}
static void SetStatusMsg(int id)
{
    g_statusId=id; g_statusW=g_statusH=0;
    lstrcpynW(g_status,Tr(id),256);
}
static void RefreshStatus(void)
{
    if(g_statusId==S_ZONEBORDS)      SetStatusZone(TRUE ,g_statusW,g_statusH,g_statusWgc);
    else if(g_statusId==S_ZONEACTIVE)SetStatusZone(FALSE,g_statusW,g_statusH,g_statusWgc);
    else if(g_statusId>=0)           SetStatusMsg(g_statusId);
}

static void Capture(RECT r,BOOL force)
{
    int w=r.right-r.left,h=r.bottom-r.top;
    if(w<2||h<2) return;

    /* etat HDR de l'ecran sous la zone, re-interroge toutes les 2 s : basculer
       HDR/SDR sans deplacer la selection doit etre vu. Doit etre connu AVANT
       de choisir la voie de capture. */
    {
        DWORD now=GetTickCount();
        if(g_hdrTick==0||(DWORD)(now-g_hdrTick)>=2000){
            POINT c={(r.left+r.right)/2,(r.top+r.bottom)/2};
            g_hdrTick=now; g_hdrActive=HdrActiveAt(c);
        }
    }

    HDC scr=GetDC(NULL);
    if(!g_capDC) g_capDC=CreateCompatibleDC(scr);

    if(!g_capBmp||g_capW!=w||g_capH!=h){
        BITMAPINFO bi; memset(&bi,0,sizeof(bi));
        bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth=w;
        bi.bmiHeader.biHeight=-h;
        bi.bmiHeader.biPlanes=1;
        bi.bmiHeader.biBitCount=32;
        bi.bmiHeader.biCompression=BI_RGB;
        void*nb=NULL;
        HBITMAP nbm=CreateDIBSection(g_capDC,&bi,DIB_RGB_COLORS,&nb,NULL,0);
        HGDIOBJ old=SelectObject(g_capDC,nbm);
        if(g_capBmp) DeleteObject(g_capBmp);   /* ancien DIB : plus selectionne */
        (void)old;                             /* 1ere fois : bitmap stock, a garder */
        g_capBmp=nbm; g_capBits=nb; g_capW=w; g_capH=h;
    }
    if(!g_capBmp){ ReleaseDC(NULL,scr); return; }

    /* Sortie HDR ? Alors WGC : BitBlt nous rendrait l'image APRES la
       compression HDR->SDR de Windows, avec les hautes lumieres deja collees a
       255. WGC rend le FP16 lineaire d'avant, intact. */
    g_hdrStretch=1.0;
    int viaWgc=0;
    if(g_hdrActive){
        HMONITOR hm=MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi; memset(&mi,0,sizeof mi); mi.cbSize=sizeof mi;
        if(WgcOpen(hm)&&GetMonitorInfoW(hm,(LPMONITORINFO)&mi)){
            /* le blanc SDR se mesure une fois ; 0 = inconnu, donc on retente au
               plus une fois toutes les 5 s (chaque tentative affiche un aplat
               blanc a l'ecran, ca ne doit pas clignoter en continu) */
            if(g_sdrWhite<=0.0&&(DWORD)(GetTickCount()-g_calTick)>=5000){
                g_calTick=GetTickCount();
                CalibrateSdrWhite();
            }
            int x0=r.left-mi.rcMonitor.left, y0=r.top-mi.rcMonitor.top;
            /* on exige que la zone tienne dans le moniteur : le DIB est deja
               alloue a w x h, le rogner ici les ferait desynchroniser */
            if(x0>=0&&y0>=0&&x0+w<=g_wW&&y0+h<=g_wH&&g_sdrWhite>0.2&&WgcGrab()){
                double peak=WgcPeak(x0,y0,w,h);
                double scale=(peak>1.0)?peak:1.0;
                g_hdrStretch=scale;
                WgcBlit(x0,y0,w,h,(BYTE*)g_capBits,scale);
                viaWgc=1;
                WgcUngrab();
            }
        }
    }
    if(!viaWgc) BitBlt(g_capDC,0,0,w,h,scr,r.left,r.top,SRCCOPY|CAPTUREBLT);
    ReleaseDC(NULL,scr);

    /* bandes de fond (letterbox / pillarbox) : on les mesure puis on les
       retire de l'echantillonnage. Toute la geometrie en decoule (leur
       epaisseur peut changer d'une image a l'autre). */
    int bt=0,bb=0,bl=0,br=0;
    if(g_ignoreBars) DetectBars((BYTE*)g_capBits,w,h,&bt,&bb,&bl,&br);

    int ix0=bl,iy0=bt;
    int iw=w-bl-br,ih=h-bt-bb;
    if(iw<4||ih<4){                     /* zone utile vide : on garde tout */
        ix0=0; iy0=0; iw=w; ih=h;
        bt=bb=bl=br=0;
    }

    double area=(double)iw*(double)ih;
    int stride=(int)ceil(sqrt(area/(double)MAX_SAMPLES));
    if(stride<1) stride=1;

    int nx=(iw+stride-1)/stride,ny=(ih+stride-1)/stride;
    int cap=nx*ny;
    if(cap>g_sCap){
        free(g_sR); free(g_sG); free(g_sB); free(g_sCol);
        g_sR=(BYTE*)malloc(cap); g_sG=(BYTE*)malloc(cap);
        g_sB=(BYTE*)malloc(cap); g_sCol=(WORD*)malloc((size_t)cap*sizeof(WORD));
        g_sCap=cap;
    }
    if(!g_sR||!g_sG||!g_sB||!g_sCol) return;
    g_sCount=0; g_sCols=nx;

    SeedRng(0x9E3779B9u);
    DWORD hash=2166136261u;
    int hstep=0;
    int mx=0,nsat=0;                 /* niveau max mesure, echantillons a 255 */
    BYTE*px=(BYTE*)g_capBits;
    for(int y=iy0;y<iy0+ih;y+=stride){
        int ci=0;
        for(int x=ix0;x<ix0+iw;x+=stride,ci++){
            int sx=x,sy=y;
            if(stride>1){
                sx=x+(int)((Jitter()+0.5)*stride);
                sy=y+(int)((Jitter()+0.5)*stride);
                if(sx>=ix0+iw) sx=ix0+iw-1;
                if(sy>=iy0+ih) sy=iy0+ih-1;
            }
            BYTE*row=px+(size_t)sy*w*4;
            BYTE B=row[sx*4+0],G=row[sx*4+1],R=row[sx*4+2];
            g_sR[g_sCount]=R; g_sG[g_sCount]=G; g_sB[g_sCount]=B;
            g_sCol[g_sCount]=(WORD)ci;
            g_sCount++;
            {
                int pm=(R>G)?(R>B?R:B):(G>B?G:B);
                if(pm>mx) mx=pm;
                if(pm==255) nsat++;
            }
            if((hstep++&3)==0){
                hash^=R; hash*=16777619u;
                hash^=G; hash*=16777619u;
                hash^=B; hash*=16777619u;
            }
        }
    }

    /* etat HDR de l'ecran sous la zone (re-interroge tous les 2 s : l'utilisateur
       peut basculer HDR/SDR sans bouger la selection) */
    g_hdrSatPct=g_sCount?(int)((long)nsat*100/g_sCount):0;
    g_hdrGain=1.0;
    /* la correction "plage comprimee" ne concerne que BitBlt : avec WGC l'image
       est deja juste, et un contenu qui n'a tout simplement pas de hautes
       lumieres ne doit surtout pas etre etire */
    if(g_hdrActive&&g_hdrNorm&&!viaWgc&&mx>0&&mx<=250){
        /* plage comprimee : information intacte, on ramene le blanc mesure a 255 */
        g_hdrGain=255.0/(double)mx;
        int half=mx/2;
        for(int i=0;i<g_sCount;i++){
            int v;
            v=(g_sR[i]*255+half)/mx; g_sR[i]=(BYTE)(v>255?255:v);
            v=(g_sG[i]*255+half)/mx; g_sG[i]=(BYTE)(v>255?255:v);
            v=(g_sB[i]*255+half)/mx; g_sB[i]=(BYTE)(v>255?255:v);
        }
    }

    /* le hash ne couvre que les pixels retenus : si les bandes bougent, les
       colonnes du waveform changent aussi -> on force le rendu. */
    BOOL barsChanged=(bt!=g_barT||bb!=g_barB||bl!=g_barL||br!=g_barR);
    BOOL geom=barsChanged||nx!=g_prevCols;
    if(!force&&hash==g_lastHash&&!geom) return;
    g_lastHash=hash;
    g_barT=bt; g_barB=bb; g_barL=bl; g_barR=br; g_prevCols=nx;
    /* le repere de la zone mesuree suit les bandes : on repeint l'overlay */
    if(barsChanged&&g_hOverlay) InvalidateRect(g_hOverlay,NULL,FALSE);

    SetStatusZone((bt||bb||bl||br)?TRUE:FALSE,w,h,viaWgc?TRUE:FALSE);
    g_hasSelection=TRUE;
    RenderAll();
}

static void SetTracking(BOOL on)
{
    g_paused=!on;
    if(!g_hasSelection){ ShowWindow(g_hOverlay,SW_HIDE); return; }
    if(on){
        SetWindowPos(g_hOverlay,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        ShowWindow(g_hOverlay,SW_SHOWNOACTIVATE);
        g_refreshCounter=0;
        Capture(g_selRectScreen,TRUE);
    } else {
        ShowWindow(g_hOverlay,SW_HIDE);
        SetStatusMsg(S_PAUSED);
    }
    InvalidateRect(g_hMain,NULL,FALSE);
}

static void ResetZone(void)
{
    g_hasSelection=FALSE; g_paused=FALSE;
    g_sCount=0; g_sCols=0; g_lastHash=0;
    g_barT=g_barB=g_barL=g_barR=0; g_prevCols=-1;
    ShowWindow(g_hOverlay,SW_HIDE);
    RenderAll();
    SetStatusMsg(S_RESETZONE);
    InvalidateRect(g_hMain,NULL,FALSE);
}

/* ================= overlay ================= */
static LRESULT CALLBACK OverlayProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    switch(m){
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        HBRUSH k=CreateSolidBrush(RGB(255,0,255));
        FillRect(dc,&rc,k); DeleteObject(k);
        RECT lr=g_selRectScreen;
        OffsetRect(&lr,-g_overlayOrigin.x,-g_overlayOrigin.y);
        HPEN p=CreatePen(PS_SOLID,2,g_frameColor);
        HGDIOBJ op=SelectObject(dc,p),obr=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Rectangle(dc,lr.left,lr.top,lr.right,lr.bottom);

        /* zone reellement mesuree (bandes de fond retirees) : repere blanc
           discret en pointilles. Masque pendant le glisser : les bandes de la
           capture precedente ne correspondent pas encore au rectangle en cours. */
        if(!g_dragging&&(g_barT||g_barB||g_barL||g_barR)){
            HPEN mp=CreatePen(PS_DOT,1,RGB(225,225,225));
            HGDIOBJ om=SelectObject(dc,mp);
            Rectangle(dc,lr.left+g_barL,lr.top+g_barT,lr.right-g_barR,lr.bottom-g_barB);
            SelectObject(dc,om); DeleteObject(mp);
        }
        SelectObject(dc,obr); SelectObject(dc,op); DeleteObject(p);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(h,m,w,l);
}

/* ================= fenetres de scope detachees ================= */
static LRESULT CALLBACK ScopeWndProc(HWND h,UINT m,WPARAM wp,LPARAM lp)
{
    /* l'index est transmis par lpCreateParams et lu des WM_NCCREATE : les
       messages envoyes PENDANT CreateWindow (WM_NCCALCSIZE, WM_SIZE...)
       doivent deja voir le bon scope, sinon ils manipulent le vectorscope. */
    if(m==WM_NCCREATE){
        CREATESTRUCT*cs=(CREATESTRUCT*)lp;
        SetWindowLongPtr(h,GWLP_USERDATA,(LONG_PTR)cs->lpCreateParams);
        return DefWindowProc(h,m,wp,lp);
    }

    int idx=(int)(INT_PTR)GetWindowLongPtr(h,GWLP_USERDATA);
    /* une fenetre detachee ne porte jamais le vectorscope */
    if(idx<SC_WAVE||idx>=SC_COUNT) return DefWindowProc(h,m,wp,lp);

    switch(m){
    case WM_NCCALCSIZE:
        /* supprime toute zone non-client : plus de cadre systeme, donc plus de
           bande blanche ; la fenetre est entierement peinte par nos soins */
        if(wp) return 0;
        break;
    case WM_NCHITTEST:{
        POINT p={(short)LOWORD(lp),(short)HIWORD(lp)};
        RECT r; GetWindowRect(h,&r);
        int x=p.x-r.left,y=p.y-r.top;
        int w=r.right-r.left,hh=r.bottom-r.top;
        BOOL L=(x<RESIZE_EDGE),R=(x>=w-RESIZE_EDGE);
        BOOL T=(y<RESIZE_EDGE),B=(y>=hh-RESIZE_EDGE);
        if(T&&L) return HTTOPLEFT;
        if(T&&R) return HTTOPRIGHT;
        if(B&&L) return HTBOTTOMLEFT;
        if(B&&R) return HTBOTTOMRIGHT;
        if(L) return HTLEFT;
        if(R) return HTRIGHT;
        if(T) return HTTOP;
        if(B) return HTBOTTOM;
        return HTCAPTION;   /* le corps deplace la fenetre */
    }
    case WM_MOVE:{
        /* la place est memoriee a chaque deplacement : c'est elle que l'ini
           retrouvera au prochain lancement, fenetre fermee ou pas */
        RECT r; if(GetWindowRect(h,&r)){ g_sc[idx].wr=r; g_sc[idx].wrValid=TRUE; }
        return 0;
    }
    case WM_SIZE:{
        RECT rc; GetClientRect(h,&rc);
        RECT r; if(GetWindowRect(h,&r)){ g_sc[idx].wr=r; g_sc[idx].wrValid=TRUE; }
        if(rc.right>2&&rc.bottom>2){
            EnsureScopeBmp(idx,rc.right,rc.bottom);
            RenderScope(idx);
            InvalidateRect(h,NULL,FALSE);
        }
        return 0;
    }
    case WM_EXITSIZEMOVE:
        SaveSettings();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);

        /* on remplit TOUT le client avant le blit : aucun pixel non peint */
        HBRUSH b=CreateSolidBrush(UI_BG);
        FillRect(dc,&rc,b); DeleteObject(b);

        if(g_sc[idx].bits)
            BitBlt(dc,0,0,g_sc[idx].w,g_sc[idx].h,g_sc[idx].dc,0,0,SRCCOPY);

        HPEN p=CreatePen(PS_SOLID,1,RGB(70,70,76));
        HGDIOBJ op=SelectObject(dc,p),ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Rectangle(dc,0,0,rc.right,rc.bottom);
        SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(p);

        EndPaint(h,&ps);
        return 0;
    }
    case WM_CLOSE:
        g_sc[idx].on=FALSE;
        ApplySplitMode();
        FitWindowToContent(g_hMain);
        RecomputeLayout(g_hMain);
        return 0;
    case WM_DESTROY:
        g_sc[idx].wnd=NULL;
        return 0;
    }
    return DefWindowProc(h,m,wp,lp);
}

/* ================= bitmaps ================= */
static void FreeScopeBmp(int idx)
{
    Scope*s=&g_sc[idx];
    if(s->bmp){ DeleteObject(s->bmp); s->bmp=NULL; }
    free(s->tpl); s->tpl=NULL;
    s->bits=NULL; s->w=0; s->h=0;
}

static void EnsureScopeBmp(int idx,int w,int h)
{
    Scope*s=&g_sc[idx];
    if(w<8) w=8;
    if(h<8) h=8;
    if(s->bits&&s->w==w&&s->h==h) return;

    BITMAPINFO bi; memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w;
    bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    void*nb=NULL;
    HBITMAP nbm=CreateDIBSection(s->dc,&bi,DIB_RGB_COLORS,&nb,NULL,0);
    SelectObject(s->dc,nbm);
    FreeScopeBmp(idx);
    s->bmp=nbm; s->bits=(DWORD*)nb; s->w=w; s->h=h;
    s->tpl=(DWORD*)malloc((size_t)w*h*sizeof(DWORD));
    RebuildTemplate(idx);
}

/* ================= split ================= */
static void ApplySplitMode(void)
{
    for(int i=SC_WAVE;i<SC_COUNT;i++){
        Scope*s=&g_sc[i];
        BOOL want=(g_split&&s->on);
        if(want&&!s->wnd){
            RECT mr; GetWindowRect(g_hMain,&mr);
            int x=mr.right+UI(12),y=mr.top+(i-SC_WAVE)*(UI(340)+UI(14));
            int cx=UI(560),cy=UI(340);
            if(s->wrValid){                 /* place et taille gardees dans l'ini */
                x=s->wr.left; y=s->wr.top;
                cx=s->wr.right-s->wr.left; cy=s->wr.bottom-s->wr.top;
            }
            s->wnd=CreateWindowExA(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
                SCOPEW_CLASS,"",WS_POPUP|WS_THICKFRAME,
                x,y,cx,cy,NULL,NULL,g_inst,(LPVOID)(INT_PTR)i);
            /* force le recalcul du cadre supprime par WM_NCCALCSIZE */
            SetWindowPos(s->wnd,NULL,0,0,0,0,
                SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED|SWP_NOACTIVATE);
            ShowWindow(s->wnd,SW_SHOWNOACTIVATE);
        } else if(!want&&s->wnd){
            HWND w=s->wnd;
            s->wnd=NULL;
            DestroyWindow(w);
        }
    }
}

/* ================= preferences ================= */
static const int MOD_VKS[4]={VK_MENU,VK_CONTROL,VK_SHIFT,VK_LWIN};
static const int MOD_STR[4]={S_ALT,S_CTRL,S_SHIFT,S_WIN};
static const int PAUSE_VKS[3]={VK_ESCAPE,VK_F2,VK_F3};

static const wchar_t*PauseLabel(int i)
{ return (i==0)?Tr(S_ESC):(i==1?L"F2":L"F3"); }

static void ApplySlider(int x)
{
    double t=(double)(x-g_sliderRc.left)/(double)(g_sliderRc.right-g_sliderRc.left);
    if(t<0)t=0;
    if(t>1)t=1;
    g_brightness=0.25+t*2.75;
    RenderAll();
}

static void RadioAA(DWORD*buf,int W,int H,HDC dc,RECT r,BOOL sel,const wchar_t*label)
{
    double rad=UI(5);
    double cx=r.left+UI(9),cy=r.top+UI(11);
    if(sel){
        AARing(buf,W,H,cx,cy,rad,1.4,120,220,255,1.0);
        AADisc(buf,W,H,cx,cy,rad*0.52,120,220,255,1.0);
    } else AARing(buf,W,H,cx,cy,rad,1.2,125,125,132,1.0);
    SetTextColor(dc,sel?RGB(120,220,255):RGB(215,215,220));
    DrawTxtW(dc,r.left+UI(22),r.top+UI(2),label);
}

static void CheckAA(DWORD*buf,int W,int H,HDC dc,RECT r,BOOL on,const wchar_t*label)
{
    double half=UI(6);
    double cx=r.left+UI(9),cy=r.top+UI(10);
    if(on){
        AASquare(buf,W,H,(int)cx,(int)cy,(int)half,40,110,170,1.0);
        AARing(buf,W,H,cx,cy,half+1.6,1.0,90,180,255,0.6);
        AALine(buf,W,H,cx-half*0.53,cy+half*0.03,cx-half*0.13,cy+half*0.47,1.8,170,230,255,1.0);
        AALine(buf,W,H,cx-half*0.13,cy+half*0.47,cx+half*0.57,cy-half*0.50,1.8,170,230,255,1.0);
    } else {
        AASquare(buf,W,H,(int)cx,(int)cy,(int)half,52,52,58,1.0);
        AARing(buf,W,H,cx,cy,half+1.6,1.0,125,125,132,0.9);
    }
    SetTextColor(dc,RGB(215,215,220));
    DrawTxtW(dc,r.left+UI(24),r.top+UI(2),label);
}

static BOOL EnsurePopupBuf(HDC ref,int W,int H)
{
    if(!g_pfDC) g_pfDC=CreateCompatibleDC(ref);
    if(g_pfBits&&g_pfW==W&&g_pfH==H) return TRUE;
    if(g_pfBmp){ DeleteObject(g_pfBmp); g_pfBmp=NULL; }
    BITMAPINFO bi; memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=W;
    bi.bmiHeader.biHeight=-H;
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    void*nb=NULL;
    g_pfBmp=CreateDIBSection(g_pfDC,&bi,DIB_RGB_COLORS,&nb,NULL,0);
    SelectObject(g_pfDC,g_pfBmp);
    g_pfBits=(DWORD*)nb; g_pfW=W; g_pfH=H;
    return g_pfBits!=NULL;
}

static void PopupBackground(int W,int H)
{
    for(int i=0;i<W*H;i++) g_pfBits[i]=0x00262629;
    for(int x=0;x<W;x++){ g_pfBits[x]=0x005F5F64; g_pfBits[(H-1)*W+x]=0x005F5F64; }
    for(int y=0;y<H;y++){ g_pfBits[y*W]=0x005F5F64; g_pfBits[y*W+W-1]=0x005F5F64; }
}

static LRESULT CALLBACK PrefsProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    switch(m){
    case WM_CREATE:{
        int y=UI(32);
        for(int i=0;i<4;i++){ SetRect(&g_modRc[i],UI(14),y,UI(236),y+UI(21)); y+=UI(21); }
        y+=UI(26);
        for(int i=0;i<3;i++){ SetRect(&g_pauseRc[i],UI(14),y,UI(236),y+UI(21)); y+=UI(21); }
        y+=UI(26);
        for(int i=0;i<2;i++){ SetRect(&g_wfModeRc[i],UI(14),y,UI(236),y+UI(21)); y+=UI(21); }
        y+=UI(14);
        SetRect(&g_skinRc,UI(14),y,UI(236),y+UI(21));   y+=UI(23);
        SetRect(&g_rgbCurRc,UI(14),y,UI(236),y+UI(21)); y+=UI(23);
        SetRect(&g_clipRc,UI(14),y,UI(236),y+UI(21));   y+=UI(23);
        SetRect(&g_barsRc,UI(14),y,UI(236),y+UI(21));  y+=UI(23);
        SetRect(&g_hdrRc, UI(14),y,UI(236),y+UI(21));  y+=UI(40);
        for(int i=0;i<6;i++) SetRect(&g_swatchRc[i],UI(16+i*35),y,UI(16+i*35+26),y+UI(26));
        y+=UI(54);
        SetRect(&g_sliderRc,UI(18),y,UI(228),y+UI(8));
        y+=UI(40);
        for(int i=0;i<2;i++){ SetRect(&g_langRc[i],UI(14),y,UI(236),y+UI(21)); y+=UI(21); }
        return 0;
    }
    case WM_LBUTTONDOWN:{
        POINT p={(short)LOWORD(l),(short)HIWORD(l)};
        for(int i=0;i<4;i++) if(PtInRect(&g_modRc[i],p)){ g_modVK=MOD_VKS[i]; SaveSettings(); InvalidateRect(h,NULL,TRUE); return 0; }
        for(int i=0;i<3;i++) if(PtInRect(&g_pauseRc[i],p)){ g_pauseVK=PAUSE_VKS[i]; SaveSettings(); InvalidateRect(h,NULL,TRUE); return 0; }
        for(int i=0;i<2;i++) if(PtInRect(&g_wfModeRc[i],p)){
            g_wfRGB=(i==1);
            RebuildTemplate(SC_WAVE); RenderAll();
            SaveSettings();
            InvalidateRect(h,NULL,TRUE); return 0;
        }
        if(PtInRect(&g_skinRc,p)){ g_showSkin=!g_showSkin; RebuildTemplate(SC_VEC); RenderAll(); SaveSettings(); InvalidateRect(h,NULL,TRUE); return 0; }
        if(PtInRect(&g_rgbCurRc,p)){ g_rgbAtCursor=!g_rgbAtCursor; InvalidateRect(g_hMain,NULL,FALSE); SaveSettings(); InvalidateRect(h,NULL,TRUE); return 0; }
        if(PtInRect(&g_clipRc,p)){ g_clipping=!g_clipping; RenderAll(); SaveSettings(); InvalidateRect(h,NULL,TRUE); return 0; }
        if(PtInRect(&g_barsRc,p)){
            g_ignoreBars=!g_ignoreBars;
            if(g_hasSelection&&!g_paused) Capture(g_selRectScreen,TRUE);
            SaveSettings();
            InvalidateRect(h,NULL,TRUE); return 0;
        }
        if(PtInRect(&g_hdrRc,p)){
            g_hdrNorm=!g_hdrNorm;
            if(g_hasSelection&&!g_paused) Capture(g_selRectScreen,TRUE);
            SaveSettings();
            InvalidateRect(h,NULL,TRUE); return 0;
        }
        for(int i=0;i<6;i++) if(PtInRect(&g_swatchRc[i],p)){
            g_frameColor=g_swatch[i];
            if(g_hOverlay) InvalidateRect(g_hOverlay,NULL,FALSE);
            SaveSettings();
            InvalidateRect(h,NULL,TRUE); return 0;
        }
        for(int i=0;i<2;i++) if(PtInRect(&g_langRc[i],p)){
            if(g_lang!=i){ g_lang=i; ApplyLanguage(); }
            return 0;
        }
        RECT hit=g_sliderRc; InflateRect(&hit,UI(8),UI(10));
        if(PtInRect(&hit,p)){ g_sliderDrag=TRUE; SetCapture(h); ApplySlider(p.x); InvalidateRect(h,NULL,TRUE); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if(g_sliderDrag){ ApplySlider((short)LOWORD(l)); InvalidateRect(h,NULL,TRUE); }
        return 0;
    case WM_LBUTTONUP:
        if(g_sliderDrag){ g_sliderDrag=FALSE; ReleaseCapture(); SaveSettings(); }
        return 0;
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE&&!g_sliderDrag) DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        int W=rc.right,H=rc.bottom;
        if(W<1||H<1||!EnsurePopupBuf(dc,W,H)){ EndPaint(h,&ps); return 0; }
        PopupBackground(W,H);
        SetBkMode(g_pfDC,TRANSPARENT);

        SelectObject(g_pfDC,g_hFontBold);
        SetTextColor(g_pfDC,RGB(145,145,152));
        DrawTxtW(g_pfDC,UI(14),UI(10),Tr(S_HSEL));
        SelectObject(g_pfDC,g_hFont);
        for(int i=0;i<4;i++) RadioAA(g_pfBits,W,H,g_pfDC,g_modRc[i],g_modVK==MOD_VKS[i],Tr(MOD_STR[i]));

        SelectObject(g_pfDC,g_hFontBold);
        SetTextColor(g_pfDC,RGB(145,145,152));
        DrawTxtW(g_pfDC,UI(14),g_pauseRc[0].top-UI(19),Tr(S_HPAUSE));
        SelectObject(g_pfDC,g_hFont);
        for(int i=0;i<3;i++) RadioAA(g_pfBits,W,H,g_pfDC,g_pauseRc[i],g_pauseVK==PAUSE_VKS[i],PauseLabel(i));

        SelectObject(g_pfDC,g_hFontBold);
        SetTextColor(g_pfDC,RGB(145,145,152));
        DrawTxtW(g_pfDC,UI(14),g_wfModeRc[0].top-UI(19),Tr(S_HWAVE));
        SelectObject(g_pfDC,g_hFont);
        for(int i=0;i<2;i++) RadioAA(g_pfBits,W,H,g_pfDC,g_wfModeRc[i],(g_wfRGB?1:0)==i,
                                     Tr(i==0?S_LUMA:S_PARADEM));

        CheckAA(g_pfBits,W,H,g_pfDC,g_skinRc,  g_showSkin,   Tr(S_CKSKIN));
        CheckAA(g_pfBits,W,H,g_pfDC,g_rgbCurRc,g_rgbAtCursor,Tr(S_CKRGB));
        CheckAA(g_pfBits,W,H,g_pfDC,g_clipRc,  g_clipping,   Tr(S_CKCLIP));
        CheckAA(g_pfBits,W,H,g_pfDC,g_barsRc,  g_ignoreBars, Tr(S_CKBARS));
        CheckAA(g_pfBits,W,H,g_pfDC,g_hdrRc,   g_hdrNorm,    Tr(S_CKHDR));

        SelectObject(g_pfDC,g_hFontBold);
        SetTextColor(g_pfDC,RGB(145,145,152));
        DrawTxtW(g_pfDC,UI(14),g_swatchRc[0].top-UI(19),Tr(S_HCOLOR));
        SelectObject(g_pfDC,g_hFont);
        for(int i=0;i<6;i++){
            RECT s=g_swatchRc[i];
            int scx=(s.left+s.right)/2,scy=(s.top+s.bottom)/2;
            int shalf=(s.right-s.left)/2-2;
            AASquare(g_pfBits,W,H,scx,scy,shalf,
                GetRValue(g_swatch[i]),GetGValue(g_swatch[i]),GetBValue(g_swatch[i]),1.0);
            if(g_swatch[i]==g_frameColor)
                AARing(g_pfBits,W,H,scx,scy,shalf+3.0,2.0,90,180,255,1.0);
        }

        SelectObject(g_pfDC,g_hFontBold);
        SetTextColor(g_pfDC,RGB(145,145,152));
        { wchar_t t[64]; swprintf(t,64,Tr(S_HBRIGHT),g_brightness);
          DrawTxtW(g_pfDC,UI(14),g_sliderRc.top-UI(23),t); }
        SelectObject(g_pfDC,g_hFont);
        {
            double ty=(g_sliderRc.top+g_sliderRc.bottom)/2.0;
            double x0=g_sliderRc.left,x1=g_sliderRc.right;
            double t=(g_brightness-0.25)/2.75;
            double kx=x0+t*(x1-x0);
            AALine(g_pfBits,W,H,x0,ty,x1,ty,6.0,66,66,72,1.0);
            AALine(g_pfBits,W,H,x0,ty,kx,ty,6.0,110,180,240,1.0);
            AADisc(g_pfBits,W,H,kx,ty,7.5,235,235,240,1.0);
            AARing(g_pfBits,W,H,kx,ty,7.5,1.2,150,150,158,0.8);
        }

        SelectObject(g_pfDC,g_hFontBold);
        SetTextColor(g_pfDC,RGB(145,145,152));
        DrawTxtW(g_pfDC,UI(14),g_langRc[0].top-UI(19),Tr(S_HLANG));
        SelectObject(g_pfDC,g_hFont);
        for(int i=0;i<2;i++) RadioAA(g_pfBits,W,H,g_pfDC,g_langRc[i],g_lang==i,
                                     Tr(i==0?S_EN_LBL:S_ZH_LBL));

        BitBlt(dc,0,0,W,H,g_pfDC,0,0,SRCCOPY);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_DESTROY:
        g_hPrefs=NULL;
        InvalidateRect(g_hMain,NULL,FALSE);
        return 0;
    }
    return DefWindowProc(h,m,w,l);
}

/* ================= menu Scopes ================= */
static LRESULT CALLBACK ScopesProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    switch(m){
    case WM_CREATE:{
        int y=UI(14);
        for(int i=SC_WAVE;i<SC_COUNT;i++){
            SetRect(&g_scChkRc[i],UI(14),y,UI(206),y+UI(21));
            y+=UI(23);
        }
        SetRect(&g_scSplitRc,UI(14),y+UI(12),UI(206),y+UI(33));
        return 0;
    }
    case WM_LBUTTONDOWN:{
        POINT p={(short)LOWORD(l),(short)HIWORD(l)};
        BOOL changed=FALSE;
        for(int i=SC_WAVE;i<SC_COUNT;i++){
            if(PtInRect(&g_scChkRc[i],p)){ g_sc[i].on=!g_sc[i].on; changed=TRUE; break; }
        }
        if(!changed&&PtInRect(&g_scSplitRc,p)){ g_split=!g_split; changed=TRUE; }
        if(changed){
            ApplySplitMode();
            FitWindowToContent(g_hMain);
            RecomputeLayout(g_hMain);
            SaveSettings();
            InvalidateRect(h,NULL,TRUE);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE) DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        int W=rc.right,H=rc.bottom;
        if(W<1||H<1||!EnsurePopupBuf(dc,W,H)){ EndPaint(h,&ps); return 0; }
        PopupBackground(W,H);
        SetBkMode(g_pfDC,TRANSPARENT);
        SelectObject(g_pfDC,g_hFont);

        for(int i=SC_WAVE;i<SC_COUNT;i++)
            CheckAA(g_pfBits,W,H,g_pfDC,g_scChkRc[i],g_sc[i].on,Tr(SC_STR[i]));

        int sy=g_scSplitRc.top-UI(7);
        AALine(g_pfBits,W,H,UI(12),sy,W-UI(12),sy,1.0,90,90,96,0.6);
        CheckAA(g_pfBits,W,H,g_pfDC,g_scSplitRc,g_split,Tr(S_SPLIT));

        BitBlt(dc,0,0,W,H,g_pfDC,0,0,SRCCOPY);
        EndPaint(h,&ps);
        return 0;
    }
    case WM_DESTROY:
        g_hScopes=NULL;
        InvalidateRect(g_hMain,NULL,FALSE);
        return 0;
    }
    return DefWindowProc(h,m,w,l);
}

static void OpenPopup(HWND owner,const char*cls,HWND*slot,int mi,int w,int h)
{
    if(*slot){ DestroyWindow(*slot); *slot=NULL; return; }
    POINT p={g_miRect[mi].left,g_miRect[mi].bottom+2};
    ClientToScreen(owner,&p);
    *slot=CreateWindowExA(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,cls,"",
        WS_POPUP,p.x,p.y,w,h,owner,NULL,g_inst,NULL);
    ShowWindow(*slot,SW_SHOW);
    SetForegroundWindow(*slot);
}

/* ================= barre de menu ================= */
static const wchar_t*MenuLabel(int i)
{
    switch(i){
    case MI_ONOFF: return (g_hasSelection&&!g_paused)?Tr(S_ON):Tr(S_OFF);
    case MI_SCOPES:return Tr(S_SCOPES);
    case MI_PREFS: return Tr(S_PREFS);
    case MI_RESET: return Tr(S_RESET);
    }
    return L"";
}

static void DrawTopBar(HDC dc,RECT client)
{
    RECT bar={0,0,client.right,MENUBAR_H};
    HBRUSH bg=CreateSolidBrush(UI_BG);
    FillRect(dc,&bar,bg); DeleteObject(bg);
    HPEN sep=CreatePen(PS_SOLID,1,RGB(58,58,63));
    HGDIOBJ op=SelectObject(dc,sep);
    MoveToEx(dc,0,MENUBAR_H-1,NULL); LineTo(dc,client.right,MENUBAR_H-1);
    SelectObject(dc,op); DeleteObject(sep);

    HGDIOBJ of=SelectObject(dc,g_hFont);
    SetBkMode(dc,TRANSPARENT);
    for(int i=0;i<MI_COUNT;i++){
        RECT r=g_miRect[i];
        BOOL hov=(g_hoverMI==i),dim=FALSE;
        COLORREF txt=RGB(214,214,220);
        if(i==MI_ONOFF){
            if(!g_hasSelection){ dim=TRUE; txt=RGB(108,108,114); }
            else txt=g_paused?RGB(224,180,110):RGB(110,232,150);
        }
        if(i==MI_RESET&&!g_hasSelection){ dim=TRUE; txt=RGB(108,108,114); }
        if(i==MI_PREFS&&g_hPrefs) hov=TRUE;
        if(i==MI_SCOPES&&g_hScopes) hov=TRUE;

        if(hov&&!dim){
            HBRUSH hb=CreateSolidBrush(RGB(52,52,58));
            FillRect(dc,&r,hb);
            DeleteObject(hb);
        }
        SetTextColor(dc,txt);
        DrawTextW(dc,MenuLabel(i),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    if(of) SelectObject(dc,of);
}

static int MenuHit(POINT p)
{
    for(int i=0;i<MI_COUNT;i++) if(PtInRect(&g_miRect[i],p)) return i;
    return -1;
}

/* ================= layout : 2 scopes max par ligne ================= */
static int VisibleScopes(int*list)
{
    int n=0;
    list[n++]=SC_VEC;
    if(!g_split){
        for(int i=SC_WAVE;i<SC_COUNT;i++)
            if(g_sc[i].on) list[n++]=i;
    }
    return n;
}

static void FitWindowToContent(HWND hwnd)
{
    RECT cr; GetClientRect(hwnd,&cr);
    RECT wr; GetWindowRect(hwnd,&wr);
    int chromeW=(wr.right-wr.left)-(cr.right-cr.left);
    int chromeH=(wr.bottom-wr.top)-(cr.bottom-cr.top);

    int list[SC_COUNT];
    int n=VisibleScopes(list);
    int rows=(n+1)/2;
    int cols=(n>1)?2:1;

    int cell=UI(380);   /* taille de reference d'une cellule */
    int contentW=cols*cell+(cols-1)*SPLIT_W;
    int contentH=rows*cell+(rows-1)*SPLIT_W;

    int wantW=contentW+2*MARGIN+chromeW;
    int wantH=contentH+2*MARGIN+MENUBAR_H+STATUS_H+chromeH;
    SetWindowPos(hwnd,NULL,0,0,wantW,wantH,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}

static void RecomputeLayout(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd,&rc);

    /* largeur de chaque item MESUREE dans la police courante : un libelle
       chinois et un libelle anglais n'ont pas du tout la meme taille, et une
       largeur figee rognerait l'un ou l'autre. ON/OFF est le libelle qui change
       d'etat : on retient la plus large des deux chaines pour que la barre ne
       bouge pas quand on bascule la suivi. */
    int widths[MI_COUNT];
    {
        HDC dc=GetDC(hwnd);
        HGDIOBJ of=SelectObject(dc,g_hFont);
        for(int i=0;i<MI_COUNT;i++){
            int w=0;
            if(i==MI_ONOFF){
                int a=WideW(dc,Tr(S_ON)),b=WideW(dc,Tr(S_OFF));
                w=(a>b)?a:b;
            } else w=WideW(dc,MenuLabel(i));
            widths[i]=w+UI(20);
            if(widths[i]<UI(40)) widths[i]=UI(40);
        }
        if(of) SelectObject(dc,of);
        ReleaseDC(hwnd,dc);
    }

    int x=UI(8);
    for(int i=0;i<MI_COUNT-1;i++){
        g_miRect[i].left=x; g_miRect[i].right=x+widths[i];
        g_miRect[i].top=UI(4);  g_miRect[i].bottom=MENUBAR_H-UI(5);
        x+=widths[i]+UI(6);
    }
    g_miRect[MI_RESET].right=rc.right-UI(8);
    g_miRect[MI_RESET].left=g_miRect[MI_RESET].right-widths[MI_RESET];
    g_miRect[MI_RESET].top=UI(4);
    g_miRect[MI_RESET].bottom=MENUBAR_H-5;

    int contentX=MARGIN;
    int contentY=MENUBAR_H+MARGIN;
    int contentW=(rc.right-rc.left)-2*MARGIN;
    int contentH=(rc.bottom-rc.top)-MENUBAR_H-2*MARGIN-STATUS_H;
    if(contentW<MIN_CANVAS) contentW=MIN_CANVAS;
    if(contentH<MIN_CANVAS) contentH=MIN_CANVAS;

    int list[SC_COUNT];
    int n=VisibleScopes(list);
    int rows=(n+1)/2;

    for(int r=0;r<MAX_ROWS;r++){
        SetRectEmpty(&g_rowSplit[r]);
        g_rowSplitA[r]=g_rowSplitB[r]=-1;
    }

    int rowH=(contentH-(rows-1)*SPLIT_W)/rows;
    if(rowH<MIN_CANVAS) rowH=MIN_CANVAS;

    for(int r=0;r<rows;r++){
        int i0=r*2;
        int cnt=((i0+1)<n)?2:1;      /* 2 scopes max par ligne */
        int rowY=contentY+r*(rowH+SPLIT_W);
        int usable=contentW-(cnt-1)*SPLIT_W;

        double pa=g_sc[list[i0]].prop;
        double pb=(cnt==2)?g_sc[list[i0+1]].prop:0.0;
        double sum=pa+pb;
        if(sum<=0) sum=1;

        int cx=contentX;
        for(int c=0;c<cnt;c++){
            int idx=list[i0+c];
            int colW=(c==cnt-1)?(contentX+contentW-cx)
                               :(int)(usable*pa/sum);
            if(colW<UI(80)) colW=UI(80);

            if(idx==SC_VEC){
                int size=(colW<rowH)?colW:rowH;
                if(size<MIN_CANVAS) size=MIN_CANVAS;
                if(size>MAX_CANVAS) size=MAX_CANVAS;
                EnsureScopeBmp(SC_VEC,size,size);
                int ox=cx+(colW-size)/2;
                int oy=rowY+(rowH-size)/2;
                SetRect(&g_sc[SC_VEC].rc,ox,oy,ox+size,oy+size);
            } else {
                EnsureScopeBmp(idx,colW,rowH);
                SetRect(&g_sc[idx].rc,cx,rowY,cx+colW,rowY+rowH);
            }

            cx+=colW;
            if(c==0&&cnt==2){
                SetRect(&g_rowSplit[r],cx,rowY,cx+SPLIT_W,rowY+rowH);
                g_rowSplitA[r]=list[i0];
                g_rowSplitB[r]=list[i0+1];
                cx+=SPLIT_W;
            }
        }
    }

    {
        RECT v=g_sc[SC_VEC].rc;
        HDC dc=GetDC(hwnd);
        HGDIOBJ of=SelectObject(dc,g_hFont);
        int tw=WideW(dc,Tr(S_ZOOM2));
        if(of) SelectObject(dc,of);
        ReleaseDC(hwnd,dc);
        int bw=UI(20)+tw+UI(6);       /* boite + espace + libelle mesure */
        SetRect(&g_zoomChkRc,v.left+UI(8),v.bottom-UI(26),
                           v.left+UI(8)+bw,v.bottom-UI(6));
    }

    RenderAll();
    InvalidateRect(hwnd,NULL,TRUE);
}

/* ================= fenetre principale ================= */
static void ApplyDarkTitleBar(HWND hwnd)
{
    DWORD light=1,sz=sizeof(light);
    HKEY k;
    if(RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0,KEY_READ,&k)==ERROR_SUCCESS){
        DWORD type=REG_DWORD;
        RegQueryValueExA(k,"AppsUseLightTheme",NULL,&type,(LPBYTE)&light,&sz);
        RegCloseKey(k);
    }
    BOOL dark=(light==0)?TRUE:FALSE;
    DwmSetWindowAttribute(hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
}

/* ================= polices et langue =================
   GDI ne substitue pas une famille pour une autre : rendre du chinois avec
   "Segoe UI" produit des cadres vides la ou le glyphes manque. On a donc une
   famille explicite par langue, verifiee dans le registre des polices de
   l'installation avant d'etre adoptee (repli en cascade). */
static BOOL g_fontFound=FALSE;
static int CALLBACK FontExistProc(const ENUMLOGFONTEXW*elf,const NEWTEXTMETRICEXW*nm,DWORD ft,LPCWSTR want)
{
    (void)nm; (void)ft;
    if(_wcsicmp(elf->elfLogFont.lfFaceName,want)==0){
        g_fontFound=TRUE; return FALSE;
    }
    return TRUE;
}
static BOOL FontExists(const wchar_t*face)
{
    HDC dc=GetDC(NULL);
    if(!dc) return TRUE;
    LOGFONTW lf; memset(&lf,0,sizeof(lf));
    lf.lfCharSet=DEFAULT_CHARSET;
    lstrcpyW(lf.lfFaceName,face);
    g_fontFound=FALSE;
    EnumFontFamiliesExW(dc,&lf,(FONTENUMPROCW)FontExistProc,(LPARAM)face,0);
    ReleaseDC(NULL,dc);
    return g_fontFound;
}

static void CreateFonts(void)
{
    const wchar_t*face=L"Segoe UI";
    if(g_lang==L_ZH){
        static const wchar_t*cjk[]={L"Microsoft YaHei UI",L"Microsoft YaHei",
                                    L"SimHei",L"SimSun",L"NSimSun"};
        for(int i=0;i<(int)(sizeof(cjk)/sizeof(cjk[0]));i++)
            if(FontExists(cjk[i])){ face=cjk[i]; break; }
    }
    g_hFont=CreateFontW(UI(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS,face);
    g_hFontBold=CreateFontW(UI(12),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS,face);
}
static void DestroyFonts(void)
{
    if(g_hFont){ DeleteObject(g_hFont); g_hFont=NULL; }
    if(g_hFontBold){ DeleteObject(g_hFontBold); g_hFontBold=NULL; }
}

/* Changer de langue retracte tout ce qui depend du texte : la police, les
   gabarits (les libelles sont GRAVES dans le bitmap de chaque scope) et la
   mise en page (les largeurs sont mesurees). */
static void ApplyLanguage(void)
{
    DestroyFonts();
    CreateFonts();
    RefreshStatus();                             /* le statut est fige, lui */
    for(int i=0;i<SC_COUNT;i++) RebuildTemplate(i);
    if(g_hMain) RecomputeLayout(g_hMain);   /* rend et repeint la principale */
    if(g_hOverlay) InvalidateRect(g_hOverlay,NULL,FALSE);
    if(g_hPrefs)  InvalidateRect(g_hPrefs,NULL,TRUE);
    if(g_hScopes) InvalidateRect(g_hScopes,NULL,TRUE);
    for(int i=SC_WAVE;i<SC_COUNT;i++)
        if(g_sc[i].wnd) InvalidateRect(g_sc[i].wnd,NULL,FALSE);
    SaveSettings();
}

static LRESULT CALLBACK MainProc(HWND hwnd,UINT m,WPARAM wp,LPARAM lp)
{
    switch(m){
    case WM_CREATE:{
        CreateFonts();

        ApplyDarkTitleBar(hwnd);

        for(int i=0;i<SC_COUNT;i++) g_sc[i].dc=CreateCompatibleDC(NULL);
        g_sc[SC_VEC].on=TRUE;
        /* les proportions et les options viennent de LoadSettings (ini) : ici,
           on ne touche que ce qui depend de la fenetre en cours de creation */
        RecomputeLayout(hwnd);

        int ox=GetSystemMetrics(SM_XVIRTUALSCREEN);
        int oy=GetSystemMetrics(SM_YVIRTUALSCREEN);
        int ow=GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int oh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_overlayOrigin.x=ox; g_overlayOrigin.y=oy;

        g_hOverlay=CreateWindowExA(
            WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
            OVERLAY_CLASS,"",WS_POPUP,ox,oy,ow,oh,NULL,NULL,g_inst,NULL);
        SetLayeredWindowAttributes(g_hOverlay,RGB(255,0,255),0,LWA_COLORKEY);
        SetWindowDisplayAffinity(g_hOverlay,WDA_EXCLUDEFROMCAPTURE);

        SetTimer(hwnd,1,25,NULL);
        return 0;
    }
    case WM_SETTINGCHANGE:
        ApplyDarkTitleBar(hwnd);
        return 0;
    case WM_DISPLAYCHANGE:
        /* basculer HDR/SDR ou brancher un ecran : tout est a re-apprendre */
        g_hdrTick=0;
        WgcClose(); g_wTried=0; g_sdrWhite=0.0; g_calTick=0;
        if(g_hasSelection&&!g_paused) Capture(g_selRectScreen,TRUE);
        return 0;
    case WM_GETMINMAXINFO:{
        MINMAXINFO*mi=(MINMAXINFO*)lp;
        mi->ptMinTrackSize.x=MIN_CANVAS+2*MARGIN+UI(20);
        mi->ptMinTrackSize.y=MIN_CANVAS+2*MARGIN+MENUBAR_H+STATUS_H+UI(40);
        return 0;
    }
    case WM_SIZE:
        if(g_sc[SC_VEC].dc) RecomputeLayout(hwnd);
        return 0;
    case WM_MOVE:
        if(!IsIconic(hwnd)){
            RECT r;
            if(GetWindowRect(hwnd,&r)){ g_mainRect=r; g_mainRectValid=TRUE; }
        }
        return 0;
    case WM_SETCURSOR:{
        POINT p; GetCursorPos(&p); ScreenToClient(hwnd,&p);
        for(int r=0;r<MAX_ROWS;r++)
            if(!IsRectEmpty(&g_rowSplit[r])&&PtInRect(&g_rowSplit[r],p)){
                SetCursor(LoadCursor(NULL,IDC_SIZEWE));
                return TRUE;
            }
        break;
    }
    case WM_LBUTTONDOWN:{
        POINT p={(short)LOWORD(lp),(short)HIWORD(lp)};

        for(int r=0;r<MAX_ROWS;r++){
            if(!IsRectEmpty(&g_rowSplit[r])&&PtInRect(&g_rowSplit[r],p)){
                g_splitDragRow=r;
                g_splitDragX=p.x;
                g_dragPropA=g_sc[g_rowSplitA[r]].prop;
                g_dragPropB=g_sc[g_rowSplitB[r]].prop;
                SetCapture(hwnd);
                return 0;
            }
        }

        if(PtInRect(&g_zoomChkRc,p)){
            g_zoom2=!g_zoom2;
            RenderVec();
            SaveSettings();
            InvalidateRect(hwnd,NULL,FALSE);
            return 0;
        }

        int i=MenuHit(p);
        if(i==MI_ONOFF){ if(g_hasSelection) SetTracking(g_paused); }
        else if(i==MI_RESET){ if(g_hasSelection) ResetZone(); }
        else if(i==MI_SCOPES){ OpenPopup(hwnd,SCOPES_CLASS,&g_hScopes,MI_SCOPES,UI(220),UI(140)); InvalidateRect(hwnd,NULL,FALSE); }
        else if(i==MI_PREFS){ OpenPopup(hwnd,PREFS_CLASS,&g_hPrefs,MI_PREFS,UI(252),UI(583)); InvalidateRect(hwnd,NULL,FALSE); }
        return 0;
    }
    case WM_MOUSEMOVE:{
        POINT p={(short)LOWORD(lp),(short)HIWORD(lp)};

        if(g_splitDragRow>=0){
            int r=g_splitDragRow;
            RECT rc; GetClientRect(hwnd,&rc);
            int contentW=(rc.right-rc.left)-2*MARGIN-SPLIT_W;
            double sum=g_dragPropA+g_dragPropB;
            double dprop=(double)(p.x-g_splitDragX)*sum/(double)(contentW>0?contentW:1);
            double a=g_dragPropA+dprop,b=g_dragPropB-dprop;
            if(a>0.3&&b>0.3){
                g_sc[g_rowSplitA[r]].prop=a;
                g_sc[g_rowSplitB[r]].prop=b;
                RecomputeLayout(hwnd);
            }
            return 0;
        }

        BOOL zh=PtInRect(&g_zoomChkRc,p);
        if(zh!=g_zoomHover){ g_zoomHover=zh; InvalidateRect(hwnd,NULL,FALSE); }

        int i=MenuHit(p);
        if(i!=g_hoverMI){ g_hoverMI=i; InvalidateRect(hwnd,NULL,FALSE); }
        if(!g_tracking){
            TRACKMOUSEEVENT t={sizeof(t),TME_LEAVE,hwnd,0};
            TrackMouseEvent(&t); g_tracking=TRUE;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if(g_splitDragRow>=0){ g_splitDragRow=-1; ReleaseCapture(); SaveSettings(); }
        return 0;
    case WM_EXITSIZEMOVE:
        /* deplace ou redimensionne a la souris : la place merite d'etre garde */
        SaveSettings();
        return 0;
    case WM_MOUSELEAVE:
        g_tracking=FALSE;
        if(g_hoverMI!=-1||g_zoomHover){ g_hoverMI=-1; g_zoomHover=FALSE; InvalidateRect(hwnd,NULL,FALSE); }
        return 0;
    case WM_TIMER:{
        if(++g_topmostCounter>=40){
            g_topmostCounter=0;
            SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            if(g_hasSelection&&!g_paused)
                SetWindowPos(g_hOverlay,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }

        if(g_rgbAtCursor&&++g_cursorCounter>=4){
            g_cursorCounter=0;
            POINT c; GetCursorPos(&c);
            HDC scr=GetDC(NULL);
            COLORREF col=GetPixel(scr,c.x,c.y);
            ReleaseDC(NULL,scr);
            if(col!=CLR_INVALID){
                BYTE nr=GetRValue(col),ng=GetGValue(col),nb=GetBValue(col);
                if(nr!=g_curR||ng!=g_curG||nb!=g_curB){
                    g_curR=nr; g_curG=ng; g_curB=nb;
                    RECT v=g_sc[SC_VEC].rc;
                    RECT ir={v.left,v.bottom-UI(26),v.right,v.bottom};
                    InvalidateRect(hwnd,&ir,FALSE);
                }
            }
        }

        BOOL mod=(GetAsyncKeyState(g_modVK)&0x8000)!=0;
        BOOL lb =(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
        BOOL pz =(GetAsyncKeyState(g_pauseVK)&0x8000)!=0;

        if(!g_dragging){
            if(mod&&lb){
                POINT cp; GetCursorPos(&cp);
                RECT mr; GetWindowRect(hwnd,&mr);
                if(PtInRect(&mr,cp)) return 0;
                for(int i=SC_WAVE;i<SC_COUNT;i++){
                    if(g_sc[i].wnd){
                        RECT sr; GetWindowRect(g_sc[i].wnd,&sr);
                        if(PtInRect(&sr,cp)) return 0;
                    }
                }
                g_dragging=TRUE; g_paused=FALSE;
                g_dragStart=cp;
                g_selRectScreen.left=g_selRectScreen.right=cp.x;
                g_selRectScreen.top=g_selRectScreen.bottom=cp.y;
                SetWindowPos(g_hOverlay,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                ShowWindow(g_hOverlay,SW_SHOWNOACTIVATE);
            } else if(pz&&g_hasSelection&&!g_paused){
                SetTracking(FALSE);
            } else if(g_hasSelection&&!g_paused){
                if(++g_refreshCounter>=REFRESH_TICKS){
                    g_refreshCounter=0;
                    Capture(g_selRectScreen,FALSE);
                }
            }
        } else {
            POINT c; GetCursorPos(&c);
            g_selRectScreen.left  =min(g_dragStart.x,c.x);
            g_selRectScreen.right =max(g_dragStart.x,c.x);
            g_selRectScreen.top   =min(g_dragStart.y,c.y);
            g_selRectScreen.bottom=max(g_dragStart.y,c.y);
            InvalidateRect(g_hOverlay,NULL,FALSE);

            if(!lb){
                g_dragging=FALSE;
                RECT s=g_selRectScreen;
                if((s.right-s.left)>3&&(s.bottom-s.top)>3) Capture(s,TRUE);
                else {
                    ShowWindow(g_hOverlay,SW_HIDE);
                    g_hasSelection=FALSE;
                    SetStatusMsg(S_TOOSMALL);
                    InvalidateRect(hwnd,NULL,FALSE);
                }
                g_refreshCounter=0;
            }
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);

        HBRUSH bg=CreateSolidBrush(UI_BG);
        RECT body={0,MENUBAR_H,rc.right,rc.bottom};
        FillRect(dc,&body,bg); DeleteObject(bg);

        DrawTopBar(dc,rc);

        int list[SC_COUNT];
        int n=VisibleScopes(list);
        for(int i=0;i<n;i++){
            Scope*s=&g_sc[list[i]];
            if(s->bits&&!IsRectEmpty(&s->rc))
                BitBlt(dc,s->rc.left,s->rc.top,s->w,s->h,s->dc,0,0,SRCCOPY);
        }

        for(int r=0;r<MAX_ROWS;r++){
            if(IsRectEmpty(&g_rowSplit[r])) continue;
            RECT sr=g_rowSplit[r];
            int mx=(sr.left+sr.right)/2;
            HPEN p=CreatePen(PS_SOLID,1,RGB(70,70,76));
            HGDIOBJ op=SelectObject(dc,p);
            MoveToEx(dc,mx,sr.top+8,NULL); LineTo(dc,mx,sr.bottom-8);
            SelectObject(dc,op); DeleteObject(p);
        }

        HGDIOBJ of=SelectObject(dc,g_hFont);
        SetBkMode(dc,TRANSPARENT);

        {
            RECT z=g_zoomChkRc;
            int bs=UI(14),q=bs/14; if(q<1)q=1;
            int cy0=(z.top+z.bottom)/2;
            TEXTMETRICW tm;
            { HGDIOBJ o=SelectObject(dc,g_hFont); GetTextMetricsW(dc,&tm); SelectObject(dc,o); }
            int bx=z.left+UI(2),by=cy0-bs/2;
            HPEN p=CreatePen(PS_SOLID,1,g_zoom2?RGB(90,180,255):RGB(120,120,126));
            HBRUSH b=CreateSolidBrush(g_zoom2?RGB(40,110,170):RGB(46,46,52));
            HGDIOBJ op=SelectObject(dc,p),obb=SelectObject(dc,b);
            RoundRect(dc,bx,by,bx+bs,by+bs,UI(3),UI(3));
            SelectObject(dc,obb); SelectObject(dc,op);
            DeleteObject(b); DeleteObject(p);
            if(g_zoom2){
                HPEN cp=CreatePen(PS_SOLID,2,RGB(170,230,255));
                HGDIOBJ ocp=SelectObject(dc,cp);
                MoveToEx(dc,bx+3*q,by+7*q,NULL);
                LineTo(dc,bx+6*q,by+10*q);
                LineTo(dc,bx+11*q,by+4*q);
                SelectObject(dc,ocp); DeleteObject(cp);
            }
            SetTextColor(dc,g_zoomHover?RGB(230,230,235):RGB(150,150,158));
            DrawTxtW(dc,bx+bs+UI(6),cy0-tm.tmHeight/2,Tr(S_ZOOM2));
        }

        if(g_rgbAtCursor){
            wchar_t t[48];
            swprintf(t,48,Tr(S_RGBAT),g_curR,g_curG,g_curB);
            SetTextColor(dc,RGB(135,135,142));
            RECT v=g_sc[SC_VEC].rc;
            RECT tr={v.left,v.bottom-UI(24),v.right-UI(8),v.bottom-UI(4)};
            DrawTextW(dc,t,-1,&tr,DT_RIGHT|DT_SINGLELINE);
        }

        SetTextColor(dc,RGB(148,148,155));
        RECT sr={MARGIN,rc.bottom-STATUS_H,rc.right-MARGIN,rc.bottom};
        DrawTextW(dc,g_status,-1,&sr,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        if(of) SelectObject(dc,of);

        EndPaint(hwnd,&ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd,1);
        SaveSettings();   /* derniere place et derniers reglages de la session */
        g_hMain=NULL;
        for(int i=0;i<SC_COUNT;i++){
            if(g_sc[i].wnd){ HWND w=g_sc[i].wnd; g_sc[i].wnd=NULL; DestroyWindow(w); }
            FreeScopeBmp(i);
            if(g_sc[i].dc) DeleteDC(g_sc[i].dc);
            g_sc[i].dc=NULL;
        }
        if(g_pfBmp) DeleteObject(g_pfBmp);
        if(g_pfDC) DeleteDC(g_pfDC);
        if(g_capDC){ DeleteDC(g_capDC); g_capDC=NULL; }
        if(g_capBmp){ DeleteObject(g_capBmp); g_capBmp=NULL; }
        DestroyFonts();
        if(g_hOverlay){ DestroyWindow(g_hOverlay); g_hOverlay=NULL; }
        free(g_sR); free(g_sG); free(g_sB); free(g_sCol);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd,m,wp,lp);
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE hp,LPSTR cl,int ns)
{
    (void)hp; (void)cl;

    /* A faire AVANT la premiere fenetre et le premier DC.
       Un process "DPI unaware" est virtualise par Windows : il croit son ecran
       plus petit (2560 x 1440 pour une dalle 4K a 150 %) et le DWM agrandit ses
       fenetres d'autant. Mais BitBlt sur le DC d'ecran, lui, recopie les pixels
       PHYSIQUES aux memes coordonnees (verifie : a zero pres sur toute la
       surface). Le cadre de l'overlay et la zone capturee ne designent alors
       plus la meme region de l'ecran : les scopes statistiquent a cote de la
       zone encadree, et tout repere base sur la capture tombe de traviole.
       Un process "aware" met les deux dans le meme espace, pixel pour pixel. */
    {
        HMODULE u=GetModuleHandleA("user32.dll");
        BOOL (WINAPI *setctx)(HANDLE)=u?(BOOL (WINAPI *)(HANDLE))
            GetProcAddress(u,"SetProcessDpiAwarenessContext"):NULL;
        if(!setctx || !setctx((HANDLE)-4))   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
            SetProcessDPIAware();            /* repli : Windows anterieur a 1703 */
    }
    {
        HDC scr=GetDC(NULL);
        int dpi=GetDeviceCaps(scr,LOGPIXELSX);
        ReleaseDC(NULL,scr);
        if(dpi<96) dpi=96;
        g_uiScale=(int)((double)dpi*100.0/96.0+0.5);
    }

    g_inst=hi;
    /* l'ini se lit APRES l'echelle DPI (les tailles par defaut en dependent) et
       AVANT la moindre fenetre : c'est lui qui decide de la langue, des scopes
       allumes, des proportions et de la place de chaque fenetre. */
    LoadSettings();
    SetStatusMsg(S_HINT);
    g_icon=LoadIconA(hi,MAKEINTRESOURCEA(1));
    HBRUSH darkBrush=CreateSolidBrush(UI_BG);

    WNDCLASSA wc; memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc=MainProc; wc.hInstance=hi;
    wc.lpszClassName=MAIN_CLASS;
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hIcon=g_icon;
    wc.hbrBackground=darkBrush;
    RegisterClassA(&wc);

    WNDCLASSA wo; memset(&wo,0,sizeof(wo));
    wo.lpfnWndProc=OverlayProc; wo.hInstance=hi;
    wo.lpszClassName=OVERLAY_CLASS;
    wo.hCursor=LoadCursor(NULL,IDC_CROSS);
    RegisterClassA(&wo);

    WNDCLASSA wpc; memset(&wpc,0,sizeof(wpc));
    wpc.lpfnWndProc=PrefsProc; wpc.hInstance=hi;
    wpc.lpszClassName=PREFS_CLASS;
    wpc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wpc.hbrBackground=darkBrush;
    RegisterClassA(&wpc);

    WNDCLASSA wsc; memset(&wsc,0,sizeof(wsc));
    wsc.lpfnWndProc=ScopesProc; wsc.hInstance=hi;
    wsc.lpszClassName=SCOPES_CLASS;
    wsc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wsc.hbrBackground=darkBrush;
    RegisterClassA(&wsc);

    /* fenetres de scope detachees : fond sombre des la creation, aucune
       zone non-client -> pas de bande blanche */
    WNDCLASSA wsw; memset(&wsw,0,sizeof(wsw));
    wsw.lpfnWndProc=ScopeWndProc; wsw.hInstance=hi;
    wsw.lpszClassName=SCOPEW_CLASS;
    wsw.hCursor=LoadCursor(NULL,IDC_ARROW);
    wsw.hIcon=g_icon;
    wsw.hbrBackground=darkBrush;
    RegisterClassA(&wsw);

    int w=UI(380)+2*MARGIN+UI(16);
    int h=UI(380)+2*MARGIN+MENUBAR_H+STATUS_H+UI(40);
    int x=CW_USEDEFAULT,y=CW_USEDEFAULT;
    if(g_mainRectValid){
        x=g_mainRect.left;  y=g_mainRect.top;
        w=g_mainRect.right-g_mainRect.left;
        h=g_mainRect.bottom-g_mainRect.top;
    }

    g_hMain=CreateWindowExA(WS_EX_TOPMOST,MAIN_CLASS,"ScopeWalker",
        WS_OVERLAPPEDWINDOW,x,y,w,h,
        NULL,NULL,hi,NULL);

    if(g_icon){
        SendMessage(g_hMain,WM_SETICON,ICON_BIG,(LPARAM)g_icon);
        SendMessage(g_hMain,WM_SETICON,ICON_SMALL,(LPARAM)g_icon);
    }

    ShowWindow(g_hMain,ns);
    UpdateWindow(g_hMain);
    if(g_split) ApplySplitMode();   /* les fenetres detachees reviennent aussi */

    MSG msg;
    while(GetMessage(&msg,NULL,0,0)){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    WgcClose();
    return (int)msg.wParam;
}
