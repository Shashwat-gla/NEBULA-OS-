
#include <stdio.h>      // printf, fgets, fopen, fclose, remove
#include <string.h>     // strlen, strcmp, strchr, strcspn, memmove
#include <stdlib.h>     // system, exit
#include <time.h>       // time, ctime, difftime
#include <ctype.h>      // isspace
#include <direct.h>     // _getcwd, _chdir, _mkdir (Windows)
#include <io.h>         // _findfirst, _findnext, _findclose (Windows)
#include <sys/stat.h>   // File information and permissions metadata

#define VERSION "0.2"
#define COMMAND_SIZE 512
#define PATH_SIZE 1024

typedef enum {
    GUEST,
    USER,
    ADMIN
} Role;

Role currentRole = USER;
time_t startTime;

/* Remove leading and trailing whitespace. */
void trim(char *s) {
    size_t start = 0;
    size_t len;

    while (isspace((unsigned char)s[start]))
        start++;

    if (start > 0)
        memmove(s, s + start, strlen(s + start) + 1);

    len = strlen(s);

    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* Return the readable role name. */
const char *roleName(void) {
    if (currentRole == ADMIN) return "ADMIN";
    if (currentRole == USER) return "USER";
    return "GUEST";
}

/*
   Demonstration-only role permissions.
   These are application-level checks, not OS security.
*/
int canWrite(void) {
    return currentRole == USER || currentRole == ADMIN;
}

int canDelete(void) {
    return currentRole == ADMIN;
}

void banner(void) {
    printf("\n====================================\n");
    printf("          N A B U L A  O S\n");
    printf("          CLI VERSION %s\n", VERSION);
    printf("====================================\n");
    printf("Type 'help' to see commands.\n");
}

void help(void) {
    printf("\n--- NABULA COMMAND DIRECTORY ---\n");
    printf("help                 Show commands\n");
    printf("info                 System information\n");
    printf("version              Version information\n");
    printf("about                About Nabula OS\n");
    printf("date                 Current date/time\n");
    printf("uptime               CLI session uptime\n");
    printf("whoami               Current demo user\n");
    printf("role                 Show current role\n");
    printf("permissions          Show role permissions\n");
    printf("switchrole <role>    Demo role switching\n");
    printf("echo <text>          Print text\n");
    printf("clear                Clear terminal\n");
    printf("pwd                  Current directory\n");
    printf("ls                   List files and folders\n");
    printf("cd <directory>       Change directory\n");
    printf("mkdir <directory>    Create a directory\n");
    printf("touch <file>         Create a file if absent\n");
    printf("cat <file>           Read a text file\n");
    printf("write <file> <text>  Create/overwrite a text file\n");
    printf("rm <file>            Delete a file (ADMIN only)\n");
    printf("exit                 Close Nabula CLI\n");
}

void showInfo(void) {
    printf("\n--- SYSTEM INFORMATION ---\n");
    printf("Product       : Nabula OS CLI\n");
    printf("Version       : %s\n", VERSION);
    printf("Interface     : Command Line\n");
    printf("Host          : Windows\n");
    printf("Runtime       : Hosted C application\n");
    printf("Current role  : %s\n", roleName());
}

void showAbout(void) {
    printf("\nNabula OS is an experimental command-line project.\n");
    printf("Goals: simplicity, file management and access control.\n");
    printf("This version is a hosted CLI, not a bootable kernel.\n");
}

void showDate(void) {
    time_t now = time(NULL);
    char *text = ctime(&now);

    if (text != NULL)
        printf("Date and time: %s", text);
    else
        printf("Unable to read date/time.\n");
}

void showUptime(void) {
    time_t now = time(NULL);

    if (now == (time_t)-1 || startTime == (time_t)-1) {
        printf("Unable to calculate uptime.\n");
        return;
    }

    long seconds = (long)difftime(now, startTime);

    if (seconds < 0) {
        printf("Invalid elapsed time.\n");
        return;
    }

    printf("CLI uptime: %ldh %ldm %lds\n",
           seconds / 3600,
           (seconds % 3600) / 60,
           seconds % 60);
}

void showPermissions(void) {
    printf("\n--- DEMO ACCESS POLICY ---\n");
    printf("Current role: %s\n", roleName());
    printf("Read/list files : YES\n");
    printf("Create/edit     : %s\n",
           canWrite() ? "YES" : "NO");
    printf("Delete files    : %s\n",
           canDelete() ? "YES" : "NO");
    printf("Role switching  : DEMO ONLY\n");
    printf("WARNING: This is not real OS security.\n");
}

void showPwd(void) {
    char path[PATH_SIZE];

    if (_getcwd(path, sizeof(path)) != NULL)
        printf("%s\n", path);
    else
        printf("Unable to get current directory.\n");
}

/* List entries in the current Windows directory. */
void listFiles(void) {
    struct _finddata_t entry;
    intptr_t handle = _findfirst("*", &entry);

    if (handle == -1) {
        printf("Unable to list directory.\n");
        return;
    }

    do {
        if (strcmp(entry.name, ".") != 0 &&
            strcmp(entry.name, "..") != 0) {
            if (entry.attrib & _A_SUBDIR)
                printf("[DIR]  %s\n", entry.name);
            else
                printf("[FILE] %s\n", entry.name);
        }
    } while (_findnext(handle, &entry) == 0);

    _findclose(handle);
}

void changeDirectory(const char *path) {
    if (path == NULL || *path == '\0') {
        printf("Usage: cd <directory>\n");
        return;
    }

    if (_chdir(path) == 0)
        showPwd();
    else
        printf("Cannot change directory.\n");
}

void makeDirectory(const char *name) {
    if (!canWrite()) {
        printf("Permission denied: USER role required.\n");
        return;
    }

    if (name == NULL || *name == '\0') {
        printf("Usage: mkdir <directory>\n");
        return;
    }

    if (_mkdir(name) == 0)
        printf("Directory created: %s\n", name);
    else
        printf("Unable to create directory.\n");
}

void touchFile(const char *name) {
    FILE *fp;

    if (!canWrite()) {
        printf("Permission denied: USER role required.\n");
        return;
    }

    if (name == NULL || *name == '\0') {
        printf("Usage: touch <file>\n");
        return;
    }

    /* Append mode creates a missing file without truncating an
       existing file's contents. */
    fp = fopen(name, "ab");

    if (fp == NULL) {
        printf("Unable to create/open file.\n");
        return;
    }

    fclose(fp);
    printf("File is ready: %s\n", name);
}

void readFile(const char *name) {
    FILE *fp;
    char line[1024];

    if (name == NULL || *name == '\0') {
        printf("Usage: cat <file>\n");
        return;
    }

    fp = fopen(name, "r");

    if (fp == NULL) {
        printf("Unable to open file.\n");
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
        printf("%s", line);

    fclose(fp);
}

void writeFile(char *args) {
    char *space;
    char *name;
    char *text;
    FILE *fp;

    if (!canWrite()) {
        printf("Permission denied: USER role required.\n");
        return;
    }

    if (args == NULL || *args == '\0') {
        printf("Usage: write <file> <text>\n");
        return;
    }

    name = args;
    space = strchr(args, ' ');

    if (space == NULL) {
        printf("Usage: write <file> <text>\n");
        return;
    }

    *space = '\0';
    text = space + 1;
    trim(name);
    trim(text);

    if (*name == '\0' || *text == '\0') {
        printf("Filename and text are required.\n");
        return;
    }

    /* WARNING: "w" replaces existing file contents. */
    fp = fopen(name, "w");

    if (fp == NULL) {
        printf("Unable to write file.\n");
        return;
    }

    fprintf(fp, "%s\n", text);
    fclose(fp);
    printf("File written: %s\n", name);
}

void deleteFile(const char *name) {
    if (!canDelete()) {
        printf("Permission denied: ADMIN role required.\n");
        return;
    }

    if (name == NULL || *name == '\0') {
        printf("Usage: rm <file>\n");
        return;
    }

    if (remove(name) == 0)
        printf("File deleted: %s\n", name);
    else
        printf("Unable to delete file.\n");
}

/*
   Dispatch a parsed command.
   args points to the remaining text after the command.
*/
void executeCommand(char *cmd, char *args) {
    if (strcmp(cmd, "help") == 0) {
        help();
    }
    else if (strcmp(cmd, "info") == 0) {
        showInfo();
    }
    else if (strcmp(cmd, "version") == 0) {
        printf("Nabula OS CLI %s\n", VERSION);
    }
    else if (strcmp(cmd, "about") == 0) {
        showAbout();
    }
    else if (strcmp(cmd, "date") == 0) {
        showDate();
    }
    else if (strcmp(cmd, "uptime") == 0) {
        showUptime();
    }
    else if (strcmp(cmd, "whoami") == 0) {
        printf("nabula-user (%s role)\n", roleName());
    }
    else if (strcmp(cmd, "role") == 0) {
        printf("Current role: %s\n", roleName());
    }
    else if (strcmp(cmd, "permissions") == 0) {
        showPermissions();
    }
    else if (strcmp(cmd, "switchrole") == 0) {
        if (args == NULL) {
            printf("Usage: switchrole guest|user|admin\n");
        }
        else if (strcmp(args, "guest") == 0) {
            currentRole = GUEST;
            printf("Demo role changed to GUEST.\n");
        }
        else if (strcmp(args, "user") == 0) {
            currentRole = USER;
            printf("Demo role changed to USER.\n");
        }
        else if (strcmp(args, "admin") == 0) {
            currentRole = ADMIN;
            printf("Demo role changed to ADMIN.\n");
            printf("Warning: role switching is not authenticated.\n");
        }
        else {
            printf("Unknown role. Use guest, user or admin.\n");
        }
    }
    else if (strcmp(cmd, "echo") == 0) {
        if (args != NULL)
            printf("%s\n", args);
        else
            printf("\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        if (system("cls") != 0)
            printf("Unable to clear screen.\n");
    }
    else if (strcmp(cmd, "pwd") == 0) {
        showPwd();
    }
    else if (strcmp(cmd, "ls") == 0) {
        listFiles();
    }
    else if (strcmp(cmd, "cd") == 0) {
        changeDirectory(args);
    }
    else if (strcmp(cmd, "mkdir") == 0) {
        makeDirectory(args);
    }
    else if (strcmp(cmd, "touch") == 0) {
        touchFile(args);
    }
    else if (strcmp(cmd, "cat") == 0) {
        readFile(args);
    }
    else if (strcmp(cmd, "write") == 0) {
        writeFile(args);
    }
    else if (strcmp(cmd, "rm") == 0) {
        deleteFile(args);
    }
    else {
        printf("Unknown command: %s\n", cmd);
        printf("Type 'help' for available commands.\n");
    }
}

int main(void) {
    char input[COMMAND_SIZE];

    startTime = time(NULL);
    banner();

    while (1) {
        char *cmd;
        char *args = NULL;
        char *separator;

        printf("\nNabula> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\nInput closed.\n");
            break;
        }

        /* Detect a line longer than the input buffer. */
        if (strchr(input, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            printf("Input too long. Try again.\n");
            continue;
        }

        input[strcspn(input, "\n")] = '\0';
        trim(input);

        if (*input == '\0')
            continue;

        cmd = input;
        separator = strchr(input, ' ');

        if (separator != NULL) {
            *separator = '\0';
            args = separator + 1;
            trim(args);
        }

        if (strcmp(cmd, "exit") == 0) {
            printf("Closing Nabula CLI...\n");
            break;
        }

        executeCommand(cmd, args);
    }

    printf("Nabula CLI closed.\n");
    return 0;
}
