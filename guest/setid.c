#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"setid:%d errno=%d\n",__LINE__,errno); exit(1); } } while(0)
static void copy(const char* from,const char* to) {
    int input=open(from,O_RDONLY), output=open(to,O_WRONLY|O_CREAT|O_EXCL,0755); CHECK(input>=0&&output>=0);
    char bytes[4096]; ssize_t n;
    while((n=read(input,bytes,sizeof(bytes)))>0) CHECK(write(output,bytes,n)==n);
    CHECK(n==0);close(input);close(output);
}
static void user(void) { CHECK(setgroups(0,NULL)==0);CHECK(setresgid(1000,1000,1000)==0);CHECK(setresuid(1000,1000,1000)==0); }
static void finished(pid_t child) { int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0); }
int main(int argc,char** argv) {
    if(argc==2) {
        int elevated=!strcmp(argv[1],"elevated"); uid_t r,e,s;
        CHECK(getuid()==1000&&geteuid()==(elevated?0:1000));
        CHECK(getresuid(&r,&e,&s)==0&&r==1000&&e==(elevated?0:1000)&&s==e);
        CHECK(getgid()==1000&&getegid()==(elevated?2000:1000));
        CHECK(getauxval(AT_SECURE)==(unsigned)elevated);
        CHECK(getauxval(AT_UID)==1000&&getauxval(AT_EUID)==e);
        if(elevated) { CHECK(getenv("LD_LIBRARY_PATH")==NULL); CHECK(setuid(0)==0&&getuid()==0); }
        else { CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1); CHECK(setuid(0)==-1&&errno==EPERM); }
        return 0;
    }
    char dir[128], executable[180], script[180], writable[180];
    snprintf(dir,sizeof(dir),"/tmp/setid-%d",getpid());CHECK(mkdir(dir,0755)==0);
    snprintf(executable,sizeof(executable),"%s/program",dir);copy(argv[0],executable);
    CHECK(chown(executable,0,2000)==0&&chmod(executable,06755)==0);
    for(int nnp=0;nnp<2;nnp++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            user();
            if(nnp) { CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);CHECK(prctl(PR_SET_NO_NEW_PRIVS,0,0,0,0)==-1&&errno==EINVAL); }
            CHECK(setenv("LD_LIBRARY_PATH","/tmp/unused-setid-path",1)==0);
            execl(executable,executable,nnp?"nnp":"elevated",NULL);CHECK(0);
        }
        finished(child);
    }
    snprintf(script,sizeof(script),"%s/script",dir);
    int fd=open(script,O_WRONLY|O_CREAT|O_EXCL,0755);CHECK(fd>=0);
    const char text[]="#!/bin/sh\ntest $(id -u) = 1000\n";CHECK(write(fd,text,sizeof(text)-1)==sizeof(text)-1);close(fd);CHECK(chmod(script,04755)==0);
    pid_t child=fork();CHECK(child>=0);if(!child){user();execl(script,script,NULL);CHECK(0);}finished(child);
    snprintf(writable,sizeof(writable),"%s/writable",dir);
    for(int operation=0;operation<4;operation++) {
        fd=open(writable,O_WRONLY|O_CREAT|O_TRUNC,0777);CHECK(fd>=0);CHECK(write(fd,"data",4)==4);close(fd);CHECK(chmod(writable,06777)==0);
        child=fork();CHECK(child>=0);
        if(!child) {
            user();fd=open(writable,O_RDWR|(operation==1?O_TRUNC:0));CHECK(fd>=0);
            if(operation==0) CHECK(write(fd,"x",1)==1);
            if(operation==2) CHECK(ftruncate(fd,2)==0);
            if(operation==3) { void* p=mmap(NULL,sysconf(_SC_PAGESIZE),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);CHECK(munmap(p,sysconf(_SC_PAGESIZE))==0); }
            struct stat st;CHECK(fstat(fd,&st)==0&&!(st.st_mode&06000));close(fd);_exit(0);
        }
        finished(child);
    }
    CHECK(unlink(writable)==0&&unlink(script)==0&&unlink(executable)==0&&rmdir(dir)==0);
    for(int resource=0;resource<RLIM_NLIMITS;resource++) {
        struct rlimit limit;CHECK(getrlimit(resource,&limit)==0);CHECK(setrlimit(resource,&limit)==0);
    }
    struct rlimit invalid={257,256};CHECK(setrlimit(RLIMIT_NOFILE,&invalid)==-1&&errno==EINVAL);
    invalid=(struct rlimit){257,257};CHECK(setrlimit(RLIMIT_NOFILE,&invalid)==-1&&errno==EPERM);
    errno=0;CHECK(getpriority(PRIO_PROCESS,0)==0&&errno==0);CHECK(setpriority(PRIO_PROCESS,0,0)==0);
    int master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0&&grantpt(master)==0&&unlockpt(master)==0);
    char* slave=ptsname(master);CHECK(slave!=NULL);
    struct stat before,after;CHECK(stat(slave,&before)==0&&S_ISCHR(before.st_mode));
    CHECK(chown(slave,1000,5)==0&&chmod(slave,0620)==0);
    int slave_fd=open(slave,O_RDWR|O_NOCTTY);CHECK(slave_fd>=0&&fstat(slave_fd,&after)==0);
    CHECK(after.st_uid==1000&&after.st_gid==5&&(after.st_mode&0777)==0620&&after.st_ino==before.st_ino);
    CHECK(fchown(slave_fd,0,0)==0&&fchmod(slave_fd,0600)==0&&stat(slave,&after)==0&&after.st_uid==0);
    close(master);CHECK(fstat(slave_fd,&after)==0);close(slave_fd);
    // New PTYs are unowned. Acquiring one in a new session must update its
    // session and foreground group, while keeping other sessions isolated.
    master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0&&unlockpt(master)==0);
    slave_fd=open(ptsname(master),O_RDWR|O_NOCTTY);CHECK(slave_fd>=0);
    CHECK(tcgetsid(slave_fd)==-1&&errno==ENOTTY);
    child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(setsid()==getpid());CHECK(ioctl(slave_fd,TIOCSCTTY,0)==0);
        CHECK(tcgetsid(slave_fd)==getsid(0)&&tcgetpgrp(slave_fd)==getpgrp());
        int tty=open("/dev/tty",O_RDWR);CHECK(tty>=0);close(tty);
        pid_t nested=fork();CHECK(nested>=0);
        if(!nested) {
            CHECK(ioctl(slave_fd,TIOCSCTTY,0)==-1&&errno==EPERM);
            CHECK(setsid()==getpid());
            CHECK(tcgetpgrp(slave_fd)==-1&&errno==ENOTTY);
            CHECK(ioctl(slave_fd,TIOCSCTTY,0)==-1&&errno==EPERM);
            CHECK(tcsetpgrp(slave_fd,getpgrp())==-1&&errno==ENOTTY);
            _exit(0);
        }
        finished(nested);CHECK(tcgetsid(slave_fd)==getsid(0));_exit(0);
    }
    finished(child);
    CHECK(tcgetsid(master)==-1&&errno==ENOTTY);
    child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(setsid()==getpid());CHECK(ioctl(slave_fd,TIOCSCTTY,0)==0);
        CHECK(tcgetsid(slave_fd)==getsid(0)&&tcgetpgrp(slave_fd)==getpgrp());_exit(0);
    }
    finished(child);close(slave_fd);close(master);
    int sockets[2];char buffer[8];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    child=fork();CHECK(child>=0);
    if(!child) { close(sockets[0]);CHECK(write(sockets[1],"abc",3)==3);usleep(50000);CHECK(write(sockets[1],"def",3)==3);close(sockets[1]);_exit(0); }
    close(sockets[1]);CHECK(recv(sockets[0],buffer,6,MSG_WAITALL|MSG_PEEK)==6&&!memcmp(buffer,"abcdef",6));
    CHECK(recv(sockets[0],buffer,6,MSG_WAITALL)==6&&!memcmp(buffer,"abcdef",6));
    CHECK(recv(sockets[0],buffer,1,MSG_WAITALL)==0);close(sockets[0]);finished(child);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);CHECK(write(sockets[1],"abc",3)==3);
    CHECK(recv(sockets[0],buffer,6,MSG_WAITALL|MSG_DONTWAIT)==3&&!memcmp(buffer,"abc",3));
    CHECK(write(sockets[1],"def",3)==3);close(sockets[1]);
    CHECK(recv(sockets[0],buffer,6,MSG_WAITALL)==3&&!memcmp(buffer,"def",3));close(sockets[0]);
    puts("guest set-ID exec, secure environment, no_new_privs and write stripping ok");return 0;
}
