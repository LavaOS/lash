#include <minos/sysstd.h>
#include <minos/status.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <environ.h>
#include <ctype.h>
#include <assert.h>
#include <minos/tty/tty.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

bool fatal_error = false;
int exit_code = 0;

typedef struct {
    char* name;
    char* value;
} ScriptVar;
typedef struct {
    char* name;
    char* body;  // Everything between { }
    size_t body_len;
} ScriptBlock;

#define MAX_SCRIPT_VARS 1024
static ScriptVar script_vars[MAX_SCRIPT_VARS];
static size_t script_var_count = 0;

void redirect_stdout_to_file(const char* filename) {
    close(STDOUT_FILENO);
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd != STDOUT_FILENO) {
        // Danger!
    }
}
void redirect_stdin_from_file(const char* filename) {
    close(STDIN_FILENO);
    open(filename, O_RDONLY);
}

intptr_t readline(char* buf, size_t bufmax) {
    intptr_t e;
    size_t n = 0;
    while(n < bufmax) {
        e = read(STDIN_FILENO, buf + n, bufmax - n);
        if(e < 0) return e;
        assert(e != 0);
        n += e;
        if(buf[n-1] == '\n') break;
    }
    if(n >= bufmax) return -BUFFER_TOO_SMALL;
    return n;
}
char* trim_r(char* buf) {
    char* start = buf;
    while(*buf) buf++;
    buf--;
    while(buf >= start && isspace(buf[0])) {
        *buf = '\0';
        buf--;
    }
    return start;
}
char* trim_l(const char* buf) {
    while(buf[0] && isspace(buf[0])) buf++;
    return (char*)buf;
}
typedef struct ArenaNode {
    struct ArenaNode* next;
    size_t len, cap;
    char data[];
} ArenaNode;
typedef struct {
    ArenaNode* node;
} Arena;
#define _STRINGIFY(x) # x
#define STRINGIFY(x) _STRINGIFY(x)
ArenaNode* arena_node_new(size_t size) {
    size = ((size+15)/16)*16;
    ArenaNode* node = malloc(sizeof(ArenaNode)+size);
    assert(node && "Ran out of memory");
    node->next = NULL;
    node->len = 0; 
    node->cap = size;
    return node;
}
#define alignup_to(n, t) (((n+(sizeof(t)-1)) / sizeof(t))*sizeof(t))
void* arena_node_alloc_within(ArenaNode* node, size_t size) {
    if(node->cap < node->len+size) return NULL;
    void* at = node->data+node->len;
    node->len += size;
    node->len = alignup_to(node->len, uintptr_t);
    return at;
}

