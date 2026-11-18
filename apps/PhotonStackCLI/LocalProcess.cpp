#include "LocalProcess.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>
#if defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace {
volatile std::sig_atomic_t interrupted=0;
void interrupt(int value){interrupted=value;}
struct Signals {
    struct sigaction previousInt{},previousTerm{};
    Signals(){interrupted=0;struct sigaction a{};a.sa_handler=interrupt;sigemptyset(&a.sa_mask);sigaction(SIGINT,&a,&previousInt);sigaction(SIGTERM,&a,&previousTerm);}
    ~Signals(){sigaction(SIGINT,&previousInt,nullptr);sigaction(SIGTERM,&previousTerm,nullptr);}
};
}
#endif
void runLocalProcess(const std::filesystem::path& executable,const std::vector<std::string>& args,
                     const std::filesystem::path& directory,unsigned timeoutSeconds){
#if defined(__APPLE__) || defined(__linux__)
    if(!executable.is_absolute()||!std::filesystem::is_regular_file(executable)||access(executable.c_str(),X_OK)||timeoutSeconds<1||timeoutSeconds>600)
        throw std::invalid_argument("Select an executable local solve-field and a 1...600 second timeout");
    Signals signals;
    std::vector<std::string> storage{executable.string()};storage.insert(storage.end(),args.begin(),args.end());
    std::vector<char*> argv;for(auto& s:storage)argv.push_back(s.data());argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);posix_spawnattr_init(&attributes);
    int error=posix_spawn_file_actions_addchdir_np(&actions,directory.c_str());
    if(!error)error=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if(!error)error=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"solver.log",O_WRONLY|O_CREAT|O_EXCL,0600);
    if(!error)error=posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO);
    sigset_t mask,defaults;sigemptyset(&mask);sigemptyset(&defaults);sigaddset(&defaults,SIGINT);sigaddset(&defaults,SIGTERM);
    if(!error)error=posix_spawnattr_setsigmask(&attributes,&mask);
    if(!error)error=posix_spawnattr_setsigdefault(&attributes,&defaults);
    if(!error)error=posix_spawnattr_setpgroup(&attributes,0);
    if(!error)error=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
    pid_t pid=-1;
    if(!error)error=posix_spawn(&pid,executable.c_str(),&actions,&attributes,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);
    if(error)throw std::runtime_error("Cannot start solve-field: "+std::string(std::strerror(error)));
    const auto start=std::chrono::steady_clock::now();std::string failure;int status=0;
    for(;;){
        const auto state=waitpid(pid,&status,WNOHANG);
        if(state==pid)break;
        if(state<0&&errno!=EINTR){failure="Cannot wait for local solver";break;}
        std::error_code ec;const auto size=std::filesystem::file_size(directory/"solver.log",ec);
        if(interrupted)failure="Local plate solve cancelled";
        else if(std::chrono::steady_clock::now()-start>std::chrono::seconds(timeoutSeconds))failure="Local plate solve timed out";
        else if(!ec&&size>16*1024*1024)failure="Local solver log exceeded limit";
        if(!failure.empty())break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Also prevent a successful wrapper from leaving a helper behind.
    kill(-pid,SIGTERM);
    if(!failure.empty()){
        std::this_thread::sleep_for(std::chrono::milliseconds(150));kill(-pid,SIGKILL);
        while(waitpid(pid,&status,0)<0&&errno==EINTR){}
        throw std::runtime_error(failure);
    }
    kill(-pid,SIGKILL);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("Local solve-field failed; diagnostic log is on stderr");
#else
    (void)executable;(void)args;(void)directory;(void)timeoutSeconds;
    throw std::runtime_error("Local plate solving is supported on macOS and Linux");
#endif
}
