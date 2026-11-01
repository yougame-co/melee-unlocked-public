"""Round 38c offline regressions for Windows-only status and version diagnostics."""
from pathlib import Path
import unittest
import online_menu_test as menu
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


class Round38cTest(unittest.TestCase):
    def compile(self, name, text, *defines):
        menu.OnlineMenuTest().compile_fixture(name, text, *defines)

    def test_version_failure_steps_and_no_sensitive_diagnostics(self):
        net = read('port/runtime/hle/slippi_net.cpp')
        check = body(net, 'CheckOnlineCompatibility')
        self.compile('38c-version', r'''
#include <nlohmann/json.hpp>
#include <string>
#include <mutex>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
using json = nlohmann::json;
std::string logged;
namespace host { void log(const char* f,...) { char b[256]; va_list a;va_start(a,f);vsnprintf(b,sizeof b,f,a);va_end(a);logged=b; } }
namespace report {
struct HttpFailure { const char* step; unsigned long code; };
HttpFailure last_http_failure() { return {"send-request",12029}; }
bool http_get(const std::string&,std::string*,int*) { return false; }
}
const char* SLIPPI_SEMVER="3.6.4";
std::string str_or(const json& j,const char* k,const char* fallback) {
 auto it=j.find(k);return it!=j.end() && it->is_string()?it->get<std::string>():fallback;
}
bool release_version_valid(const std::string& s){return s=="3.6.4" || s=="3.6.5";}
bool version_newer(const std::string& s,const char*){return s=="3.6.5";}
struct UserInfo {std::string uid="synthetic-user",latest_version;};
using VersionFetch=std::function<bool(const std::string&,std::string*,int*)>;
struct User { UserInfo info_;std::mutex mutex_;UserInfo GetUserInfo(){return info_;}
bool check(std::string* error,VersionFetch fetch={}) {
''' + check + r'''
} };
int main(){
 User u;std::string error;int checks=0;
 auto expect=[&](const char* step){if(logged!=std::string("slippi: version check failed: ")+step)std::abort();++checks;};
 if(u.check(&error))std::abort();expect("send-request 12029");
 auto fetch=[&](bool ok,int status,std::string text){return [=](const std::string&,std::string* out,int* code){*out=text;*code=status;return ok;};};
 if(u.check(&error,fetch(false,0,"sensitive-response")))std::abort();expect("http-get 0");
 if(u.check(&error,fetch(true,503,"sensitive-response")))std::abort();expect("http-status 503");
 if(u.check(&error,fetch(true,200,"broken")))std::abort();expect("json 0");
 if(u.check(&error,fetch(true,200,"{}")))std::abort();expect("latest-version 0");
 if(u.check(&error,fetch(true,200,R"({"latestVersion":"3.6.5"})")))std::abort();expect("update-required 0");
 if(error!="Slippi 3.6.5 is out. Update Melee Unlocked")std::abort();++checks;
 if(!u.check(&error,fetch(true,200,R"({"latestVersion":"3.6.4"})")) || !error.empty())std::abort();++checks;
 u.info_.uid.clear();if(u.check(&error))std::abort();expect("signed-out 0");
 printf("version diagnostics: %d checks passed\n",checks);
}
''')

    def test_status_switch_reason_persistence_and_generation_fence(self):
        link = read('port/runtime/hle/yougame_link.cpp')
        production = '\n'.join('void ' + name + signature + '{' + body(link, name) + '}\n'
                               for name, signature in (
                                   ('set_status', '(const std::string& s)'),
                                   ('blend_slippi_stopped', '(uint64_t epoch, const std::string& reason)')))
        production += 'std::string slippi_link_status(const std::string& status) {' + body(link, 'slippi_link_status') + '}\n'
        self.compile('38c-status', r'''
#include "yougame_blend.h"
#include <mutex>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace blend=yougame::blend;
namespace host {void log(const char*,...) {}}
namespace slippi_linking {const char* kSearching="other-search";enum class Screen{Search};struct P {std::string line(Screen){return "link";}} presentation;}
std::mutex g_mutex;
std::string g_status=blend::kSearchBoth,g_blend_reason;
uint64_t g_status_since=0;
uint64_t now_ms(){return 7;}
bool search_stopped(){return false;}
struct Generation {bool current(uint64_t epoch){return epoch==3;}} g_search_generation;
''' + production + r'''
int main(){int checks=0;
 auto check=[&](bool ok){if(!ok)std::abort();++checks;};
 blend_slippi_stopped(2,"offline");check(g_status==blend::kSearchBoth && g_blend_reason.empty());
 blend_slippi_stopped(3,"Could not check Slippi version. Try again.");
 check(g_status==blend::kSearchYouGameOnly);
 check(slippi_link_status(g_status)=="Searching YouGame players\nSlippi isn't reachable right now");
 set_status(blend::kSearchBoth);check(g_status==blend::kSearchYouGameOnly);
 blend_slippi_stopped(3,"Slippi 3.6.5 is out. Update Melee Unlocked");
 check(slippi_link_status(g_status)=="Searching YouGame players\nSlippi 3.6.5 is out. Update Melee Unlocked");
 g_status="connecting";g_status_since=99;g_blend_reason.clear();
 blend_slippi_stopped(2,"offline");check(g_status=="connecting" && g_blend_reason.empty() && g_status_since==99);
 blend_slippi_stopped(3,"offline");check(g_status=="connecting" && g_status_since==99);
 check(g_blend_reason=="Slippi isn't reachable right now");
 set_status(blend::kSearchBoth);check(g_status==blend::kSearchYouGameOnly);
 check(slippi_link_status(g_status)=="Searching YouGame players\nSlippi isn't reachable right now");
 g_blend_reason="current reason";blend_slippi_stopped(2,"stale failure");check(g_blend_reason=="current reason");
 printf("blend status: %d checks passed\n",checks);
}
''')
        net = read('port/runtime/hle/slippi_net.cpp')
        blend = body(net, 'blendMatchmaking')
        stopped = blend[blend.index('const auto slippi_failed ='):blend.index('  for (;;) {')]
        self.assertIn('alone = coord.yougame_only();', stopped)
        self.assertIn('if (alone) yougame::blend_slippi_stopped(search.epoch, error);', stopped)
        blocked = blend[blend.index('if (IsBlockedMatch('):blend.index('bool won_slippi')]
        # A blocked match takes a new ticket, as Slippi's own search does; it is not a stop.
        self.assertNotIn('slippi_failed(', blocked)
        self.assertIn('slippi = Slippi::Checking;', blocked)
        self.assertIn('g_blend_reason.clear();', body(link, 'begin_search'))
        ui = read('port/runtime/gx/pc_settings.cpp')
        self.assertIn('status == yougame::blend::kSearchBoth || status == yougame::blend::kSearchYouGameOnly', ui)
        self.assertIn('ImGui::TextUnformatted(yougame::slippi_link_status(status).c_str())', ui)

    def test_http_failure_branches_capture_error_before_cleanup(self):
        http = body(read('port/runtime/hle/slippi_report.cpp'), 'http')
        types = '''#include <cstdint>
using DWORD=uint32_t;using WORD=uint16_t;using USHORT=uint16_t;using BOOL=int;
using LPCWSTR=const wchar_t*;using LPWSTR=wchar_t*;using LPVOID=void*;
using LPDWORD=DWORD*;using DWORD_PTR=uintptr_t;
#define TRUE 1
'''
        winhttp = read('port/linux/compat/winhttp.h').replace('#include "windows.h"', types).replace('#pragma once', '')
        self.compile('38c-http', winhttp + r'''
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
int mode=0,closed=0,queries=0;DWORD last_error=0;
std::string logged;
struct HttpFailure {const char* step;unsigned long code;};
thread_local HttpFailure http_failure{"http-get",0};
namespace host {void log(const char* f,...){char b[256];va_list a;va_start(a,f);vsnprintf(b,sizeof b,f,a);va_end(a);logged=b;}}
const wchar_t* USER_AGENT=L"test";
std::wstring widen(const std::string&){return L"synthetic";}
DWORD GetLastError(){return last_error;}
bool step(int at){if(mode==at){last_error=1000+at;return false;}return true;}
BOOL WinHttpCrackUrl(LPCWSTR,DWORD,DWORD,URL_COMPONENTS*){return step(1);}
HINTERNET WinHttpOpen(LPCWSTR,DWORD,LPCWSTR,LPCWSTR,DWORD){return step(2)?(void*)1:nullptr;}
BOOL WinHttpSetTimeouts(HINTERNET,int,int,int,int){return step(3);}
HINTERNET WinHttpConnect(HINTERNET,LPCWSTR,INTERNET_PORT,DWORD){return step(4)?(void*)2:nullptr;}
HINTERNET WinHttpOpenRequest(HINTERNET,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR*,DWORD){return step(5)?(void*)3:nullptr;}
BOOL WinHttpSendRequest(HINTERNET,LPCWSTR,DWORD,LPVOID,DWORD,DWORD,DWORD_PTR){return step(6);}
BOOL WinHttpReceiveResponse(HINTERNET,LPVOID){return step(7);}
BOOL WinHttpQueryHeaders(HINTERNET,DWORD,LPCWSTR,LPVOID b,LPDWORD,LPDWORD){*(DWORD*)b=mode==11?503:200;return step(8);}
BOOL WinHttpQueryDataAvailable(HINTERNET,LPDWORD n){*n=queries++?0:3;return step(9);}
BOOL WinHttpReadData(HINTERNET,LPVOID b,DWORD,LPDWORD n){((char*)b)[0]='a';((char*)b)[1]='b';((char*)b)[2]='c';*n=3;return step(10);}
BOOL WinHttpCloseHandle(HINTERNET){++closed;last_error=42;return TRUE;}
bool http(const char* method,const std::string& url,const std::wstring& headers,const std::string& body,int* status,std::string* response){
''' + http + r'''
}
int main(){int checks=0;const char* steps[]={"","crack-url","open-session","set-timeouts","connect","open-request","send-request","receive-response","query-status","query-data","read-data","http-status"};
 for(mode=0;mode<=11;++mode){closed=queries=0;logged.clear();last_error=0;int status=-1;std::string response="unchanged";
 bool ok=http("GET","synthetic",L"","",&status,&response);
 if(mode>=1 && mode<=10){
  if(ok || http_failure.step!=std::string(steps[mode]) || http_failure.code!=1000ul+mode || response!="unchanged")std::abort();++checks;
 }else{if(!ok || status!=(mode==11?503:200) || response!="abc")std::abort();++checks;}
 int expected=mode==1 || mode==2?0:mode==3 || mode==4?1:mode==5?2:3;
 if(closed!=expected)std::abort();++checks;
 }
 printf("HTTP failures: 12 scenarios, %d checks passed\n",checks);
}
''')

    def test_http_captures_every_failing_step_without_payloads(self):
        report = read('port/runtime/hle/slippi_report.cpp')
        http = body(report, 'http')
        for step in ('crack-url', 'open-session', 'set-timeouts', 'connect', 'open-request',
                     'send-request', 'receive-response', 'query-status', 'query-data', 'read-data'):
            self.assertIn('failed("' + step + '")', http)
        self.assertIn('const DWORD code = GetLastError();', http)
        self.assertIn('http_failure = {"http-status", code};', http)
        compat = body(read('port/linux/compat/compat.cpp'), 'WinHttpReceiveResponse')
        self.assertIn('curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_error);', compat)
        self.assertIn('slippi http: curl-perform %d errno %ld', compat)
        self.assertIn('slippi http: curl-init errno %d', compat)
        for line in http.splitlines():
            if 'host::log(' in line:
                for forbidden in ('url', 'body', 'response', 'out.c_str'):
                    self.assertNotIn(forbidden, line)


if __name__ == '__main__':
    unittest.main()
