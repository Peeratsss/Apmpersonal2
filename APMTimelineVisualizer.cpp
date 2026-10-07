#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cwctype>
#include <cmath>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"gdiplus.lib")
using namespace Gdiplus;

static const int GW=1200, GH=300, FPS=50;
static const COLORREF PURPLE=RGB(162,73,245);
static const COLORREF GREEN=RGB(0,255,0);
static HWND gWnd,gEdit,gLoad,gPlay,gReset,gExport;
static ULONG_PTR gdToken;
static Bitmap* img=nullptr;
static UINT iw=0,ih=0;
static LARGE_INTEGER pf{},playStart{};
static bool playing=false;
static double nowTime=0, lengthSec=.5, playOrigin=0;
static size_t eventPos=0;
struct Ev{double t;std::wstring name;bool down;};
static std::vector<Ev> evs;
static std::vector<std::wstring> held;

static std::wstring trim(std::wstring s){
 size_t a=0,b=s.size(); while(a<b&&iswspace(s[a]))a++; while(b>a&&iswspace(s[b-1]))b--;
 return s.substr(a,b-a);
}
static std::wstring upper(std::wstring s){for(auto&c:s)c=towupper(c);return s;}
static std::wstring exeDir(){
 wchar_t p[MAX_PATH]{}; GetModuleFileNameW(nullptr,p,MAX_PATH); std::wstring s=p;
 size_t x=s.find_last_of(L"\\/"); if(x!=std::wstring::npos)s.resize(x); return s;
}
static std::wstring imagePath(){return exeDir()+L"\\image.png";}

