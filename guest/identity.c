#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/auxv.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"identity line %d: %s (errno %d)\n",__LINE__,#x,errno); exit(1); } } while (0)
static void path(char* out, const char* root, const char* leaf) { snprintf(out,256,"%s/%s",root,leaf); }
static void file(const char* name, mode_t mode, uid_t uid, gid_t gid) {
    int fd=open(name,O_WRONLY|O_CREAT|O_EXCL,mode);CHECK(fd>=0);CHECK(write(fd,"secret",6)==6);CHECK(fchown(fd,uid,gid)==0);CHECK(close(fd)==0);
}
static int denied(const char* name, int flags) { errno=0; int fd=open(name,flags,0600);if(fd>=0)close(fd);return fd==-1&&errno==EACCES; }
int main(int argc,char** argv) {
    char base[128], name[256], other[256];
    if(argc==3&&!strcmp(argv[1],"user")) {
        strcpy(base,argv[2]);
        CHECK(getuid()==1000&&geteuid()==1000&&getgid()==1000&&getegid()==1000);
        CHECK(getauxval(AT_UID)==1000&&getauxval(AT_EUID)==1000&&getauxval(AT_GID)==1000&&getauxval(AT_EGID)==1000);
        uid_t r,e,s;CHECK(getresuid(&r,&e,&s)==0&&r==1000&&e==1000&&s==1000);
        gid_t groups[4];CHECK(getgroups(4,groups)==1&&groups[0]==2000);
        CHECK(setuid(0)==-1&&errno==EPERM);CHECK(setresuid(-1,0,-1)==-1&&errno==EPERM);
        CHECK(setgid(0)==-1&&errno==EPERM);CHECK(setgroups(0,0)==-1&&errno==EPERM);
        CHECK(kill(getppid(),0)==-1&&errno==EPERM);
        path(name,base,"private/data");CHECK(denied(name,O_RDONLY));
        path(name,base,"private");CHECK(chdir(name)==-1&&errno==EACCES);
        path(name,base,"secret");CHECK(denied(name,O_WRONLY|O_TRUNC));CHECK(access(name,R_OK)==-1&&errno==EACCES);
        CHECK(chmod(name,0777)==-1&&errno==EPERM);CHECK(chown(name,1000,1000)==-1&&errno==EPERM);
        CHECK(fchmodat(AT_FDCWD,name,0777,AT_SYMLINK_NOFOLLOW)==-1&&errno==EPERM);
        path(name,base,"group");int fd=open(name,O_RDONLY);CHECK(fd>=0);close(fd);
        CHECK(denied(name,O_WRONLY));
        path(name,base,"home/new");umask(0077);fd=open(name,O_RDWR|O_CREAT|O_EXCL,0666);CHECK(fd>=0);
        struct stat st;CHECK(fstat(fd,&st)==0&&st.st_uid==1000&&st.st_gid==1000&&(st.st_mode&0777)==0600);close(fd);
        CHECK(chown(name,-1,2000)==0);CHECK(chown(name,1001,-1)==-1&&errno==EPERM);
        path(name,base,"shared/new");fd=open(name,O_CREAT|O_RDWR,0600);CHECK(fd>=0);CHECK(fstat(fd,&st)==0&&st.st_gid==2000);close(fd);
        path(name,base,"sticky/other");CHECK(unlink(name)==-1&&errno==EPERM);
        path(other,base,"sticky/renamed");CHECK(rename(name,other)==-1&&errno==EPERM);
        path(name,base,"sticky/link");CHECK(symlink("../secret",name)==0);CHECK(lstat(name,&st)==0&&S_ISLNK(st.st_mode)&&st.st_uid==1000);
        CHECK(rename(name,other)==0);CHECK(lstat(other,&st)==0&&st.st_uid==1000);char target[64];CHECK(readlink(other,target,sizeof(target))==9);CHECK(unlink(other)==0);
        snprintf(name,sizeof(name),"/proc/%d/environ",getppid());CHECK(denied(name,O_RDONLY));
        fd=open("/proc/self/status",O_RDONLY);CHECK(fd>=0);char status[2048];int n=read(fd,status,sizeof(status)-1);CHECK(n>0);status[n]=0;close(fd);
        CHECK(strstr(status,"Uid:\t1000\t1000\t1000\t1000"));CHECK(strstr(status,"Groups:\t2000"));
        int sockets[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);struct ucred cred; socklen_t size=sizeof(cred);
        CHECK(getsockopt(sockets[0],SOL_SOCKET,SO_PEERCRED,&cred,&size)==0&&cred.pid==getpid()&&cred.uid==1000&&cred.gid==1000);close(sockets[0]);close(sockets[1]);
        CHECK(setresuid(-1,1000,-1)==0);return 0;
    }
    CHECK(getuid()==0);CHECK(chmod("/",0755)==0);CHECK(chmod("/tmp",01777)==0);
    snprintf(base,sizeof(base),"/tmp/identity-%d",getpid());CHECK(mkdir(base,0755)==0);
    path(name,base,"private");CHECK(mkdir(name,0700)==0);path(name,base,"private/data");file(name,0644,0,0);
    path(name,base,"secret");file(name,0600,0,0);
    path(name,base,"group");file(name,0440,0,2000);
    path(name,base,"home");CHECK(mkdir(name,0700)==0);CHECK(chown(name,1000,1000)==0);
    path(name,base,"shared");CHECK(mkdir(name,02770)==0);CHECK(chown(name,0,2000)==0);CHECK(chmod(name,02770)==0);
    path(name,base,"sticky");CHECK(mkdir(name,01777)==0);CHECK(chmod(name,01777)==0);
    path(name,base,"sticky/other");file(name,0600,1001,1001);
    pid_t child=fork();CHECK(child>=0);
    if(!child) { gid_t group=2000;CHECK(setgroups(1,&group)==0);CHECK(setresgid(1000,1000,1000)==0);CHECK(setresuid(1000,1000,1000)==0);execl(argv[0],argv[0],"user",base,NULL);CHECK(0); }
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    path(name,base,"secret");struct stat st;CHECK(stat(name,&st)==0&&st.st_size==6);
    const char* entries[]={"private/data","secret","group","home/new","shared/new","sticky/other"};
    for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++){path(name,base,entries[i]);CHECK(unlink(name)==0);}
    const char* dirs[]={"private","home","shared","sticky"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++){path(name,base,dirs[i]);CHECK(rmdir(name)==0);}CHECK(rmdir(base)==0);
    puts("credentials, user permissions, ownership, procfs and peer identity ok");return 0;
}
