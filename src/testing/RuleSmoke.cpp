#include "testing/RuleSmoke.h"
#include "organize/RuleEvaluator.h"
#include "organize/AutoOrganizeTransaction.h"
#include "ui/OrganizeRulesDialog.h"
#include "ui/AutoOrganizePreviewWindow.h"
#include <algorithm>
#include <CommCtrl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

struct OrganizeRulesSmokeAccess {
    static bool Ready(const AutoOrganizePreviewWindow& w) {
        return w.state_==AutoOrganizePreviewWindow::ViewState::Ready;
    }
    static const lattice::organize::Plan& Plan(const AutoOrganizePreviewWindow& w) { return w.plan_; }
    static bool Scanning(const AutoOrganizePreviewWindow& w) { return w.state_==AutoOrganizePreviewWindow::ViewState::Scanning; }
    static bool ClickApply(AutoOrganizePreviewWindow& w) {
        w.Render();
        const auto hit=std::find_if(w.hitTargets_.begin(),w.hitTargets_.end(),[](const auto& value){
            return value.kind==AutoOrganizePreviewWindow::HitKind::Apply;
        });
        if(hit==w.hitTargets_.end()) return false;
        const int x=static_cast<int>((hit->bounds.left+hit->bounds.right)*w.dpi_/192.0f);
        const int y=static_cast<int>((hit->bounds.top+hit->bounds.bottom)*w.dpi_/192.0f);
        SendMessageW(w.Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        SendMessageW(w.Window(),WM_LBUTTONUP,0,MAKELPARAM(x,y)); return true;
    }
};

namespace {
using namespace lattice::organize;
void Require(bool condition,const char* label) { if(!condition) throw std::runtime_error(label); }
bool WildcardOracle(const std::wstring& pattern,const std::wstring& value) {
    std::vector<bool> before(value.size()+1),next(value.size()+1); before[0]=true;
    for(const wchar_t part:pattern) {
        std::fill(next.begin(),next.end(),false); next[0]=part==L'*'&&before[0];
        for(std::size_t j=1;j<=value.size();++j) next[j]=part==L'*'?(before[j]||next[j-1]):
            before[j-1]&&(part==L'?'||CompareStringOrdinal(&part,1,&value[j-1],1,TRUE)==CSTR_EQUAL);
        before.swap(next);
    }
    return before.back();
}
void CheckWildcardSpans() {
    std::vector<std::wstring> patterns{L""},values{L""};
    std::size_t start=0,end=1;
    for(int depth=0;depth<4;++depth) { for(std::size_t i=start;i<end;++i) for(wchar_t c:std::wstring(L"aB*?")) patterns.push_back(patterns[i]+c); start=end;end=patterns.size(); }
    start=0;end=1;
    for(int depth=0;depth<4;++depth) { for(std::size_t i=start;i<end;++i) for(wchar_t c:std::wstring(L"Ab")) values.push_back(values[i]+c); start=end;end=values.size(); }
    for(const auto& p:patterns) for(const auto& v:values) Require(RuleWildcardMatches(p,v)==WildcardOracle(p,v),"literal span differs from DP oracle");
    Require(RuleWildcardMatches(L"项目*?.PDF",L"项目甲乙.pdf"),"Unicode span and wildcard");
    std::cout<<"wildcard_dp_cases="<<patterns.size()*values.size()<<"\n";
}
std::filesystem::path EvidenceRoot() {
    wchar_t value[32768]{}; GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",value,ARRAYSIZE(value));
    Require(value[0]!=0,"isolated evidence required"); return value;
}
std::string Bytes(const std::wstring& path) {
    std::ifstream in{std::filesystem::path(path),std::ios::binary}; return {std::istreambuf_iterator<char>(in),{}};
}
void Capture(HWND h,const wchar_t* name) {
    RECT r{}; GetClientRect(h,&r); HDC dc=GetDC(h), memory=CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=r.right;
    info.bmiHeader.biHeight=-r.bottom; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB; void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0); const auto old=SelectObject(memory,bitmap);
    RedrawWindow(h,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);
    PrintWindow(h,memory,PW_CLIENTONLY); const DWORD size=static_cast<DWORD>(r.right*r.bottom*4);
    BITMAPFILEHEADER header{}; header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+size;
    std::ofstream out{EvidenceRoot()/name,std::ios::binary}; out.write(reinterpret_cast<const char*>(&header),sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader)); out.write(static_cast<char*>(pixels),size);
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(h,dc);
}
void Click(HWND h,int id) { SendMessageW(h,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(h,id))); }
void Choose(HWND h,int id,int n) {
    SendMessageW(GetDlgItem(h,id),CB_SETCURSEL,n,0);
    SendMessageW(h,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(h,id)));
}
void Set(HWND h,int id,const wchar_t* v) { SetWindowTextW(GetDlgItem(h,id),v); }
int uiStep=0, uiMode=0; std::string uiError;
VOID CALLBACK Drive(HWND,UINT,UINT_PTR timer,DWORD) {
    HWND h=FindWindowW(L"Lattice.OrganizeRules",nullptr); if(!h) return;
    try {
        if(uiMode==0) {
            if(uiStep==0) { Capture(h,L"rules-empty.bmp"); Click(h,101); Set(h,105,L"项目文档"); Set(h,107,L"项目*"); Choose(h,108,1);
                Set(h,109,L".pdf;.docx"); Choose(h,115,0); Capture(h,L"rules-edit.bmp"); Click(h,117); ++uiStep; }
        } else if(uiMode==1) {
            if(uiStep==0) { Click(h,101); Set(h,105,L"项目文档"); Choose(h,115,0); Click(h,118); Capture(h,L"rules-invalid.bmp");
                Require(IsWindow(h),"invalid rule closed editor"); Set(h,107,L"项目*"); Choose(h,108,1); Set(h,109,L".pdf;.docx");
                SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE",L"1"); Click(h,118); Capture(h,L"rules-saving.bmp"); ++uiStep; }
            else if(uiStep==1&&IsWindowEnabled(GetDlgItem(h,118))) {
                Require(IsWindow(h),"write failure closed editor"); Capture(h,L"rules-save-error.bmp");
                SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE",nullptr); Click(h,119); ++uiStep; }
        } else if(uiMode==2) { Capture(h,L"rules-saved.bmp"); Click(h,117); ++uiStep; }
        else { Click(h,117); ++uiStep; }
        if(!IsWindow(h)) KillTimer(nullptr,timer);
    } catch(const std::exception& e) { uiError=e.what(); SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE",nullptr); Click(h,117); KillTimer(nullptr,timer); }
}
void DialogTest(HINSTANCE instance,ConfigStore& store,int mode,OrganizeRulesDialog::Result expected) {
    uiMode=mode; uiStep=0; uiError.clear(); const auto timer=SetTimer(nullptr,0,150,Drive);
    const auto result=OrganizeRulesDialog::Show(instance,nullptr,store); KillTimer(nullptr,timer);
    Require(uiError.empty(),uiError.c_str()); Require(result==expected,"editor result"); Require(uiStep>0,"editor not exercised");
}
void RequiredLoad(HINSTANCE instance) {
        const auto originalFile=EvidenceRoot()/L"规则上限.txt"; std::ofstream(originalFile)<<"workspace load";
        constexpr std::uint64_t now=864000000000ULL*1000;
        AutoOrganizePreviewInput input; input.useRules=true;
        input.snapshot.categories.push_back({L"stress-category",L"压力格子",L"stress-monitor",false});
        input.layoutContext.monitors.push_back({L"stress-monitor",{0,0,2560,1440},144});
        AutoOrganizePreviewWindow preview(instance,nullptr,[&](){return input;},[](const Plan&,const LayoutPlan&,HWND){});
        // Required upper-bound load on the real existing preview worker. No
        // desktop takeover, new production worker or extra acceptance stage.

        for(int i=0;i<64;++i) {
            OrganizeRule rule; rule.name=L"负载规则"; rule.targetCategoryId=L"stress-category"; rule.id=L"stress-"+std::to_wstring(i); rule.timeField=RuleTimeField::Any;
            rule.namePattern=i==63?L"*":L"*"+std::wstring(64,L'a')+L"b";
            input.rules.push_back(std::move(rule));
        }
        for(int i=0;i<400;++i) {
            ItemSnapshot value; value.path=originalFile.wstring(); value.kind=DesktopItemKind::File; value.monitorId=L"stress-monitor"; value.id=L"stress-item-"+std::to_wstring(i);
            value.displayName=std::wstring(128,L'a')+L"z.txt";
            value.parsingIdentity=originalFile.wstring()+L"-"+std::to_wstring(i);
            input.snapshot.items.push_back(std::move(value));
        }
        const auto stressBegin=GetTickCount64();
        Require(preview.Create(),"stress preview create"); ShowWindow(preview.Window(),SW_SHOWNORMAL);
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        double maxUi=0, maxMouse=0, maxPaint=0; unsigned dispatches=0;
        while(OrganizeRulesSmokeAccess::Scanning(preview)&&GetTickCount64()-stressBegin<30000) {
            LARGE_INTEGER begin{},mouse{},paint{}; QueryPerformanceCounter(&begin);
            SendMessageW(preview.Window(),WM_MOUSEMOVE,0,MAKELPARAM(20,20));
            QueryPerformanceCounter(&mouse); UpdateWindow(preview.Window()); QueryPerformanceCounter(&paint);
            maxUi=std::max(maxUi,1000.0*(paint.QuadPart-begin.QuadPart)/frequency.QuadPart);
            maxMouse=std::max(maxMouse,1000.0*(mouse.QuadPart-begin.QuadPart)/frequency.QuadPart);
            maxPaint=std::max(maxPaint,1000.0*(paint.QuadPart-mouse.QuadPart)/frequency.QuadPart); ++dispatches;
            MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            Sleep(5);
        }
        const auto stressMs=GetTickCount64()-stressBegin;
        std::cout<<"rules_64x400_ms="<<stressMs<<" ui_dispatches="<<dispatches<<" max_ui_ms="<<maxUi<<" max_mouse_ms="<<maxMouse<<" max_paint_ms="<<maxPaint<<"\n";
        Require(OrganizeRulesSmokeAccess::Ready(preview)&&OrganizeRulesSmokeAccess::Plan(preview).decisions.size()==400,"64x400 background complete");
        Require(dispatches>0&&maxUi<=16,"64x400 UI input exceeds 16ms"); preview.Close();
        const auto cancelBegin=GetTickCount64(); Require(preview.Create(),"stress cancellation create");
        preview.Close(); const auto cancelMs=GetTickCount64()-cancelBegin;
        std::cout<<"rules_64x400_ms="<<stressMs<<" ui_dispatches="<<dispatches<<" max_ui_ms="<<maxUi<<" cancel_create_close_ms="<<cancelMs<<"\n";
        std::atomic<bool> cancelled{true}; Require(BuildRulePlan(input.snapshot,input.rules,now,&cancelled).decisions.empty(),"cancelled rule plan published");
}
}
int RunSmokeOrganizeRules(HINSTANCE instance) {
    try {
        CheckWildcardSpans();
        wchar_t requiredOnly[2]{};
        if(GetEnvironmentVariableW(L"LATTICE_SMOKE_RULES_REQUIRED_ONLY",requiredOnly,ARRAYSIZE(requiredOnly))&&requiredOnly[0]==L'1') { RequiredLoad(instance); return 0; }
        const auto evidence=EvidenceRoot(); std::filesystem::create_directories(evidence);
        constexpr std::uint64_t day=864000000000ULL, now=1000*day;
        OrganizeRule r; r.id=L"r1"; r.name=L"项目文档"; r.namePattern=L"项目*"; r.itemType=RuleItemType::File;
        r.extensions=L".pdf;.docx"; r.newCategoryName=L"文档";
        ItemSnapshot item; item.id=L"a"; item.parsingIdentity=L"a-identity"; item.path=L"F:\\fixture\\项目A.PDF";
        item.displayName=L"项目A.PDF"; item.kind=DesktopItemKind::File; item.monitorId=L"\\\\.\\DISPLAY1";
        item.modificationTime=now-day; item.creationTime=now-31*day;
        Require(RuleWildcardMatches(L"*CODE.?XE",L"F:\\Code.exe"),"wildcard case and ?");
        Require(!RuleWildcardMatches(L"项目?",L"项目AB"),"wildcard length");
        Require(OrganizeRuleMatches(r,item,now),"file type and extensions AND");
        auto wrong=item; wrong.kind=DesktopItemKind::Shortcut; Require(!OrganizeRuleMatches(r,wrong,now),"original kind");
        wrong=item; wrong.path=L"F:\\fixture\\项目A.png"; Require(!OrganizeRuleMatches(r,wrong,now),"extension exclusion");
        auto timed=r; timed.timeField=RuleTimeField::Modified; timed.days=1;
        Require(OrganizeRuleMatches(timed,item,now),"time boundary inclusive"); timed.age=RuleAge::Older;
        Require(!OrganizeRuleMatches(timed,item,now),"older boundary exclusive"); timed.timeField=RuleTimeField::Created;
        Require(OrganizeRuleMatches(timed,item,now),"created older"); wrong=item; wrong.creationTime=0;
        Require(!OrganizeRuleMatches(timed,wrong,now),"unknown timestamp fails closed");
        auto shortcut=r; shortcut.itemType=RuleItemType::Shortcut; shortcut.extensions.clear(); shortcut.namePattern.clear(); shortcut.targetPattern=L"*\\Code.exe";
        wrong=item; wrong.kind=DesktopItemKind::Shortcut; wrong.targetPath=L"F:\\Apps\\Code.EXE";
        Require(OrganizeRuleMatches(shortcut,wrong,now),"resolved shortcut target"); wrong.targetPath.clear();
        Require(!OrganizeRuleMatches(shortcut,wrong,now),"unresolved target"); wrong.path=L"::{fixture}";
        Require(!OrganizeRuleMatches(shortcut,wrong,now),"virtual shell item");
        std::wstring error; auto invalid=r; invalid.namePattern.clear(); invalid.itemType=RuleItemType::Any; invalid.extensions.clear();
        Require(!ValidateOrganizeRules({invalid},error),"no catch all"); Require(!ValidateOrganizeRules({r,r},error),"duplicate rule IDs");
        invalid=r; invalid.extensions=L".pdf;"; Require(!ValidateOrganizeRules({invalid},error),"malformed extension list");
        Snapshot snapshot; snapshot.items={item}; auto second=r; second.id=L"r2"; second.newCategoryName=L"其它";
        auto plan=BuildRulePlan(snapshot,{r,second},now);
        Require(plan.decisions.size()==1&&plan.decisions[0].selected&&plan.groups.size()==2&&plan.groups[0].createNewCategory,"one item new group");
        Require(plan.groups[0].name==L"文档"&&plan.groups[0].id.find_first_of(L"\\/:?")==std::wstring::npos,"priority and safe ID");
        auto disabled=r; disabled.enabled=false;
        Require(BuildRulePlan(snapshot,{disabled,second},now).decisions[0].targetCategoryName==L"其它","disabled rule skipped");
        Require(BuildRulePlan(snapshot,{second,r},now).decisions[0].targetCategoryName==L"其它","priority reorder");
        auto absent=r; absent.newCategoryName.clear(); absent.targetCategoryId=L"deleted-category";
        Require(!BuildRulePlan(snapshot,{absent,second},now).decisions[0].selected,"missing first target cannot fall through");
        std::vector<OrganizeRule> tooMany(65,r);
        Require(!ValidateOrganizeRules(tooMany,error),"rule count bound");
        ExistingCategorySnapshot cat; cat.id=L"existing"; cat.name=L"文档"; cat.monitorId=item.monitorId; snapshot.categories.push_back(cat);
        plan=BuildRulePlan(snapshot,{r,second},now); Require(plan.decisions[0].targetCategoryId==cat.id&&plan.groups[0].existingCategory,"repeat reuses category");
        snapshot.categories[0].locked=true; plan=BuildRulePlan(snapshot,{r,second},now);
        Require(!plan.decisions[0].selected,"locked first match cannot fall through"); snapshot.categories[0].locked=false;
        snapshot.items[0].sourceCategoryId=cat.id; plan=BuildRulePlan(snapshot,{r},now); Require(!plan.decisions[0].selected,"same category no change");
        snapshot.items[0].sourceCategoryId.clear(); r.newCategoryName.clear(); r.targetCategoryId=cat.id;
        ConfigStore store; AppConfig config; CategoryConfig category; category.id=cat.id; category.name=cat.name;
        category.storageFolder=L"existing"; category.layout.monitorId=item.monitorId; config.categories={category};
        config.window.x=731; Require(store.SaveAppConfig(config),"config setup");
        const auto healthyConfig=Bytes(store.ConfigPath());
        for (const char* count : {"invalid", "999999999999999999999999999999", "65"}) {
            { std::ofstream broken{std::filesystem::path(store.ConfigPath()),std::ios::binary};
              broken << "schemaVersion=14\norganizeRule.count=" << count << "\n"; }
            const auto brokenBytes=Bytes(store.ConfigPath());
            const auto broken=store.LoadAppConfig();
            Require(broken.organizeRulesReadError,"corrupt rule count accepted");
            Require(!store.SaveAppConfig(broken)&&Bytes(store.ConfigPath())==brokenBytes,"corrupt rules overwritten");
        }
        { std::ofstream restored{std::filesystem::path(store.ConfigPath()),std::ios::binary}; restored << healthyConfig; }
        const auto before=Bytes(store.ConfigPath()); DialogTest(instance,store,0,OrganizeRulesDialog::Result::Cancelled);
        Require(before==Bytes(store.ConfigPath()),"cancel changed config");
        DialogTest(instance,store,1,OrganizeRulesDialog::Result::Preview);
        const auto saved=store.LoadAppConfig(); Require(saved.organizeRules.size()==1&&saved.organizeRules[0].extensions==L".pdf;.docx"&&saved.window.x==731,"editor persisted rules only");
        DialogTest(instance,store,2,OrganizeRulesDialog::Result::Cancelled);
        auto rules=saved.organizeRules; rules[0].name=L"更新规则";
        auto latest=saved; latest.window.x=882; latest.categories[0].name=L"已并发改名";
        Require(store.SaveAppConfig(latest),"concurrent layout setup");
        Require(store.SaveOrganizeRulesAsync(saved.organizeRules,rules,nullptr,WM_APP+80,1)&&ConfigStore::DrainPendingWrites(5000),"rules mutation drain");
        const auto merged=store.LoadAppConfig(); Require(merged.window.x==882&&merged.categories[0].name==L"已并发改名"&&merged.organizeRules==rules,"rules lost latest layout");
        auto stale=saved; stale.window.x=999; Require(store.SaveAppConfig(stale,true)&&store.LoadAppConfig().organizeRules==rules,"stale layout lost rule edit");
        const auto conflictBefore=Bytes(store.ConfigPath());
        Require(store.SaveOrganizeRulesAsync(saved.organizeRules,{},nullptr,WM_APP+80,2)&&ConfigStore::DrainPendingWrites(5000),"conflict request");
        Require(conflictBefore==Bytes(store.ConfigPath()),"stale rules overwrite");
        AutoOrganizeApplyRequest request; request.transactionId=L"rule-apply"; request.usesRules=true; request.expectedRules=rules;
        AutoOrganizeMoveRequest move; move.item.id=item.id; move.item.path=item.path; move.item.displayName=item.displayName;
        move.identity=item.path; move.expectedSourceIndex=-1; move.targetCategoryId=cat.id; request.moves={move};
        Require(store.ApplyAutoOrganizeAsync(request,nullptr,WM_APP+80,3)&&ConfigStore::DrainPendingWrites(5000),"rule apply");
        Require(store.LoadAppConfig().categories[0].itemIds==std::vector<std::wstring>{item.id},"apply membership");
        Require(store.UndoAutoOrganizeAsync(nullptr,WM_APP+80,4)&&ConfigStore::DrainPendingWrites(5000),"rule undo");
        Require(store.LoadAppConfig().categories[0].itemIds.empty()&&store.LoadAppConfig().organizeRules==rules,"undo lost rules");
        request.expectedRules=saved.organizeRules; AppConfig candidate; AutoOrganizeTransactionResult result;
        Require(!ApplyAutoOrganizeTransaction(store.LoadAppConfig(),request,candidate,result)&&result.conflict,"apply stale rules accepted");
        const auto beforeFailure=Bytes(store.ConfigPath()); SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE",L"1");
        const bool queued=store.SaveOrganizeRulesAsync(rules,{},nullptr,WM_APP+80,5); const bool drained=ConfigStore::DrainPendingWrites(5000);
        SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE",nullptr);
        Require(queued&&!drained&&beforeFailure==Bytes(store.ConfigPath()),"failed save changed bytes");
        const DWORD gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS), user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        for(int i=0;i<20;++i) DialogTest(instance,store,3,OrganizeRulesDialog::Result::Cancelled);
        const DWORD finalGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS), finalUser=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
        std::cout<<"editor resource before="<<gdi<<","<<user<<" after="<<finalGdi<<","<<finalUser<<"\n";
        Require(finalGdi<=gdi&&finalUser<=user,"editor resource leak");
        // Exercise the existing background preview and its actual hit route,
        // using a real workspace file and the same persisted rules/transaction.
        const auto originalFile=evidence/L"项目验证.txt"; std::ofstream(originalFile)<<"workspace original";
        auto previewRule=rules[0]; previewRule.extensions=L".txt"; previewRule.timeField=RuleTimeField::Modified;
        Require(store.SaveOrganizeRulesAsync(rules,{previewRule},nullptr,WM_APP+80,6)&&ConfigStore::DrainPendingWrites(5000),"preview rules save");
        AutoOrganizePreviewInput input; input.useRules=true; input.rules={previewRule};
        ItemSnapshot real=item; real.path=originalFile.wstring(); real.parsingIdentity=real.path; real.displayName=L"项目验证.txt";
        real.creationTime=real.modificationTime=0; input.snapshot.items={real}; input.snapshot.categories={cat};
        input.layoutContext.monitors.push_back({item.monitorId,{0,0,2560,1440},144});
        bool applied=false; AutoOrganizePreviewWindow preview(instance,nullptr,[&](){return input;},
            [&](const Plan& p,const LayoutPlan&,HWND) {
                AutoOrganizeApplyRequest req; req.transactionId=p.id; req.usesRules=true; req.expectedRules=p.evaluatedRules;
                AutoOrganizeMoveRequest actual; actual.item.id=real.id; actual.item.path=real.path; actual.item.displayName=real.displayName;
                actual.identity=real.path; actual.expectedSourceIndex=-1; actual.targetCategoryId=cat.id; req.moves={actual};
                applied=store.ApplyAutoOrganizeAsync(req,nullptr,WM_APP+80,7);
            });
        Require(preview.Create(),"rule preview create"); ShowWindow(preview.Window(),SW_SHOWNORMAL);
        const auto deadline=GetTickCount64()+5000;
        while(!OrganizeRulesSmokeAccess::Ready(preview)&&GetTickCount64()<deadline) {
            MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(5);
        }
        Require(OrganizeRulesSmokeAccess::Ready(preview),"rule preview ready");
        const auto& ready=OrganizeRulesSmokeAccess::Plan(preview);
        Require(ready.usesRules&&ready.decisions.size()==1&&ready.decisions[0].selected&&ready.evaluatedRules==input.rules,"background rule plan");
        Capture(preview.Window(),L"rules-preview.bmp"); Require(OrganizeRulesSmokeAccess::ClickApply(preview)&&applied,"preview actual apply hit");
        Require(ConfigStore::DrainPendingWrites(5000),"preview apply drain"); preview.Close();
        ConfigStore reopened; Require(reopened.LoadAppConfig().categories[0].itemIds==std::vector<std::wstring>{real.id},"preview apply restart membership");
        Require(reopened.UndoAutoOrganizeAsync(nullptr,WM_APP+80,8)&&ConfigStore::DrainPendingWrites(5000),"preview restart undo");
        Require(reopened.LoadAppConfig().categories[0].itemIds.empty()&&std::filesystem::exists(originalFile),"preview undo original unchanged");
        RequiredLoad(instance);
        std::cout<<"PASS: four conditions AND, first match, locked/missing, safe single group and repeat reuse; editor cancel/invalid/failure/save-preview; latest field mutation, stale conflict, apply/undo, atomic failure; existing background rule preview, real workspace file time, apply hit, restart undo, original unchanged; 20 reopen no GDI/USER growth\n";
        return 0;
    } catch(const std::exception& e) { SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE",nullptr); std::cerr<<"Rule smoke failed: "<<e.what()<<"\n"; return 401; }
}