#define MIN_ARENA_SIZE 1024
void* arena_alloc(Arena* arena, size_t size) {
    size_t ncap = size < MIN_ARENA_SIZE ? MIN_ARENA_SIZE : size*2;
    if(!arena->node) {
        arena->node = arena_node_new(ncap);
        return arena_node_alloc_within(arena->node, size);
    }
    ArenaNode* prev = NULL;
    ArenaNode* node = arena->node;
    while(node) {
        void* at = arena_node_alloc_within(node, size);
        if(at) return at;
        prev = node;
        node = node->next;
    }
    prev->next = arena_node_new(ncap);
    return arena_node_alloc_within(prev->next, size);
}
void arena_reset(Arena* arena) {
    ArenaNode* node = arena->node;
    while(node) {
        node->len = 0;
        node = node->next;
    }
}
void arena_drop(Arena* arena) {
    ArenaNode* node = arena->node;
    while(node) {
        ArenaNode* next = node->next;
        free(node);
        node = next;
    }
    arena->node = NULL;
}
char* dup_str_range(Arena* arena, const char* start, const char* end) {
    size_t len = (size_t)(end-start);
    char* str = arena_alloc(arena, len+1);
    memcpy(str, start, len);
    str[len] = '\0';
    return str;
}
char* strip_cmd(Arena* arena, char** str_result) {
    char* at=*str_result;
    char* begin=at;
    while(*at) {
        if(isspace(*at)) {
            *str_result = at+1;
            return dup_str_range(arena, begin, at);
        }
        at++;
    }
    *str_result = at;
    return dup_str_range(arena, begin, at);
}
char* strip_arg(Arena* arena, char** str_result) {
    char* at=trim_l(*str_result);
    char* begin=at;
    if(at[0] == '"') {
        fprintf(stderr, "ERROR: PARSING QUOTED ARGUMENTS IS NOT YET SUPPORTED");
        exit(1);
    }
    while(*at) {
        if(isspace(*at)) {
            *str_result = at+1;
            return dup_str_range(arena, begin, at);
        }
        at++;
    }
    *str_result = at;
    return dup_str_range(arena, begin, at);
}
#define LINEBUF_MAX 1024
#define MAX_ARGS 128
typedef struct {
    size_t pid;
    size_t exit_code;
} Cmd;
#define EXEC_STATUS_OFF 1024 
intptr_t spawn_cmd(Cmd* cmd, char** argv) {
    int e = fork();
    if(e == 0) {
        execvp(argv[0], argv);
        exit(EXEC_STATUS_OFF + errno);
    } else if (e >= 0) {
        cmd->pid = e;
        return 0;
    } else {
        return -errno;
    }
}
intptr_t wait_cmd(Cmd* cmd) {
    intptr_t e = wait_pid(cmd->pid);
    cmd->exit_code = e;
    if(e < 0) return e;
    return e > EXEC_STATUS_OFF ? -(e - EXEC_STATUS_OFF) : e; 
}

void handle_var(char** args, size_t arg_count);
void run_command_string(Arena* arena, const char* cmd_str, char* cwd, 
                        char** args, size_t* arg_count);
void run_script_file(Arena* arena, const char* filename, char* cwd,
                     char** args, size_t* arg_count);
void run_command_with_redirection(char** args);

void run_cmd(char** argv) {
    // Check built-in commands
    if (strcmp(argv[0], "VAR") == 0) {
        size_t count = 0;
        while (argv[count]) count++;
        handle_var(argv, count);
        return;
    }
    if (strcmp(argv[0], "cd") == 0) {
        // Handle cd (needs in-process execution)
        const char* path = argv[1] ? argv[1] : "/";
        if (chdir(path) < 0) {
            fprintf(stderr, "cd: %s\n", strerror(errno));
        }
        return;
    }
    Cmd cmd = { 0 };
    intptr_t e;
    if((e=spawn_cmd(&cmd, argv)) < 0) {
        fprintf(stderr, "ERROR: FORK %s\n", status_str(e));
        exit_code = 1;
        fatal_error = true;
        return;
    }
    if((e=wait_cmd(&cmd)) < 0) {
        fprintf(stderr, "BAD COMMAND!\n", status_str(e));
        return;
    }
    if(e != 0) printf("%s CLOSED WITH CODE %d\n", argv[0], (int)e);
}
void run_command_string(Arena* arena, const char* cmd_str, char* cwd, 
                        char** args, size_t* arg_count) {
    char* line = arena_alloc(arena, strlen(cmd_str) + 1);
    strcpy(line, cmd_str);
    line = trim_r(line);
    
    if (line[0] == '\0') return;
    
    *arg_count = 0;
    char* cmd = strip_cmd(arena, &line);
    args[(*arg_count)++] = cmd;
    
    line = trim_l(line);
    while (line[0]) {
        if (*arg_count == MAX_ARGS) break;
        char* arg = strip_arg(arena, &line);
        args[(*arg_count)++] = arg;
        line = trim_l(line);
    }
    args[(*arg_count)++] = NULL;
    run_cmd(args);
}
void run_script_file(Arena* arena, const char* filename, char* cwd,
                     char** args, size_t* arg_count) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open script: %s\n", filename);
        return;
    }
    
    char linebuf[1024];
    while (fgets(linebuf, sizeof(linebuf), f)) {
        char* line = trim_r(linebuf);
        if (line[0] == '\0' || line[0] == '#') continue;  // Skip comments and empty
        
        run_command_string(arena, line, cwd, args, arg_count);
    }
    fclose(f);
}
void script_var_set(const char* name, const char* value) {
    for (size_t i = 0; i < script_var_count; ++i) {
        if (strcmp(script_vars[i].name, name) == 0) {
            free(script_vars[i].value);
            script_vars[i].value = strdup(value);
            return;
        }
    }
    if (script_var_count < MAX_SCRIPT_VARS) {
        script_vars[script_var_count].name = strdup(name);
        script_vars[script_var_count].value = strdup(value);
        script_var_count++;
    }
}

