#include "ui/OrganizeRulesDialog.h"
#include "ui/DialogStyle.h"
#include <CommCtrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>

namespace {
using namespace lattice::organize;
constexpr wchar_t kClass[] = L"Lattice.OrganizeRules";
constexpr UINT kSaved = WM_APP + 80;
enum Id { List=100, Add, Remove, Up, Down, Name, Enabled, Pattern, Type, Extensions,
          TargetPattern, Time, Age, Days, Destination, Category, NewCategory, Cancel, Save, Preview, Close };
constexpr COLORREF bg=RGB(7,37,48), panel=RGB(12,49,62), hover=RGB(24,65,80),
    border=RGB(66,124,143), accent=RGB(79,171,204), text=RGB(244,249,251), muted=RGB(157,193,205);
struct State {
    HINSTANCE instance{}; HWND hwnd{}, owner{}, tooltip{}; UINT dpi=96;
    const ConfigStore* store{}; std::vector<OrganizeRule> initial, rules;
    std::vector<CategoryConfig> categories;
    std::vector<HWND> controls; std::map<int,std::wstring> tips;
    HFONT font{}, smallFont{}, titleFont{}; HBRUSH background{}, fieldBrush{};
    int selected=-1, hovered=0, hoveredRow=-1; bool loading=false, saving=false, preview=false;
    std::uint64_t token{}; std::wstring message;
    OrganizeRulesDialog::Result result=OrganizeRulesDialog::Result::Cancelled;
};
int S(const State& s,int n) { return MulDiv(n,static_cast<int>(s.dpi),96); }
HWND C(const State& s,int id) { return GetDlgItem(s.hwnd,id); }
std::wstring Value(HWND h) {
    const int n=GetWindowTextLengthW(h); std::wstring v(static_cast<std::size_t>(n)+1,L'\0');
    GetWindowTextW(h,v.data(),n+1); v.resize(static_cast<std::size_t>(n)); return v;
}
int Choice(const State& s,int id) { return static_cast<int>(SendMessageW(C(s,id),CB_GETCURSEL,0,0)); }
void Select(const State& s,int id,int n) { SendMessageW(C(s,id),CB_SETCURSEL,n,0); InvalidateRect(C(s,id),nullptr,FALSE); }
void Text(State& s,HDC dc,const wchar_t* value,int x,int y,int w,int h,bool useSmallFont=false,COLORREF color=text) {
    RECT r{S(s,x),S(s,y),S(s,x+w),S(s,y+h)}; SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT);
    const auto old=SelectObject(dc,useSmallFont?s.smallFont:s.font);
    DrawTextW(dc,value,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS); SelectObject(dc,old);
}
LRESULT CALLBACK ControlProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
    auto& s=*reinterpret_cast<State*>(data);
    const bool edit=id==Name||id==Pattern||id==Extensions||id==TargetPattern||id==Days||id==NewCategory;
    const bool combo=id==Enabled||id==Type||id==Time||id==Age||id==Destination||id==Category;
    if(m==WM_PAINT&&combo) {
        PAINTSTRUCT ps{}; const auto dc=BeginPaint(h,&ps);
        lattice::ui::DrawDialogCombo(h,dc,s.font,s.dpi,s.hovered==static_cast<int>(id),{10,12,4,2}); EndPaint(h,&ps); return 0;
    }
    if(m==WM_NCCALCSIZE&&edit) {
        const auto result=DefSubclassProc(h,m,w,l);
        RECT* r=w?&reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0]:reinterpret_cast<RECT*>(l);
        r->left+=S(s,12); r->right-=S(s,12); r->top+=S(s,8); r->bottom-=S(s,8); return result;
    }
    if(m==WM_ENABLE) { RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME); }
    if (m==WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0}; TrackMouseEvent(&track);
        if(id==List) {
            const LRESULT hit=SendMessageW(h,LB_ITEMFROMPOINT,0,l);
            const int row=HIWORD(hit)==0?LOWORD(hit):-1;
            if(s.hoveredRow!=row) { s.hoveredRow=row; InvalidateRect(h,nullptr,FALSE); }
        }
        if (s.hovered!=static_cast<int>(id)) { s.hovered=static_cast<int>(id); InvalidateRect(h,nullptr,FALSE); }
    } else if(m==WM_MOUSELEAVE) { s.hovered=0; if(id==List) s.hoveredRow=-1; InvalidateRect(h,nullptr,FALSE); }
    else if(m==WM_SETFOCUS || m==WM_KILLFOCUS) InvalidateRect(h,nullptr,FALSE);
    else if(m==WM_NCDESTROY) RemoveWindowSubclass(h,ControlProc,id);
    // Edits retain native keyboard, IME, selection, clipboard and accessibility.
    if(m==WM_NCPAINT && edit) {
        lattice::ui::DrawDialogEditBorder(h,s.fieldBrush,s.hovered==static_cast<int>(id),accent,border,S(s,10)); return 0;
    }
    return DefSubclassProc(h,m,w,l);
}
HWND Control(State& s,const wchar_t* cls,const wchar_t* label,int id,int x,int y,int w,int h,DWORD style,const wchar_t* tip) {
    HWND control=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,
        S(s,x),S(s,y),S(s,w),S(s,h),s.hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),s.instance,nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(s.font),FALSE);
    SetWindowTheme(control,L"",L"");
    SetWindowSubclass(control,ControlProc,id,reinterpret_cast<DWORD_PTR>(&s));
    SetWindowPos(control,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    s.controls.push_back(control); s.tips[id]=tip;
    TOOLINFOW ti{sizeof(ti)}; ti.uFlags=TTF_IDISHWND|TTF_SUBCLASS; ti.hwnd=s.hwnd;
    ti.uId=reinterpret_cast<UINT_PTR>(control); ti.lpszText=s.tips[id].data();
    SendMessageW(s.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti)); return control;
}
void Button(State& s,const wchar_t* label,int id,int x,int y,int w,const wchar_t* tip) {
    Control(s,L"BUTTON",label,id,x,y,w,36,BS_OWNERDRAW,tip);
}
void Edit(State& s,int id,int x,int y,int w,int limit,const wchar_t* cue,const wchar_t* tip) {
    HWND h=Control(s,L"EDIT",L"",id,x,y,w,37,ES_AUTOHSCROLL,tip);
    SendMessageW(h,EM_SETLIMITTEXT,limit,0); SendMessageW(h,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(cue));
    RECT r{S(s,9),S(s,8),S(s,w-9),S(s,30)}; SendMessageW(h,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&r));
}
void Combo(State& s,int id,int x,int y,int w,std::initializer_list<const wchar_t*> labels,const wchar_t* tip) {
    HWND h=Control(s,L"COMBOBOX",L"",id,x,y,w,250,CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,tip);
    SendMessageW(h,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),S(s,34));
    SendMessageW(h,CB_SETITEMHEIGHT,0,S(s,30));
    for(auto label:labels) SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
}
void RefreshList(State& s) {
    SendMessageW(C(s,List),LB_RESETCONTENT,0,0);
    const int listHeight=std::max(1,std::min(3,static_cast<int>(s.rules.size())))*74;
    SetWindowPos(C(s,List),nullptr,S(s,14),S(s,107),S(s,216),S(s,listHeight),SWP_NOZORDER|SWP_NOACTIVATE);
    for(int id=Add;id<=Down;++id) SetWindowPos(C(s,id),nullptr,S(s,15+49*(id-Add)),S(s,116+listHeight),S(s,44),S(s,32),SWP_NOZORDER|SWP_NOACTIVATE);
    for(const auto& rule:s.rules) SendMessageW(C(s,List),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(rule.name.c_str()));
    SendMessageW(C(s,List),LB_SETCURSEL,s.selected,0); InvalidateRect(C(s,List),nullptr,FALSE);
}
void FieldsEnabled(State& s) {
    const bool active=s.selected>=0 && !s.saving;
    for(int id=Name;id<=NewCategory;++id) EnableWindow(C(s,id),active);
    EnableWindow(C(s,Extensions),active&&Choice(s,Type)==static_cast<int>(RuleItemType::File));
    const int type=Choice(s,Type);
    EnableWindow(C(s,TargetPattern),active&&(type==0||type==3||type==4));
    EnableWindow(C(s,Age),active&&Choice(s,Time)>0); EnableWindow(C(s,Days),active&&Choice(s,Time)>0);
    const bool isNew=Choice(s,Destination)==1;
    ShowWindow(C(s,Category),isNew?SW_HIDE:SW_SHOW); ShowWindow(C(s,NewCategory),isNew?SW_SHOW:SW_HIDE);
    EnableWindow(C(s,List),!s.saving); EnableWindow(C(s,Add),!s.saving&&s.rules.size()<kMaximumOrganizeRules);
    EnableWindow(C(s,Remove),active); EnableWindow(C(s,Up),active&&s.selected>0);
    EnableWindow(C(s,Down),active&&static_cast<std::size_t>(s.selected+1)<s.rules.size());
    for(int id=Cancel;id<=Close;++id) EnableWindow(C(s,id),!s.saving);
}
void ReadFields(State& s) {
    if(s.loading||s.selected<0||static_cast<std::size_t>(s.selected)>=s.rules.size()) return;
    auto& rule=s.rules[static_cast<std::size_t>(s.selected)];
    rule.name=Value(C(s,Name)); rule.enabled=Choice(s,Enabled)==0; rule.namePattern=Value(C(s,Pattern));
    rule.itemType=static_cast<RuleItemType>(Choice(s,Type));
    rule.extensions=rule.itemType==RuleItemType::File?Value(C(s,Extensions)):L"";
    rule.targetPattern=(rule.itemType==RuleItemType::Any||rule.itemType==RuleItemType::Shortcut||rule.itemType==RuleItemType::UrlShortcut)
        ?Value(C(s,TargetPattern)):L"";
    rule.timeField=static_cast<RuleTimeField>(Choice(s,Time)); rule.age=static_cast<RuleAge>(Choice(s,Age));
    const auto days=Value(C(s,Days));
    try { std::size_t consumed=0; rule.days=std::stoi(days,&consumed); if(consumed!=days.size()) rule.days=0; }
    catch(...) { rule.days=0; }
    rule.newCategoryName=Choice(s,Destination)==1?Value(C(s,NewCategory)):L"";
    rule.targetCategoryId.clear(); const int category=Choice(s,Category);
    if(Choice(s,Destination)==0&&category>=0&&static_cast<std::size_t>(category)<s.categories.size())
        rule.targetCategoryId=s.categories[static_cast<std::size_t>(category)].id;
}
void LoadFields(State& s) {
    s.loading=true;
    const OrganizeRule empty{}; const auto& r=s.selected<0?empty:s.rules[static_cast<std::size_t>(s.selected)];
    SetWindowTextW(C(s,Name),r.name.c_str()); Select(s,Enabled,r.enabled?0:1);
    SetWindowTextW(C(s,Pattern),r.namePattern.c_str()); Select(s,Type,static_cast<int>(r.itemType));
    SetWindowTextW(C(s,Extensions),r.extensions.c_str()); SetWindowTextW(C(s,TargetPattern),r.targetPattern.c_str());
    Select(s,Time,static_cast<int>(r.timeField)); Select(s,Age,static_cast<int>(r.age));
    SetWindowTextW(C(s,Days),std::to_wstring(r.days).c_str()); Select(s,Destination,r.newCategoryName.empty()?0:1);
    SetWindowTextW(C(s,NewCategory),r.newCategoryName.c_str());
    int selected=-1; for(std::size_t i=0;i<s.categories.size();++i) if(s.categories[i].id==r.targetCategoryId) selected=static_cast<int>(i);
    Select(s,Category,selected); s.loading=false; FieldsEnabled(s); InvalidateRect(s.hwnd,nullptr,FALSE);
}
void MakeControls(State& s) {
    s.background=CreateSolidBrush(bg); s.fieldBrush=CreateSolidBrush(panel);
    s.font=CreateFontW(-S(s,14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    s.smallFont=CreateFontW(-S(s,12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    s.titleFont=CreateFontW(-S(s,21),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    s.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        0,0,0,0,s.hwnd,nullptr,s.instance,nullptr);
    SetWindowTheme(s.tooltip,L"",L""); SendMessageW(s.tooltip,TTM_SETTIPBKCOLOR,panel,0);
    SendMessageW(s.tooltip,TTM_SETTIPTEXTCOLOR,text,0); SendMessageW(s.tooltip,TTM_SETMAXTIPWIDTH,0,S(s,300));
    SendMessageW(s.tooltip,TTM_SETDELAYTIME,TTDT_INITIAL,500); SendMessageW(s.tooltip,WM_SETFONT,reinterpret_cast<WPARAM>(s.smallFont),FALSE);
    Control(s,L"LISTBOX",L"",List,14,109,216,260,LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL,
        L"选择规则进行编辑；列表从上到下采用首条匹配。");
    SendMessageW(C(s,List),LB_SETITEMHEIGHT,0,S(s,74));
    Button(s,L"新增",Add,14,390,49,L"新增规则；保存前不改变桌面。");
    Button(s,L"删除",Remove,68,390,49,L"删除当前规则，不删除文件。");
    Button(s,L"上移",Up,122,390,49,L"提高规则优先级。"); Button(s,L"下移",Down,176,390,49,L"降低规则优先级。");
    Edit(s,Name,391,96,542,80,L"规则名称",L"用于识别规则和展示匹配依据。");
    Combo(s,Enabled,391,146,542,{L"启用",L"停用"},L"停用规则不参与下一次预览。");
    Edit(s,Pattern,391,198,542,256,L"留空不限；支持 * 和 ?",L"按图标显示名称匹配，不区分大小写；* 任意字符，? 一个字符。");
    Combo(s,Type,391,249,349,{L"不限",L"普通文件",L"文件夹",L"快捷方式",L"URL 快捷方式"},L"匹配原件类型，不依据快捷方式目标扩展名。");
    Edit(s,Extensions,748,249,185,256,L"扩展名；留空不限",L"多个扩展名以分号分隔，例如 .pdf;.docx。");
    Edit(s,TargetPattern,391,300,542,1024,L"留空不限；例如 *\\Code.exe",L"匹配已解析的快捷方式目标；无法解析不匹配。");
    Combo(s,Time,391,351,215,{L"不限",L"修改时间",L"创建时间"},L"读取原件的创建或修改时间，读取失败不匹配。");
    Combo(s,Age,614,351,214,{L"最近",L"超过"},L"以本次预览生成时刻计算，最近包含边界。");
    Edit(s,Days,836,351,75,5,L"30",L"输入 1–36500 天。");
    Combo(s,Destination,391,402,267,{L"已有格子",L"新格子"},L"新格子须在预览中确认位置；同屏同名格子会复用。");
    Combo(s,Category,666,402,267,{},L"按稳定格子身份保存；目标缺失或锁定时保持原位。");
    Edit(s,NewCategory,666,402,267,80,L"新格子名称",L"只创建 Lattice 格子；不移动真实文件。");
    for(const auto& category:s.categories) SendMessageW(C(s,Category),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(category.name.c_str()));
    Button(s,L"取消",Cancel,717,594,54,L"丢弃本次未保存编辑。");
    Button(s,L"保存",Save,779,594,54,L"只保存规则，不改变图标归属。");
    Button(s,L"保存并预览",Preview,841,594,96,L"保存后打开现有整理预览，仍须确认应用。");
    Button(s,L"×",Close,922,12,28,L"关闭并丢弃未保存编辑。");
    s.selected=s.rules.empty()?-1:0; RefreshList(s); LoadFields(s);
}
void DrawItem(State& s,const DRAWITEMSTRUCT& d) {
    if((d.CtlID>=Add && d.CtlID<=Down) || (d.CtlID>=Cancel && d.CtlID<=Close)) {
        FillRect(d.hDC,&d.rcItem,s.background);
        lattice::ui::DrawDialogButton(d,d.CtlID<=Down?s.smallFont:s.font,s.dpi,s.hovered==static_cast<int>(d.CtlID),d.CtlID==Preview,10); return;
    }
    const bool selected=(d.itemState&ODS_SELECTED)!=0;
    auto brush=CreateSolidBrush(selected||(d.CtlID==List&&static_cast<int>(d.itemID)==s.hoveredRow)?hover:(d.CtlID==List?bg:panel)); FillRect(d.hDC,&d.rcItem,brush); DeleteObject(brush);
    if(d.itemID==static_cast<UINT>(-1)) return;
    if(d.CtlID==List) {
        if(d.itemID>=s.rules.size()) return; const auto& r=s.rules[d.itemID];
        const auto pen=CreatePen(PS_SOLID,1,selected?accent:bg); const auto old=SelectObject(d.hDC,pen);
        const auto oldBrush=SelectObject(d.hDC,GetStockObject(HOLLOW_BRUSH));
        RoundRect(d.hDC,d.rcItem.left,d.rcItem.top+S(s,3),d.rcItem.right,d.rcItem.bottom-S(s,3),S(s,5),S(s,5));
        SelectObject(d.hDC,oldBrush); SelectObject(d.hDC,old); DeleteObject(pen);
        const int y=MulDiv(d.rcItem.top,96,static_cast<int>(s.dpi));
        Text(s,d.hDC,r.name.c_str(),12,y+10,190,22);
        std::wstring target=r.newCategoryName; for(const auto& c:s.categories) if(c.id==r.targetCategoryId) target=c.name;
        if(target.empty()) target=L"未选择目标";
        Text(s,d.hDC,((r.enabled?L"已启用 · ":L"已停用 · ")+target).c_str(),12,y+33,190,20,true,muted);
    } else {
        wchar_t label[256]{}; SendMessageW(d.hwndItem,CB_GETLBTEXT,d.itemID,reinterpret_cast<LPARAM>(label));
        RECT r=d.rcItem; r.left+=S(s,12); SetBkMode(d.hDC,TRANSPARENT);
        SetTextColor(d.hDC,IsWindowEnabled(d.hwndItem)?text:muted); const auto old=SelectObject(d.hDC,s.font);
        DrawTextW(d.hDC,label,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS); SelectObject(d.hDC,old);
    }
}
LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE) { s=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); s->hwnd=h;
        SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s)); }
    if(!s) return DefWindowProcW(h,m,w,l);
    switch(m) {
    case WM_CREATE: MakeControls(*s); return 0;
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight=S(*s,30); return TRUE;
    case WM_DRAWITEM: DrawItem(*s,*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT: {
        HDC dc=reinterpret_cast<HDC>(w); SetTextColor(dc,IsWindowEnabled(reinterpret_cast<HWND>(l))?text:muted);
        SetBkColor(dc,panel);
        return reinterpret_cast<LRESULT>(reinterpret_cast<HWND>(l)==C(*s,List)?s->background:s->fieldBrush); }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: { const auto hit=DefWindowProcW(h,m,w,l); POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(h,&p);
        return hit==HTCLIENT&&p.y<S(*s,62)&&p.x<S(*s,915)?HTCAPTION:hit; }
    case WM_PAINT: {
        PAINTSTRUCT ps{}; const auto dc=BeginPaint(h,&ps); RECT rc{}; GetClientRect(h,&rc); FillRect(dc,&rc,s->background);
        const auto pen=CreatePen(PS_SOLID,1,RGB(40,85,99)); const auto old=SelectObject(dc,pen);
        const auto oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH)); RoundRect(dc,0,0,rc.right,rc.bottom,S(*s,9),S(*s,9));
        MoveToEx(dc,0,S(*s,62),nullptr); LineTo(dc,rc.right,S(*s,62)); MoveToEx(dc,0,S(*s,568),nullptr); LineTo(dc,rc.right,S(*s,568));
        MoveToEx(dc,S(*s,244),S(*s,62),nullptr); LineTo(dc,S(*s,244),S(*s,568));
        SelectObject(dc,oldBrush); SelectObject(dc,old); DeleteObject(pen);
        const auto oldFont=SelectObject(dc,s->titleFont); SetTextColor(dc,text); SetBkMode(dc,TRANSPARENT);
        RECT title{S(*s,24),S(*s,16),S(*s,270),S(*s,49)}; DrawTextW(dc,L"整理规则",-1,&title,DT_SINGLELINE|DT_VCENTER); SelectObject(dc,oldFont);
        Text(*s,dc,L"按列表顺序，首条匹配规则生效",675,19,244,25,true,muted);
        Text(*s,dc,L"规则列表",14,80,180,20,true,muted);
        if(s->rules.empty()) Text(*s,dc,L"暂无规则，点击新增",26,125,190,26,true,muted);
        Text(*s,dc,L"一条规则内的条件须同时满足。",14,158+std::max(1,std::min(3,static_cast<int>(s->rules.size())))*74,216,18,true,muted);
        Text(*s,dc,L"未匹配的项目保持原位置。",14,176+std::max(1,std::min(3,static_cast<int>(s->rules.size())))*74,216,18,true,muted);
        const wchar_t* labels[]{L"规则名称",L"状态",L"名称匹配",L"项目类型",L"快捷方式目标",L"文件时间",L"放入格子"};
        for(int i=0;i<7;++i) Text(*s,dc,labels[i],271,(i==0?96:i==1?146:198+51*(i-2)),106,37);
        Text(*s,dc,L"天",919,351,14,37);
        RECT help{S(*s,271),S(*s,461),S(*s,933),S(*s,531)}; FillRect(dc,&help,s->fieldBrush);
        Text(*s,dc,L"保存规则不会立即整理桌面。",283,471,630,24,false,RGB(114,220,179));
        Text(*s,dc,L"点击“保存并预览”后审查命中项目；确认应用后才改变归属，完成后可撤销。",283,495,630,24,true,muted);
        Text(*s,dc,s->message.c_str(),271,538,662,22,true,s->saving?muted:RGB(255,158,158));
        Text(*s,dc,L"完全本地 · 不移动真实文件 · 不后台持续执行",22,590,570,40,true,muted);
        EndPaint(h,&ps); return 0; }
    case WM_COMMAND: {
        const int id=LOWORD(w), notification=HIWORD(w);
        if(s->loading||s->saving) return 0;
        if(id==List&&notification==LBN_SELCHANGE) { ReadFields(*s); s->selected=static_cast<int>(SendMessageW(C(*s,List),LB_GETCURSEL,0,0)); LoadFields(*s); return 0; }
        if(id>=Name&&id<=NewCategory&&(notification==EN_CHANGE||notification==CBN_SELCHANGE)) {
            ReadFields(*s); FieldsEnabled(*s); InvalidateRect(C(*s,List),nullptr,FALSE); return 0; }
        if(id==Cancel||id==Close||id==IDCANCEL) { DestroyWindow(h); return 0; }
        if(id==Add) { ReadFields(*s); if(s->rules.size()>=kMaximumOrganizeRules) return 0;
            static std::atomic<std::uint64_t> sequence{}; OrganizeRule rule;
            rule.id=L"rule-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++sequence); rule.name=L"新规则";
            s->rules.push_back(std::move(rule)); s->selected=static_cast<int>(s->rules.size()-1); RefreshList(*s); LoadFields(*s); SetFocus(C(*s,Name)); return 0; }
        if(id>=Remove&&id<=Down&&s->selected>=0) { ReadFields(*s);
            if(id==Remove) { s->rules.erase(s->rules.begin()+s->selected); s->selected=std::min(s->selected,static_cast<int>(s->rules.size())-1); }
            else { const int next=s->selected+(id==Up?-1:1); if(next<0||static_cast<std::size_t>(next)>=s->rules.size()) return 0;
                std::swap(s->rules[s->selected],s->rules[next]); s->selected=next; }
            RefreshList(*s); LoadFields(*s); return 0; }
        if(id==Save||id==Preview||id==IDOK) { ReadFields(*s);
            if(!ValidateOrganizeRules(s->rules,s->message)) { InvalidateRect(h,nullptr,FALSE); return 0; }
            s->preview=id==Preview; s->token=GetTickCount64(); s->saving=true; s->message=L"正在保存规则…"; FieldsEnabled(*s);
            if(!s->store->SaveOrganizeRulesAsync(s->initial,s->rules,h,kSaved,s->token)) {
                s->saving=false; s->message=L"保存任务未入队，原配置保持不变；请重试。"; FieldsEnabled(*s); }
            InvalidateRect(h,nullptr,FALSE); return 0; }
        break; }
    case kSaved: {
        std::unique_ptr<AutoOrganizeTransactionResult> r(reinterpret_cast<AutoOrganizeTransactionResult*>(l));
        if(!r||r->token!=s->token) return 0; s->saving=false;
        if(r->succeeded) { s->result=s->preview?OrganizeRulesDialog::Result::Preview:OrganizeRulesDialog::Result::Saved; DestroyWindow(h); }
        else { s->message=r->message; FieldsEnabled(*s); InvalidateRect(h,nullptr,FALSE); } return 0; }
    case WM_CLOSE: if(!s->saving) DestroyWindow(h); return 0;
    case WM_DESTROY: {
        MSG pending{}; while(PeekMessageW(&pending,h,kSaved,kSaved,PM_REMOVE)) delete reinterpret_cast<AutoOrganizeTransactionResult*>(pending.lParam);
        if(s->tooltip) DestroyWindow(s->tooltip); DeleteObject(s->font); DeleteObject(s->smallFont); DeleteObject(s->titleFont);
        DeleteObject(s->background); DeleteObject(s->fieldBrush); return 0; }
    }
    return DefWindowProcW(h,m,w,l);
}
}
OrganizeRulesDialog::Result OrganizeRulesDialog::Show(HINSTANCE instance,HWND owner,const ConfigStore& store) {
    const auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    State s; s.instance=instance; s.owner=owner; s.store=&store;
    const AppConfig config=store.LoadAppConfig(); s.initial=config.organizeRules; s.rules=config.organizeRules; s.categories=config.categories;
    if(config.organizeRulesReadError) s.message=L"配置中的规则损坏，未执行；保存有效规则可修复。";
    HMONITOR monitor=MonitorFromWindow(owner,MONITOR_DEFAULTTOPRIMARY); MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor,&info);
    s.dpi=owner?GetDpiForWindow(owner):GetDpiForSystem(); if(s.dpi<96) s.dpi=96;
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc=Proc; cls.hInstance=instance; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName=kClass;
    RegisterClassExW(&cls);
    HWND dialog=CreateWindowExW(WS_EX_TOOLWINDOW,kClass,L"Lattice 整理规则",WS_POPUP|WS_CLIPCHILDREN,
        info.rcWork.left+(info.rcWork.right-info.rcWork.left-S(s,960))/2,
        info.rcWork.top+(info.rcWork.bottom-info.rcWork.top-S(s,654))/2,S(s,960),S(s,654),owner,nullptr,instance,&s);
    if(!dialog) { if(previous) SetThreadDpiAwarenessContext(previous); return Result::Cancelled; }
    const bool enabled=owner&&IsWindowEnabled(owner); if(enabled) EnableWindow(owner,FALSE);
    ShowWindow(dialog,SW_SHOW); SetForegroundWindow(dialog);
    MSG msg{}; bool quit=false; int code=0;
    while(IsWindow(dialog)) { const BOOL result=GetMessageW(&msg,nullptr,0,0);
        if(result<=0) { quit=result==0; code=static_cast<int>(msg.wParam); break; }
        if(!IsDialogMessageW(dialog,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if(IsWindow(dialog)) DestroyWindow(dialog);
    if(enabled&&IsWindow(owner)) { EnableWindow(owner,TRUE); if(!quit) SetForegroundWindow(owner); }
    if(previous) SetThreadDpiAwarenessContext(previous); if(quit) PostQuitMessage(code);
    return s.result;
}
