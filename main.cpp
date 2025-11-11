#include <iostream>
#include <sstream>
#include <cstdlib>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <array>
#include <algorithm>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cerrno>
#include <signal.h>

#define INPUT_SIZE 512
#define TOKEN_SIZE 100

/*
    EXIT --done
    ECHO --done
    LS --done
    CD --done
    PWD --done
    CLEAR --done
    EXPORT --done
    ENV --done
    DISK --done(hexdump)
    GET_ALL_PARTITIONS --done(fdisk)
    UNKNOWN --done
    CRON_TAB --done
    unmount_CRON --done
*/

extern char** environ;

static volatile sig_atomic_t sighup_received = 0;

void signal_handler(int sig);
bool is_executable(const char* path);
int my_exec(const char* file, char** argv);
void my_execvp(char** argv);
void change_directory(std::string path);
void export_command (char** argv);
void create_cron_vfs();
void unmount_cron_vfs();
void execute_command(char** argv);
char* expand_variable(const char* token);
void shell_loop();
void tokenizer(char* input, char** argv);

void signal_handler(int sig) {
    if (sig == SIGHUP) {
        sighup_received = 1;
        std::cout << "configuration reloaded\n";
    }

    if (sig == SIGINT) {
        std::cout << "\nexiting...\n";
        exit(0);
    }
}

bool is_executable(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return S_ISREG(st.st_mode) && 
        (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
}

int my_exec(const char* file, char** argv) {
    if (strchr(file, '/') != NULL){
        execve(file, argv, environ);    
        return -1;
    }

    const char* path_env = getenv("PATH");
    if (!path_env) {
        path_env = "/usr/bin:/bin:/usr/local/bin";
    }

    char* path_copy  = strdup(path_env);
    if (!path_copy) {
        errno = ENOMEM;
        return -1;
    }

    char* dir = strtok(path_copy, ":");
    char full_path[4096];

    while (dir != NULL) {
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, file);
        if (is_executable(full_path)) {
            free(path_copy);
            execve(full_path, argv, environ);
            return -1;
        }

        dir = strtok(NULL, ":");
    }
    
    free(path_copy);
    errno = ENOENT;
    return -1;
}

void my_execvp(char** argv) {
    pid_t pid = fork();

    if (pid == 0) {
        if (my_exec(argv[0], argv) == -1) {
            fprintf(stderr, "command not found: %s\n", argv[0]);
            exit(127);
        }
    }   
    else if (pid < 0) {
        perror("Internal error");
    }
    else {
        int status;
        waitpid(pid, &status, 0);
    }
}

void change_directory(std::string path) {
    if (path.empty() || path == "~") {
        const char* home = getenv("HOME");
        if (home) {
            std::filesystem::current_path(home);
            return;
        } else {
            perror("cd: $HOME not set\n");
            return;
        }
    }

    std::string copyPath = path;
    try {
        std::filesystem::current_path(path);
    }
    catch (std::filesystem::filesystem_error err) {
        std::cout << "cd: The directory \'" << copyPath
           << "\' does not exist" << std::endl;
    }
}

void export_command(char** argv) {
    if (argv[1] == NULL ) {
        perror("export: no arguments");
    } else {
        for (int i = 1; argv[i] != NULL; i++) {
            std::string arg = argv[i];
            size_t equalPos = arg.find("=");

            if (equalPos != std::string::npos) {
                std::string name = arg.substr(0, equalPos);
                std::string value = arg.substr(equalPos + 1);

                if (name.empty()) {
                    perror("export: invalid variable name");
                    return;
                }

                setenv(name.c_str(), value.c_str(), 1);
            }
            else {
                const char* current_value = getenv(arg.c_str());                 
                if (current_value) {
                    setenv(arg.c_str(), current_value, 1);
                }
                else {
                    setenv(arg.c_str(), "", 1);
                }
            }
        }
    }
}

void create_cron_vfs() {
    std::cout << "\nstarting mount\n";

    const char* vfs_dir = "/tmp/vfs";
    std::filesystem::create_directories(vfs_dir);
    
    std::cout << vfs_dir << " was created\n";

    if (system("mountpoint -q /tmp/vfs/ 2>/dev/null") == 0) {
        std::cout << "/tmp/vfs/ is already mounted - unmounting...\n";
        system("sudo unmount /tmp/vfs");
        std::cout << "unmounting was complete\n";
    }

    std::cout << "mounting tmpfs to " << vfs_dir << "...\n";

    if (system("sudo mount -t tmpfs tmpfs /tmp/vfs") != 0) {
        std::cerr << "ERROR: failed to mount tmpfs\n";
        return;
    }

    std::cout << vfs_dir << " was mounted\n";

    std::string cmd = "crontab -l";
    FILE* pipe = popen(cmd.c_str(), "r");

    if (!pipe) {
        std::cerr << "cron: failed to read crontab\n";
        unmount_cron_vfs();
        return;
    }

    char line[1024];
    int idx = 1;

    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char fname[64];
        snprintf(fname, sizeof(fname), "%s/job_%03d", vfs_dir, idx);
        printf("file %s/job_%03d was created\n", vfs_dir, idx++);

        std::ofstream out(fname);
        if (out.is_open()) {
            out << line << std::endl;
            out.close();
            std::cout << "exported: " << fname << " -> " <<
                line << std::endl;
        }
    }

    pclose(pipe);

    std::cout << "mounting was complete\n\n";
}

