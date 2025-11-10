#include <iostream>
#include <cstdlib>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
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
    UMOUNT_CRON
*/

extern char** environ;

static volatile sig_atomic_t sighup_received = 0;

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
    char** argv;
}

void umount_cron_vfs() {
    char** argv;

}

void execute_command(char** argv) {
    if (argv[0] == NULL)
        return;

    std::vector<std::string> commands {
         "echo", "pwd", "ls", "cd", "clear", "mkdir", "env", "cat",
         "export", "fdisk", "sudo", "hexdump", "crontab", "cron",
         "kill", "ps", "umount_cron", 
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
    else if (strcmp(argv[0], "umount_cron") == 0) {
        umount_cron_vfs();
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