// Get a variable
const char* script_var_get(const char* name) {
    for (size_t i = 0; i < script_var_count; ++i) {
        if (strcmp(script_vars[i].name, name) == 0)
            return script_vars[i].value;
    }
    return NULL;
}

// Parse VAR command
void handle_var(char** args, size_t arg_count) {
    if (arg_count < 3) {
        fprintf(stderr, "VAR: usage: VAR <name> <value>\n");
        return;
    }
    // args[1] = name, args[2+] = value (everything after name)
    script_var_set(args[1], args[2]);
}
void run_command_with_redirection(char** args) {
    for (size_t i = 0; args[i]; ++i) {
        if (strcmp(args[i], ">") == 0) {
            args[i] = NULL;
            const char* file = args[i + 1];
            if (file) {
                close(STDOUT_FILENO);
                open(file, O_WRONLY | O_CREAT | O_TRUNC);
            }
            run_cmd(args);
            return;
        }
        if (strcmp(args[i], "<") == 0) {
            args[i] = NULL;
            const char* file = args[i + 1];
            if (file) {
                close(STDIN_FILENO);
                open(file, O_RDONLY);
            }
            run_cmd(args);
            return;
        }
    }
    run_cmd(args);
}
void parse_script_blocks(const char* filename, Arena* arena, 
                         char* cwd, char** args, size_t* arg_count) {
    FILE* f = fopen(filename, "r");
    if (!f) return;
    
    char linebuf[1024];
    bool in_block = false;
    char block_name[256];
    char block_body[4096];
    size_t body_len = 0;
    
    while (fgets(linebuf, sizeof(linebuf), f)) {
        char* line = trim_r(linebuf);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        if (!in_block) {
            // Check for block definition: name(args) {
            char* brace = strchr(line, '{');
            if (brace) {
                *brace = '\0';
                strcpy(block_name, line);
                in_block = true;
                body_len = 0;
                // Everything after { on same line
                char* after = brace + 1;
                while (*after && *after != '}') {
                    block_body[body_len++] = *after++;
                }
            } else {
                // Regular command
                run_command_string(arena, line, cwd, args, &arg_count);
            }
        } else {
            // Inside block
            char* close = strchr(line, '}');
            if (close) {
                // End of block
                *close = '\0';
                strcpy(block_body + body_len, line);
                body_len += strlen(line);
                in_block = false;
                
                // Execute block if it's a function
                // Store block for later use
            } else {
                strcpy(block_body + body_len, line);
                body_len += strlen(line);
                block_body[body_len++] = '\n';
            }
        }
    }
    fclose(f);
}
int main(int argc, char** argv) {
    Arena arena={0};
    char* linebuf = malloc(LINEBUF_MAX);
    intptr_t e = 0;
    assert(MAX_ARGS > 0);
    char** args = malloc(MAX_ARGS*sizeof(*args));
    char* cwd = malloc(PATH_MAX);

    if(getcwd(cwd, PATH_MAX) == NULL) {
        fprintf(stderr, "ERROR: FAILED TO GETCWD\n");
        free(args); free(cwd); free(linebuf);
        return 1;
    }
    
    size_t arg_count=0;
    bool running = true;
    bool interactive = true;
    const char* script_file = NULL;
    const char* command_str = NULL;

    // Parse shell arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--command") == 0) {
            if (i + 1 < argc) {
                // Take everything after -c as the command
                command_str = argv[i + 1];
                // Rest of args are for the command
                // Build command line from remaining args
                size_t cmd_len = strlen(argv[i + 1]);
                for (int j = i + 2; j < argc; ++j) {
                    cmd_len += strlen(argv[j]) + 1;
                }
                char* full_cmd = malloc(cmd_len + 1);
                strcpy(full_cmd, argv[i + 1]);
                for (int j = i + 2; j < argc; ++j) {
                    strcat(full_cmd, " ");
                    strcat(full_cmd, argv[j]);
                }
                command_str = full_cmd;
                interactive = false;
                break;
            }
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--script") == 0) {
            if (i + 1 < argc) {
                script_file = argv[i + 1];
                interactive = false;
                break;
            }
        }
    }

    if (!interactive) {
        if (command_str) {
            // Execute single command
            run_command_string(&arena, command_str, cwd, args, &arg_count);
        } else if (script_file) {
            // Execute script file
            run_script_file(&arena, script_file, cwd, args, &arg_count);
        }
    } else {
        // Interactive mode
        printf("Welcome to LASH! LAvaos SHell.\n");
        while(running) {
            const char* u = getenv("USER");

            printf("\033[36m");
            printf("%s ", cwd);
            printf("\033[0m");
            printf("LASH");
            printf("> ");
            arena_reset(&arena);
            arg_count=0;
            fflush(stdout);
            if((e=readline(linebuf, LINEBUF_MAX-1)) < 0) {
                fprintf(stderr, "FAILED TO READ ON STDIN: %s\n", status_str(e));
                exit_code = 1;
                fatal_error = true;
                break;
            }

            if (fatal_error)
                break;

            linebuf[e] = 0;
            char* line = trim_r(linebuf);
            // Empty
            if(line[0] == '\0') continue;
            char* cmd = strip_cmd(&arena, &line);
            args[arg_count++] = cmd;
            while((line=trim_l(line))[0]) {
                if(arg_count == MAX_ARGS) {
                    printf("TOO MANY ARGUMENTS\n");
                    continue;
                }
                char* arg = strip_arg(&arena, &line);
                args[arg_count++] = arg;
            }
            if(strcmp(cmd, "exit") == 0) {
                running = false;
                if (arg_count == 1) exit_code=0;
                else if (arg_count == 2) {
                    char* end;
                    exit_code = strtoll(args[1], &end, 10);
                    if(end[0] != '\0') {
                        fprintf(stderr, "INVALID EXIT CODE `%s`\n", args[1]);
                        exit_code = 1;
                        continue;
                    }
                } else {
                    fprintf(stderr, "EXIT: TOO MANY ARGUMENTS\n");
                    continue;
                }
            } else if (strcmp(cmd, "reset") == 0) {
                tty_set_flags(fileno(stdin), TTY_ECHO);
            } else if (strcmp(cmd, "cd") == 0) {
                const char* path = (arg_count < 2) ? "/" : args[1];

                if (arg_count > 2) {
                    fprintf(stderr, "CD: TOO MANY ARGUMENTS\n");
                    continue;
                }

                if((e = chdir(path)) < 0) {
                    fprintf(stderr, "FAILED TO CD INTO `%s`: %s\n", path, status_str(e));
                    continue;
                }

                if(getcwd(cwd, PATH_MAX) == NULL) {
                    fprintf(stderr, "FAILED TO GETCWD: %s\n", strerror(errno));
                    exit(1);
                }
            } else {
                args[arg_count++] = NULL;
                run_cmd(args);
            }
        }
    }
    arena_drop(&arena);
    free(args); free(cwd); free(linebuf);
    return exit_code;
}
