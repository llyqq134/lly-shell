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
#include <filesystem>
#include <fstream>

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

void my_execvp(char** argv) {
    pid_t pid = fork();

    if (pid == 0) {
        int child_status = execvp(*argv, argv);
        if (child_status < 0) {
            perror("Error: wrong input");
        }
        exit(0);
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
    if (path.compare("") == 0) {
        std::filesystem::current_path("/home/lly");
        return;
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

void execute_command(char** argv) {
    std::vector<std::string> commands {
         "echo", "pwd", "ls", "cd", "clear", "mkdir", "env",
         "export", "fdisk", "sudo", "hexdump", "cron", "cat",
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

void tokenizer(char* input, char** argv) {
    char *tokens = strtok(input, " ");

    while (tokens != NULL) {    
        *argv++ = tokens;
        tokens = strtok(NULL, " ");
    }

    *argv = NULL;
}

void shell_loop(char** env) {
    char input[INPUT_SIZE];
    char *argv[TOKEN_SIZE];
    std::ofstream bash_history(".bash_history", std::ios_base::app);

    while(true) {
        std::filesystem::path path = std::filesystem::current_path();
        printf("llyshell %s-> ", path.c_str());
        std::cin.getline(input, INPUT_SIZE);

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
    char** env;
    shell_loop(env);
    
    return 0;
}