void unmount_cron_vfs() {
    const std::filesystem::path path = "/tmp/vfs";

    std::cout << "\nstarting unmount " << path << "...\n";
    if (system("sudo unmount /tmp/vfs") == 0) {
        std::cout << path << " was unmounted\n";
    } else {
        std::cout << path << " wasnt mounted\n";
    }
    try {
        if (std::filesystem::exists(path)) {
            std::filesystem::remove_all(path);
            std::filesystem::remove_all("/tmp/vfs");
            std::cout << "/tmp/vfs was deleted\n";
        }
        else {
            std::cout << path << " doesnt exist\n";
        }
    } catch (const std::filesystem::filesystem_error &e) {
        std::cerr << "ERROR: error removing " << path << ": " <<
            e.what() << std::endl;
    }
    
    std::cout << "unmount was complete\n\n";
}

void execute_command(char** argv) {
    if (argv[0] == NULL)
        return;

    std::vector<std::string> commands {
         "echo", "pwd", "ls", "cd", "clear", "mkdir", "env", "cat",
         "export", "fdisk", "sudo", "hexdump", "crontab", "mount_cron",
         "kill", "ps", "unmount_cron", 
    };

    if (strcmp(argv[0], "exit") == 0) {
        exit(0);
    }
    else if (strcmp(argv[0], "cd") == 0) {
        if (argv[1] == NULL) {
            change_directory("");
        }
        else {
            change_directory(argv[1]);
        }
    }
    else if (strcmp(argv[0], "export") == 0) {
        export_command(argv); 
    }
    else if (strcmp(argv[0], "cron") == 0) {
        create_cron_vfs();
    }
    else if (strcmp(argv[0], "unmount_cron") == 0) {
        unmount_cron_vfs();
    }
    else if (std::find(
                begin(commands),
                end(commands),
                argv[0]) != end(commands)) {
        my_execvp(argv);
    }
    else {
         printf("unknown command\n");
    }
}

char* expand_variable(const char* token) {
    if (!token || token[0] != '$') {
        return strdup(token);
    }

    const char* var_start = token + 1;
    const char* p = var_start;
    while (*p && (std::isalnum(*p) || *p == '_')) {
        ++p;
    }

    if (p == var_start) {
        return strdup(token);
    }

    std::string name(var_start, p - var_start);
    std::string result;
    const char* value = std::getenv(name.c_str());
    if (value) {
        result = value;
    }
    
    result += p;

    return strdup(result.c_str());
}

static std::vector<char*> token_storage;

void tokenizer(char* input, char** argv) {
    for (char* ptr : token_storage) {
        free(ptr);
    }
    token_storage.clear();

    char *token = strtok(input, " \t\n");

    int i = 0;
    while (token != NULL && i < TOKEN_SIZE - 1) { 
        char* expand = expand_variable(token);
        
        if (expand) {
            token_storage.push_back(expand);
            argv[i++] = expand;
        }

        token = strtok(NULL, " \t\n");
    }

    argv[i]= NULL;
}

void shell_loop() {
    char input[INPUT_SIZE];
    char *argv[TOKEN_SIZE];
    std::ofstream bash_history(".bash_history", std::ios_base::app);

    while(true) {
        std::string path = std::filesystem::current_path().c_str();
        std::string home = "/home/lly";
        size_t home_pos = 0;
        if ((home_pos = path.find(home)) != std::string::npos) {
            path.replace(home_pos, home.length(), "~");
        }
        printf("llyshell %s-> ", path.c_str());
            
        if (!std::cin.getline(input, INPUT_SIZE)) {
            std::cout << std::endl;
            for (char* ptr : token_storage) {
                free(ptr);
            }

            break;
        }

        if (strcmp(input, "\0") == 0) {
            continue;
        }
        
        bash_history << input << std::endl;
        tokenizer(input, argv);
        execute_command(argv);
    }

    bash_history.close();
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGHUP, signal_handler);
    shell_loop();
    
    return 0;
}
