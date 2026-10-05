#include "deployment.h"
#include "vfs_metadata.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>

bool DeployGuestFeatures(const std::string& root, AAssetManager* assets,
                         const goblin::RunLimits& limits, goblin::SessionControl* control, std::string* error) {
    goblin::Vfs vfs;
    if (!vfs.Mount(root,error)) return false;
    const std::string stage="/var/lib/goblin/deployment";
    for (const auto& directory : {std::string("/var/lib/goblin"),stage}) {
        int rc=vfs.Mkdir(directory,0700);
        if (rc<0 && rc!=-EEXIST) { *error="Preparing guest deployment: "+std::string(strerror(-rc)); return false; }
        struct stat st{};
        if (vfs.Stat(directory,&st,false)<0 || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0022)) {
            *error="Guest deployment directory must be owned by root and protected from other users"; return false;
        }
    }
    AAssetDir* directory=AAssetManager_openDir(assets,"deployment");
    if (!directory) { *error="Guest deployment assets are missing"; return false; }
    bool ok=true;
    while (const char* name=AAssetDir_getNextFileName(directory)) {
        if (strchr(name,'/') || !strcmp(name,".") || !strcmp(name,"..")) { ok=false; break; }
        AAsset* asset=AAssetManager_open(assets,("deployment/"+std::string(name)).c_str(),AASSET_MODE_BUFFER);
        if (!asset) { ok=false; break; }
        const std::string destination=stage+"/"+name, temporary=destination+".new";
        vfs.Unlink(temporary,false);
        goblin::HostFile file(vfs.Open(temporary,O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,0600));
        const char* data=static_cast<const char*>(AAsset_getBuffer(asset));
        size_t size=AAsset_getLength64(asset), at=0;
        ok=file.fd>=0 && data && vfs.CheckGrowth(file.fd,size)==0;
        while (ok && at<size) {
            ssize_t n=write(file.fd,data+at,size-at);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { ok=false; break; }
            at+=n;
        }
        if (ok) ok=fsync(file.fd)==0 && vfs.Rename(temporary,destination)==0;
        AAsset_close(asset);
        if (!ok) break;
    }
    AAssetDir_close(directory);
    if (!ok) { *error="Staging packaged guest deployment failed"; return false; }
    std::string output, errors;
    auto result=goblin::RunInRoot(root,{"/bin/sh",stage+"/configure.sh"},limits,control,false,&output,&errors);
    if (!result.exited || result.status || !result.error.empty()) {
        *error="Guest deployment failed: "+result.error+"\n"+output+errors; return false;
    }
    return true;
}
