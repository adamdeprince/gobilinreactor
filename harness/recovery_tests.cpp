#include "recovery_tests.h"
#include "metric_report.h"
#include "session.h"
#include <atomic>
#include <chrono>
#include <signal.h>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

bool RunRecoveryTests(const std::string& data,AAssetManager* assets,
                      const std::function<void(const std::string&)>& log) {
    goblin::HostFile lock(open((data+"/debian.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600));
    if(lock.fd<0||flock(lock.fd,LOCK_EX|LOCK_NB)<0){log("FAILED: persistent session already active");return false;}
    const std::string root=data+"/debian",base="/tmp/goblin-upgrade-fixture";
    goblin::Vfs vfs;std::string error;
    if(!vfs.Mount(root,&error)){log("FAILED: prepare persistent Debian before recovery tests: "+error);return false;}
    for(const char* name:{"recovery-1.deb","recovery-2.deb","bench","forkbench"}) {
        AAsset* asset=AAssetManager_open(assets,name,AASSET_MODE_BUFFER);
        if(!asset){log("FAILED: missing recovery asset "+std::string(name));return false;}
        goblin::HostFile fd(vfs.Open("/tmp/goblin-"+std::string(name),O_WRONLY|O_CREAT|O_TRUNC,0755));
        bool ok=fd.fd>=0&&write(fd.fd,AAsset_getBuffer(asset),AAsset_getLength64(asset))==AAsset_getLength64(asset);
        AAsset_close(asset);if(!ok){log("FAILED: installing recovery asset");return false;}
    }
    goblin::RunLimits limits;limits.wall_time_ms=120000;
    auto run=[&](const std::string& name,const std::string& command,bool expect_success=true,std::string* captured=nullptr) {
        log("-- Recovery: "+name+" --");std::string out,err;
        auto r=goblin::RunInRoot(root,{"/bin/bash","--noprofile","--norc","-ec",command},limits,nullptr,false,&out,&err);
        bool ok=r.exited&&r.error.empty()&&(expect_success?r.status==0:r.status!=0);
        log(std::string(ok?"PASS: ":"FAILED: ")+name+" status="+std::to_string(r.status)+" "+r.error);
        log(MetricReport(name,r));
        if(captured)*captured=out+err;
        if(!ok) {std::string all=out+err;for(size_t at=0;at<all.size();){size_t end=all.find('\n',at);if(end==std::string::npos)end=all.size();log(all.substr(at,end-at));at=end+1;}}
        return ok;
    };
    auto dpkg=[&](const std::string& target) {return "dpkg --force-script-chrootless --force-confold --root='"+target+"' ";};
    auto seed=[&](const std::string& target) {
        return "mkdir -p '"+target+"/var/lib/dpkg/updates' '"+target+"/var/lib/dpkg/info' '"+target+"/root'; touch '"+target+"/var/lib/dpkg/status'; "+dpkg(target)+"-i /tmp/goblin-recovery-1.deb; printf 'user configuration\\n' >'"+target+"/etc/goblin-recovery.conf'; printf 'keep my notes\\n' >'"+target+"/root/notes'";
    };
    auto verify=[&](const std::string& target) {
        return "test -z \"$("+dpkg(target)+"--audit)\"; test \"$(dpkg-query --admindir='"+target+"/var/lib/dpkg' -W -f='${Status}|${Version}' goblin-recovery-fixture)\" = 'install ok installed|2'; test \"$(cat '"+target+"/usr/share/goblin-recovery/value')\" = version-2; test \"$(cat '"+target+"/var/lib/goblin-proof/configured')\" = version-2; test \"$(cat '"+target+"/etc/goblin-recovery.conf')\" = 'user configuration'; test \"$(cat '"+target+"/root/notes')\" = 'keep my notes'; test \"$(wc -c <'"+target+"/usr/share/goblin-recovery/payload')\" = 2097152";
    };
    const std::string killed=base+"/killed",state=data+"/recovery-phase";
    if(access(state.c_str(),F_OK)!=0) {
        if(!run("install original package in isolated database","rm -rf -- '"+base+"'; "+seed(killed)))return false;
        goblin::SessionControl control;std::atomic<bool> finished{false};goblin::RunResult result;std::string output,errors;
        std::thread worker([&]{result=goblin::RunInRoot(root,{"/bin/bash","-ec",dpkg(killed)+"-i /tmp/goblin-recovery-2.deb"},limits,&control,false,&output,&errors);finished=true;});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        bool reached=false;
        while(!finished&&std::chrono::steady_clock::now()<deadline) {
            if(access((root+killed+"/var/lib/goblin-proof/checkpoint").c_str(),F_OK)==0){reached=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if(reached) {
            // These markers are outside the guest root. The host runner restarts
            // the APK after this deliberate SIGKILL, preserving the package DB.
            bool durable=true;
            for(const auto& path:{state,data+"/recovery-restart"}) {
                goblin::HostFile fd(open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600));
                const char text[]="postinst-checkpoint\n";
                durable&=fd.fd>=0&&write(fd.fd,text,sizeof(text)-1)==sizeof(text)-1&&fsync(fd.fd)==0;
            }
            goblin::HostFile directory(open(data.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC));
            durable&=directory.fd>=0&&fsync(directory.fd)==0;
            if(durable) {
                log("PASS: reached version-2 postinst; killing the Android broker process");
                kill(getpid(),SIGKILL);_exit(99);
            }
        }
        control.stop=true;worker.join();log("FAILED: upgrade did not reach a durable restart checkpoint: "+result.error+output+errors);return false;
    }
    if(!run("resume configuration after Android process death",
        "test -f '"+killed+"/var/lib/goblin-proof/checkpoint'; touch '"+killed+"/var/lib/goblin-proof/continue'; "+dpkg(killed)+"--configure -a; "+verify(killed)))return false;
    const std::string full=base+"/full";
    if(!run("prepare disk exhaustion upgrade",seed(full)+"; touch '"+full+"/var/lib/goblin-proof/continue'"))return false;
    if(vfs.SetDiskLimit(limits.disk_bytes)<0){log("FAILED: reading baseline disk usage");return false;}
    limits.disk_bytes=vfs.disk_usage()+(256u<<10);
    std::string failure;
    bool rejected=run("upgrade fails at the disk quota",dpkg(full)+"-i /tmp/goblin-recovery-2.deb",false,&failure);
    if(!rejected||failure.find("No space left on device")==std::string::npos){log("FAILED: expected real dpkg ENOSPC: "+failure);return false;}
    limits={};limits.wall_time_ms=120000;
    if(!run("retry after restoring disk capacity",dpkg(full)+"-i /tmp/goblin-recovery-2.deb; "+verify(full)))return false;
    if(!run("main Debian database remains clean","test -z \"$(dpkg --audit)\""))return false;
    for(unsigned sample=0;sample<5;sample++) {
        auto r=goblin::RunInRoot(root,{"/bin/true"},limits,nullptr,false,nullptr,nullptr);
        if(!r.exited||r.status||!r.error.empty()){log("FAILED: startup benchmark");return false;}
        log(MetricReport("startup_true_"+std::to_string(sample),r));
    }
    for(const char* bench:{"bench","forkbench"}) {
        std::string out,err;auto r=goblin::RunInRoot(root,{"/tmp/goblin-"+std::string(bench)},limits,nullptr,false,&out,&err);
        log(MetricReport(bench,r));
        if(!r.exited||r.status||!r.error.empty()){log("FAILED: "+std::string(bench)+r.error+err);return false;}
        log("PASS: "+out);
    }
    if(!run("remove isolated recovery fixtures","rm -rf -- '"+base+"' /tmp/goblin-recovery-1.deb /tmp/goblin-recovery-2.deb /tmp/goblin-bench /tmp/goblin-forkbench"))return false;
    unlink(state.c_str());unlink((data+"/recovery-restart").c_str());return true;
}