static bool parseTimeline(const std::wstring& text){
 std::vector<Ev> v; std::wstringstream all(text); std::wstring line; double t=0;
 while(std::getline(all,line)){
  line=trim(line); if(line.empty()||line[0]==L'#')continue;
  std::wstringstream ss(line); double ms;
  if(!(ss>>ms)||ms<0)return false;
  std::wstring n; std::getline(ss,n); n=trim(n);
  if(n.size()<2)return false;
  wchar_t sign=n.back(); if(sign!=L'+'&&sign!=L'-')return false;
  n=trim(n.substr(0,n.size()-1)); if(n.empty())return false;
  t+=ms/1000.0; v.push_back({t,n,sign==L'+'});
 }
 if(v.empty())return false;
 evs=std::move(v); lengthSec=std::max(.5,evs.back().t+.5);
 nowTime=0;eventPos=0;held.clear();return true;
}
static void setHeld(const std::wstring& n,bool down){
 std::wstring u=upper(n); auto it=std::find(held.begin(),held.end(),u);
 if(down){if(it==held.end())held.push_back(u);}
 else if(it!=held.end())held.erase(it);
}
static bool isHeld(const std::wstring& n){
 return std::find(held.begin(),held.end(),upper(n))!=held.end();
}
static void resetState(){held.clear();eventPos=0;}
static void processTo(double t){
 while(eventPos<evs.size()&&evs[eventPos].t<=t+1e-9){
  setHeld(evs[eventPos].name,evs[eventPos].down);eventPos++;
 }
}
static int apm(double t){
 int n=0;double s=t-60;
 for(auto&e:evs){if(e.t>t)break;if(e.down&&e.t>=s)n++;}
 return n;
}
static std::vector<std::wstring> last10(double t){
 std::vector<std::wstring> r;
 for(auto i=evs.rbegin();i!=evs.rend();++i){
  if(i->t>t)continue;if(!i->down)continue;r.push_back(i->name);if(r.size()==10)break;
 }
 std::reverse(r.begin(),r.end());return r;
}
static void text(HDC dc,const std::wstring&s,int l,int t,int r,int b,int size,bool bold,COLORREF c){
 HFONT f=CreateFontW(-size,0,0,0,bold?FW_BOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
  OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Arial");
 HFONT old=(HFONT)SelectObject(dc,f);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,c);
 RECT q{l,t,r,b};DrawTextW(dc,s.c_str(),-1,&q,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
 SelectObject(dc,old);DeleteObject(f);
}
static bool loadImage(){
 if(img)return true;
 img=new Bitmap(imagePath().c_str(),FALSE);
 if(!img||img->GetLastStatus()!=Ok){delete img;img=nullptr;return false;}
 iw=img->GetWidth();ih=img->GetHeight();return iw&&ih;
}

struct R{const wchar_t*n;float l,t,r,b;};
#define K(n,l,t,r,b) {L##n,l,t,r,b}
static const R regions[] =
{
 // FUNCTION ROW
 K("ESC",       .030f,.250f,.065f,.350f),
 K("F1",        .075f,.250f,.110f,.350f),
 K("F2",        .115f,.250f,.150f,.350f),
 K("F3",        .155f,.250f,.190f,.350f),
 K("F4",        .195f,.250f,.230f,.350f),
 K("F5",        .235f,.250f,.270f,.350f),
 K("F6",        .275f,.250f,.310f,.350f),
 K("F7",        .315f,.250f,.350f,.350f),
 K("F8",        .355f,.250f,.390f,.350f),
 K("F9",        .395f,.250f,.430f,.350f),
 K("F10",       .435f,.250f,.475f,.350f),
 K("F11",       .480f,.250f,.520f,.350f),
 K("F12",       .525f,.250f,.565f,.350f),

 // NUMBER / SYMBOL ROW: ` 1 2 3 4 5 6 7 8 9 0 - = BACKSPACE
 K("`",         .030f,.360f,.075f,.470f),
 K("1",         .078f,.360f,.115f,.470f),
 K("2",         .118f,.360f,.155f,.470f),
 K("3",         .158f,.360f,.195f,.470f),
 K("4",         .198f,.360f,.235f,.470f),
 K("5",         .238f,.360f,.275f,.470f),
 K("6",         .278f,.360f,.315f,.470f),
 K("7",         .318f,.360f,.355f,.470f),
 K("8",         .358f,.360f,.395f,.470f),
 K("9",         .398f,.360f,.435f,.470f),
 K("0",         .438f,.360f,.475f,.470f),
 K("-",         .478f,.360f,.515f,.470f),
 K("=",         .518f,.360f,.555f,.470f),
 K("BACKSPACE", .558f,.360f,.650f,.470f),

 // QWERTY ROW: TAB Q W E R T Y U I O P [ ] \
 K("TAB",       .030f,.480f,.090f,.590f),
 K("Q",         .093f,.480f,.130f,.590f),
 K("W",         .133f,.480f,.170f,.590f),
 K("E",         .173f,.480f,.210f,.590f),
 K("R",         .213f,.480f,.250f,.590f),
 K("T",         .253f,.480f,.290f,.590f),
 K("Y",         .293f,.480f,.330f,.590f),
 K("U",         .333f,.480f,.370f,.590f),
 K("I",         .373f,.480f,.410f,.590f),
 K("O",         .413f,.480f,.450f,.590f),
 K("P",         .453f,.480f,.490f,.590f),
 K("[",         .493f,.480f,.530f,.590f),
 K("]",         .533f,.480f,.570f,.590f),
 K("\\",        .573f,.480f,.650f,.590f),

 // HOME ROW: CAPS A S D F G H J K L ; ' ENTER
 K("CAPS",       .030f,.600f,.100f,.710f),
 K("A",          .103f,.600f,.140f,.710f),
 K("S",          .143f,.600f,.180f,.710f),
 K("D",          .183f,.600f,.220f,.710f),
 K("F",          .223f,.600f,.260f,.710f),
 K("G",          .263f,.600f,.300f,.710f),
 K("H",          .303f,.600f,.340f,.710f),
 K("J",          .343f,.600f,.380f,.710f),
 K("K",          .383f,.600f,.420f,.710f),
 K("L",          .423f,.600f,.460f,.710f),
 K(";",          .463f,.600f,.500f,.710f),
 K("'",          .503f,.600f,.540f,.710f),
 K("ENTER",      .543f,.600f,.650f,.710f),

 // SHIFT ROW: SHIFT Z X C V B N M , . / SHIFT
 K("SHIFT",      .030f,.720f,.115f,.830f),
 K("Z",          .118f,.720f,.155f,.830f),
 K("X",          .158f,.720f,.195f,.830f),
 K("C",          .198f,.720f,.235f,.830f),
 K("V",          .238f,.720f,.275f,.830f),
 K("B",          .278f,.720f,.315f,.830f),
 K("N",          .318f,.720f,.355f,.830f),
 K("M",          .358f,.720f,.395f,.830f),
 K(",",          .398f,.720f,.435f,.830f),
 K(".",          .438f,.720f,.475f,.830f),
 K("/",          .478f,.720f,.515f,.830f),
 K("RSHIFT",     .518f,.720f,.650f,.830f),

 // CONTROL ROW: CTRL WIN ALT SPACE ALT CTRL
 K("CTRL",       .030f,.840f,.085f,.950f),
 K("WIN",        .088f,.840f,.128f,.950f),
 K("ALT",        .131f,.840f,.171f,.950f),
 K("SPACE",      .174f,.840f,.445f,.950f),
 K("RALT",       .448f,.840f,.488f,.950f),
 K("RCTRL",      .491f,.840f,.550f,.950f),

 // ARROW KEYS — separate cluster to the right of CTRL/SPACE area
 K("UP",         .570f,.755f,.610f,.815f),
 K("LEFT",       .525f,.825f,.565f,.895f),
 K("DOWN",       .570f,.825f,.610f,.895f),
 K("RIGHT",      .615f,.825f,.655f,.895f),

 // MOUSE — right side: two buttons, wheel, two side buttons
 K("LMB",        .760f,.300f,.845f,.520f),
 K("RMB",        .845f,.300f,.930f,.520f),
 K("MMB",        .800f,.520f,.890f,.680f),
 K("X1",         .735f,.500f,.780f,.620f),
 K("X2",         .910f,.500f,.955f,.620f)
};
#undef K

static const R* regionFor(std::wstring n){
 n=upper(trim(n));
 if(n==L"ESCAPE")n=L"ESC";if(n==L"RETURN")n=L"ENTER";if(n==L"BACK")n=L"BACKSPACE";
 if(n==L"LEFT SHIFT")n=L"SHIFT";if(n==L"RIGHT SHIFT")n=L"RSHIFT";
 if(n==L"LEFT CTRL")n=L"CTRL";if(n==L"RIGHT CTRL")n=L"RCTRL";
 if(n==L"LEFT ALT")n=L"ALT";if(n==L"RIGHT ALT")n=L"RALT";
 if(n==L"ARROWUP")n=L"UP";if(n==L"ARROWDOWN")n=L"DOWN";
 if(n==L"ARROWLEFT")n=L"LEFT";if(n==L"ARROWRIGHT")n=L"RIGHT";
 for(auto&r:regions)if(n==upper(r.n))return&r;
 return nullptr;
}
static bool green(BYTE r,BYTE g,BYTE b){
 return g>100&&g>(BYTE)std::min(255,(int)(r*1.25))&&g>(BYTE)std::min(255,(int)(b*1.25));
}
static void pressPixels(HDC dc,int w,int h,const R&r){
 int l=std::max(0,(int)(r.l*w)),t=std::max(0,(int)(r.t*h));
 int rr=std::min(w,(int)(r.r*w)),bb=std::min(h,(int)(r.b*h));
 for(int y=t;y<bb;y++)for(int x=l;x<rr;x++){
  COLORREF c=GetPixel(dc,x,y);if(c==CLR_INVALID)continue;
  BYTE R8=GetRValue(c),G8=GetGValue(c),B8=GetBValue(c);
  if(green(R8,G8,B8))continue;
  if((int)R8+(int)G8+(int)B8>=500)SetPixelV(dc,x,y,PURPLE);
 }
}
static bool regionHeld(const R& r){
 for(const auto& n:held)if(regionFor(n)==&r)return true;
 return false;
}
static void drawPressed(HDC dc,int w,int h){
 for(auto&r:regions)if(regionHeld(r))pressPixels(dc,w,h,r);
}
static void dynamicInfo(HDC dc,int w,int h){
 wchar_t b[32]{};swprintf_s(b,L"%d",apm(nowTime));
 text(dc,b,(int)(w*.485),(int)(h*.365),(int)(w*.570),(int)(h*.500),24,true,RGB(0,0,0));
 auto a=last10(nowTime);int y=(int)(h*.535);
 for(size_t i=0;i<a.size();i++)text(dc,a[i],(int)(w*.495),y+(int)i*20,(int)(w*.675),y+(int)(i+1)*20,13,true,RGB(0,0,0));
}
static void renderFinal(HDC dc,int w,int h){
 if(!loadImage()){
  HBRUSH q=CreateSolidBrush(GREEN);RECT r{0,0,w,h};FillRect(dc,&r,q);DeleteObject(q);
  text(dc,L"image.png NOT FOUND",0,0,w,h,30,true,RGB(0,0,0));return;
 }
 Graphics g(dc);g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
 g.DrawImage(img,Rect(0,0,w,h),0,0,(INT)iw,(INT)ih,UnitPixel);
 drawPressed(dc,w,h);dynamicInfo(dc,w,h);
}
static void renderPreview(HDC dc,int w,int h){
 HBRUSH q=CreateSolidBrush(RGB(18,18,22));RECT r{0,0,w,h};FillRect(dc,&r,q);DeleteObject(q);
 wchar_t b[64]{};swprintf_s(b,L"APM %d",apm(nowTime));text(dc,b,0,25,w,90,50,true,RGB(255,255,255));
 wchar_t t[64]{};swprintf_s(t,L"%02d:%05.2f",(int)(nowTime/60),fmod(nowTime,60.0));
 text(dc,t,0,90,w,130,22,false,RGB(255,255,255));
 auto a=last10(nowTime);std::wstring line;
 for(size_t i=0;i<a.size();i++){if(i)line+=L"  ";line+=a[i];}
 text(dc,line,30,140,w-30,190,18,true,RGB(255,255,255));
 int bx=40,by=h-35,bw=w-80,bh=12;HBRUSH d=CreateSolidBrush(RGB(55,55,60));
 RECT br{bx,by,bx+bw,by+bh};FillRect(dc,&br,d);DeleteObject(d);
 double p=lengthSec?nowTime/lengthSec:0;p=std::max(0.0,std::min(1.0,p));
 HBRUSH f=CreateSolidBrush(PURPLE);RECT pr{bx,by,bx+(int)(bw*p),by+bh};FillRect(dc,&pr,f);DeleteObject(f);
}
static HBITMAP dib(HDC ref,int w,int h,HDC*mem){
 *mem=CreateCompatibleDC(ref);if(!*mem)return nullptr;
 BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;
 bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
 bi.bmiHeader.biCompression=BI_RGB;void*bits=nullptr;
 HBITMAP b=CreateDIBSection(*mem,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
 if(!b){DeleteDC(*mem);*mem=nullptr;}return b;
}
static int gifEncoder(CLSID*c){
 UINT n=0,s=0;GetImageEncodersSize(&n,&s);if(!s)return -1;
 std::vector<BYTE>v(s);auto*p=(ImageCodecInfo*)v.data();GetImageEncoders(n,s,p);
 for(UINT i=0;i<n;i++)if(wcscmp(p[i].MimeType,L"image/gif")==0){*c=p[i].Clsid;return(int)i;}
 return -1;
}
static void prop(Bitmap*b,PROPID id,WORD type,DWORD len,void*v){
 PropertyItem p{};p.id=id;p.type=type;p.length=len;p.value=v;b->SetPropertyItem(&p);
}
static Bitmap* frame(double t){
 HDC s=GetDC(nullptr);if(!s)return nullptr;HDC dc=nullptr;HBITMAP b=dib(s,GW,GH,&dc);ReleaseDC(nullptr,s);
 if(!b)return nullptr;HBITMAP old=(HBITMAP)SelectObject(dc,b);
 nowTime=t;resetState();processTo(t);renderFinal(dc,GW,GH);SelectObject(dc,old);
 Bitmap*out=new Bitmap(b,nullptr);DeleteObject(b);DeleteDC(dc);
 if(!out||out->GetLastStatus()!=Ok){delete out;return nullptr;}return out;
}
static bool exportGif(){
 if(evs.empty()){MessageBoxW(gWnd,L"Load a timeline first.",L"Export GIF",MB_OK|MB_ICONWARNING);return false;}
 if(!loadImage()){MessageBoxW(gWnd,L"image.png must be beside APMTimelineVisualizer.exe.",L"Export GIF",MB_OK|MB_ICONERROR);return false;}
 CLSID c{};if(gifEncoder(&c)<0)return false;
 std::wstring out=exeDir()+L"\\APM_Replay.gif";DeleteFileW(out.c_str());
 ULONG delay=2;WORD loop=0;
 EncoderParameters st{};st.Count=1;st.Parameter[0].Guid=EncoderSaveFlag;st.Parameter[0].Type=EncoderParameterValueTypeLong;
 st.Parameter[0].NumberOfValues=1;ULONG mf=EncoderValueMultiFrame;st.Parameter[0].Value=&mf;
 Bitmap*first=frame(0);if(!first)return false;
 prop(first,PropertyTagFrameDelay,PropertyTagTypeLong,sizeof(ULONG),&delay);
 prop(first,PropertyTagLoopCount,PropertyTagTypeShort,sizeof(WORD),&loop);
 if(first->Save(out.c_str(),&c,&st)!=Ok){delete first;return false;}
 EncoderParameters add{};add.Count=1;add.Parameter[0].Guid=EncoderSaveFlag;add.Parameter[0].Type=EncoderParameterValueTypeLong;
 add.Parameter[0].NumberOfValues=1;ULONG ft=EncoderValueFrameDimensionTime;add.Parameter[0].Value=&ft;
 long long n=std::max<long long>(1,(long long)std::ceil(lengthSec*FPS));
 for(long long i=1;i<n;i++){Bitmap*x=frame((double)i/FPS);if(!x){
   EncoderParameters flush{}; flush.Count=1;
   flush.Parameter[0].Guid=EncoderSaveFlag;
   flush.Parameter[0].Type=EncoderParameterValueTypeLong;
   flush.Parameter[0].NumberOfValues=1;
   ULONG fv=EncoderValueFlush; flush.Parameter[0].Value=&fv;
   first->SaveAdd(&flush); delete first; return false;
  }
  prop(x,PropertyTagFrameDelay,PropertyTagTypeLong,sizeof(ULONG),&delay);
  if(first->SaveAdd(x,&add)!=Ok){
   delete x;
   EncoderParameters flush{}; flush.Count=1;
   flush.Parameter[0].Guid=EncoderSaveFlag;
   flush.Parameter[0].Type=EncoderParameterValueTypeLong;
   flush.Parameter[0].NumberOfValues=1;
   ULONG fv=EncoderValueFlush; flush.Parameter[0].Value=&fv;
   first->SaveAdd(&flush); delete first; return false;
  }
  delete x;
 }
 { EncoderParameters flush{}; flush.Count=1;
   flush.Parameter[0].Guid=EncoderSaveFlag;
   flush.Parameter[0].Type=EncoderParameterValueTypeLong;
   flush.Parameter[0].NumberOfValues=1;
   ULONG fv=EncoderValueFlush; flush.Parameter[0].Value=&fv;
   first->SaveAdd(&flush); }
 delete first;
 MessageBoxW(gWnd,(L"GIF exported:\n"+out).c_str(),L"Export GIF",MB_OK|MB_ICONINFORMATION);return true;
}
static std::wstring editText(HWND h){
 int n=GetWindowTextLengthW(h);if(n<=0)return L"";std::wstring s(n+1,L'\0');GetWindowTextW(h,&s[0],n+1);s.resize(n);return s;
}
static void layout(HWND h){
 RECT r{};GetClientRect(h,&r);int w=r.right,H=r.bottom;
 MoveWindow(gEdit,10,10,w-20,H-170,TRUE);
 MoveWindow(gLoad,10,H-150,100,30,TRUE);
 MoveWindow(gPlay,120,H-150,90,30,TRUE);
 MoveWindow(gReset,220,H-150,90,30,TRUE);
 MoveWindow(gExport,320,H-150,110,30,TRUE);
}
static void tick(){
 if(!playing)return;LARGE_INTEGER q{};QueryPerformanceCounter(&q);
 nowTime=playOrigin+(double)(q.QuadPart-playStart.QuadPart)/pf.QuadPart;
 if(nowTime>=lengthSec){nowTime=lengthSec;playing=false;SetWindowTextW(gPlay,L"Play");}
 resetState();processTo(nowTime);InvalidateRect(gWnd,nullptr,FALSE);
}
static LRESULT CALLBACK wndProc(HWND h,UINT m,WPARAM w,LPARAM l){
 switch(m){
 case WM_SIZE:layout(h);return 0;
 case WM_TIMER:tick();return 0;
 case WM_COMMAND:
  if((HWND)l==gLoad){
   if(!parseTimeline(editText(gEdit))){
    MessageBoxW(h,L"Timeline error.\n\nUse only:\n<delta_ms> <key>+\n<delta_ms> <key>-",
      L"Timeline error",MB_OK|MB_ICONERROR);
   }else{
    playing=false;nowTime=0;resetState();SetWindowTextW(gPlay,L"Play");InvalidateRect(h,nullptr,FALSE);
   }
  } else if((HWND)l==gPlay){if(playing){playing=false;SetWindowTextW(gPlay,L"Play");}
   else if(!evs.empty()){playing=true;playOrigin=nowTime;QueryPerformanceCounter(&playStart);SetWindowTextW(gPlay,L"Pause");}}
  else if((HWND)l==gReset){playing=false;nowTime=0;resetState();SetWindowTextW(gPlay,L"Play");InvalidateRect(h,nullptr,FALSE);}
  else if((HWND)l==gExport)exportGif();
  else if((HWND)l==gEdit && HIWORD(w)==EN_CHANGE)InvalidateRect(h,nullptr,FALSE);
  return 0;
 case WM_PAINT:{
  PAINTSTRUCT p{};HDC dc=BeginPaint(h,&p);RECT r{};GetClientRect(h,&r);
  int ph=(int)std::max<LONG>(100L,(LONG)r.bottom-175L);renderPreview(dc,r.right,ph);EndPaint(h,&p);return 0;}
 case WM_DESTROY:KillTimer(h,1);delete img;GdiplusShutdown(gdToken);PostQuitMessage(0);return 0;
 }
 return DefWindowProcW(h,m,w,l);
}
int APIENTRY wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){
 GdiplusStartupInput in{};if(GdiplusStartup(&gdToken,&in,nullptr)!=Ok)return 1;
 QueryPerformanceFrequency(&pf);
 WNDCLASSW wc{};wc.lpfnWndProc=wndProc;wc.hInstance=hi;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
 wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName=L"APMTimelineVisualizer";
 RegisterClassW(&wc);
 gWnd=CreateWindowExW(0,wc.lpszClassName,L"APM Timeline Visualizer",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
  CW_USEDEFAULT,CW_USEDEFAULT,1000,760,nullptr,nullptr,hi,nullptr);
 if(!gWnd){GdiplusShutdown(gdToken);return 1;}
 gEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,
  0,0,0,0,gWnd,nullptr,hi,nullptr);
 gLoad=CreateWindowW(L"BUTTON",L"Load Timeline",WS_CHILD|WS_VISIBLE,0,0,0,0,gWnd,nullptr,hi,nullptr);
 gPlay=CreateWindowW(L"BUTTON",L"Play",WS_CHILD|WS_VISIBLE,0,0,0,0,gWnd,nullptr,hi,nullptr);
 gReset=CreateWindowW(L"BUTTON",L"Reset",WS_CHILD|WS_VISIBLE,0,0,0,0,gWnd,nullptr,hi,nullptr);
 gExport=CreateWindowW(L"BUTTON",L"Export GIF",WS_CHILD|WS_VISIBLE,0,0,0,0,gWnd,nullptr,hi,nullptr);
 SetWindowTextW(gEdit,L"3234 S+\r\n94 S-\r\n203 T+\r\n109 T-\r\n0 A+\r\n157 R+\r\n31 A-\r\n78 R-\r\n94 T+\r\n125 T-");
 SetTimer(gWnd,1,16,nullptr);layout(gWnd);
 MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}return(int)m.wParam;
}
